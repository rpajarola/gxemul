/*
 *  Copyright (C) 2026  Rico Pajarola.  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 *  OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 *  SUCH DAMAGE.
 *
 *
 *  The control socket: lets a program (a script, a test harness, an AI
 *  agent) drive the emulator with one line of JSON per request, response,
 *  and event. See doc/control.html for the protocol.
 *
 *  --control unix:path or --control tcp:port makes the emulator listen;
 *  control_poll() is called from the main loop (at most every few ms) and
 *  while the debugger waits at its prompt, so requests are handled on the
 *  emulator's own thread, between slices of instructions. One client at a
 *  time.
 *
 *  gxemul --ctl socket ... is a client, for sending one request from a
 *  shell (control_client_main()).
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "sockets.h"
#ifndef _WIN32
#include <sys/un.h>
#endif

#include "console.h"
#include "control.h"
#include "cpu.h"
#include "debugger.h"
#include "emul.h"
#include "machine.h"
#include "misc.h"

#include "thirdparty/cJSON.h"


#define	CONTROL_PROTO_VERSION		1

/*  How often the main loop looks at the socket:  */
#define	CONTROL_POLL_INTERVAL_US	5000

#define	CONTROL_MAX_LINE		(1 << 20)
#define	CONTROL_READ_MAX		(1 << 20)
#define	CONTROL_READ_DEFAULT		65536

/*  A client which doesn't read its responses for this long is dropped:  */
#define	CONTROL_SEND_TIMEOUT_SEC	5


extern bool emul_shutdown;
extern int debugger_cur_cpu;
extern int debugger_cur_machine;

static struct emul *ctl_emul;
static int listen_fd = -1;
static int client_fd = -1;
static char *unix_path = NULL;

static char *inbuf = NULL;
static size_t inlen = 0;

static bool in_poll = false;
static int64_t last_poll_us = 0;

/*  How the machine last stopped:  */
static struct {
	bool		valid;
	char		reason[40];
	int		cpu;
	char		pc[24];
	int		bp;		/*  breakpoint id, or 0  */
	int64_t		ninstrs;
} last_stop;

struct ctl_error {
	const char	*code;
	char		message[200];
};

#define	CTL_HANDLER(n)	static bool n(cJSON *req, cJSON *result, \
	struct ctl_error *err)


static int64_t ninstrs_now(void)
{
	struct machine *m = ctl_emul->machines[0];
	return m->cpus[m->bootstrap_cpu]->ninstrs;
}


static struct machine *cur_machine(void)
{
	return ctl_emul->machines[debugger_cur_machine];
}


/*
 *  An address as a string, 8 or 16 hex digits depending on the CPU:
 */
static void format_addr(char *buf, size_t len, struct cpu *cpu, uint64_t v)
{
	if (cpu->is_32bit)
		snprintf(buf, len, "0x%08" PRIx32, (uint32_t) v);
	else
		snprintf(buf, len, "0x%016" PRIx64, (uint64_t) v);
}


static void add_addr(cJSON *o, const char *key, struct cpu *cpu, uint64_t v)
{
	char buf[24];
	format_addr(buf, sizeof(buf), cpu, v);
	cJSON_AddStringToObject(o, key, buf);
}


static bool fail(struct ctl_error *err, const char *code, const char *fmt, ...)
{
	va_list ap;

	err->code = code;
	va_start(ap, fmt);
	vsnprintf(err->message, sizeof(err->message), fmt, ap);
	va_end(ap);
	return false;
}


/*
 *  get_int():
 *
 *  An integer parameter: a JSON number, or a string such as "10" or
 *  "0x10". Absent means def. Returns false (with err set) if it's
 *  something else.
 */
static bool get_int(cJSON *req, const char *key, int64_t def, int64_t *valuep,
	struct ctl_error *err)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(req, key);
	char *end;

	*valuep = def;
	if (item == NULL || cJSON_IsNull(item))
		return true;

	if (cJSON_IsNumber(item)) {
		*valuep = (int64_t) item->valuedouble;
		return true;
	}

	if (cJSON_IsString(item) && item->valuestring[0] != '\0') {
		*valuep = strtoll(item->valuestring, &end, 0);
		if (*end == '\0')
			return true;
	}

	return fail(err, "bad_arg", "'%s' should be an integer", key);
}


/*
 *  get_expr():
 *
 *  An address parameter, as an expression for debugger_parse_expression():
 *  a string as is, or a number. Returns a malloced string, or NULL (with
 *  err set).
 */
static char *get_expr(cJSON *req, const char *key, struct ctl_error *err)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(req, key);
	char buf[24], *s;

	if (cJSON_IsString(item) && item->valuestring[0] != '\0') {
		CHECK_ALLOCATION(s = strdup(item->valuestring));
		return s;
	}

	if (cJSON_IsNumber(item) && item->valuedouble >= 0) {
		snprintf(buf, sizeof(buf), "0x%" PRIx64,
		    (uint64_t) item->valuedouble);
		CHECK_ALLOCATION(s = strdup(buf));
		return s;
	}

	fail(err, "bad_arg", "'%s' should be an address", key);
	return NULL;
}


/*
 *  get_console():
 *
 *  The "console" parameter: a console handle (not the emulator's own
 *  terminal, handle 0), the main console of the first machine by default.
 */
static bool get_console(cJSON *req, int *handlep, bool for_input,
	struct ctl_error *err)
{
	const char *name, *machine_name;
	bool input, output;
	int64_t h;

	if (!get_int(req, "console", ctl_emul->machines[0]->main_console_handle,
	    &h, err))
		return false;

	if (h <= MAIN_CONSOLE || h >= console_n_handles() ||
	    !console_handle_info(h, &name, &machine_name, &input, &output) ||
	    (for_input? !input : !output))
		return fail(err, "no_such_console", "no console %" PRIi64
		    " for %s", h, for_input? "input" : "output");

	*handlep = h;
	return true;
}


/*****************************************************************************/


/*
 *  Sending:
 */

static void disconnect_client(void)
{
	if (client_fd < 0)
		return;

	socket_close(client_fd);
	client_fd = -1;
	inlen = 0;
}


static bool send_all(int fd, const char *buf, size_t len)
{
#ifdef MSG_NOSIGNAL
	int flags = MSG_NOSIGNAL;
#else
	int flags = 0;
#endif

	while (len > 0) {
		ssize_t n = send(fd, buf, len, flags);
		if (n > 0) {
			buf += n;
			len -= n;
			continue;
		}

		int e = socket_error();
		if (n < 0 && e == SOCKET_EINTR)
			continue;
		if (n < 0 && socket_would_block(e)) {
			fd_set wfds;
			struct timeval tv;

			FD_ZERO(&wfds);
			FD_SET(fd, &wfds);
			tv.tv_sec = CONTROL_SEND_TIMEOUT_SEC;
			tv.tv_usec = 0;
			if (select(fd + 1, NULL, &wfds, NULL, &tv) > 0)
				continue;
		}

		return false;
	}

	return true;
}


/*
 *  send_msg():
 *
 *  Sends a message to the client as one line, and frees it.
 */
static void send_msg(cJSON *msg)
{
	char *s;

	if (client_fd >= 0) {
		CHECK_ALLOCATION(s = cJSON_PrintUnformatted(msg));
		size_t len = strlen(s);
		CHECK_ALLOCATION(s = (char *) realloc(s, len + 2));
		s[len] = '\n';
		s[len + 1] = '\0';

		if (!send_all(client_fd, s, len + 1))
			disconnect_client();

		free(s);
	}

	cJSON_Delete(msg);
}


static void add_stop(cJSON *o)
{
	cJSON_AddStringToObject(o, "reason", last_stop.reason);
	if (last_stop.bp > 0)
		cJSON_AddNumberToObject(o, "bp", last_stop.bp);
	cJSON_AddNumberToObject(o, "cpu", last_stop.cpu);
	cJSON_AddStringToObject(o, "pc", last_stop.pc);
}


/*
 *  control_note_stop():
 *
 *  Called by the debugger when it stops at its prompt. Remembers how, for
 *  the status command, and sends a stopped event.
 */
void control_note_stop(struct machine *m, const char *reason)
{
	int cpu = debugger_cur_cpu, bp = 0;

	if (listen_fd < 0)
		return;

	if (cpu < 0 || cpu >= m->ncpus)
		cpu = 0;

	/*  Which CPU, at which breakpoint? (Compared as in cpu_dyntrans.c,
	    in 32 bits for 32-bit CPUs.)  */
	if (strcmp(reason, "breakpoint") == 0)
		for (int j = 0; j < m->ncpus && bp == 0; j++) {
			uint64_t mask = m->cpus[j]->is_32bit?
			    0xffffffffULL : ~0ULL;
			for (int i = 0; i < m->breakpoints.n; i++)
				if ((m->cpus[j]->pc & mask) ==
				    (m->breakpoints.addr[i] & mask)) {
					cpu = j;
					bp = m->breakpoints.id[i];
					break;
				}
		}

	last_stop.valid = true;
	strlcpy(last_stop.reason, reason, sizeof(last_stop.reason));
	last_stop.cpu = cpu;
	format_addr(last_stop.pc, sizeof(last_stop.pc), m->cpus[cpu],
	    m->cpus[cpu]->pc);
	last_stop.bp = bp;
	last_stop.ninstrs = ninstrs_now();

	if (client_fd < 0)
		return;

	cJSON *ev = cJSON_CreateObject();
	cJSON_AddStringToObject(ev, "event", "stopped");
	add_stop(ev);
	cJSON_AddNumberToObject(ev, "ninstrs", last_stop.ninstrs);
	send_msg(ev);
}


/*****************************************************************************/


/*
 *  Command handlers. Each fills in result, or returns false with err set.
 */

CTL_HANDLER(cmd_hello)
{
	cJSON *machines = cJSON_AddArrayToObject(result, "machines");
	cJSON *consoles = cJSON_AddArrayToObject(result, "consoles");

	cJSON_AddNumberToObject(result, "proto", CONTROL_PROTO_VERSION);
	cJSON_AddStringToObject(result, "version", VERSION);
	cJSON_AddStringToObject(result, "state",
	    debugger_is_stopped()? "stopped" : "running");

	for (int i = 0; i < ctl_emul->n_machines; i++) {
		struct machine *m = ctl_emul->machines[i];
		cJSON *mo = cJSON_CreateObject();
		cJSON *cpus = cJSON_CreateArray();

		cJSON_AddStringToObject(mo, "name", m->name != NULL?
		    m->name : "");
		if (m->machine_name != NULL)
			cJSON_AddStringToObject(mo, "type", m->machine_name);

		for (int j = 0; j < m->ncpus; j++) {
			struct cpu *c = m->cpus[j];
			cJSON *co = cJSON_CreateObject();
			cJSON_AddNumberToObject(co, "id", j);
			cJSON_AddStringToObject(co, "name", c->name);
			cJSON_AddStringToObject(co, "arch",
			    c->cpu_family != NULL? c->cpu_family->name : "");
			cJSON_AddNumberToObject(co, "bits",
			    c->is_32bit? 32 : 64);
			cJSON_AddStringToObject(co, "endian",
			    c->byte_order == EMUL_BIG_ENDIAN? "big" : "little");
			cJSON_AddItemToArray(cpus, co);
		}

		cJSON_AddItemToObject(mo, "cpus", cpus);
		cJSON_AddItemToArray(machines, mo);
	}

	for (int h = MAIN_CONSOLE + 1; h < console_n_handles(); h++) {
		const char *name, *machine_name;
		bool input, output;

		if (!console_handle_info(h, &name, &machine_name,
		    &input, &output))
			continue;

		cJSON *co = cJSON_CreateObject();
		cJSON_AddNumberToObject(co, "id", h);
		cJSON_AddStringToObject(co, "name", name);
		cJSON_AddStringToObject(co, "machine", machine_name);
		cJSON_AddBoolToObject(co, "input", input);
		cJSON_AddBoolToObject(co, "output", output);
		cJSON_AddItemToArray(consoles, co);
	}

	cJSON_AddNumberToObject(result, "default_console",
	    ctl_emul->machines[0]->main_console_handle);
	return true;
}


CTL_HANDLER(cmd_status)
{
	struct machine *m = cur_machine();
	cJSON *cpus;

	cJSON_AddStringToObject(result, "state",
	    debugger_is_stopped()? "stopped" : "running");

	if (last_stop.valid) {
		cJSON *stop = cJSON_AddObjectToObject(result, "stop");
		add_stop(stop);
		cJSON_AddNumberToObject(stop, "ninstrs", last_stop.ninstrs);
	}

	cpus = cJSON_AddArrayToObject(result, "cpus");
	for (int j = 0; j < m->ncpus; j++) {
		cJSON *co = cJSON_CreateObject();
		cJSON_AddNumberToObject(co, "id", j);
		add_addr(co, "pc", m->cpus[j], m->cpus[j]->pc);
		cJSON_AddBoolToObject(co, "halted", !m->cpus[j]->running);
		cJSON_AddItemToArray(cpus, co);
	}

	return true;
}


CTL_HANDLER(cmd_pause)
{
	bool stopped = debugger_is_stopped();

	cJSON_AddBoolToObject(result, "already_stopped", stopped);
	if (!stopped)
		debugger_ctl_pause();
	return true;
}


CTL_HANDLER(cmd_continue)
{
	bool stopped = debugger_is_stopped();

	cJSON_AddBoolToObject(result, "already_running", !stopped);
	if (stopped)
		debugger_ctl_continue();
	return true;
}


CTL_HANDLER(cmd_step)
{
	int64_t n;

	if (!get_int(req, "n", 1, &n, err))
		return false;
	if (n < 1 || n > INT32_MAX)
		return fail(err, "bad_arg", "'n' should be at least 1");
	if (!debugger_is_stopped())
		return fail(err, "not_stopped", "the machine is running; "
		    "pause it first");

	debugger_ctl_step(n);
	return true;
}


CTL_HANDLER(cmd_quit)
{
	emul_shutdown = true;
	if (debugger_is_stopped())
		debugger_ctl_continue();
	return true;
}


static void add_breakpoint(cJSON *o, struct machine *m, int i)
{
	cJSON_AddNumberToObject(o, "id", m->breakpoints.id[i]);
	add_addr(o, "addr", m->cpus[0], m->breakpoints.addr[i]);
	cJSON_AddStringToObject(o, "expr", m->breakpoints.string[i] != NULL?
	    m->breakpoints.string[i] : "");
}


CTL_HANDLER(cmd_break_add)
{
	struct machine *m = cur_machine();
	char *expr = get_expr(req, "pc", err);
	int i;

	if (expr == NULL)
		return false;

	i = debugger_breakpoint_add(m, expr);
	if (i < 0) {
		fail(err, "bad_arg", "couldn't parse '%s'", expr);
		free(expr);
		return false;
	}

	free(expr);
	add_breakpoint(result, m, i);
	return true;
}


CTL_HANDLER(cmd_break_del)
{
	struct machine *m = cur_machine();
	int64_t id;

	if (!get_int(req, "id", -1, &id, err))
		return false;

	for (int i = 0; i < m->breakpoints.n; i++)
		if (m->breakpoints.id[i] == id) {
			debugger_breakpoint_delete(m, i);
			return true;
		}

	return fail(err, "no_such_breakpoint", "no breakpoint with id %"
	    PRIi64, id);
}


CTL_HANDLER(cmd_break_list)
{
	struct machine *m = cur_machine();
	cJSON *list = cJSON_AddArrayToObject(result, "breakpoints");

	for (int i = 0; i < m->breakpoints.n; i++) {
		cJSON *o = cJSON_CreateObject();
		add_breakpoint(o, m, i);
		cJSON_AddItemToArray(list, o);
	}

	return true;
}


static bool get_hex_encoding(cJSON *req, bool *hexp, struct ctl_error *err)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(req, "encoding");

	*hexp = false;
	if (item == NULL || cJSON_IsNull(item))
		return true;
	if (cJSON_IsString(item) && strcmp(item->valuestring, "text") == 0)
		return true;
	if (cJSON_IsString(item) && strcmp(item->valuestring, "hex") == 0) {
		*hexp = true;
		return true;
	}

	return fail(err, "bad_arg", "'encoding' should be \"text\" or \"hex\"");
}


CTL_HANDLER(cmd_console_read)
{
	int64_t since, max;
	uint64_t start, total;
	unsigned char *buf;
	char *out, *p;
	size_t n;
	bool hex;
	int h = 0;

	if (!get_console(req, &h, false, err) ||
	    !get_int(req, "since", 0, &since, err) ||
	    !get_int(req, "max", CONTROL_READ_DEFAULT, &max, err) ||
	    !get_hex_encoding(req, &hex, err))
		return false;

	if (since < 0)
		since = 0;
	if (max < 1 || max > CONTROL_READ_MAX)
		max = CONTROL_READ_MAX;

	CHECK_ALLOCATION(buf = (unsigned char *) malloc(max));
	n = console_output_read(h, since, buf, max, &start, &total);

	/*  Text: bytes 0x80-0xff as U+0080-U+00FF, in UTF-8, without NULs.
	    Hex: two digits per byte.  */
	CHECK_ALLOCATION(out = (char *) malloc(2 * n + 1));
	p = out;
	for (size_t i = 0; i < n; i++) {
		unsigned char b = buf[i];
		if (hex) {
			snprintf(p, 3, "%02x", b);
			p += 2;
		} else if (b >= 0x80) {
			*p++ = 0xc0 | (b >> 6);
			*p++ = 0x80 | (b & 0x3f);
		} else if (b != 0)
			*p++ = b;
	}
	*p = '\0';

	cJSON_AddStringToObject(result, "data", out);
	cJSON_AddNumberToObject(result, "start", start);
	cJSON_AddNumberToObject(result, "offset", start + n);
	cJSON_AddBoolToObject(result, "dropped", start > (uint64_t) since);

	free(out);
	free(buf);
	return true;
}


static int hexdigit(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}


CTL_HANDLER(cmd_console_write)
{
	cJSON *data = cJSON_GetObjectItemCaseSensitive(req, "data");
	const char *s;
	size_t len, written = 0;
	bool hex;
	int h = 0, room;

	if (!get_console(req, &h, true, err) ||
	    !get_hex_encoding(req, &hex, err))
		return false;
	if (!cJSON_IsString(data))
		return fail(err, "bad_arg", "'data' should be a string");

	s = data->valuestring;
	len = strlen(s);
	if (hex) {
		if (len % 2 != 0)
			return fail(err, "bad_arg", "odd number of hex digits");
		for (size_t i = 0; i < len; i++)
			if (hexdigit(s[i]) < 0)
				return fail(err, "bad_arg", "not hex: '%c'",
				    s[i]);
		len /= 2;
	}

	room = console_input_room(h);
	while (written < len && (int) written < room) {
		int ch = hex? hexdigit(s[written * 2]) * 16 +
		    hexdigit(s[written * 2 + 1]) : (unsigned char) s[written];
		console_makeavail(h, ch);
		written ++;
	}

	cJSON_AddNumberToObject(result, "written", written);
	return true;
}


/*
 *  run_captured():
 *
 *  Runs a debugger command with stdout going to a temporary file, and
 *  returns what it printed (malloced, without color escape sequences or
 *  carriage returns), or NULL if that couldn't be arranged.
 */
static char *run_captured(char *cmd)
{
	FILE *f = tmpfile();
	char *text, *src, *dst;
	int saved;
	long len;

	if (f == NULL)
		return NULL;

	fflush(stdout);
	saved = dup(STDOUT_FILENO);
	if (saved < 0 || dup2(fileno(f), STDOUT_FILENO) < 0) {
		if (saved >= 0)
			close(saved);
		fclose(f);
		return NULL;
	}

	debugger_execute_cmd(cmd, strlen(cmd));

	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	close(saved);

	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);
	if (len < 0)
		len = 0;

	CHECK_ALLOCATION(text = (char *) malloc(len + 1));
	len = fread(text, 1, len, f);
	text[len] = '\0';
	fclose(f);

	/*  Remove ESC [ ... letter, and CRs:  */
	for (src = dst = text; *src; src++) {
		if (src[0] == '\033' && src[1] == '[') {
			src += 2;
			while (*src && !((*src >= 'a' && *src <= 'z') ||
			    (*src >= 'A' && *src <= 'Z')))
				src ++;
			if (*src == '\0')
				break;
			continue;
		}
		if (*src != '\r')
			*dst++ = *src;
	}
	*dst = '\0';

	return text;
}


CTL_HANDLER(cmd_raw)
{
	cJSON *line = cJSON_GetObjectItemCaseSensitive(req, "line");
	char *cmd, *text;
	size_t len;

	if (!cJSON_IsString(line))
		return fail(err, "bad_arg", "'line' should be a string");

	/*  Without leading and trailing spaces, as at the prompt:  */
	const char *s = line->valuestring;
	while (*s == ' ')
		s ++;
	CHECK_ALLOCATION(cmd = strdup(s));
	len = strlen(cmd);
	while (len > 0 && cmd[len-1] == ' ')
		cmd[--len] = '\0';

	if (len == 0) {
		free(cmd);
		return fail(err, "bad_arg", "empty 'line'");
	}

	text = run_captured(cmd);
	free(cmd);
	if (text == NULL)
		return fail(err, "capture_failed", "couldn't capture the "
		    "command's output");

	cJSON_AddStringToObject(result, "text", text);
	free(text);
	return true;
}


static const struct {
	const char	*name;
	bool		(*f)(cJSON *, cJSON *, struct ctl_error *);
} ctl_cmds[] = {
	{ "hello",		cmd_hello },
	{ "status",		cmd_status },
	{ "pause",		cmd_pause },
	{ "continue",		cmd_continue },
	{ "step",		cmd_step },
	{ "quit",		cmd_quit },
	{ "break_add",		cmd_break_add },
	{ "break_del",		cmd_break_del },
	{ "break_list",		cmd_break_list },
	{ "console_read",	cmd_console_read },
	{ "console_write",	cmd_console_write },
	{ "raw",		cmd_raw },
	{ NULL,			NULL }
};


/*
 *  handle_line():
 *
 *  Handles one request, and sends its response.
 */
static void handle_line(const char *line, size_t len)
{
	cJSON *req, *resp, *result = NULL, *cmd, *id;
	struct ctl_error err;
	bool ok = false;

	/*  Ignore empty lines:  */
	size_t i = 0;
	while (i < len && (line[i] == ' ' || line[i] == '\t' ||
	    line[i] == '\r'))
		i ++;
	if (i == len)
		return;

	err.code = NULL;
	err.message[0] = '\0';

	resp = cJSON_CreateObject();
	req = cJSON_ParseWithLength(line, len);

	id = cJSON_GetObjectItemCaseSensitive(req, "id");
	if (id != NULL)
		cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));

	cmd = cJSON_GetObjectItemCaseSensitive(req, "cmd");
	if (!cJSON_IsObject(req))
		fail(&err, "bad_request", "not a JSON object");
	else if (!cJSON_IsString(cmd))
		fail(&err, "bad_request", "no \"cmd\"");
	else {
		int k;
		for (k = 0; ctl_cmds[k].name != NULL; k++)
			if (strcmp(ctl_cmds[k].name, cmd->valuestring) == 0)
				break;

		if (ctl_cmds[k].name == NULL)
			fail(&err, "unknown_cmd", "unknown command '%s'",
			    cmd->valuestring);
		else {
			result = cJSON_CreateObject();
			ok = ctl_cmds[k].f(req, result, &err);
		}
	}

	cJSON_AddBoolToObject(resp, "ok", ok);
	if (ok)
		cJSON_AddItemToObject(resp, "result", result);
	else {
		cJSON *e = cJSON_AddObjectToObject(resp, "error");
		cJSON_AddStringToObject(e, "code", err.code);
		cJSON_AddStringToObject(e, "message", err.message);
		cJSON_Delete(result);
	}
	cJSON_AddNumberToObject(resp, "ninstrs", ninstrs_now());

	cJSON_Delete(req);
	send_msg(resp);
}


/*****************************************************************************/


static void accept_clients(void)
{
	int s, one = 1;

	while ((s = accept(listen_fd, NULL, NULL)) >= 0) {
#ifdef FD_CLOEXEC
		fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
#ifdef SO_NOSIGPIPE
		setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (void *) &one,
		    sizeof(one));
#endif
		(void) one;

		if (client_fd >= 0) {
			const char *busy = "{\"ok\":false,\"error\":{\"code\":"
			    "\"busy\",\"message\":\"another client is "
			    "connected\"}}\n";
			send_all(s, busy, strlen(busy));
			socket_close(s);
			continue;
		}

		socket_set_nonblocking(s);
		client_fd = s;
		inlen = 0;
	}
}


static void read_client(void)
{
	char buf[4096];

	while (client_fd >= 0) {
		ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
		if (n <= 0) {
			int e = socket_error();
			if (n == 0 || (!socket_would_block(e) &&
			    e != SOCKET_EINTR))
				disconnect_client();
			return;
		}

		if (inlen + n > CONTROL_MAX_LINE) {
			cJSON *resp = cJSON_CreateObject();
			cJSON_AddBoolToObject(resp, "ok", false);
			cJSON *e = cJSON_AddObjectToObject(resp, "error");
			cJSON_AddStringToObject(e, "code", "bad_request");
			cJSON_AddStringToObject(e, "message", "line too long");
			send_msg(resp);
			disconnect_client();
			return;
		}

		memcpy(inbuf + inlen, buf, n);
		inlen += n;

		/*  Handle each complete line:  */
		char *nl;
		while (client_fd >= 0 &&
		    (nl = (char *) memchr(inbuf, '\n', inlen)) != NULL) {
			size_t linelen = nl - inbuf;
			handle_line(inbuf, linelen);
			if (client_fd < 0)
				return;
			memmove(inbuf, nl + 1, inlen - linelen - 1);
			inlen -= linelen + 1;
		}
	}
}


/*
 *  control_poll():
 *
 *  Accepts a client, and handles its requests. Unless force is true, this
 *  only looks at the socket every CONTROL_POLL_INTERVAL_US.
 */
void control_poll(bool force)
{
	if (listen_fd < 0 || in_poll)
		return;

	if (!force) {
		struct timeval tv;
		gettimeofday(&tv, NULL);
		int64_t now = (int64_t) tv.tv_sec * 1000000 + tv.tv_usec;
		if (now - last_poll_us < CONTROL_POLL_INTERVAL_US)
			return;
		last_poll_us = now;
	}

	/*  Reading first notices a client which has gone, before a new one
	    is turned away as busy:  */
	in_poll = true;
	read_client();
	accept_clients();
	read_client();
	in_poll = false;
}


static void remove_unix_socket(void)
{
	if (unix_path != NULL)
		unlink(unix_path);
}


/*
 *  control_init():
 *
 *  Starts listening, as given by --control: "unix:path" or "tcp:port".
 *  Exits on errors.
 */
void control_init(struct emul *emul, const char *spec)
{
	int one = 1;

	ctl_emul = emul;

	if (strncmp(spec, "tcp:", 4) == 0) {
		struct sockaddr_in sin;
		socklen_t sinlen = sizeof(sin);
		char *end;
		long port = strtol(spec + 4, &end, 10);

		if (spec[4] == '\0' || *end != '\0' || port < 0 ||
		    port > 65535) {
			fprintf(stderr, "--control: bad port in '%s'\n", spec);
			exit(1);
		}

		listen_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (listen_fd < 0) {
			perror("--control: socket");
			exit(1);
		}
		setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
		    (void *) &one, sizeof(one));

		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sin.sin_port = htons(port);
		if (bind(listen_fd, (struct sockaddr *) &sin,
		    sizeof(sin)) < 0) {
			fprintf(stderr, "--control: can't listen on port %li "
			    "(error %i)\n", port, socket_error());
			exit(1);
		}

		getsockname(listen_fd, (struct sockaddr *) &sin, &sinlen);
		fprintf(stderr, "gxemul: control socket: tcp:%i\n",
		    ntohs(sin.sin_port));
	} else if (strncmp(spec, "unix:", 5) == 0) {
#ifdef _WIN32
		fprintf(stderr, "--control: unix: sockets aren't supported "
		    "on Windows; use tcp:port\n");
		exit(1);
#else
		struct sockaddr_un sun;
		struct stat st;
		const char *path = spec + 5;

		if (path[0] == '\0' || strlen(path) >= sizeof(sun.sun_path)) {
			fprintf(stderr, "--control: bad path in '%s'\n", spec);
			exit(1);
		}

		/*  Replace an old socket, but nothing else:  */
		if (lstat(path, &st) == 0) {
			if (!S_ISSOCK(st.st_mode)) {
				fprintf(stderr, "--control: %s exists, and "
				    "isn't a socket\n", path);
				exit(1);
			}
			unlink(path);
		}

		listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (listen_fd < 0) {
			perror("--control: socket");
			exit(1);
		}

		memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_UNIX;
		strlcpy(sun.sun_path, path, sizeof(sun.sun_path));
		if (bind(listen_fd, (struct sockaddr *) &sun,
		    sizeof(sun)) < 0) {
			fprintf(stderr, "--control: can't listen on %s: %s\n",
			    path, strerror(errno));
			exit(1);
		}

		CHECK_ALLOCATION(unix_path = strdup(path));
		atexit(remove_unix_socket);
#endif
	} else {
		fprintf(stderr, "--control: expected unix:path or tcp:port, "
		    "not '%s'\n", spec);
		exit(1);
	}

	if (listen(listen_fd, 1) < 0) {
		perror("--control: listen");
		exit(1);
	}

#ifdef FD_CLOEXEC
	/*  (A reset execs gxemul again, which listens anew.)  */
	fcntl(listen_fd, F_SETFD, FD_CLOEXEC);
#endif
	socket_set_nonblocking(listen_fd);

	CHECK_ALLOCATION(inbuf = (char *) malloc(CONTROL_MAX_LINE));

	console_enable_output_log();
}


/*
 *  control_shutdown():
 *
 *  Tells the client that the emulator is exiting, and stops listening.
 */
void control_shutdown(void)
{
	if (listen_fd < 0)
		return;

	cJSON *ev = cJSON_CreateObject();
	cJSON_AddStringToObject(ev, "event", "exited");
	cJSON_AddNumberToObject(ev, "ninstrs", ninstrs_now());
	send_msg(ev);

	disconnect_client();
	socket_close(listen_fd);
	listen_fd = -1;

	remove_unix_socket();
	free(unix_path);
	unix_path = NULL;
}


/*****************************************************************************/


/*
 *  The client: gxemul --ctl socket [--wait] [--timeout seconds] cmd [arg]
 */

static int client_usage(void)
{
	fprintf(stderr, "usage: gxemul --ctl unix:path|tcp:port [--wait] "
	    "[--timeout seconds] command [argument]\n"
	    "The argument is a JSON object with the request's parameters,"
	    " or a plain value\nfor raw (line), step (n), break_add (pc), "
	    "break_del (id), console_read\n(since), or console_write (data)."
	    " See doc/control.html.\n");
	return 2;
}


static int client_connect(const char *spec)
{
	int s;

	if (strncmp(spec, "tcp:", 4) == 0) {
		struct sockaddr_in sin;
		long port = atol(spec + 4);

		s = socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0)
			return -1;
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sin.sin_port = htons(port);
		if (connect(s, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
			socket_close(s);
			return -1;
		}
		return s;
	}

#ifndef _WIN32
	if (strncmp(spec, "unix:", 5) == 0) {
		struct sockaddr_un sun;

		s = socket(AF_UNIX, SOCK_STREAM, 0);
		if (s < 0)
			return -1;
		memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_UNIX;
		strlcpy(sun.sun_path, spec + 5, sizeof(sun.sun_path));
		if (connect(s, (struct sockaddr *) &sun, sizeof(sun)) < 0) {
			socket_close(s);
			return -1;
		}
		return s;
	}
#endif

	return -1;
}


/*
 *  client_readline():
 *
 *  Reads one line from s into a malloced string. Returns NULL on EOF or
 *  error, and sets *timed_out if the deadline (if any) passed first.
 */
static char *client_readline(int s, char **bufp, size_t *lenp,
	struct timeval *deadline, bool *timed_out)
{
	char tmp[4096];

	*timed_out = false;
	for (;;) {
		char *nl = (char *) memchr(*bufp, '\n', *lenp);
		if (nl != NULL) {
			size_t linelen = nl - *bufp;
			char *line;
			CHECK_ALLOCATION(line = (char *) malloc(linelen + 1));
			memcpy(line, *bufp, linelen);
			line[linelen] = '\0';
			memmove(*bufp, nl + 1, *lenp - linelen - 1);
			*lenp -= linelen + 1;
			return line;
		}

		if (deadline != NULL) {
			struct timeval now, tv;
			fd_set rfds;

			gettimeofday(&now, NULL);
			int64_t left = (int64_t) (deadline->tv_sec -
			    now.tv_sec) * 1000000 + (deadline->tv_usec -
			    now.tv_usec);
			if (left <= 0) {
				*timed_out = true;
				return NULL;
			}
			tv.tv_sec = left / 1000000;
			tv.tv_usec = left % 1000000;
			FD_ZERO(&rfds);
			FD_SET(s, &rfds);
			if (select(s + 1, &rfds, NULL, NULL, &tv) == 0) {
				*timed_out = true;
				return NULL;
			}
		}

		ssize_t n = recv(s, tmp, sizeof(tmp), 0);
		if (n < 0 && socket_error() == SOCKET_EINTR)
			continue;
		if (n <= 0)
			return NULL;

		CHECK_ALLOCATION(*bufp = (char *) realloc(*bufp, *lenp + n + 1));
		memcpy(*bufp + *lenp, tmp, n);
		*lenp += n;
	}
}


int control_client_main(int argc, char *argv[])
{
	/*  Commands which take a plain value, and its parameter:  */
	static const struct {
		const char *cmd, *key;
		bool number;
	} plain[] = {
		{ "raw", "line", false },
		{ "step", "n", true },
		{ "break_add", "pc", false },
		{ "break_del", "id", true },
		{ "console_read", "since", true },
		{ "console_write", "data", false },
		{ NULL, NULL, false }
	};

	const char *spec, *cmd, *arg = NULL;
	struct timeval deadline;
	bool wait = false, have_deadline = false, timed_out;
	char *buf = NULL, *line, *reqstr;
	size_t buflen = 0;
	cJSON *req;
	int i = 2, s, ret;

	if (argc < 4)
		return client_usage();
	spec = argv[i++];

	while (i < argc && strncmp(argv[i], "--", 2) == 0) {
		if (strcmp(argv[i], "--wait") == 0) {
			wait = true;
			i ++;
		} else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
			double t = atof(argv[i + 1]);
			gettimeofday(&deadline, NULL);
			deadline.tv_sec += (time_t) t;
			deadline.tv_usec += (long) ((t - (time_t) t) * 1e6);
			if (deadline.tv_usec >= 1000000) {
				deadline.tv_sec ++;
				deadline.tv_usec -= 1000000;
			}
			have_deadline = true;
			i += 2;
		} else
			return client_usage();
	}

	if (i >= argc)
		return client_usage();
	cmd = argv[i++];
	if (i < argc)
		arg = argv[i++];
	if (i < argc)
		return client_usage();

	/*  The request:  */
	if (arg != NULL && arg[0] == '{') {
		req = cJSON_Parse(arg);
		if (!cJSON_IsObject(req)) {
			fprintf(stderr, "gxemul --ctl: not a JSON object: %s\n",
			    arg);
			return 2;
		}
	} else {
		req = cJSON_CreateObject();
		if (arg != NULL) {
			int k;
			for (k = 0; plain[k].cmd != NULL; k++)
				if (strcmp(plain[k].cmd, cmd) == 0)
					break;
			if (plain[k].cmd == NULL) {
				fprintf(stderr, "gxemul --ctl: %s takes its "
				    "parameters as a JSON object\n", cmd);
				return 2;
			}
			if (plain[k].number) {
				char *end;
				long long v = strtoll(arg, &end, 0);
				if (arg[0] == '\0' || *end != '\0') {
					fprintf(stderr, "gxemul --ctl: not a "
					    "number: %s\n", arg);
					return 2;
				}
				cJSON_AddNumberToObject(req, plain[k].key, v);
			} else
				cJSON_AddStringToObject(req, plain[k].key, arg);
		}
	}
	cJSON_DeleteItemFromObjectCaseSensitive(req, "cmd");
	cJSON_AddStringToObject(req, "cmd", cmd);
	if (cJSON_GetObjectItemCaseSensitive(req, "id") == NULL)
		cJSON_AddNumberToObject(req, "id", 1);

	s = client_connect(spec);
	if (s < 0) {
		fprintf(stderr, "gxemul --ctl: can't connect to %s\n", spec);
		cJSON_Delete(req);
		return 2;
	}

	CHECK_ALLOCATION(reqstr = cJSON_PrintUnformatted(req));
	cJSON_Delete(req);
	size_t reqlen = strlen(reqstr);
	CHECK_ALLOCATION(reqstr = (char *) realloc(reqstr, reqlen + 2));
	strlcpy(reqstr + reqlen, "\n", 2);
	if (!send_all(s, reqstr, reqlen + 1)) {
		fprintf(stderr, "gxemul --ctl: can't send to %s\n", spec);
		socket_close(s);
		return 2;
	}
	free(reqstr);

	/*  The response (events before it are skipped):  */
	ret = 2;
	bool got_response = false, wait_for_stop = false;
	while ((line = client_readline(s, &buf, &buflen,
	    have_deadline? &deadline : NULL, &timed_out)) != NULL) {
		cJSON *msg = cJSON_Parse(line);
		cJSON *ok = cJSON_GetObjectItemCaseSensitive(msg, "ok");

		if (ok == NULL) {
			cJSON_Delete(msg);
			free(line);
			continue;
		}

		printf("%s\n", line);
		ret = cJSON_IsTrue(ok)? 0 : 1;

		cJSON *result = cJSON_GetObjectItemCaseSensitive(msg, "result");
		wait_for_stop = wait && ret == 0 &&
		    (strcmp(cmd, "continue") == 0 || strcmp(cmd, "step") == 0 ||
		    (strcmp(cmd, "pause") == 0 && !cJSON_IsTrue(
		    cJSON_GetObjectItemCaseSensitive(result,
		    "already_stopped"))));

		cJSON_Delete(msg);
		free(line);
		got_response = true;
		break;
	}

	/*  ... and with --wait, the stop:  */
	if (got_response && wait_for_stop) {
		ret = 2;
		while ((line = client_readline(s, &buf, &buflen,
		    have_deadline? &deadline : NULL, &timed_out)) != NULL) {
			cJSON *msg = cJSON_Parse(line);
			cJSON *ev = cJSON_GetObjectItemCaseSensitive(msg,
			    "event");
			bool done = cJSON_IsString(ev) &&
			    (strcmp(ev->valuestring, "stopped") == 0 ||
			    strcmp(ev->valuestring, "exited") == 0);

			if (done)
				printf("%s\n", line);
			cJSON_Delete(msg);
			free(line);
			if (done) {
				ret = 0;
				break;
			}
		}
	}

	if (ret == 2 && timed_out) {
		fprintf(stderr, "gxemul --ctl: timed out\n");
		ret = 3;
	} else if (ret == 2)
		fprintf(stderr, "gxemul --ctl: connection closed\n");

	fflush(stdout);
	free(buf);
	socket_close(s);
	return ret;
}
