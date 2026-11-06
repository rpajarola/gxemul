/*
 *  Copyright (C) 2003-2021  Anders Gavare.  All rights reserved.
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
 *  Debug message functionality.
 *
 *  To print a debug message, use debugmsg() or debugmsg_cpu(). The _cpu
 *  variant is if the message comes from a particular CPU. In that case, the
 *  name of the machine and cpu id may be printed.
 *
 *  In addition to the actual message to be printed, a subsystem, a component
 *  name, and a verbosity level also need to be supplied. The component name
 *  may be empty.
 *
 *  If it is relatively expensive to construct the message, then it would be
 *  a bad idea to do that every time the debugmsg is called in case it is
 *  rarely shown anyway (due to verbosity level being higher than the current
 *  setting). Then, the following construct may be used to quickly check before
 *  constructing the message that it will be shown:
 *
 *	if (ENOUGH_VERBOSITY(SUBSYS_XXX, VERBOSITY_YYY)) {
 *		char msg[....];
 *		// Construct msg here...
 *
 *		debugmsg(SUBSYS_XXX, "component", VERBOSITY_YYY, "%s", msg);
 *	}
 *
 *
 *  TODO:
 *	New debugger commands:
 *		command for showing debug output in new xterms?
 *		command for enabling breakpoints for subsystem output?
 *
 *	Convert existing debug() and fatal() calls to the new debugmsg() call.
 *
 *	Make sure that all the hardcoded subsystems are actually used, and/or
 *	move registering to individual subsystems.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "control.h"
#include "cpu.h"
#include "emul.h"
#include "machine.h"
#include "misc.h"

#undef debugmsg
#undef debugmsg_cpu

int verbose = 0;
int quiet_mode = 0;

extern bool emul_executing;
extern bool single_step;
extern bool about_to_enter_single_step;

size_t debugmsg_nr_of_subsystems = 0;
static const char **debugmsg_subsystem_name = NULL;
int *debugmsg_current_verbosity = NULL;

int debugmsg_active_verbosity = -1;
int debugmsg_destinations = DEBUG_DEST_NONE;
static char *debugmsg_logfile_path = NULL;
static FILE *debugmsg_logfile = NULL;

static int default_verbosity = VERBOSITY_WARNING;

static size_t debug_indent = 0;
static bool debug_old_currently_at_start_of_line = true;

#define	DEBUG_BUFSIZE		2048
#define	DEBUG_INDENTATION	4


/*
 *  va_debug():
 *
 *  Used internally by the old debug() and fatal() functions.
 */
static void va_debug(va_list argp, const char *fmt)
{
	char buf[DEBUG_BUFSIZE + 1];
	buf[0] = buf[DEBUG_BUFSIZE] = 0;
	vsnprintf(buf, DEBUG_BUFSIZE, fmt, argp);

	char* s = buf;
	while (*s) {
		if (debug_old_currently_at_start_of_line) {
			for (size_t i = 0; i < debug_indent * DEBUG_INDENTATION; i++)
				printf(" ");
		}

		printf("%c", *s);

		debug_old_currently_at_start_of_line = false;
		if (*s == '\n' || *s == '\r')
			debug_old_currently_at_start_of_line = true;
		s++;
	}
}


/*
 *  debugmsg_va():
 *
 *  Used internally by debugmsg().
 */
static void debugmsg_va(struct cpu* cpu, int subsystem,
	const char *name, int verbosity, va_list argp, const char *fmt)
{
	/*  For log events on the control socket, at its own level:  */
	if (verbosity <= control_log_level) {
		char msg[DEBUG_BUFSIZE];
		va_list ap2;
		va_copy(ap2, argp);
		vsnprintf(msg, sizeof(msg), fmt, ap2);
		va_end(ap2);
		control_log(cpu, (subsystem >= 0 && (size_t)subsystem < debugmsg_nr_of_subsystems) ?
		    debugmsg_subsystem_name[subsystem] : "", name, verbosity, msg);
	}

	bool ss = single_step || about_to_enter_single_step;
	bool subsys_enabled = (subsystem >= 0 &&
	    (size_t)subsystem < debugmsg_nr_of_subsystems &&
	    debugmsg_current_verbosity[subsystem] >= verbosity);

	bool do_console = ((debugmsg_destinations & DEBUG_DEST_CONSOLE) && subsys_enabled) ||
	    (ss && subsystem == SUBSYS_CPU);
	bool do_file = ((debugmsg_destinations & DEBUG_DEST_FILE) && debugmsg_logfile != NULL && subsys_enabled);
	bool do_stderr = ((debugmsg_destinations & DEBUG_DEST_STDERR) && subsys_enabled);

	if (!do_console && !do_file && !do_stderr)
		return;

	char msg_buf[DEBUG_BUFSIZE];
	msg_buf[0] = msg_buf[DEBUG_BUFSIZE - 1] = 0;
	vsnprintf(msg_buf, DEBUG_BUFSIZE, fmt, argp);

	/* 1. Console output (with color / decorations if enabled) */
	if (do_console) {
		char* s = msg_buf;
		bool debug_currently_at_start_of_line = true;
		bool show_decorations = emul_executing && !ss;

		if (console_are_slaves_allowed())
			show_decorations = false;

		while (*s) {
			if (debug_currently_at_start_of_line) {
				if (show_decorations) {
					color_normal();
					printf("[ ");
				} else {
					for (size_t i = 0; i < debug_indent * DEBUG_INDENTATION; ++i)
						printf(" ");
				}

				bool print_subsystem_name =
				    (subsystem >= 0 && (size_t)subsystem < debugmsg_nr_of_subsystems) &&
				    debugmsg_subsystem_name[subsystem][0] != '\0' &&
				    (debug_indent == 0 || (name == NULL || name[0] == '\0'));
				bool print_colon = false;

				if (cpu != NULL) {
					struct emul* emul = cpu->machine->emul;
					if (emul->n_machines > 1) {
						printf("machine \"%s\" cpu%i: ",
						    cpu->machine->name ? cpu->machine->name : "(no name)",
						    cpu->cpu_id);
						if (subsystem == SUBSYS_CPU)
							print_subsystem_name = false;
					} else if (cpu->machine->ncpus > 1) {
						printf("cpu%i: ", cpu->cpu_id);
						if (subsystem == SUBSYS_CPU)
							print_subsystem_name = false;
					}
				}

				if (verbosity == VERBOSITY_ERROR)
					color_error(false);
				else
					color_debugmsg_subsystem();

				if (print_subsystem_name) {
					printf("%s", debugmsg_subsystem_name[subsystem]);
					print_colon = true;
				}

				if (name != NULL && name[0] != '\0' &&
				    (!print_subsystem_name || strcasecmp(debugmsg_subsystem_name[subsystem], name) != 0)) {
					if (print_subsystem_name)
						printf(" ");

					if (verbosity != VERBOSITY_ERROR)
						color_debugmsg_name();

					printf("%s", name);
					print_colon = true;
				}

				if (print_colon)
					printf(": ");

				if (show_decorations) {
					for (size_t i = 0; i < debug_indent * DEBUG_INDENTATION; ++i)
						printf(" ");
				}

				if (verbosity == VERBOSITY_ERROR)
					color_error(true);
				else
					color_normal();
			}

			debug_currently_at_start_of_line = false;

			switch (*s) {
			case '\n':
				color_normal();
				if (show_decorations)
					printf(" ]");
				debug_currently_at_start_of_line = true;
				printf("\n");
				break;
			default:
				printf("%c", *s);
			}
			s++;
		}

		color_normal();
		if (show_decorations && !debug_currently_at_start_of_line)
			printf(" ]\n");
		else if (!debug_currently_at_start_of_line)
			printf("\n");
	}

	/* 2. File and stderr output (clean plain text without ANSI escape sequences) */
	if (do_file || do_stderr) {
		char line_buf[DEBUG_BUFSIZE + 256];
		char prefix[256];
		prefix[0] = '\0';
		size_t pfx_len = 0;

		if (cpu != NULL) {
			struct emul* emul = cpu->machine->emul;
			if (emul->n_machines > 1)
				pfx_len += snprintf(prefix + pfx_len, sizeof(prefix) - pfx_len,
				    "machine \"%s\" cpu%i: ",
				    cpu->machine->name ? cpu->machine->name : "(no name)",
				    cpu->cpu_id);
			else if (cpu->machine->ncpus > 1)
				pfx_len += snprintf(prefix + pfx_len, sizeof(prefix) - pfx_len,
				    "cpu%i: ", cpu->cpu_id);
		}

		const char *subsys_name = (subsystem >= 0 && (size_t)subsystem < debugmsg_nr_of_subsystems) ?
		    debugmsg_subsystem_name[subsystem] : "";
		if (subsys_name[0] != '\0') {
			pfx_len += snprintf(prefix + pfx_len, sizeof(prefix) - pfx_len,
			    "%s", subsys_name);
		}

		if (name != NULL && name[0] != '\0' &&
		    (subsys_name[0] == '\0' || strcasecmp(subsys_name, name) != 0)) {
			pfx_len += snprintf(prefix + pfx_len, sizeof(prefix) - pfx_len,
			    "%s%s", subsys_name[0] != '\0' ? " " : "", name);
		}

		if (pfx_len > 0)
			pfx_len += snprintf(prefix + pfx_len, sizeof(prefix) - pfx_len, ": ");

		char *s = msg_buf;
		while (*s) {
			char *nl = strchr(s, '\n');
			size_t cur_len = nl ? (size_t)(nl - s) : strlen(s);

			if (cur_len > 0 && s[cur_len - 1] == '\r')
				cur_len--;

			if (cur_len > 0 || (nl && *(nl + 1) != '\0')) {
				line_buf[0] = '\0';
				size_t lpos = 0;
				for (size_t i = 0; i < debug_indent * DEBUG_INDENTATION && lpos + 1 < sizeof(line_buf); i++)
					line_buf[lpos++] = ' ';
				lpos += snprintf(line_buf + lpos, sizeof(line_buf) - lpos,
				    "%s%.*s\n", prefix, (int)cur_len, s);

				if (do_file)
					fputs(line_buf, debugmsg_logfile);
				if (do_stderr)
					fputs(line_buf, stderr);
			}

			if (!nl)
				break;
			s = nl + 1;
		}
		if (do_file)
			fflush(debugmsg_logfile);
		if (do_stderr)
			fflush(stderr);
	}
}


/*
 *  debugmsg():
 *
 *  The main routine to call, when something should be printed.
 */
void debugmsg(int subsystem, const char *name,
	int verbosity_required, const char *fmt, ...)
{
	va_list argp;

	va_start(argp, fmt);
	debugmsg_va(NULL, subsystem, name, verbosity_required, argp, fmt);
	va_end(argp);
}


/*
 *  debugmsg():
 *
 *  The main routine to call, when something should be printed and we are
 *  in the context of a specific CPU.
 */
void debugmsg_cpu(struct cpu* cpu, int subsystem, const char *name,
	int verbosity_required, const char *fmt, ...)
{
	va_list argp;

	va_start(argp, fmt);
	debugmsg_va(cpu, subsystem, name, verbosity_required, argp, fmt);
	va_end(argp);
}


/*
 *  debug_indentation():
 *
 *  Modify the debug indentation. diff should be +1 to increase, and -1 to
 *  decrease after printing something.
 *
 *  This affects both the older debug() and fatal() functions, and the new
 *  debugmsg/debugmsg_cpu() functions.
 */
void debug_indentation(int diff)
{
	debug_indent += diff;

	if ((ssize_t)debug_indent < 0) {
		fprintf(stderr, "WARNING: debug_indent too low!\n");
		debug_indent = 0;
	}
}


/*
 *  legacy_log():
 *
 *  Lines printed by the older debug() and fatal() functions, as log events
 *  on the control socket. A line is often printed a piece at a time, so
 *  the pieces are collected until a newline; a line with any fatal() part
 *  is a warning, else it is at the debug level. "[ name: message ]" gives
 *  the name and the message.
 */
static char legacy_line[DEBUG_BUFSIZE];
static size_t legacy_len = 0;
static int legacy_level = VERBOSITY_DEBUG;

static void legacy_log(int verbosity, va_list argp, const char *fmt)
{
	char buf[DEBUG_BUFSIZE];

	vsnprintf(buf, sizeof(buf), fmt, argp);
	if (verbosity < legacy_level)
		legacy_level = verbosity;

	for (char *s = buf; *s; s++) {
		if (*s != '\n') {
			if (*s != '\r' && legacy_len < sizeof(legacy_line) - 1)
				legacy_line[legacy_len++] = *s;
			continue;
		}

		legacy_line[legacy_len] = '\0';
		char *msg = legacy_line, *name = NULL;
		while (*msg == ' ')
			msg ++;

		size_t len = strlen(msg);
		if (msg[0] == '[' && msg[1] == ' ' && len >= 4 &&
		    strcmp(msg + len - 2, " ]") == 0) {
			msg[len - 2] = '\0';
			msg += 2;

			char *colon = strstr(msg, ": ");
			if (colon != NULL && colon - msg < 32 &&
			    memchr(msg, ' ', colon - msg) == NULL) {
				*colon = '\0';
				name = msg;
				msg = colon + 2;
			}
		}

		if (*msg != '\0' && legacy_level <= control_log_level)
			control_log(NULL, "legacy", name, legacy_level, msg);

		legacy_len = 0;
		legacy_level = VERBOSITY_DEBUG;
	}
}


/*
 *  debug() and fatal():
 *
 *  These are here because of all the legacy code that uses them. Basically,
 *  debug() is mapped to something that is only displayed with verbosity
 *  at the default level, and fatal() is used to always print stuff. With some
 *  hacks to make it work reasonable with -q and when executing normally or
 *  when executing single-stepping.
 */
void debug(const char *fmt, ...)
{
	va_list argp;
	bool ss = single_step || about_to_enter_single_step;
	int v = verbose;

	if (control_log_level >= VERBOSITY_WARNING) {
		va_start(argp, fmt);
		legacy_log(VERBOSITY_DEBUG, argp, fmt);
		va_end(argp);
	}
	if (emul_executing)
		v--;
	if (ss)
		v++;

	if ((quiet_mode && !ss) || v < 0)
		return;

	va_start(argp, fmt);
	va_debug(argp, fmt);
	va_end(argp);
}


void fatal(const char *fmt, ...)
{
	va_list argp;

	if (control_log_level >= VERBOSITY_WARNING) {
		va_start(argp, fmt);
		legacy_log(VERBOSITY_WARNING, argp, fmt);
		va_end(argp);
	}

	va_start(argp, fmt);
	va_debug(argp, fmt);
	va_end(argp);
}


void debugmsg_recompute_active(void)
{
	int active = -1;

	if (control_log_level > active)
		active = control_log_level;

	if (single_step || about_to_enter_single_step) {
		if (VERBOSITY_INFO > active)
			active = VERBOSITY_INFO;
	}

	if (debugmsg_destinations & (DEBUG_DEST_CONSOLE | DEBUG_DEST_FILE | DEBUG_DEST_STDERR)) {
		for (size_t i = 0; i < debugmsg_nr_of_subsystems; ++i) {
			if (debugmsg_current_verbosity[i] > active)
				active = debugmsg_current_verbosity[i];
		}
	}

	debugmsg_active_verbosity = active;
}


bool debugmsg_set_logfile(const char *path)
{
	if (path == NULL || strcmp(path, "off") == 0 ||
	    strcmp(path, "none") == 0 || path[0] == '\0') {
		if (debugmsg_logfile != NULL) {
			fclose(debugmsg_logfile);
			debugmsg_logfile = NULL;
		}
		free(debugmsg_logfile_path);
		debugmsg_logfile_path = NULL;
		debugmsg_destinations &= ~DEBUG_DEST_FILE;
		debugmsg_recompute_active();
		return true;
	}

	FILE *f = fopen(path, "a");
	if (f == NULL) {
		fprintf(stderr, "debugmsg: could not open logfile '%s': %s\n",
		    path, strerror(errno));
		return false;
	}

	if (debugmsg_logfile != NULL)
		fclose(debugmsg_logfile);
	debugmsg_logfile = f;
	free(debugmsg_logfile_path);
	debugmsg_logfile_path = strdup(path);
	debugmsg_destinations |= DEBUG_DEST_FILE;
	debugmsg_recompute_active();
	return true;
}


const char *debugmsg_get_logfile(void)
{
	return debugmsg_logfile_path;
}


void debugmsg_set_destinations(int dest_mask)
{
	debugmsg_destinations = dest_mask;
	debugmsg_recompute_active();
}


int debugmsg_get_destinations(void)
{
	return debugmsg_destinations;
}


bool debugmsg_parse_level(const char *s, int *level_out)
{
	if (s == NULL || s[0] == '\0')
		return false;

	if (strcasecmp(s, "off") == 0 || strcasecmp(s, "none") == 0 ||
	    strcmp(s, "-1") == 0) {
		*level_out = VERBOSITY_OFF;
		return true;
	}
	if (strcasecmp(s, "error") == 0 || strcasecmp(s, "err") == 0 ||
	    strcasecmp(s, "e") == 0 || strcmp(s, "0") == 0) {
		*level_out = VERBOSITY_ERROR;
		return true;
	}
	if (strcasecmp(s, "warning") == 0 || strcasecmp(s, "warn") == 0 ||
	    strcasecmp(s, "w") == 0 || strcmp(s, "1") == 0) {
		*level_out = VERBOSITY_WARNING;
		return true;
	}
	if (strcasecmp(s, "info") == 0 || strcasecmp(s, "inf") == 0 ||
	    strcasecmp(s, "i") == 0 || strcmp(s, "2") == 0) {
		*level_out = VERBOSITY_INFO;
		return true;
	}
	if (strcasecmp(s, "debug") == 0 || strcasecmp(s, "dbg") == 0 ||
	    strcasecmp(s, "d") == 0 || strcmp(s, "3") == 0) {
		*level_out = VERBOSITY_DEBUG;
		return true;
	}

	char *end = NULL;
	long val = strtol(s, &end, 10);
	if (end != NULL && *end == '\0') {
		*level_out = (int)val;
		return true;
	}
	return false;
}


const char *debugmsg_level_name(int level)
{
	switch (level) {
	case VERBOSITY_OFF:
		return "off";
	case VERBOSITY_ERROR:
		return "error";
	case VERBOSITY_WARNING:
		return "warning";
	case VERBOSITY_INFO:
		return "info";
	case VERBOSITY_DEBUG:
		return "debug";
	default: {
		static char buf[32];
		snprintf(buf, sizeof(buf), "%i", level);
		return buf;
	}
	}
}


int debugmsg_subsystem_lookup(const char *name)
{
	if (name == NULL || name[0] == '\0')
		return -1;
	for (size_t i = 0; i < debugmsg_nr_of_subsystems; ++i)
		if (strcasecmp(name, debugmsg_subsystem_name[i]) == 0)
			return (int)i;
	return -1;
}


const char *debugmsg_subsystem_name_by_id(int subsys)
{
	if (subsys >= 0 && (size_t)subsys < debugmsg_nr_of_subsystems)
		return debugmsg_subsystem_name[subsys];
	return "";
}


void debugmsg_set_verbosity_level(int subsystem, int verbosity)
{
	if (subsystem != SUBSYS_ALL) {
		if (subsystem >= 0 && (size_t)subsystem < debugmsg_nr_of_subsystems)
			debugmsg_current_verbosity[subsystem] = verbosity;
	} else {
		for (size_t i = 0; i < debugmsg_nr_of_subsystems; ++i)
			debugmsg_current_verbosity[i] = verbosity;
		default_verbosity = verbosity;
	}
	debugmsg_recompute_active();
}


void debugmsg_add_verbosity_level(int subsystem, int verbosity_delta)
{
	if (subsystem != SUBSYS_ALL) {
		if (subsystem >= 0 && (size_t)subsystem < debugmsg_nr_of_subsystems)
			debugmsg_current_verbosity[subsystem] += verbosity_delta;
	} else {
		for (size_t i = 0; i < debugmsg_nr_of_subsystems; ++i)
			debugmsg_current_verbosity[i] += verbosity_delta;
		default_verbosity += verbosity_delta;
	}

	for (size_t i = 0; i < debugmsg_nr_of_subsystems; ++i)
		if (debugmsg_current_verbosity[i] < VERBOSITY_OFF)
			debugmsg_current_verbosity[i] = VERBOSITY_OFF;

	debugmsg_recompute_active();
}


/*
 *  debugmsg_register_subsystem():
 *
 *  Registers a new subsystem.
 */
int debugmsg_register_subsystem(const char* name)
{
	if (name == NULL || name[0] == '\0')
		return SUBSYS_STARTUP;

	for (size_t i = 0; i < debugmsg_nr_of_subsystems; ++i)
		if (strcasecmp(name, debugmsg_subsystem_name[i]) == 0)
			return (int)i;

	ssize_t subsys = debugmsg_nr_of_subsystems;

	debugmsg_nr_of_subsystems++;
	debugmsg_subsystem_name = realloc(debugmsg_subsystem_name,
	    sizeof(char*) * debugmsg_nr_of_subsystems);
	debugmsg_current_verbosity = realloc(debugmsg_current_verbosity,
	    sizeof(int) * debugmsg_nr_of_subsystems);

	debugmsg_subsystem_name[subsys] = strdup(name);
	debugmsg_current_verbosity[subsys] = default_verbosity;

	debugmsg_recompute_active();
	return (int)subsys;
}


/*
 *  debugmsg_change_settings():
 *
 *  Changes the current verbosity level settings for a given setting name.
 */
void debugmsg_change_settings(char *subsystem_name, char *n)
{
	int v;
	if (!debugmsg_parse_level(n, &v)) {
		printf("Invalid verbosity level '%s' (use off, error, warning, info, debug)\n", n);
		return;
	}

	if (strcasecmp("ALL", subsystem_name) == 0) {
		debugmsg_set_verbosity_level(SUBSYS_ALL, v);
		debugmsg_print_settings("ALL");
		return;
	}

	int subsys = debugmsg_subsystem_lookup(subsystem_name);
	if (subsys < 0) {
		/* Register subsystem dynamically so it can be configured before device init */
		subsys = debugmsg_register_subsystem(subsystem_name);
	}

	debugmsg_set_verbosity_level(subsys, v);
	debugmsg_print_settings(subsystem_name);

	if (v >= VERBOSITY_INFO && debugmsg_destinations == DEBUG_DEST_NONE && control_log_level < 0) {
		printf("(Note: debug output is not sent to console by default. Use 'log file <path>' or 'log console on' to view.)\n");
	}
}


/*
 *  debugmsg_print_settings():
 *
 *  Prints the current verbosity level settings.
 */
void debugmsg_print_settings(const char *subsystem_name)
{
	if (subsystem_name == NULL || subsystem_name[0] == '\0' || strcasecmp("ALL", subsystem_name) == 0) {
		printf("Debug log destinations:\n");
		printf("  Console:  %s\n", (debugmsg_destinations & DEBUG_DEST_CONSOLE) ? "on" : "off (default)");
		printf("  Stderr:   %s\n", (debugmsg_destinations & DEBUG_DEST_STDERR) ? "on" : "off");
		printf("  File:     %s\n", (debugmsg_destinations & DEBUG_DEST_FILE) && debugmsg_logfile_path ?
		    debugmsg_logfile_path : "none");
		printf("  Active highest level: %s\n\n", debugmsg_level_name(debugmsg_active_verbosity));
	}

	int n = 0;

	for (size_t i = 0; i < debugmsg_nr_of_subsystems; ++i) {
		if (subsystem_name != NULL && subsystem_name[0] != '\0' &&
		    strcasecmp("ALL", subsystem_name) != 0) {
			if (strcasecmp(subsystem_name, debugmsg_subsystem_name[i]) != 0)
				continue;
		}

		const char* name = debugmsg_subsystem_name[i];
		if (i == SUBSYS_STARTUP)
			name = "(startup related)";

		if (n == 0)
			printf("Subsystem:          Level:\n");

		printf("%17s   %s\n", name, debugmsg_level_name(debugmsg_current_verbosity[i]));

		++n;
	}

	if (n == 0)
		printf("Unknown debugmsg subsystem name '%s'\n", subsystem_name);
}


/*
 *  debugmsg_init():
 *
 *  Initializes the debugmsg functionality.
 */
void debugmsg_init(void)
{
	default_verbosity = VERBOSITY_WARNING;
	debugmsg_destinations = DEBUG_DEST_NONE;
	debugmsg_active_verbosity = -1;

	debugmsg_nr_of_subsystems = 11;
	debugmsg_subsystem_name = malloc(sizeof(char*) * debugmsg_nr_of_subsystems);
	debugmsg_current_verbosity = malloc(sizeof(int) * debugmsg_nr_of_subsystems);

	debugmsg_subsystem_name[SUBSYS_STARTUP]   = "";
	debugmsg_subsystem_name[SUBSYS_EMUL]      = "emul";
	debugmsg_subsystem_name[SUBSYS_DISK]      = "disk";
	debugmsg_subsystem_name[SUBSYS_NET]       = "net";
	debugmsg_subsystem_name[SUBSYS_MACHINE]   = "machine";
	debugmsg_subsystem_name[SUBSYS_DEVICE]    = "device";
	debugmsg_subsystem_name[SUBSYS_CPU]       = "cpu";
	debugmsg_subsystem_name[SUBSYS_MEMORY]    = "memory";
	debugmsg_subsystem_name[SUBSYS_EXCEPTION] = "exception";
	debugmsg_subsystem_name[SUBSYS_PROMEMUL]  = "promemul";
	debugmsg_subsystem_name[SUBSYS_DISPLAY]   = "display";

	// Default verbosity levels.
	for (size_t i = 0; i < debugmsg_nr_of_subsystems; ++i)
		debugmsg_current_verbosity[i] = default_verbosity;

	/* Environment variable overrides: */
	const char *logfile = getenv("GXEMUL_LOGFILE");
	if (logfile != NULL && logfile[0] != '\0')
		debugmsg_set_logfile(logfile);

	const char *loglevel = getenv("GXEMUL_LOGLEVEL");
	if (loglevel != NULL && loglevel[0] != '\0') {
		char *s = strdup(loglevel);
		char *p = s;
		while (p != NULL && *p != '\0') {
			char *comma = strchr(p, ',');
			if (comma != NULL)
				*comma++ = '\0';
			char *eq = strchr(p, '=');
			if (eq != NULL) {
				*eq++ = '\0';
				debugmsg_change_settings(p, eq);
			} else {
				debugmsg_change_settings("ALL", p);
			}
			p = comma;
		}
		free(s);
	}

	debugmsg_recompute_active();
}


