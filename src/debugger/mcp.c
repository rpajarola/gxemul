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
 *  The MCP server: gxemul --mcp unix:path|tcp:port lets an AI agent (such
 *  as Claude Code) use a running emulator's control socket as MCP tools.
 *
 *  It speaks MCP (JSON-RPC 2.0, one message per line) on stdin and stdout,
 *  and turns each tool call into control socket requests, on a connection
 *  of its own; so the emulator can be started, stopped and restarted
 *  independently, and gxemul --ctl keeps working between the calls. On top
 *  of the control socket's commands, tools which run the machine can wait
 *  for it to stop, and screendump returns the screen as an image. See
 *  doc/control.html.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "sockets.h"

#include "control.h"
#include "misc.h"

#include "thirdparty/cJSON.h"


/*  The newest MCP version this server knows; an older one which the client
    asks for is used instead:  */
#define	MCP_PROTOCOL_VERSION	"2025-06-18"

/*  How long to wait for the emulator to answer a request:  */
#define	MCP_RESPONSE_TIMEOUT_SEC	60

/*  The longest a tool waits for the machine to stop:  */
#define	MCP_MAX_WAIT_SEC		3600


static const char *mcp_spec;
static int tmp_counter = 0;


/*  What a tool does, besides sending its command:  */
enum tool_kind {
	TOOL_PLAIN,
	TOOL_RUN,		/*  waits for the machine to stop  */
	TOOL_WAIT,		/*  only waits  */
	TOOL_STATUS,		/*  hello and status  */
	TOOL_SCREENDUMP		/*  returns an image  */
};

#define	WAIT_PARAM(def)	"\"wait\": {\"type\": \"number\", \"description\": " \
	"\"Seconds to wait for the machine to stop (default " def "). If it " \
	"is still running then, the result says so, and wait can keep " \
	"waiting.\"}"

#define	CPU_PARAM	"\"cpu\": {\"type\": \"integer\", \"description\": " \
	"\"The CPU (default: the debugger's current CPU).\"}"

#define	CONSOLE_PARAM	"\"console\": {\"type\": \"integer\", " \
	"\"description\": \"A console id, from status (default: the " \
	"machine's main console).\"}"

#define	ENCODING_PARAM	"\"encoding\": {\"enum\": [\"text\", \"hex\"], " \
	"\"description\": \"text (the default), or hex for the bytes " \
	"exactly.\"}"

#define	SPACE_PARAM	"\"space\": {\"enum\": [\"virtual\", \"physical\"], " \
	"\"description\": \"Virtual addresses go through the CPU's address " \
	"translation (the default).\"}"

#define	ADDR		"An address: hex, or any expression the debugger " \
	"understands, such as \\\"panic\\\", \\\"memcpy+0x40\\\" or " \
	"\\\"sp+16\\\"."

static const struct mcp_tool {
	const char	*name;
	const char	*cmd;		/*  the control socket command  */
	enum tool_kind	kind;
	int		wait;		/*  TOOL_RUN, TOOL_WAIT: default wait  */
	const char	*description;
	const char	*properties;	/*  the input schema's properties  */
	const char	*required;	/*  ... and its required list  */
} tools[] = {
	{ "status", NULL, TOOL_STATUS, 0,
	  "The emulator's version, its machines (with their CPUs) and "
	  "consoles, whether it is running or stopped, how it last stopped, "
	  "and each CPU's pc. Start with this.",
	  "", "" },

	{ "pause", "pause", TOOL_RUN, 10,
	  "Pauses the machine, and says where it stopped.",
	  WAIT_PARAM("10"), "" },

	{ "continue", "continue", TOOL_RUN, 0,
	  "Continues the stopped machine. With wait, waits for it to stop "
	  "again, as at a breakpoint or watch, and says how it stopped.",
	  WAIT_PARAM("0: return right away"), "" },

	{ "step", "step", TOOL_RUN, 30,
	  "Runs n instructions (the machine must be stopped), and says where "
	  "it stopped, with the next instruction.",
	  "\"n\": {\"type\": \"integer\", \"description\": \"How many "
	  "instructions (default 1).\"}, " WAIT_PARAM("30"), "" },

	{ "run_until", "run_until", TOOL_RUN, 60,
	  "Runs the machine until the first of the given conditions happens "
	  "(console output, a pc, a number of instructions, or a time), and "
	  "says which one. Also ends when the machine stops for another "
	  "reason, such as a breakpoint.",
	  "\"console\": {\"type\": \"object\", \"description\": \"Stop when "
	  "a console prints one of the patterns.\", \"properties\": {"
	  "\"pattern\": {\"anyOf\": [{\"type\": \"string\"}, {\"type\": "
	  "\"array\", \"items\": {\"type\": \"string\"}}], \"description\": "
	  "\"Plain text (not a regular expression), or a list of them.\"}, "
	  CONSOLE_PARAM ", \"since\": {\"type\": \"integer\", \"description\": "
	  "\"A console offset to search from (default: what is printed from "
	  "now on).\"}, \"input\": {\"type\": \"string\", \"description\": "
	  "\"Typed into the console once the search has started. It "
	  "is typed as it is; end it with a carriage return character (in "
	  "JSON, \\\"boot\\\\r\\\") for Return, not a backslash and "
	  "an r.\"}}, \"required\": [\"pattern\"]}, "
	  "\"pc\": {\"type\": \"string\", \"description\": \"Stop when this "
	  "address is reached. " ADDR "\"}, "
	  "\"insns\": {\"type\": \"integer\", \"description\": \"Stop after "
	  "this many more instructions.\"}, "
	  "\"timeout_ms\": {\"type\": \"integer\", \"description\": \"Stop "
	  "after this many milliseconds.\"}, "
	  WAIT_PARAM("60, or longer than timeout_ms"), "" },

	{ "wait", NULL, TOOL_WAIT, 60,
	  "Waits for the running machine to stop, and says how it stopped. "
	  "If it is already stopped, says how it last stopped.",
	  WAIT_PARAM("60"), "" },

	{ "break_add", "break_add", TOOL_PLAIN, 0,
	  "Adds a breakpoint. Returns its id, which never changes.",
	  "\"pc\": {\"type\": \"string\", \"description\": \"" ADDR "\"}",
	  "\"pc\"" },

	{ "break_del", "break_del", TOOL_PLAIN, 0,
	  "Removes a breakpoint.",
	  "\"bp\": {\"type\": \"integer\", \"description\": \"The "
	  "breakpoint's id.\"}", "\"bp\"" },

	{ "break_list", "break_list", TOOL_PLAIN, 0,
	  "Lists the breakpoints.", "", "" },

	{ "console_read", "console_read", TOOL_PLAIN, 0,
	  "Reads what the guest printed on a serial console. The emulator "
	  "keeps the last 1 MB, counted by an offset which only grows; pass "
	  "the returned offset as since next time to get only what is new.",
	  CONSOLE_PARAM ", \"since\": {\"type\": \"integer\", \"description\": "
	  "\"The offset to read from (default 0).\"}, \"max\": {\"type\": "
	  "\"integer\", \"description\": \"At most this many bytes (default "
	  "65536).\"}, " ENCODING_PARAM, "" },

	{ "console_write", "console_write", TOOL_PLAIN, 0,
	  "Types into a serial console, as if from its terminal. Text is "
	  "typed as it is: a backslash is a backslash. For Return, set "
	  "enter; other control characters must be the characters "
	  "themselves (in JSON, \"\\u0003\" for Control-C), or use "
	  "encoding hex (\"03\").",
	  CONSOLE_PARAM ", \"data\": {\"type\": \"string\", \"description\": "
	  "\"What to type.\"}, \"enter\": {\"type\": \"boolean\", "
	  "\"description\": \"Press Return (a carriage return, 0x0d) after "
	  "data.\"}, " ENCODING_PARAM, "" },

	{ "regs", "regs", TOOL_PLAIN, 0,
	  "Reads a CPU's registers, as hex strings.",
	  CPU_PARAM ", \"names\": {\"type\": \"array\", \"items\": {\"type\": "
	  "\"string\"}, \"description\": \"Which registers (default: "
	  "all).\"}", "" },

	{ "reg_set", "reg_set", TOOL_PLAIN, 0,
	  "Sets a register (the machine must be stopped).",
	  CPU_PARAM ", \"name\": {\"type\": \"string\"}, \"value\": {\"type\": "
	  "\"string\", \"description\": \"A value or expression.\"}",
	  "\"name\", \"value\"" },

	{ "mem_read", "mem_read", TOOL_PLAIN, 0,
	  "Reads memory, as hex. Doesn't cause guest exceptions; an unmapped "
	  "address is an error.",
	  "\"addr\": {\"type\": \"string\", \"description\": \"" ADDR "\"}, "
	  "\"len\": {\"type\": \"integer\", \"description\": \"Bytes "
	  "(default 64, at most 1 MB).\"}, " SPACE_PARAM ", " CPU_PARAM,
	  "\"addr\"" },

	{ "mem_write", "mem_write", TOOL_PLAIN, 0,
	  "Writes memory (the machine must be stopped).",
	  "\"addr\": {\"type\": \"string\", \"description\": \"" ADDR "\"}, "
	  "\"data\": {\"type\": \"string\", \"description\": \"The bytes, in "
	  "hex.\"}, " SPACE_PARAM ", " CPU_PARAM, "\"addr\", \"data\"" },

	{ "disasm", "disasm", TOOL_PLAIN, 0,
	  "Disassembles instructions.",
	  "\"addr\": {\"type\": \"string\", \"description\": \"Where to "
	  "start (default: the pc). " ADDR "\"}, \"count\": {\"type\": "
	  "\"integer\", \"description\": \"How many (default 10).\"}, "
	  CPU_PARAM, "" },

	{ "symbol", "symbol", TOOL_PLAIN, 0,
	  "Looks up a symbol's address by name, or which symbol an address "
	  "is in.",
	  "\"name\": {\"type\": \"string\"}, \"addr\": {\"type\": \"string\", "
	  "\"description\": \"" ADDR "\"}", "" },

	{ "watch_add", "watch_add", TOOL_PLAIN, 0,
	  "Watches CPU exceptions (MIPS only, so far) or the guest's "
	  "accesses to a device's registers. What a watch sees goes into the "
	  "trace (trace_read); with action stop, it also stops the machine.",
	  "\"kind\": {\"enum\": [\"exception\", \"device\"]}, \"names\": "
	  "{\"anyOf\": [{\"type\": \"string\"}, {\"type\": \"array\", "
	  "\"items\": {\"type\": \"string\"}}], \"description\": \"Exception "
	  "names, such as TLBL or SYS (default: any).\"}, \"device\": "
	  "{\"type\": \"string\", \"description\": \"A device name, from "
	  "devices.\"}, \"offset\": {\"type\": \"string\", \"description\": "
	  "\"The start of a range of the device's registers.\"}, \"len\": "
	  "{\"type\": \"string\", \"description\": \"The range's length "
	  "(default: all of the device).\"}, \"access\": {\"enum\": [\"r\", "
	  "\"w\", \"rw\"]}, \"action\": {\"enum\": [\"stop\", \"trace\"], "
	  "\"description\": \"stop (the default) or trace only.\"}",
	  "\"kind\"" },

	{ "watch_del", "watch_del", TOOL_PLAIN, 0,
	  "Removes a watch.",
	  "\"watch\": {\"type\": \"integer\", \"description\": \"The watch's "
	  "id.\"}", "\"watch\"" },

	{ "watch_list", "watch_list", TOOL_PLAIN, 0,
	  "Lists the watches.", "", "" },

	{ "trace_read", "trace_read", TOOL_PLAIN, 0,
	  "Reads what watches saw (the last 32768 events); pass the returned "
	  "next as since to get only what is new.",
	  "\"since\": {\"type\": \"integer\", \"description\": \"A sequence "
	  "number (default 0).\"}, \"max\": {\"type\": \"integer\", "
	  "\"description\": \"At most this many events (default 1000).\"}",
	  "" },

	{ "devices", "devices", TOOL_PLAIN, 0,
	  "Lists the machine's devices, with their physical addresses.",
	  "", "" },

	{ "screendump", "screendump", TOOL_SCREENDUMP, 0,
	  "Shows what a framebuffer window shows, as an image (the machine "
	  "needs its display on).",
	  "\"screen\": {\"type\": \"integer\", \"description\": \"The "
	  "framebuffer window (default 0).\"}, \"file\": {\"type\": "
	  "\"string\", \"description\": \"Also keep the image as this PNG "
	  "file.\"}", "" },

	{ "raw", "raw", TOOL_PLAIN, 0,
	  "Runs any debugger command, as if typed at the GXemul> prompt, "
	  "and returns what it printed; e.g. \"tlbdump\", \"device list\", "
	  "\"dump 0x80001000\", or \"help\" for the list. Use pause, continue "
	  "and step rather than their debugger forms.",
	  "\"line\": {\"type\": \"string\"}", "\"line\"" },

	{ "quit", "quit", TOOL_PLAIN, 0,
	  "Exits the emulator.", "", "" },

	{ NULL, NULL, TOOL_PLAIN, 0, NULL, NULL, NULL }
};

/*  Control socket commands which have no tool of the same name (status
    is in the status tool, with hello):  */
static const char *hidden_cmds[] = { "hello", "status", "subscribe", NULL };


static const char instructions[] =
	"These tools drive a GXemul emulator through its control socket. "
	"The emulator must be running with --control (the same socket as "
	"given to --mcp), e.g. gxemul --control unix:/tmp/gx.sock -V -e "
	"3max -d disk.img (-V starts it paused). Start with status.\n"
	"Addresses and register values are hex strings; where an address "
	"is asked for, any debugger expression works too (\"panic\", "
	"\"sp+16\"). step, reg_set and mem_write need the machine stopped: "
	"pause it first. pause, step and run_until wait for the machine to "
	"stop and say how it stopped; so does continue, with wait. The "
	"serial consoles are read with console_read and typed into with "
	"console_write; screendump shows the screen. raw runs any debugger "
	"command. Every result has ninstrs, the number of instructions the "
	"machine has run.";


/*****************************************************************************/


static void send_json(cJSON *msg)
{
	char *s;

	CHECK_ALLOCATION(s = cJSON_PrintUnformatted(msg));
	fputs(s, stdout);
	fputc('\n', stdout);
	fflush(stdout);
	free(s);
	cJSON_Delete(msg);
}


static void send_result(cJSON *id, cJSON *result)
{
	cJSON *msg = cJSON_CreateObject();

	cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
	cJSON_AddItemToObject(msg, "id", cJSON_Duplicate(id, 1));
	cJSON_AddItemToObject(msg, "result", result);
	send_json(msg);
}


static void send_error(cJSON *id, int code, const char *message)
{
	cJSON *msg = cJSON_CreateObject();

	cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
	cJSON_AddItemToObject(msg, "id", id != NULL?
	    cJSON_Duplicate(id, 1) : cJSON_CreateNull());
	cJSON *e = cJSON_AddObjectToObject(msg, "error");
	cJSON_AddNumberToObject(e, "code", code);
	cJSON_AddStringToObject(e, "message", message);
	send_json(msg);
}


/*  A tool's result: text, and whether it is an error.  */
static cJSON *text_result(const char *text, bool is_error)
{
	cJSON *r = cJSON_CreateObject();
	cJSON *content = cJSON_AddArrayToObject(r, "content");
	cJSON *t = cJSON_CreateObject();

	cJSON_AddStringToObject(t, "type", "text");
	cJSON_AddStringToObject(t, "text", text);
	cJSON_AddItemToArray(content, t);
	if (is_error)
		cJSON_AddBoolToObject(r, "isError", true);
	return r;
}


static cJSON *json_result(cJSON *o)
{
	char *s;

	CHECK_ALLOCATION(s = cJSON_PrintUnformatted(o));
	cJSON *r = text_result(s, false);
	free(s);
	cJSON_Delete(o);
	return r;
}


/*****************************************************************************/


/*  A connection to the emulator, for one tool call:  */
struct conn {
	int		s;
	char		*buf;
	size_t		len;
	int		next_id;
	char		error[300];	/*  why the last request failed  */
	bool		ctl_error;	/*  ... the emulator said so  */
};


static bool conn_open(struct conn *c)
{
	c->buf = NULL;
	c->len = 0;
	c->next_id = 1;
	c->error[0] = '\0';
	c->ctl_error = false;
	c->s = control_client_connect(mcp_spec);
	if (c->s < 0)
		snprintf(c->error, sizeof(c->error), "can't connect to %s; "
		    "is gxemul running with --control %s?", mcp_spec, mcp_spec);
	return c->s >= 0;
}


static void conn_close(struct conn *c)
{
	if (c->s >= 0)
		socket_close(c->s);
	c->s = -1;
	free(c->buf);
	c->buf = NULL;
}


static void deadline_in(struct timeval *tv, double seconds)
{
	gettimeofday(tv, NULL);
	tv->tv_sec += (time_t) seconds;
	tv->tv_usec += (long) ((seconds - (time_t) seconds) * 1e6);
	if (tv->tv_usec >= 1000000) {
		tv->tv_sec ++;
		tv->tv_usec -= 1000000;
	}
}


/*
 *  conn_request():
 *
 *  Sends req (and frees it), and returns the response's result, or NULL
 *  with c->error set. Events before the response are skipped.
 */
static cJSON *conn_request(struct conn *c, cJSON *req, double *ninstrsp)
{
	struct timeval deadline;
	bool timed_out;
	char *s, *line;

	cJSON_AddNumberToObject(req, "id", c->next_id++);
	CHECK_ALLOCATION(s = cJSON_PrintUnformatted(req));
	cJSON_Delete(req);
	size_t len = strlen(s);
	CHECK_ALLOCATION(s = (char *) realloc(s, len + 2));
	strlcpy(s + len, "\n", 2);
	bool sent = control_send_all(c->s, s, len + 1);
	free(s);
	if (!sent) {
		snprintf(c->error, sizeof(c->error), "can't send to %s",
		    mcp_spec);
		return NULL;
	}

	deadline_in(&deadline, MCP_RESPONSE_TIMEOUT_SEC);
	while ((line = control_client_readline(c->s, &c->buf, &c->len,
	    &deadline, &timed_out)) != NULL) {
		cJSON *msg = cJSON_Parse(line);
		free(line);
		cJSON *ok = cJSON_GetObjectItemCaseSensitive(msg, "ok");
		if (ok == NULL) {
			cJSON_Delete(msg);
			continue;
		}

		cJSON *ni = cJSON_GetObjectItemCaseSensitive(msg, "ninstrs");
		if (ninstrsp != NULL && cJSON_IsNumber(ni))
			*ninstrsp = ni->valuedouble;

		if (cJSON_IsTrue(ok)) {
			cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(
			    msg, "result");
			cJSON_Delete(msg);
			return result != NULL? result : cJSON_CreateObject();
		}

		cJSON *e = cJSON_GetObjectItemCaseSensitive(msg, "error");
		cJSON *code = cJSON_GetObjectItemCaseSensitive(e, "code");
		cJSON *m = cJSON_GetObjectItemCaseSensitive(e, "message");
		snprintf(c->error, sizeof(c->error), "%s: %s",
		    cJSON_IsString(code)? code->valuestring : "error",
		    cJSON_IsString(m)? m->valuestring : "");
		c->ctl_error = true;
		cJSON_Delete(msg);
		return NULL;
	}

	snprintf(c->error, sizeof(c->error), timed_out? "no answer from %s "
	    "in %i seconds" : "%s closed the connection", mcp_spec,
	    MCP_RESPONSE_TIMEOUT_SEC);
	return NULL;
}


static cJSON *conn_cmd(struct conn *c, const char *cmd, double *ninstrsp)
{
	cJSON *req = cJSON_CreateObject();
	cJSON_AddStringToObject(req, "cmd", cmd);
	return conn_request(c, req, ninstrsp);
}


/*
 *  wait_stop():
 *
 *  Waits up to seconds for a stopped or exited event, and adds what
 *  happened to out: stopped (the event), with the instruction at the pc
 *  as at; exited; or running, if it didn't stop in time.
 */
static void wait_stop(struct conn *c, cJSON *out, double seconds)
{
	struct timeval deadline;
	bool timed_out = false;
	char *line;

	deadline_in(&deadline, seconds);
	while ((line = control_client_readline(c->s, &c->buf, &c->len,
	    &deadline, &timed_out)) != NULL) {
		cJSON *msg = cJSON_Parse(line);
		free(line);
		cJSON *ev = cJSON_GetObjectItemCaseSensitive(msg, "event");

		if (cJSON_IsString(ev) && strcmp(ev->valuestring,
		    "exited") == 0) {
			cJSON_AddBoolToObject(out, "exited", true);
			cJSON_Delete(msg);
			return;
		}

		if (!cJSON_IsString(ev) || strcmp(ev->valuestring,
		    "stopped") != 0) {
			cJSON_Delete(msg);
			continue;
		}

		cJSON_DeleteItemFromObjectCaseSensitive(msg, "event");
		cJSON *cpu = cJSON_GetObjectItemCaseSensitive(msg, "cpu");
		cJSON_AddItemToObject(out, "stopped", msg);

		/*  The next instruction:  */
		cJSON *req = cJSON_CreateObject();
		cJSON_AddStringToObject(req, "cmd", "disasm");
		cJSON_AddNumberToObject(req, "count", 1);
		if (cJSON_IsNumber(cpu))
			cJSON_AddNumberToObject(req, "cpu", cpu->valuedouble);
		cJSON *dis = conn_request(c, req, NULL);
		cJSON *insns = cJSON_GetObjectItemCaseSensitive(dis,
		    "instructions");
		if (cJSON_GetArraySize(insns) > 0)
			cJSON_AddItemToObject(out, "at", cJSON_DetachItemFromArray(
			    insns, 0));
		cJSON_Delete(dis);
		return;
	}

	if (timed_out) {
		cJSON_AddBoolToObject(out, "running", true);
		cJSON_AddStringToObject(out, "note", "still running; wait "
		    "waits for it to stop, pause stops it");
	} else
		cJSON_AddBoolToObject(out, "exited", true);
}


/*  The wait parameter, which tools take as seconds:  */
static double get_wait(cJSON *args, const struct mcp_tool *t)
{
	cJSON *w = cJSON_GetObjectItemCaseSensitive(args, "wait");
	double seconds = t->wait;

	if (cJSON_IsNumber(w))
		seconds = w->valuedouble;
	else if (t->cmd != NULL && strcmp(t->cmd, "run_until") == 0) {
		/*  Long enough for its timeout:  */
		cJSON *ms = cJSON_GetObjectItemCaseSensitive(args,
		    "timeout_ms");
		if (cJSON_IsNumber(ms) && ms->valuedouble / 1000 + 10 > seconds)
			seconds = ms->valuedouble / 1000 + 10;
	}

	if (seconds < 0)
		seconds = 0;
	if (seconds > MCP_MAX_WAIT_SEC)
		seconds = MCP_MAX_WAIT_SEC;
	return seconds;
}


static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64(const unsigned char *data, size_t len)
{
	char *out, *p;

	CHECK_ALLOCATION(out = p = (char *) malloc((len + 2) / 3 * 4 + 1));
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = data[i] << 16;
		if (i + 1 < len)
			v |= data[i + 1] << 8;
		if (i + 2 < len)
			v |= data[i + 2];
		*p++ = b64[(v >> 18) & 63];
		*p++ = b64[(v >> 12) & 63];
		*p++ = i + 1 < len? b64[(v >> 6) & 63] : '=';
		*p++ = i + 2 < len? b64[v & 63] : '=';
	}
	*p = '\0';
	return out;
}


static unsigned char *read_file(const char *path, size_t *lenp)
{
	FILE *f = fopen(path, "rb");
	unsigned char *data;
	long len;

	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len < 0) {
		fclose(f);
		return NULL;
	}
	CHECK_ALLOCATION(data = (unsigned char *) malloc(len + 1));
	*lenp = fread(data, 1, len, f);
	fclose(f);
	return data;
}


static void tmp_png(char *buf, size_t len)
{
#ifdef _WIN32
	const char *dir = getenv("TEMP");
	if (dir == NULL)
		dir = ".";
	snprintf(buf, len, "%s\\gxemul-mcp-%ld-%i.png", dir,
	    (long) getpid(), tmp_counter++);
#else
	const char *dir = getenv("TMPDIR");
	if (dir == NULL || dir[0] == '\0')
		dir = "/tmp";
	snprintf(buf, len, "%s/gxemul-mcp-%ld-%i.png", dir,
	    (long) getpid(), tmp_counter++);
#endif
}


/*
 *  call_tool():
 *
 *  Runs a tool, and returns its result (content, and isError).
 */
static cJSON *call_tool(const struct mcp_tool *t, const char *cmd,
	cJSON *args)
{
	struct conn c;
	cJSON *req, *result, *out;
	double ninstrs = 0;
	char tmp[4096];

	if (!conn_open(&c))
		return text_result(c.error, true);

	/*  The request: the tool's arguments, without the MCP server's own:  */
	req = cJSON_IsObject(args)? cJSON_Duplicate(args, 1) :
	    cJSON_CreateObject();
	cJSON_DeleteItemFromObjectCaseSensitive(req, "id");
	cJSON_DeleteItemFromObjectCaseSensitive(req, "cmd");
	if (t != NULL && (t->kind == TOOL_RUN || t->kind == TOOL_WAIT))
		cJSON_DeleteItemFromObjectCaseSensitive(req, "wait");

	/*  console_write's enter: a Return after the data:  */
	cJSON *enter = cJSON_GetObjectItemCaseSensitive(req, "enter");
	if (t != NULL && cmd != NULL && strcmp(cmd, "console_write") == 0 &&
	    enter != NULL) {
		bool press = cJSON_IsTrue(enter);
		cJSON *data = cJSON_GetObjectItemCaseSensitive(req, "data");
		cJSON *enc = cJSON_GetObjectItemCaseSensitive(req, "encoding");
		bool hex = cJSON_IsString(enc) &&
		    strcmp(enc->valuestring, "hex") == 0;
		const char *d = cJSON_IsString(data)? data->valuestring : "";
		char *s;

		CHECK_ALLOCATION(s = (char *) malloc(strlen(d) + 3));
		snprintf(s, strlen(d) + 3, "%s%s", d, !press? "" :
		    hex? "0d" : "\r");
		cJSON_DeleteItemFromObjectCaseSensitive(req, "enter");
		cJSON_DeleteItemFromObjectCaseSensitive(req, "data");
		cJSON_AddStringToObject(req, "data", s);
		free(s);
	}

	bool own_file = false;
	if (t != NULL && t->kind == TOOL_SCREENDUMP &&
	    cJSON_GetObjectItemCaseSensitive(req, "file") == NULL) {
		tmp_png(tmp, sizeof(tmp));
		cJSON_AddStringToObject(req, "file", tmp);
		own_file = true;
	}

	if (t != NULL && t->kind == TOOL_STATUS) {
		/*  hello, and status:  */
		cJSON_Delete(req);
		out = conn_cmd(&c, "hello", NULL);
		result = out != NULL? conn_cmd(&c, "status", &ninstrs) : NULL;
		if (result != NULL) {
			cJSON *item;
			cJSON_DeleteItemFromObjectCaseSensitive(out, "state");
			cJSON_DeleteItemFromObjectCaseSensitive(out, "proto");
			while ((item = result->child) != NULL)
				cJSON_AddItemToObject(out, item->string,
				    cJSON_DetachItemViaPointer(result, item));
			cJSON_Delete(result);
		} else {
			cJSON_Delete(out);
			out = NULL;
		}
	} else if (t != NULL && t->kind == TOOL_WAIT) {
		cJSON_Delete(req);
		out = conn_cmd(&c, "status", &ninstrs);
		cJSON *state = cJSON_GetObjectItemCaseSensitive(out, "state");
		if (cJSON_IsString(state) && strcmp(state->valuestring,
		    "running") == 0) {
			cJSON_Delete(out);
			out = cJSON_CreateObject();
			wait_stop(&c, out, get_wait(args, t));
		} else if (out != NULL) {
			cJSON *stop = cJSON_DetachItemFromObjectCaseSensitive(
			    out, "stop");
			cJSON_Delete(out);
			out = cJSON_CreateObject();
			cJSON_AddBoolToObject(out, "already_stopped", true);
			if (stop != NULL)
				cJSON_AddItemToObject(out, "stopped", stop);
		}
	} else {
		cJSON_AddStringToObject(req, "cmd", cmd);
		out = conn_request(&c, req, &ninstrs);

		if (out != NULL && t != NULL && t->kind == TOOL_RUN) {
			cJSON *already = cJSON_GetObjectItemCaseSensitive(out,
			    "already_stopped");
			double seconds = get_wait(args, t);
			if (seconds > 0 && !cJSON_IsTrue(already))
				wait_stop(&c, out, seconds);
		}
	}

	if (out == NULL) {
		cJSON *r = text_result(c.error, true);
		conn_close(&c);
		return r;
	}

	/*  The ninstrs of a stop is the later one:  */
	cJSON *stopped = cJSON_GetObjectItemCaseSensitive(out, "stopped");
	cJSON *sni = cJSON_GetObjectItemCaseSensitive(stopped, "ninstrs");
	if (cJSON_IsNumber(sni) && sni->valuedouble > ninstrs)
		ninstrs = sni->valuedouble;
	if (cJSON_GetObjectItemCaseSensitive(out, "ninstrs") == NULL)
		cJSON_AddNumberToObject(out, "ninstrs", ninstrs);
	conn_close(&c);

	if (t == NULL || t->kind != TOOL_SCREENDUMP)
		return json_result(out);

	/*  The screen, as an image:  */
	cJSON *file = cJSON_GetObjectItemCaseSensitive(out, "file");
	size_t len;
	unsigned char *png = cJSON_IsString(file)?
	    read_file(file->valuestring, &len) : NULL;
	if (png == NULL) {
		snprintf(tmp, sizeof(tmp), "can't read the screendump %s",
		    cJSON_IsString(file)? file->valuestring : "");
		cJSON_Delete(out);
		return text_result(tmp, true);
	}
	if (own_file) {
		remove(file->valuestring);
		cJSON_DeleteItemFromObjectCaseSensitive(out, "file");
	}

	cJSON *r = json_result(out);
	cJSON *img = cJSON_CreateObject();
	char *data = base64(png, len);
	free(png);
	cJSON_AddStringToObject(img, "type", "image");
	cJSON_AddStringToObject(img, "data", data);
	cJSON_AddStringToObject(img, "mimeType", "image/png");
	free(data);
	cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(r, "content"),
	    img);
	return r;
}


/*****************************************************************************/


static const struct mcp_tool *find_tool(const char *name)
{
	for (int i = 0; tools[i].name != NULL; i++)
		if (strcmp(tools[i].name, name) == 0)
			return &tools[i];
	return NULL;
}


/*  Is a control socket command a tool of its own, or hidden?  */
static bool cmd_is_covered(const char *cmd)
{
	for (int i = 0; tools[i].name != NULL; i++)
		if (tools[i].cmd != NULL && strcmp(tools[i].cmd, cmd) == 0)
			return true;
	for (int i = 0; hidden_cmds[i] != NULL; i++)
		if (strcmp(hidden_cmds[i], cmd) == 0)
			return true;
	return false;
}


static cJSON *tool_list(void)
{
	cJSON *list = cJSON_CreateArray();
	const char *name, *params;
	char *schema;

	for (int i = 0; tools[i].name != NULL; i++) {
		cJSON *o = cJSON_CreateObject();
		size_t len = strlen(tools[i].properties) +
		    strlen(tools[i].required) + 100;

		CHECK_ALLOCATION(schema = (char *) malloc(len));
		snprintf(schema, len, "{\"type\": \"object\", \"properties\": "
		    "{%s}, \"required\": [%s], \"additionalProperties\": "
		    "false}", tools[i].properties, tools[i].required);
		cJSON *s = cJSON_Parse(schema);
		if (s == NULL) {
			fprintf(stderr, "gxemul --mcp: bad schema for %s\n",
			    tools[i].name);
			s = cJSON_Parse("{\"type\": \"object\"}");
		}
		free(schema);

		cJSON_AddStringToObject(o, "name", tools[i].name);
		cJSON_AddStringToObject(o, "description",
		    tools[i].description);
		cJSON_AddItemToObject(o, "inputSchema", s);
		cJSON_AddItemToArray(list, o);
	}

	/*  Commands which have no tool of their own yet:  */
	for (int i = 0; control_command(i, &name, &params); i++) {
		if (cmd_is_covered(name))
			continue;

		cJSON *o = cJSON_CreateObject();
		char desc[300];
		snprintf(desc, sizeof(desc), "The control socket's %s "
		    "command (see doc/control.html).", name);
		cJSON_AddStringToObject(o, "name", name);
		cJSON_AddStringToObject(o, "description", desc);

		cJSON *s = cJSON_AddObjectToObject(o, "inputSchema");
		cJSON_AddStringToObject(s, "type", "object");
		cJSON *props = cJSON_AddObjectToObject(s, "properties");
		const char *p = params;
		while (*p != '\0') {
			while (*p == ' ')
				p ++;
			size_t wlen = strcspn(p, " ");
			if (wlen > 0) {
				snprintf(desc, sizeof(desc), "%.*s", (int) wlen,
				    p);
				cJSON_AddObjectToObject(props, desc);
			}
			p += wlen;
		}
		cJSON_AddBoolToObject(s, "additionalProperties", false);
		cJSON_AddItemToArray(list, o);
	}

	return list;
}


static void handle_message(const char *line)
{
	cJSON *msg = cJSON_Parse(line);
	cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
	cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
	cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");

	if (!cJSON_IsObject(msg)) {
		send_error(NULL, -32700, "not a JSON object");
		cJSON_Delete(msg);
		return;
	}

	/*  Notifications (such as notifications/initialized), and
	    responses, need no answer:  */
	if (id == NULL || !cJSON_IsString(method)) {
		cJSON_Delete(msg);
		return;
	}

	const char *m = method->valuestring;
	if (strcmp(m, "initialize") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON *v = cJSON_GetObjectItemCaseSensitive(params,
		    "protocolVersion");
		const char *version = MCP_PROTOCOL_VERSION;
		if (cJSON_IsString(v) && strcmp(v->valuestring,
		    MCP_PROTOCOL_VERSION) < 0 && (strcmp(v->valuestring,
		    "2024-11-05") == 0 || strcmp(v->valuestring,
		    "2025-03-26") == 0))
			version = v->valuestring;

		cJSON_AddStringToObject(r, "protocolVersion", version);
		cJSON *caps = cJSON_AddObjectToObject(r, "capabilities");
		cJSON_AddObjectToObject(caps, "tools");
		cJSON *info = cJSON_AddObjectToObject(r, "serverInfo");
		cJSON_AddStringToObject(info, "name", "gxemul");
		cJSON_AddStringToObject(info, "version", VERSION);
		cJSON_AddStringToObject(r, "instructions", instructions);
		send_result(id, r);
	} else if (strcmp(m, "ping") == 0) {
		send_result(id, cJSON_CreateObject());
	} else if (strcmp(m, "tools/list") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddItemToObject(r, "tools", tool_list());
		send_result(id, r);
	} else if (strcmp(m, "tools/call") == 0) {
		cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
		cJSON *args = cJSON_GetObjectItemCaseSensitive(params,
		    "arguments");
		const struct mcp_tool *t = cJSON_IsString(name)?
		    find_tool(name->valuestring) : NULL;
		const char *pname, *pparams;
		bool known = t != NULL;

		/*  A command which has no tool of its own:  */
		for (int i = 0; !known && cJSON_IsString(name) &&
		    control_command(i, &pname, &pparams); i++)
			known = strcmp(pname, name->valuestring) == 0 &&
			    !cmd_is_covered(pname);

		if (!known)
			send_error(id, -32602, "unknown tool");
		else
			send_result(id, call_tool(t, t != NULL? t->cmd :
			    name->valuestring, args));
	} else
		send_error(id, -32601, "unknown method");

	cJSON_Delete(msg);
}


/*  One line from stdin, malloced, without the newline; NULL at the end.  */
static char *read_line(void)
{
	size_t len = 0, size = 4096;
	char *buf;
	int ch;

	CHECK_ALLOCATION(buf = (char *) malloc(size));
	while ((ch = getchar()) != EOF && ch != '\n') {
		if (len + 1 >= size)
			CHECK_ALLOCATION(buf = (char *) realloc(buf,
			    size *= 2));
		buf[len++] = ch;
	}
	if (ch == EOF && len == 0) {
		free(buf);
		return NULL;
	}
	if (len > 0 && buf[len - 1] == '\r')
		len --;
	buf[len] = '\0';
	return buf;
}


/*
 *  mcp_main():
 *
 *  gxemul --mcp unix:path|tcp:port
 */
int mcp_main(int argc, char *argv[])
{
	char *line;

	if (argc != 3 || (strncmp(argv[2], "unix:", 5) != 0 &&
	    strncmp(argv[2], "tcp:", 4) != 0)) {
		fprintf(stderr, "usage: gxemul --mcp unix:path|tcp:port\n"
		    "An MCP server for AI agents, which uses the control socket"
		    " of a gxemul\nstarted with --control unix:path|tcp:port. "
		    "See doc/control.html.\n");
		return 2;
	}
	mcp_spec = argv[2];

#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif

	while ((line = read_line()) != NULL) {
		if (line[0] != '\0')
			handle_message(line);
		free(line);
	}

	return 0;
}
