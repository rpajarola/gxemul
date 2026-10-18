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
 *  File choosers for the display's menus.  SDL2 has none, so the desktop's
 *  is used: osascript on macOS, zenity or kdialog elsewhere (which run as
 *  another process, printing the path chosen), and the common file dialog
 *  on Windows (in a thread of its own).  display_files_poll() checks on
 *  it, so the emulated machine keeps running while the chooser is open.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/wait.h>
#endif

#include "machine.h"
#include "machine_config.h"
#include "misc.h"
#include "display.h"


/*  The chooser that is open (there is at most one):  */
static bool chooser_open = false;
static struct machine *open_machine;
static files_done_func open_done;
static void *open_data;
static bool open_folder;


/*
 *  Calls the done function of the open chooser, with what was chosen, and
 *  remembers the folder it was chosen in (for the next chooser).
 */
static void chosen(const char *path)
{
	char *clean = NULL;

	chooser_open = false;
	if (path != NULL) {
		/*  (osascript ends a folder with a slash.)  */
		clean = machine_config_clean_path(path);
		path = clean;
		if (open_folder)
			appdirs_set_last_dir(clean);
		else {
			char *dir, *slash;
			CHECK_ALLOCATION(dir = strdup(clean));
			slash = strrchr(dir, '/');
			if (slash != NULL) {
				slash[slash == dir ? 1 : 0] = '\0';
				appdirs_set_last_dir(dir);
			}
			free(dir);
		}
	}
	if (open_machine != NULL)
		display_backend_raise(open_machine);
	open_done(open_machine, path, open_data);
	free(clean);
}


/*  Shows a message in the machine's window, or prints it if none:  */
static void files_message(struct machine *m, const char *text)
{
	if (m != NULL)
		display_message(m, "%s", text);
	else
		fprintf(stderr, "%s\n", text);
}


#ifdef _WIN32

bool display_can_choose_files(void)
{
	return true;
}


static bool start_chooser(const char *title, const char *start_dir,
	const char *save_name, bool folder)
{
	return win32_choose_file(title, start_dir, save_name != NULL,
	    save_name, folder);
}


void display_files_poll(void)
{
	char *path;

	if (!chooser_open || !win32_choose_file_done(&path))
		return;

	chosen(path);
	free(path);
}

#else	/*  !_WIN32  */

enum chooser { NONE, OSASCRIPT, ZENITY, KDIALOG };

static pid_t chooser_pid;
static int chooser_fd;
static char *output;
static size_t output_len;


#ifndef __APPLE__
/*  Returns true if a program is in the PATH.  */
static bool in_path(const char *program)
{
	const char *path = getenv("PATH");
	char file[1024];

	while (path != NULL && *path != '\0') {
		size_t len = strcspn(path, ":");
		snprintf(file, sizeof(file), "%.*s/%s", (int) len, path,
		    program);
		if (len > 0 && access(file, X_OK) == 0)
			return true;
		path += len;
		if (*path == ':')
			path ++;
	}
	return false;
}
#endif


static enum chooser which_chooser(void)
{
	static enum chooser chooser = NONE;
	static bool checked = false;

	if (!checked) {
#ifdef __APPLE__
		chooser = OSASCRIPT;
#else
		if (in_path("zenity"))
			chooser = ZENITY;
		else if (in_path("kdialog"))
			chooser = KDIALOG;
#endif
		checked = true;
	}
	return chooser;
}


bool display_can_choose_files(void)
{
	return which_chooser() != NONE;
}


/*  Appends an AppleScript string literal to buf.  */
static void append_applescript(char *buf, size_t len, const char *text)
{
	size_t n = strlen(buf);

	if (n + 1 < len)
		buf[n++] = '"';
	for (; *text != '\0' && n + 3 < len; text++) {
		if (*text == '"' || *text == '\\')
			buf[n++] = '\\';
		buf[n++] = *text;
	}
	if (n + 1 < len)
		buf[n++] = '"';
	buf[n] = '\0';
}


static bool start_chooser(const char *title, const char *start_dir,
	const char *save_name, bool folder)
{
	const char *argv[8];
	char script[3000], arg1[300], arg2[1100];
	int argc = 0, fds[2];

	switch (which_chooser()) {
	case OSASCRIPT:
		snprintf(script, sizeof(script), "activate\nset chosen to %s "
		    "with prompt ", folder ? "choose folder" : save_name !=
		    NULL ? "choose file name" : "choose file");
		append_applescript(script, sizeof(script), title);
		if (start_dir != NULL) {
			strcat(script, " default location (POSIX file ");
			append_applescript(script, sizeof(script), start_dir);
			strcat(script, ")");
		}
		if (save_name != NULL) {
			strcat(script, " default name ");
			append_applescript(script, sizeof(script), save_name);
		}
		strcat(script, "\nreturn POSIX path of chosen\n");
		argv[argc++] = "osascript";
		argv[argc++] = "-e";
		argv[argc++] = script;
		break;

	case ZENITY:
		argv[argc++] = "zenity";
		argv[argc++] = "--file-selection";
		if (save_name != NULL)
			argv[argc++] = "--save";
		if (folder)
			argv[argc++] = "--directory";
		snprintf(arg1, sizeof(arg1), "--title=%s", title);
		argv[argc++] = arg1;
		if (start_dir != NULL || save_name != NULL) {
			snprintf(arg2, sizeof(arg2), "--filename=%s/%s",
			    start_dir != NULL ? start_dir : ".",
			    save_name != NULL ? save_name : "");
			argv[argc++] = arg2;
		}
		break;

	case KDIALOG:
		argv[argc++] = "kdialog";
		argv[argc++] = "--title";
		argv[argc++] = title;
		argv[argc++] = folder ? "--getexistingdirectory" :
		    save_name != NULL ? "--getsavefilename" :
		    "--getopenfilename";
		snprintf(arg2, sizeof(arg2), "%s%s%s",
		    start_dir != NULL ? start_dir : ".",
		    save_name != NULL ? "/" : "",
		    save_name != NULL ? save_name : "");
		argv[argc++] = arg2;
		break;

	default:
		return false;
	}
	argv[argc] = NULL;

	/*  Start the chooser, with its output to a pipe:  */
	if (pipe(fds) != 0)
		return false;

	chooser_pid = fork();
	if (chooser_pid == 0) {
		int fd = open("/dev/null", O_RDWR);
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDERR_FILENO);
		dup2(fds[1], STDOUT_FILENO);
		execvp(argv[0], (char * const *) argv);
		_exit(127);
	}

	close(fds[1]);
	if (chooser_pid < 0) {
		close(fds[0]);
		return false;
	}

	chooser_fd = fds[0];
	fcntl(chooser_fd, F_SETFL, fcntl(chooser_fd, F_GETFL) | O_NONBLOCK);
	fcntl(chooser_fd, F_SETFD, FD_CLOEXEC);
	output_len = 0;
	return true;
}


void display_files_poll(void)
{
	char buf[1024];
	ssize_t got;
	int status;

	if (!chooser_open)
		return;

	/*  Read what the chooser prints, until it exits:  */
	for (;;) {
		got = read(chooser_fd, buf, sizeof(buf));
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (got <= 0)
			break;

		CHECK_ALLOCATION(output = (char *) realloc(output,
		    output_len + got + 1));
		memcpy(output + output_len, buf, got);
		output_len += got;
	}

	close(chooser_fd);
	while (waitpid(chooser_pid, &status, 0) < 0 && errno == EINTR)
		;

	/*  The path is the first line:  */
	char *path = NULL;
	if (output_len > 0) {
		output[output_len] = '\0';
		output[strcspn(output, "\n")] = '\0';
		if (output[0] != '\0')
			path = output;
	}

	chosen(path);
}

#endif	/*  !_WIN32  */


/*
 *  display_choose_file(), display_choose_new_file():
 *
 *  Opens a file chooser, for a file to open, or for the name of a new file
 *  (with save_name as the default).  done is called (from display_files_
 *  poll()) with the path chosen, or NULL.  m may be NULL.
 */
static void choose(struct machine *m, const char *title, const char *start_dir,
	const char *save_name, bool folder, files_done_func done, void *data)
{
	struct stat st;

	if (chooser_open) {
		files_message(m, "A file chooser is already open");
		return;
	}
	if (start_dir == NULL)
		start_dir = appdirs_last_dir();

	/*  Only start in a folder that exists:  */
	if (start_dir != NULL &&
	    (stat(start_dir, &st) != 0 || !S_ISDIR(st.st_mode)))
		start_dir = NULL;

#ifndef _WIN32
	/*  (osascript needs an absolute path.)  */
	char *abs_dir = start_dir == NULL ? NULL : realpath(start_dir, NULL);
	start_dir = abs_dir;
#endif

	bool started = start_chooser(title, start_dir, save_name, folder);

#ifndef _WIN32
	free(abs_dir);
#endif

	if (!started) {
		files_message(m, "Could not open a file chooser");
		return;
	}

	chooser_open = true;
	open_machine = m;
	open_done = done;
	open_data = data;
	open_folder = folder;
}

void display_choose_file(struct machine *m, const char *title,
	const char *start_dir, files_done_func done, void *data)
{
	choose(m, title, start_dir, NULL, false, done, data);
}

void display_choose_new_file(struct machine *m, const char *title,
	const char *start_dir, const char *save_name, files_done_func done,
	void *data)
{
	choose(m, title, start_dir, save_name != NULL ? save_name : "",
	    false, done, data);
}

void display_choose_folder(struct machine *m, const char *title,
	const char *start_dir, files_done_func done, void *data)
{
	choose(m, title, start_dir, NULL, true, done, data);
}

bool display_chooser_open(void)
{
	return chooser_open;
}
