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
#include "memory.h"
#include "misc.h"
#include "settings.h"
#include "symbol.h"

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
extern volatile int single_step_breakpoint;

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

	/*  For run_until:  */
	const char	*condition;
	char		match[200];
	uint64_t	console_offset;
} last_stop;

/*
 *  run_until: runs until one of its conditions happens. Console patterns
 *  and instruction counts and timeouts are checked by control_poll(), so
 *  the machine stops within a few milliseconds of them; a pc is a
 *  temporary breakpoint, and exact.
 */
static struct {
	bool		active;

	int		console;
	char		**patterns;
	int		n_patterns;
	size_t		maxlen;		/*  longest pattern  */
	uint64_t	checked;	/*  console offset searched up to  */

	int		bp_id;		/*  0 if no pc condition  */

	bool		has_insns;
	int64_t		insns_end;

	bool		has_timeout;
	int64_t		deadline_us;

	/*  When a condition has happened:  */
	const char	*fired;
	char		match[200];
	uint64_t	match_end;
} ru;

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
	if (last_stop.condition != NULL) {
		cJSON_AddStringToObject(o, "condition", last_stop.condition);
		if (strcmp(last_stop.condition, "console") == 0) {
			cJSON_AddStringToObject(o, "match", last_stop.match);
			cJSON_AddNumberToObject(o, "console_offset",
			    last_stop.console_offset);
		}
	}
}


static void run_until_clear(void);


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
	last_stop.condition = NULL;

	/*  The end of a run_until, because of one of its conditions or not:  */
	if (ru.active) {
		if (strcmp(reason, "run_until") == 0 && ru.fired != NULL) {
			last_stop.condition = ru.fired;
			strlcpy(last_stop.match, ru.match,
			    sizeof(last_stop.match));
			last_stop.console_offset = ru.match_end;
		} else if (bp != 0 && bp == ru.bp_id) {
			strlcpy(last_stop.reason, "run_until",
			    sizeof(last_stop.reason));
			last_stop.condition = "pc";
			last_stop.bp = 0;
		}
		run_until_clear();
	}

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
		debugger_ctl_stop("pause");
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
 *  Capturing what debugger commands and disassemblers print: between
 *  capture_begin() and capture_end(), stdout goes to a temporary file.
 *  capture_pos() is how much has been printed so far, and capture_end()
 *  returns all of it (malloced). Both fail (false, NULL) if stdout can't
 *  be redirected.
 */
struct capture {
	FILE	*f;
	int	saved_fd;
};

static bool capture_begin(struct capture *cap)
{
	cap->f = tmpfile();
	if (cap->f == NULL)
		return false;

	fflush(stdout);
	cap->saved_fd = dup(STDOUT_FILENO);
	if (cap->saved_fd < 0 || dup2(fileno(cap->f), STDOUT_FILENO) < 0) {
		if (cap->saved_fd >= 0)
			close(cap->saved_fd);
		fclose(cap->f);
		return false;
	}

	return true;
}

static long capture_pos(struct capture *cap)
{
	fflush(stdout);
	return lseek(fileno(cap->f), 0, SEEK_CUR);
}

static char *capture_end(struct capture *cap, long *lenp)
{
	char *text;
	long len;

	fflush(stdout);
	dup2(cap->saved_fd, STDOUT_FILENO);
	close(cap->saved_fd);

	fseek(cap->f, 0, SEEK_END);
	len = ftell(cap->f);
	rewind(cap->f);
	if (len < 0)
		len = 0;

	CHECK_ALLOCATION(text = (char *) malloc(len + 1));
	len = fread(text, 1, len, cap->f);
	text[len] = '\0';
	fclose(cap->f);

	*lenp = len;
	return text;
}


/*
 *  strip_escapes():
 *
 *  Removes color escape sequences (ESC [ ... letter) and CRs from text,
 *  in place.
 */
static void strip_escapes(char *text)
{
	char *src, *dst;

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

	struct capture cap;
	long textlen;

	if (!capture_begin(&cap)) {
		free(cmd);
		return fail(err, "capture_failed", "couldn't capture the "
		    "command's output");
	}
	debugger_execute_cmd(cmd, len);
	text = capture_end(&cap, &textlen);
	free(cmd);

	strip_escapes(text);
	cJSON_AddStringToObject(result, "text", text);
	free(text);
	return true;
}


/*
 *  get_cpu():
 *
 *  The "cpu" parameter: a CPU of the debugger's current machine, by
 *  default the debugger's current CPU.
 */
static bool get_cpu(cJSON *req, struct cpu **cpup, struct ctl_error *err)
{
	struct machine *m = cur_machine();
	int64_t id;

	if (!get_int(req, "cpu", debugger_cur_cpu, &id, err))
		return false;
	if (id < 0 || id >= m->ncpus)
		return fail(err, "no_such_cpu", "no cpu %" PRIi64, id);

	*cpup = m->cpus[id];
	return true;
}


/*
 *  get_addr():
 *
 *  An address parameter (see get_expr()), evaluated. If it is absent and
 *  defp isn't NULL, *defp is used.
 */
static bool get_addr(cJSON *req, const char *key, const uint64_t *defp,
	uint64_t *addrp, struct ctl_error *err)
{
	char *expr;
	int res;

	if (defp != NULL && cJSON_GetObjectItemCaseSensitive(req, key) == NULL) {
		*addrp = *defp;
		return true;
	}

	expr = get_expr(req, key, err);
	if (expr == NULL)
		return false;

	res = debugger_parse_expression(cur_machine(), expr, 0, addrp);
	if (res == PARSE_NOMATCH || res == PARSE_MULTIPLE) {
		fail(err, "bad_arg", res == PARSE_MULTIPLE? "'%s' is "
		    "ambiguous" : "couldn't parse '%s'", expr);
		free(expr);
		return false;
	}

	free(expr);
	return true;
}


/*
 *  get_space():
 *
 *  The "space" parameter: "virtual" (the default) or "physical", as flags
 *  for memory_rw().
 */
static bool get_space(cJSON *req, int *flagsp, struct ctl_error *err)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(req, "space");

	*flagsp = CACHE_NONE | NO_EXCEPTIONS;
	if (item == NULL || cJSON_IsNull(item) || (cJSON_IsString(item) &&
	    strcmp(item->valuestring, "virtual") == 0))
		return true;
	if (cJSON_IsString(item) && strcmp(item->valuestring, "physical") == 0) {
		*flagsp |= PHYSICAL;
		return true;
	}

	return fail(err, "bad_arg", "'space' should be \"virtual\" or "
	    "\"physical\"");
}


/*
 *  access_mem():
 *
 *  Reads or writes len bytes at addr, a page (or byte) at a time, up to
 *  the first byte that can't be accessed. Returns the number of bytes
 *  accessed.
 */
static size_t access_mem(struct cpu *cpu, uint64_t addr, unsigned char *buf,
	size_t len, int writeflag, int flags)
{
	size_t done = 0;

	while (done < len) {
		size_t chunk = 0x1000 - ((addr + done) & 0xfff);
		if (chunk > len - done)
			chunk = len - done;

		if (cpu->memory_rw(cpu, cpu->mem, addr + done, buf + done,
		    chunk, writeflag, flags) != MEMORY_ACCESS_FAILED) {
			done += chunk;
			continue;
		}

		/*  Find the first byte which fails:  */
		while (chunk-- > 0 && cpu->memory_rw(cpu, cpu->mem, addr +
		    done, buf + done, 1, writeflag, flags) !=
		    MEMORY_ACCESS_FAILED)
			done ++;
		break;
	}

	return done;
}


static void add_hex(cJSON *o, const char *key, const unsigned char *buf,
	size_t len)
{
	char *hex;

	CHECK_ALLOCATION(hex = (char *) malloc(2 * len + 1));
	for (size_t i = 0; i < len; i++)
		snprintf(hex + 2 * i, 3, "%02x", buf[i]);
	hex[2 * len] = '\0';
	cJSON_AddStringToObject(o, key, hex);
	free(hex);
}


static bool is_reg_format(int format)
{
	return format == SETTINGS_FORMAT_HEX8 ||
	    format == SETTINGS_FORMAT_HEX16 ||
	    format == SETTINGS_FORMAT_HEX32 || format == SETTINGS_FORMAT_HEX64;
}


/*
 *  A register's value, formatted for its width:
 */
static bool format_reg(char *buf, size_t len, int format, uint64_t v)
{
	switch (format) {
	case SETTINGS_FORMAT_HEX8:
		snprintf(buf, len, "0x%02" PRIx8, (uint8_t) v);
		return true;
	case SETTINGS_FORMAT_HEX16:
		snprintf(buf, len, "0x%04" PRIx16, (uint16_t) v);
		return true;
	case SETTINGS_FORMAT_HEX32:
		snprintf(buf, len, "0x%08" PRIx32, (uint32_t) v);
		return true;
	case SETTINGS_FORMAT_HEX64:
		snprintf(buf, len, "0x%016" PRIx64, (uint64_t) v);
		return true;
	}

	return false;
}


CTL_HANDLER(cmd_regs)
{
	cJSON *names = cJSON_GetObjectItemCaseSensitive(req, "names");
	cJSON *regs;
	struct cpu *cpu = NULL;
	const char *name;
	int type, format;
	uint64_t v;
	char buf[24];

	if (!get_cpu(req, &cpu, err))
		return false;
	if (names != NULL && !cJSON_IsArray(names))
		return fail(err, "bad_arg", "'names' should be a list of "
		    "register names");

	cJSON_AddNumberToObject(result, "cpu", cpu->cpu_id);
	regs = cJSON_AddObjectToObject(result, "regs");

	/*  All registers (the hex settings of the CPU), in their order:  */
	if (names == NULL) {
		for (int i = 0; settings_entry(cpu->settings, i, &name, &type,
		    &format, &v); i++)
			if (format_reg(buf, sizeof(buf), format, v))
				cJSON_AddStringToObject(regs, name, buf);
		return true;
	}

	/*  ... or those asked for:  */
	cJSON *n;
	cJSON_ArrayForEach(n, names) {
		bool found = false;
		if (!cJSON_IsString(n))
			return fail(err, "bad_arg", "'names' should be a list "
			    "of register names");
		for (int i = 0; settings_entry(cpu->settings, i, &name, &type,
		    &format, &v); i++)
			if (strcmp(name, n->valuestring) == 0 &&
			    format_reg(buf, sizeof(buf), format, v)) {
				cJSON_AddStringToObject(regs, name, buf);
				found = true;
				break;
			}
		if (!found)
			return fail(err, "bad_arg", "no register '%s'",
			    n->valuestring);
	}

	return true;
}


CTL_HANDLER(cmd_reg_set)
{
	cJSON *name = cJSON_GetObjectItemCaseSensitive(req, "name");
	const char *ename;
	struct cpu *cpu = NULL;
	uint64_t value, old_pc, v;
	int type, format, i;

	if (!get_cpu(req, &cpu, err) ||
	    !get_addr(req, "value", NULL, &value, err))
		return false;
	if (!cJSON_IsString(name))
		return fail(err, "bad_arg", "'name' should be a register name");
	if (!debugger_is_stopped())
		return fail(err, "not_stopped", "the machine is running; "
		    "pause it first");

	for (i = 0; settings_entry(cpu->settings, i, &ename, &type, &format,
	    &v); i++)
		if (strcmp(ename, name->valuestring) == 0 &&
		    is_reg_format(format))
			break;
	if (!settings_entry(cpu->settings, i, &ename, &type, &format, &v))
		return fail(err, "bad_arg", "no register '%s'",
		    name->valuestring);

	/*  As the debugger does: sign-extend 32-bit values for MIPS:  */
	if (cpu->is_32bit && cpu->cpu_family->arch == ARCH_MIPS &&
	    (value >> 32) == 0 && (value & 0x80000000ULL))
		value |= 0xffffffff00000000ULL;

	old_pc = cpu->pc;
	if (settings_access(cpu->settings, name->valuestring, 1, &value) !=
	    SETTINGS_OK)
		return fail(err, "bad_arg", "register '%s' can't be written",
		    name->valuestring);

	/*  A new pc leaves the breakpoint we were stopped at:  */
	if (cpu->pc != old_pc)
		single_step_breakpoint = 0;

	settings_entry(cpu->settings, i, &ename, &type, &format, &v);
	char buf[24];
	format_reg(buf, sizeof(buf), format, v);
	cJSON_AddStringToObject(result, "value", buf);
	return true;
}


CTL_HANDLER(cmd_mem_read)
{
	struct cpu *cpu = NULL;
	unsigned char *buf;
	uint64_t addr;
	int64_t len;
	size_t n;
	int flags;

	if (!get_cpu(req, &cpu, err) ||
	    !get_addr(req, "addr", NULL, &addr, err) ||
	    !get_int(req, "len", 64, &len, err) ||
	    !get_space(req, &flags, err))
		return false;
	if (len < 1 || len > CONTROL_READ_MAX)
		return fail(err, "bad_arg", "'len' should be 1 to %i",
		    CONTROL_READ_MAX);

	CHECK_ALLOCATION(buf = (unsigned char *) malloc(len));
	n = access_mem(cpu, addr, buf, len, MEM_READ, flags);
	if (n == 0) {
		free(buf);
		return fail(err, "mem_fault", "can't read at 0x%" PRIx64,
		    addr);
	}

	add_addr(result, "addr", cpu, addr);
	add_hex(result, "data", buf, n);
	if (n < (size_t) len)
		add_addr(result, "fault_at", cpu, addr + n);

	free(buf);
	return true;
}


CTL_HANDLER(cmd_mem_write)
{
	cJSON *data = cJSON_GetObjectItemCaseSensitive(req, "data");
	struct cpu *cpu = NULL;
	unsigned char *buf;
	uint64_t addr;
	size_t len, n;
	int flags;

	if (!get_cpu(req, &cpu, err) ||
	    !get_addr(req, "addr", NULL, &addr, err) ||
	    !get_space(req, &flags, err))
		return false;
	if (!cJSON_IsString(data) || strlen(data->valuestring) % 2 != 0 ||
	    data->valuestring[0] == '\0')
		return fail(err, "bad_arg", "'data' should be hex digits, "
		    "two per byte");
	if (!debugger_is_stopped())
		return fail(err, "not_stopped", "the machine is running; "
		    "pause it first");

	len = strlen(data->valuestring) / 2;
	CHECK_ALLOCATION(buf = (unsigned char *) malloc(len));
	for (size_t i = 0; i < len; i++) {
		int hi = hexdigit(data->valuestring[2*i]);
		int lo = hexdigit(data->valuestring[2*i + 1]);
		if (hi < 0 || lo < 0) {
			free(buf);
			return fail(err, "bad_arg", "'data' should be hex "
			    "digits, two per byte");
		}
		buf[i] = hi * 16 + lo;
	}

	n = access_mem(cpu, addr, buf, len, MEM_WRITE, flags);
	free(buf);

	/*  The written bytes may be code which has been translated:  */
	struct machine *m = cur_machine();
	for (int j = 0; j < m->ncpus; j++)
		if (m->cpus[j]->translation_cache != NULL) {
			cpu_create_or_reset_tc(m->cpus[j]);
			m->cpus[j]->invalidate_translation_caches(m->cpus[j],
			    0, INVALIDATE_ALL);
		}

	if (n == 0)
		return fail(err, "mem_fault", "can't write at 0x%" PRIx64,
		    addr);

	cJSON_AddNumberToObject(result, "written", n);
	if (n < len)
		add_addr(result, "fault_at", cpu, addr + n);
	return true;
}


static void add_symbol(cJSON *o, const char *key, struct machine *m,
	uint64_t addr)
{
	uint64_t offset;
	char *name = get_symbol_name(&m->symbol_context, addr, &offset);
	char buf[300];

	if (name == NULL)
		return;
	if (offset == 0)
		cJSON_AddStringToObject(o, key, name);
	else {
		snprintf(buf, sizeof(buf), "%s+0x%" PRIx64, name, offset);
		cJSON_AddStringToObject(o, key, buf);
	}
}


CTL_HANDLER(cmd_disasm)
{
	struct machine *m = cur_machine();
	struct capture cap;
	struct cpu *cpu = NULL;
	uint64_t addr;
	int64_t count;
	long *pos;
	char *text;
	long textlen;
	int n = 0;

	if (!get_cpu(req, &cpu, err) ||
	    !get_addr(req, "addr", &cpu->pc, &addr, err) ||
	    !get_int(req, "count", 10, &count, err))
		return false;
	if (count < 1 || count > 1000)
		return fail(err, "bad_arg", "'count' should be 1 to 1000");

	struct {
		uint64_t	addr;
		int		len;
		unsigned char	bytes[17];
	} *instrs;
	CHECK_ALLOCATION(instrs = malloc(sizeof(*instrs) * count));
	CHECK_ALLOCATION(pos = (long *) malloc(sizeof(long) * (count + 1)));

	if (!capture_begin(&cap)) {
		free(instrs);
		free(pos);
		return fail(err, "capture_failed", "couldn't capture the "
		    "disassembler's output");
	}

	/*  As the debugger's unassemble command does:  */
	pos[0] = capture_pos(&cap);
	while (n < count) {
		unsigned char *buf = instrs[n].bytes;
		unsigned int i, failed = 0;
		int len;

		for (i = 0; i < sizeof(instrs[n].bytes); i++) {
			uint64_t a = addr + i;
			/*  (THUMB: the lowest bit means 16-bit encoding.)  */
			if (cpu->cpu_family->arch == ARCH_ARM)
				a = (addr & ~1) + i;
			buf[i] = 0;
			if (cpu->memory_rw(cpu, cpu->mem, a, buf + i, 1,
			    MEM_READ, CACHE_NONE | NO_EXCEPTIONS) ==
			    MEMORY_ACCESS_FAILED)
				failed ++;
		}
		if (failed == sizeof(instrs[n].bytes))
			break;

		len = cpu_disassemble_instr(m, cpu, buf, 0, addr);
		if (len <= 0)
			break;

		instrs[n].addr = addr;
		instrs[n].len = len;
		pos[++n] = capture_pos(&cap);
		addr += len;
	}

	text = capture_end(&cap, &textlen);

	if (n == 0) {
		free(text);
		free(instrs);
		free(pos);
		return fail(err, "mem_fault", "can't read at 0x%" PRIx64,
		    addr);
	}

	/*  Each instruction's output: perhaps a <symbol> line, then the
	    address, a colon, and the instruction; keep its last line, from
	    after the colon.  */
	cJSON *list = cJSON_AddArrayToObject(result, "instructions");
	for (int k = 0; k < n; k++) {
		cJSON *o = cJSON_CreateObject();
		long a = pos[k], b = pos[k + 1];
		char *seg, *line, *p;

		if (a < 0 || b > textlen || b < a)
			a = b = 0;
		CHECK_ALLOCATION(seg = (char *) malloc(b - a + 1));
		memcpy(seg, text + a, b - a);
		seg[b - a] = '\0';
		strip_escapes(seg);

		while ((p = strrchr(seg, '\n')) != NULL && p[1] == '\0')
			*p = '\0';
		line = strrchr(seg, '\n');
		line = line != NULL? line + 1 : seg;
		p = strchr(line, ':');
		if (p != NULL) {
			line = p + 1;
			while (*line == ' ')
				line ++;
		}

		add_addr(o, "addr", cpu, instrs[k].addr);
		add_hex(o, "bytes", instrs[k].bytes, instrs[k].len <=
		    (int) sizeof(instrs[k].bytes)? instrs[k].len :
		    (int) sizeof(instrs[k].bytes));
		cJSON_AddStringToObject(o, "text", line);
		add_symbol(o, "symbol", m, instrs[k].addr);
		cJSON_AddItemToArray(list, o);
		free(seg);
	}

	free(text);
	free(instrs);
	free(pos);
	return true;
}


CTL_HANDLER(cmd_symbol)
{
	cJSON *name = cJSON_GetObjectItemCaseSensitive(req, "name");
	struct machine *m = cur_machine();
	uint64_t addr, offset;
	char *sname;

	if (cJSON_IsString(name)) {
		if (!get_symbol_addr(&m->symbol_context, name->valuestring,
		    &addr))
			return fail(err, "no_such_symbol", "no symbol '%s'",
			    name->valuestring);
		cJSON_AddStringToObject(result, "name", name->valuestring);
		add_addr(result, "addr", m->cpus[0], addr);
		return true;
	}

	if (!get_addr(req, "addr", NULL, &addr, err))
		return fail(err, "bad_arg", "give 'name' or 'addr'");

	sname = get_symbol_name(&m->symbol_context, addr, &offset);
	if (sname == NULL)
		return fail(err, "no_such_symbol", "no symbol at 0x%" PRIx64,
		    addr);

	cJSON_AddStringToObject(result, "name", sname);
	add_addr(result, "addr", m->cpus[0], addr - offset);
	cJSON_AddNumberToObject(result, "offset", offset);
	return true;
}




static int64_t now_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t) tv.tv_sec * 1000000 + tv.tv_usec;
}


static void run_until_clear(void)
{
	struct machine *m = cur_machine();

	if (ru.bp_id != 0)
		for (int i = 0; i < m->breakpoints.n; i++)
			if (m->breakpoints.id[i] == ru.bp_id) {
				debugger_breakpoint_delete(m, i);
				break;
			}

	for (int i = 0; i < ru.n_patterns; i++)
		free(ru.patterns[i]);
	free(ru.patterns);
	memset(&ru, 0, sizeof(ru));
}


/*
 *  run_until_check():
 *
 *  Called while running: stops the machine if a condition has happened.
 */
static void run_until_check(void)
{
	if (!ru.active || ru.fired != NULL || debugger_is_stopped())
		return;

	if (ru.n_patterns > 0) {
		uint64_t from = ru.checked, start, total;
		unsigned char *buf;
		size_t n;

		/*  Search from a little before, for patterns which began in
		    the output seen last time:  */
		from = from > ru.maxlen - 1? from - (ru.maxlen - 1) : 0;
		console_output_read(ru.console, ~0ULL, NULL, 0, &start, &total);
		if (total > from) {
			CHECK_ALLOCATION(buf = (unsigned char *)
			    malloc(total - from));
			n = console_output_read(ru.console, from, buf,
			    total - from, &start, &total);

			/*  The match which ends first:  */
			size_t best_end = 0;
			int best = -1;
			for (int p = 0; p < ru.n_patterns; p++) {
				size_t plen = strlen(ru.patterns[p]);
				for (size_t i = 0; i + plen <= n; i++)
					if (memcmp(buf + i, ru.patterns[p],
					    plen) == 0) {
						if (best < 0 ||
						    i + plen < best_end) {
							best = p;
							best_end = i + plen;
						}
						break;
					}
			}

			free(buf);
			ru.checked = total;

			if (best >= 0) {
				ru.fired = "console";
				strlcpy(ru.match, ru.patterns[best],
				    sizeof(ru.match));
				ru.match_end = start + best_end;
			}
		}
	}

	if (ru.fired == NULL && ru.has_insns && ninstrs_now() >= ru.insns_end)
		ru.fired = "insns";

	if (ru.fired == NULL && ru.has_timeout && now_us() >= ru.deadline_us)
		ru.fired = "timeout";

	if (ru.fired != NULL)
		debugger_ctl_stop("run_until");
}


CTL_HANDLER(cmd_run_until)
{
	cJSON *console = cJSON_GetObjectItemCaseSensitive(req, "console");
	cJSON *pc = cJSON_GetObjectItemCaseSensitive(req, "pc");
	cJSON *pattern;
	int64_t insns, timeout_ms;
	struct machine *m = cur_machine();
	int h = 0;

	if (!get_int(req, "insns", -1, &insns, err) ||
	    !get_int(req, "timeout_ms", -1, &timeout_ms, err))
		return false;
	if (console == NULL && pc == NULL && insns < 0 && timeout_ms < 0)
		return fail(err, "bad_arg", "give at least one of 'console', "
		    "'pc', 'insns', 'timeout_ms'");

	if (console != NULL) {
		pattern = cJSON_GetObjectItemCaseSensitive(console, "pattern");
		if (!cJSON_IsObject(console) || !(cJSON_IsString(pattern) ||
		    cJSON_IsArray(pattern)))
			return fail(err, "bad_arg", "'console' should be "
			    "{\"pattern\": string or list of strings}");
		if (!get_console(console, &h, false, err))
			return false;
		cJSON *input = cJSON_GetObjectItemCaseSensitive(console,
		    "input");
		if (input != NULL && !cJSON_IsString(input))
			return fail(err, "bad_arg", "'input' should be a "
			    "string");
		if (input != NULL && !get_console(console, &h, true, err))
			return false;
		int n = cJSON_IsArray(pattern)? cJSON_GetArraySize(pattern) : 1;
		for (int i = 0; i < n; i++) {
			cJSON *p = cJSON_IsArray(pattern)?
			    cJSON_GetArrayItem(pattern, i) : pattern;
			if (!cJSON_IsString(p) || p->valuestring[0] == '\0')
				return fail(err, "bad_arg", "patterns should "
				    "be non-empty strings");
		}
		if (n == 0)
			return fail(err, "bad_arg", "no patterns");
	}

	uint64_t pcaddr = 0;
	if (pc != NULL && !get_addr(req, "pc", NULL, &pcaddr, err))
		return false;

	/*  A new run_until replaces any earlier one:  */
	run_until_clear();
	ru.active = true;

	if (console != NULL) {
		uint64_t start;
		pattern = cJSON_GetObjectItemCaseSensitive(console, "pattern");
		int n = cJSON_IsArray(pattern)? cJSON_GetArraySize(pattern) : 1;

		ru.console = h;
		CHECK_ALLOCATION(ru.patterns = (char **)
		    malloc(sizeof(char *) * n));
		for (int i = 0; i < n; i++) {
			cJSON *p = cJSON_IsArray(pattern)?
			    cJSON_GetArrayItem(pattern, i) : pattern;
			CHECK_ALLOCATION(ru.patterns[i] =
			    strdup(p->valuestring));
			if (strlen(p->valuestring) > ru.maxlen)
				ru.maxlen = strlen(p->valuestring);
		}
		ru.n_patterns = n;

		/*  From now on, or from "since". (The first search starts
		    maxlen - 1 before checked.)  */
		uint64_t total;
		int64_t since;
		console_output_read(h, ~0ULL, NULL, 0, &start, &total);
		if (!get_int(console, "since", total, &since, err)) {
			run_until_clear();
			return false;
		}
		if (since < 0 || (uint64_t) since > total)
			since = total;
		ru.checked = since + ru.maxlen - 1;
		cJSON_AddNumberToObject(result, "console_offset", since);

		/*  Type the input, now that its reply will be seen:  */
		cJSON *input = cJSON_GetObjectItemCaseSensitive(console,
		    "input");
		if (input != NULL) {
			size_t len = strlen(input->valuestring), written = 0;
			int room = console_input_room(h);
			while (written < len && (int) written < room)
				console_makeavail(h, input->valuestring[written++]);
			cJSON_AddNumberToObject(result, "written", written);
		}
	}

	if (pc != NULL) {
		char buf[24];
		snprintf(buf, sizeof(buf), "0x%" PRIx64, pcaddr);
		int i = debugger_breakpoint_add(m, buf);
		if (i < 0) {
			run_until_clear();
			return fail(err, "bad_arg", "can't stop at pc %s", buf);
		}
		ru.bp_id = m->breakpoints.id[i];
	}

	if (insns >= 0) {
		ru.has_insns = true;
		ru.insns_end = ninstrs_now() + insns;
	}

	if (timeout_ms >= 0) {
		ru.has_timeout = true;
		ru.deadline_us = now_us() + timeout_ms * 1000;
	}

	cJSON_AddBoolToObject(result, "already_running",
	    !debugger_is_stopped());
	if (debugger_is_stopped())
		debugger_ctl_continue();
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
	{ "regs",		cmd_regs },
	{ "reg_set",		cmd_reg_set },
	{ "mem_read",		cmd_mem_read },
	{ "mem_write",		cmd_mem_write },
	{ "disasm",		cmd_disasm },
	{ "symbol",		cmd_symbol },
	{ "run_until",		cmd_run_until },
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
	run_until_check();
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
	    "break_del (id), console_read\n(since), console_write (data), "
	    "mem_read (addr), disasm (addr), symbol (name\nor addr), or "
	    "run_until (a console pattern). See doc/control.html.\n");
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
		{ "mem_read", "addr", false },
		{ "disasm", "addr", false },
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
	} else if (arg != NULL && strcmp(cmd, "run_until") == 0) {
		/*  A console pattern:  */
		req = cJSON_CreateObject();
		cJSON *console = cJSON_AddObjectToObject(req, "console");
		cJSON_AddStringToObject(console, "pattern", arg);
	} else if (arg != NULL && strcmp(cmd, "symbol") == 0) {
		/*  A name, or an address if it starts with a digit:  */
		req = cJSON_CreateObject();
		cJSON_AddStringToObject(req, arg[0] >= '0' && arg[0] <= '9'?
		    "addr" : "name", arg);
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
		    strcmp(cmd, "run_until") == 0 ||
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
