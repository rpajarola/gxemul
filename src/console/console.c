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
 *  Generic console support functions.
 *
 *  This module is used by individual device drivers, for example serial
 *  controllers, keyboards, or other devices which need to attach to the
 *  emulator's real stdin/stdout.
 *
 *  The idea is that several input and output streams (console handles) are
 *  allowed. As long as the number of input streams is <= 1, then everything
 *  can be done in the emulator's default terminal window.
 *
 *  If the number of inputs is more than 1, each input needs a terminal of
 *  its own. (Otherwise the behaviour is undefined; i.e. which of two
 *  emulated serial controllers would get keyboard input?) With the -x
 *  command line option, each console handle except the main one listens on
 *  a TCP port on localhost, which can be connected to with telnet.
 *
 *  (If the -x command line option is NOT used, then no ports are opened.
 *  Instead, a warning message is printed, and input is not allowed.)
 *
 *  Note that console handles that _allow_ input but are not yet used for
 *  output are not counted. This allows a machine to have, say, 2 serial ports
 *  which can be used for both input and output, and it will still be possible
 *  to run in the default terminal window as long as only one of those serial
 *  ports is actually used.
 *
 *  Output that arrives while nothing is connected to a port is kept (up to
 *  CONSOLE_BACKLOG_LEN bytes), and sent when a connection is made.
 *
 *  The MAIN console handle (fixed as handle nr 0) is the one used by the
 *  default terminal window. A machine which registers a serial controller,
 *  which should be used as the main way of communicating with guest operating
 *  systems running on that machine, should set machine->main_console_handle
 *  to the handle of the correct port on that controller.
 *
 *
 *  NOTE: The code in this module is mostly non-reentrant.
 */

#include <errno.h>
#ifndef _WIN32
#include <fcntl.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#ifndef _WIN32
#include <termios.h>
#endif

#include "sockets.h"

#include "console.h"
#include "emul.h"
#include "machine.h"
#include "settings.h"


extern char *progname;
extern int verbose;
extern struct settings *global_settings;


#ifndef _WIN32
static struct termios console_oldtermios;
static struct termios console_curtermios;
#endif

static int console_initialized = 0;
static struct settings *console_settings = NULL;
static int console_stdout_pending;

#define	CONSOLE_FIFO_LEN	4096
#define	CONSOLE_BACKLOG_LEN	65536

#define	DEFAULT_TELNET_PORT	4000

static struct timeval console_mouse_lastupdate;
static int console_mouse_dx;		/*  relative since last call  */
static int console_mouse_dy;		/*  relative since last call  */
static int console_mouse_fb_nr;		/*  framebuffer number of
					    host movement, 0-based  */
static int console_mouse_buttons;	/*  left=4, middle=2, right=1  */

static int allow_slaves = 0;
static int telnet_port = DEFAULT_TELNET_PORT;	/*  the next port to try  */

struct console_handle {
	int		in_use;
	int		in_use_for_input;
	int		using_telnet;
	int		inputonly;
	int		outputonly;
	int		warning_printed;

	struct machine	*machine;
	char		*machine_name;
	char		*name;

	/*  When using_telnet:  */
	int		port;
	int		listen_fd;
	int		client_fd;	/*  -1 if nothing is connected  */
	int		telnet_state;
	int		last_input;

	/*  Output not yet sent to the client:  */
	unsigned char	*backlog;
	int		backlog_head;
	int		backlog_len;

	unsigned char	fifo[CONSOLE_FIFO_LEN];
	int		fifo_head;
	int		fifo_tail;
};

/*  Telnet protocol (RFC 854):  */
#define	TELNET_SE		240
#define	TELNET_IP		244
#define	TELNET_SB		250
#define	TELNET_WILL		251
#define	TELNET_WONT		252
#define	TELNET_DO		253
#define	TELNET_DONT		254
#define	TELNET_IAC		255
#define	TELNET_OPT_ECHO		1
#define	TELNET_OPT_SGA		3

enum telnet_state {
	TELNET_DATA, TELNET_GOT_IAC, TELNET_GOT_WILL, TELNET_GOT_WONT,
	TELNET_GOT_DO, TELNET_GOT_DONT, TELNET_IN_SB, TELNET_IN_SB_GOT_IAC
};

/*  A simple array of console_handles  */
static struct console_handle *console_handles = NULL;
static int n_console_handles = 0;


/*
 *  console_deinit_main():
 *
 *  Restore host's console settings.
 */
void console_deinit_main(void)
{
	if (!console_initialized)
		return;

#ifdef _WIN32
	win32_console_restore();
#else
	tcsetattr(STDIN_FILENO, TCSANOW, &console_oldtermios);
#endif

	console_initialized = 0;
}


/*
 *  console_sigcont():
 *
 *  If the user presses CTRL-Z (to stop the emulator process) and then
 *  continues, the termios settings might have been invalidated. This
 *  function restores them.
 *
 *  (This function should be set as the SIGCONT signal handler in src/emul.c.)
 */
void console_sigcont(int x)
{
#ifdef SIGCONT
	if (!console_initialized)
		return;

	/*  Make sure that the correct (current) termios setting is active:  */
	tcsetattr(STDIN_FILENO, TCSANOW, &console_curtermios);

	/*  Reset the signal handler:  */
	signal(SIGCONT, console_sigcont);
#endif
}


/*
 *  telnet_listen():
 *
 *  Starts listening on a TCP port on localhost for a console handle, using
 *  the first free port from telnet_port and up.
 */
static void telnet_listen(int handle)
{
	struct console_handle *chp = &console_handles[handle];
	struct sockaddr_in sin;
	int one = 1;

	chp->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (chp->listen_fd < 0) {
		perror("console: socket");
		exit(1);
	}

	setsockopt(chp->listen_fd, SOL_SOCKET, SO_REUSEADDR,
	    (void *) &one, sizeof(one));
#ifdef FD_CLOEXEC
	/*  (So that a reset, which execs gxemul again, gets the same port.)  */
	fcntl(chp->listen_fd, F_SETFD, FD_CLOEXEC);
#endif

	for (;;) {
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sin.sin_port = htons(telnet_port);

		if (bind(chp->listen_fd, (struct sockaddr *) &sin,
		    sizeof(sin)) == 0)
			break;

		if (socket_error() != SOCKET_EADDRINUSE ||
		    telnet_port >= 65535) {
			fprintf(stderr, "console: can't listen on port %i "
			    "(error %i)\n", telnet_port, socket_error());
			exit(1);
		}

		telnet_port ++;
	}

	if (listen(chp->listen_fd, 1) < 0) {
		perror("console: listen");
		exit(1);
	}

	socket_set_nonblocking(chp->listen_fd);

	chp->port = telnet_port ++;
	chp->client_fd = -1;
	CHECK_ALLOCATION(chp->backlog = (unsigned char *)
	    malloc(CONSOLE_BACKLOG_LEN));
	chp->backlog_head = chp->backlog_len = 0;
	chp->using_telnet = 1;
}


/*
 *  telnet_show_ports():
 *
 *  Tells the user which port to connect to for which console.
 */
static void telnet_show_ports(struct emul *emul)
{
	for (int i = 0; i < n_console_handles; i++) {
		struct console_handle *chp = &console_handles[i];
		struct machine *m = chp->machine;

		if (!chp->in_use || !chp->using_telnet)
			continue;

		printf("[ telnet localhost %i: ", chp->port);
		if (emul->n_machines > 1 && m != NULL && m->name != NULL)
			printf("%s, ", m->name);
		printf("%s", chp->name);
		if (m != NULL && m->main_console_handle == i)
			printf(" (main console)");
		else if (chp->outputonly)
			printf(" (output only)");
		printf(" ]\n");
	}
}


static void telnet_send(int fd, const unsigned char *buf, int len)
{
#ifdef MSG_NOSIGNAL
	int flags = MSG_NOSIGNAL;
#else
	int flags = 0;
#endif

	/*  Errors show up as a disconnect when reading or flushing.  */
	(void) send(fd, (const char *) buf, len, flags);
}


static void telnet_disconnect(int handle)
{
	struct console_handle *chp = &console_handles[handle];

	socket_close(chp->client_fd);
	chp->client_fd = -1;
}


/*
 *  telnet_flush_backlog():
 *
 *  Sends as much of the pending output as the client accepts.
 */
static void telnet_flush_backlog(int handle)
{
	struct console_handle *chp = &console_handles[handle];
#ifdef MSG_NOSIGNAL
	int flags = MSG_NOSIGNAL;
#else
	int flags = 0;
#endif

	while (chp->client_fd >= 0 && chp->backlog_len > 0) {
		int start = (chp->backlog_head - chp->backlog_len +
		    CONSOLE_BACKLOG_LEN) % CONSOLE_BACKLOG_LEN;
		int len = chp->backlog_len;
		ssize_t res;

		if (start + len > CONSOLE_BACKLOG_LEN)
			len = CONSOLE_BACKLOG_LEN - start;

		res = send(chp->client_fd, (const char *) chp->backlog + start,
		    len, flags);
		if (res < 0) {
			int e = socket_error();
			if (!socket_would_block(e) && e != SOCKET_EINTR)
				telnet_disconnect(handle);
			return;
		}

		chp->backlog_len -= res;
	}
}


/*
 *  telnet_accept():
 *
 *  Accepts a new connection to a console handle's port, if there is one.
 *  Only one connection at a time is allowed.
 */
static void telnet_accept(int handle)
{
	static const unsigned char negotiate[] = {
		TELNET_IAC, TELNET_WILL, TELNET_OPT_ECHO,
		TELNET_IAC, TELNET_WILL, TELNET_OPT_SGA
	};
	struct console_handle *chp = &console_handles[handle];
	int fd, one = 1;

	fd = accept(chp->listen_fd, NULL, NULL);
	if (fd < 0)
		return;
#ifdef FD_CLOEXEC
	fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif

#ifdef SO_NOSIGPIPE
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (void *) &one, sizeof(one));
#endif
	(void) one;

	if (chp->client_fd >= 0) {
		static const char busy[] = "Already in use.\r\n";
		telnet_send(fd, (const unsigned char *) busy,
		    sizeof(busy) - 1);
		socket_close(fd);
		return;
	}

	socket_set_nonblocking(fd);

	chp->client_fd = fd;
	chp->telnet_state = TELNET_DATA;
	chp->last_input = -1;

	telnet_send(fd, negotiate, sizeof(negotiate));
	telnet_flush_backlog(handle);
}


/*
 *  telnet_option():
 *
 *  Answers the client's option negotiation: the only options are the
 *  server's ECHO and SUPPRESS-GO-AHEAD, which telnet_accept() offers.
 */
static void telnet_option(int handle, int cmd, int opt)
{
	struct console_handle *chp = &console_handles[handle];
	unsigned char reply[3] = { TELNET_IAC, 0, (unsigned char) opt };

	if (cmd == TELNET_DO) {
		if (opt == TELNET_OPT_ECHO || opt == TELNET_OPT_SGA)
			return;
		reply[1] = TELNET_WONT;
	} else if (cmd == TELNET_WILL) {
		reply[1] = opt == TELNET_OPT_SGA ? TELNET_DO : TELNET_DONT;
	} else
		return;

	telnet_send(chp->client_fd, reply, sizeof(reply));
}


/*
 *  telnet_poll():
 *
 *  Accepts connections to a console handle's port, and returns 1 if
 *  anything is available from the connected client.
 */
static int telnet_poll(int handle)
{
	struct console_handle *chp = &console_handles[handle];
	fd_set rfds;
	struct timeval tv;
	int maxfd = chp->listen_fd;

	FD_ZERO(&rfds);
	FD_SET(chp->listen_fd, &rfds);
	if (chp->client_fd >= 0) {
		FD_SET(chp->client_fd, &rfds);
		if (chp->client_fd > maxfd)
			maxfd = chp->client_fd;
	}

	tv.tv_sec = 0;
	tv.tv_usec = 0;
	if (select(maxfd + 1, &rfds, NULL, NULL, &tv) <= 0)
		return 0;

	if (FD_ISSET(chp->listen_fd, &rfds)) {
		int was_connected = chp->client_fd >= 0;
		telnet_accept(handle);
		if (!was_connected)
			return 0;
	}

	return chp->client_fd >= 0 && FD_ISSET(chp->client_fd, &rfds);
}


/*
 *  telnet_input():
 *
 *  Puts characters received from a telnet client into a handle's fifo,
 *  without the telnet commands. Enter arrives as CR NUL or CR LF, which
 *  becomes just CR, as with a real terminal.
 */
static void telnet_input(int handle, const unsigned char *buf, int len)
{
	struct console_handle *chp = &console_handles[handle];

	for (int i = 0; i < len; i++) {
		int ch = buf[i];

		switch (chp->telnet_state) {
		case TELNET_DATA:
			if (ch == TELNET_IAC) {
				chp->telnet_state = TELNET_GOT_IAC;
				break;
			}
			if (chp->last_input == '\r' && (ch == '\0' ||
			    ch == '\n')) {
				chp->last_input = -1;
				break;
			}
			chp->last_input = ch;
			console_makeavail(handle, ch);
			break;
		case TELNET_GOT_IAC:
			chp->telnet_state = TELNET_DATA;
			chp->last_input = -1;
			switch (ch) {
			case TELNET_IAC:
				console_makeavail(handle, ch);
				break;
			case TELNET_IP:
				console_makeavail(handle, 3);
				break;
			case TELNET_SB:
				chp->telnet_state = TELNET_IN_SB;
				break;
			case TELNET_WILL:
			case TELNET_WONT:
			case TELNET_DO:
			case TELNET_DONT:
				chp->telnet_state = TELNET_GOT_WILL +
				    (ch - TELNET_WILL);
				break;
			}
			break;
		case TELNET_GOT_WILL:
		case TELNET_GOT_WONT:
		case TELNET_GOT_DO:
		case TELNET_GOT_DONT:
			telnet_option(handle, TELNET_WILL +
			    (chp->telnet_state - TELNET_GOT_WILL), ch);
			chp->telnet_state = TELNET_DATA;
			break;
		case TELNET_IN_SB:
			if (ch == TELNET_IAC)
				chp->telnet_state = TELNET_IN_SB_GOT_IAC;
			break;
		case TELNET_IN_SB_GOT_IAC:
			chp->telnet_state = ch == TELNET_SE ?
			    TELNET_DATA : TELNET_IN_SB;
			break;
		}
	}
}


/*
 *  telnet_output():
 *
 *  Queues a character for a handle's telnet client, and sends what it can.
 */
static void telnet_output(int handle, int ch)
{
	struct console_handle *chp = &console_handles[handle];

	if (chp->client_fd < 0)
		telnet_accept(handle);

	/*  When the backlog is full, the oldest output is dropped.  */
	for (int n = ch == TELNET_IAC ? 2 : 1; n > 0; n--) {
		chp->backlog[chp->backlog_head] = ch;
		chp->backlog_head = (chp->backlog_head + 1) %
		    CONSOLE_BACKLOG_LEN;
		if (chp->backlog_len < CONSOLE_BACKLOG_LEN)
			chp->backlog_len ++;
	}

	telnet_flush_backlog(handle);
}


/*
 *  d_avail():
 *
 *  Returns 1 if anything is available on a descriptor.
 */
#ifndef _WIN32
static int d_avail(int d)
{
	fd_set rfds;
	struct timeval tv;

	FD_ZERO(&rfds);
	FD_SET(d, &rfds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;

	for (;;) {
		// Select may return -1 if interrupted.
		int r = select(d+1, &rfds, NULL, NULL, &tv);
		if (r >= 0)
			return r;
	}
}
#endif


/*
 *  console_makeavail():
 *
 *  Put a character in the queue, so that it will be avaiable,
 *  by inserting it into the char fifo.
 */
void console_makeavail(int handle, char ch)
{
	console_handles[handle].fifo[
	    console_handles[handle].fifo_head] = ch;
	console_handles[handle].fifo_head = (
	    console_handles[handle].fifo_head + 1) % CONSOLE_FIFO_LEN;

	if (console_handles[handle].fifo_head ==
	    console_handles[handle].fifo_tail)
		fatal("[ WARNING: console fifo overrun, handle %i ]\n", handle);
}


/*
 *  console_stdin_avail():
 *
 *  Returns 1 if a char is available from a handle's read descriptor,
 *  0 otherwise.
 */
static int console_stdin_avail(int handle)
{
	if (!console_handles[handle].in_use_for_input)
		return 0;

	if (!console_handles[handle].using_telnet) {
#ifdef _WIN32
		return win32_stdin_avail();
#else
		return d_avail(STDIN_FILENO);
#endif
	}

	return telnet_poll(handle);
}


static int console_room_left_in_fifo(int handle)
{
	int roomLeftInFIFO = console_handles[handle].fifo_tail
	    - console_handles[handle].fifo_head;
	if (roomLeftInFIFO <= 0)
		roomLeftInFIFO += CONSOLE_FIFO_LEN;

	return roomLeftInFIFO;
}


/*
 *  console_charavail():
 *
 *  Returns the number of chararacters available in the fifo.
 */
int console_charavail(int handle)
{
	while (console_stdin_avail(handle)) {
		unsigned char ch[100];		/* = getchar(); */
		ssize_t len;
		int i, d;

		// If adding more would lead to a full FIFO, then let's
		// wait.
		int roomLeftInFIFO = console_room_left_in_fifo(handle);
		if (roomLeftInFIFO < (int)sizeof(ch) + 1)
			break;

		if (console_handles[handle].using_telnet) {
			d = console_handles[handle].client_fd;
			len = recv(d, (char *) ch, sizeof(ch), 0);
			if (len <= 0) {
				int e = socket_error();
				if (len == 0 || (!socket_would_block(e) &&
				    e != SOCKET_EINTR))
					telnet_disconnect(handle);
				break;
			}

			telnet_input(handle, ch, len);
			continue;
		}

#ifdef _WIN32
		len = win32_stdin_read(ch, sizeof(ch));
#else
		len = read(STDIN_FILENO, ch, sizeof(ch));
#endif
		if (len <= 0)
			break;

		for (i=0; i<len; i++) {
			/*  printf("[ %i: %i ]\n", i, ch[i]);  */

			if (!allow_slaves) {
				/*  Convert CTRL-B intro CTRL-C.  */
				if (ch[i] == 2)
					ch[i] = 3;
			}

			// CTRL-T (SIGINFO-like) info. This does not work
			// well when running with -x, since noone checks
			// for charavail at stdin then...
			//
			// if (ch[i] == 20) {
			//	printf("\n");
			//	cpu_show_cycles(machine..., true);
			// }

			console_makeavail(handle, ch[i]);
		}
	}

	return CONSOLE_FIFO_LEN - console_room_left_in_fifo(handle);
}


/*
 *  console_any_input_available():
 *
 *  Return true if keyboard or mouse input is available.
 *
 *  TODO: Mouse buttons are not checked yet.
 */
bool console_any_input_available(struct emul *emul)
{
	bool somethingAvailable = false;

	// Any non-zero mouse delta in the last second counts as input available
	if (console_mouse_dx != 0 || console_mouse_dy != 0) {
		struct timeval tv;
		gettimeofday(&tv, NULL);

		uint64_t t1 = console_mouse_lastupdate.tv_sec * 1000000 +
			console_mouse_lastupdate.tv_usec;
		uint64_t t2 = tv.tv_sec * 1000000 + tv.tv_usec;

		if (t2 - t1 < 1000000)
			somethingAvailable = true;
	}

	if (emul->n_machines == 1) {
		if (console_charavail(emul->machines[0]->main_console_handle))
			somethingAvailable = true;
	}

	return somethingAvailable;
}


/*
 *  console_readchar():
 *
 *  Returns 0..255 if a char was available, -1 otherwise.
 */
int console_readchar(int handle)
{
	int ch;

	if (!console_charavail(handle))
		return -1;

	ch = console_handles[handle].fifo[console_handles[handle].fifo_tail];
	console_handles[handle].fifo_tail ++;
	console_handles[handle].fifo_tail %= CONSOLE_FIFO_LEN;

	return ch;
}


/*
 *  console_putchar():
 *
 *  Prints a char to stdout, and sets the console_stdout_pending flag.
 */
void console_putchar(int handle, int ch)
{
	if (!console_handles[handle].in_use_for_input &&
	    !console_handles[handle].outputonly)
		console_change_inputability(handle, 1);

	if (!console_handles[handle].using_telnet) {
		/*  stdout:  */
		putchar(ch);

		/*  Assume flushes by OS or libc on newlines:  */
		if (ch == '\n')
			console_stdout_pending = 0;
		else
			console_stdout_pending = 1;

		return;
	}

	if (!console_handles[handle].in_use) {
		printf("[ console_putchar(): handle %i not in"
		    " use! ]\n", handle);
		return;
		}

	telnet_output(handle, ch);
}


/*
 *  console_flush():
 *
 *  Flushes stdout, if necessary, and resets console_stdout_pending to zero.
 */
void console_flush(void)
{
	if (console_stdout_pending)
		fflush(stdout);

	console_stdout_pending = 0;
}


/*
 *  console_mouse_coordinate_update():
 *
 *  Updates mouse coordinates. Called by for example an X11 event handler.
 *  dx and dy are relative coordinates, fb_nr is where the mouse movement
 *  took place.
 */
void console_mouse_coordinate_update(int dx, int dy, int fb_nr)
{
	gettimeofday(&console_mouse_lastupdate, NULL);
	console_mouse_dx += dx;
	console_mouse_dy += dy;
	console_mouse_fb_nr = fb_nr;
}


/*
 *  console_mouse_button():
 *
 *  Sets a mouse button to be pressed or released. Called by for example an
 *  X11 event handler.  button is 1 (left), 2 (middle), or 3 (right), and
 *  pressed = 1 for pressed, 0 for not pressed.
 */
void console_mouse_button(int button, int pressed)
{
	int mask = 1 << (3-button);

	if (pressed)
		console_mouse_buttons |= mask;
	else
		console_mouse_buttons &= ~mask;
}


/*
 *  console_getmouse():
 *
 *  Gets current mouse data into the variables pointed to by the arguments.
 *  The mouse delta x and y are then cleared.
 */
void console_getmouse(int *dx, int *dy, int *buttons, int *fb_nr)
{
	*dx = console_mouse_dx;
	*dy = console_mouse_dy;
	*buttons = console_mouse_buttons;
	*fb_nr = console_mouse_fb_nr;

	console_mouse_dx = 0;
	console_mouse_dy = 0;
}


/*
 *  console_new_handle():
 *
 *  Allocates a new console_handle struct, and returns a pointer to it.
 *
 *  For internal use.
 */
static struct console_handle *console_new_handle(const char *name, int *handlep)
{
	struct console_handle *chp;
	int i, n, found_free = -1;

	/*  Reuse an old slot, if possible:  */
	n = n_console_handles;
	for (i=0; i<n; i++)
		if (!console_handles[i].in_use) {
			found_free = i;
			break;
		}

	if (found_free == -1) {
		/*  Let's realloc console_handles[], to make room
		    for the new one:  */
		CHECK_ALLOCATION(console_handles = (struct console_handle *)
		    realloc(console_handles, sizeof(
		    struct console_handle) * (n_console_handles + 1)));
		found_free = n_console_handles;
		n_console_handles ++;
	}

	chp = &console_handles[found_free];
	memset(chp, 0, sizeof(struct console_handle));

	chp->in_use = 1;
	chp->machine_name = strdup("");
	CHECK_ALLOCATION(chp->name = strdup(name));

	*handlep = found_free;
	return chp;
}


/*
 *  console_start_slave():
 *
 *  Creates a console handle for an emulated terminal or serial port. When
 *  slaves are allowed (-x), the handle gets a TCP port of its own, for
 *  telnet. Otherwise it uses the emulator's own terminal.
 *
 *  consolename should be something like "serial 0".
 *
 *  If use_for_input is 1, input is allowed right from the start. (This
 *  can be upgraded later from 0 to 1 using the console_change_inputability()
 *  function.)
 *
 *  If use_for_input is CONSOLE_OUTPUT_ONLY, then this is an output-only stream.
 *
 *  On success, an integer >= 0 is returned. This can then be used as a
 *  'handle' when writing to or reading from an emulated console.
 *
 *  On failure, -1 is returned.
 */
int console_start_slave(struct machine *machine, const char *consolename,
	int use_for_input)
{
	struct console_handle *chp;
	int handle;

	if (machine == NULL || consolename == NULL) {
		printf("console_start_slave(): NULL ptr\n");
		exit(1);
	}

	chp = console_new_handle(consolename, &handle);
	chp->machine = machine;
	chp->in_use_for_input = use_for_input;
	if (use_for_input == CONSOLE_OUTPUT_ONLY) {
		chp->outputonly = 1;
		chp->in_use_for_input = 0;
	}

	if (machine->machine_name != NULL) {
		CHECK_ALLOCATION(chp->machine_name =
		    strdup(machine->machine_name));
	} else {
		CHECK_ALLOCATION(chp->machine_name = strdup(""));
	}

	CHECK_ALLOCATION(chp->name = strdup(consolename));

	if (allow_slaves)
		telnet_listen(handle);

	return handle;
}


/*
 *  console_start_slave_inputonly():
 *
 *  Similar to console_start_slave(), but without a TCP port. This is
 *  useful for devices such as keyboard controllers, that need to have an
 *  input queue, but no terminal of their own.
 *
 *  On success, an integer >= 0 is returned. This can then be used as a
 *  'handle' when writing to or reading from an emulated console.
 *
 *  On failure, -1 is returned.
 */
int console_start_slave_inputonly(struct machine *machine, const char *consolename,
	int use_for_input)
{
	struct console_handle *chp;
	int handle;

	if (machine == NULL || consolename == NULL) {
		printf("console_start_slave(): NULL ptr\n");
		exit(1);
	}

	chp = console_new_handle(consolename, &handle);
	chp->machine = machine;
	chp->inputonly = 1;
	chp->in_use_for_input = use_for_input;

	if (machine->name != NULL) {
		CHECK_ALLOCATION(chp->machine_name = strdup(machine->name));
	} else {
		CHECK_ALLOCATION(chp->machine_name = strdup(""));
	}

	CHECK_ALLOCATION(chp->name = strdup(consolename));

	return handle;
}


/*
 *  console_change_inputability():
 *
 *  Sets whether or not a console handle can be used for input. Return value
 *  is 1 if the change took place, 0 otherwise.
 */
int console_change_inputability(int handle, int inputability)
{
	int old;

	if (handle < 0 || handle >= n_console_handles) {
		fatal("console_change_inputability(): bad handle %i\n",
		    handle);
		exit(1);
	}

	old = console_handles[handle].in_use_for_input;
	console_handles[handle].in_use_for_input = inputability;

	if (inputability != 0) {
		if (console_warn_if_slaves_are_needed(0)) {
			console_handles[handle].in_use_for_input = old;
			if (!console_handles[handle].warning_printed) {
				fatal("%%\n%%  WARNING! Input to console ha"
				    "ndle \"%s\" wasn't enabled,\n%%  because "
				    "it", console_handles[handle].name);
				fatal(" would interfere with other inputs,\n"
				    "%%  and you did not use the -x command "
				    "line option!\n%%\n");
			}
			console_handles[handle].warning_printed = 1;
			return 0;
		}
	}

	return 1;
}


/*
 *  console_init_main():
 *
 *  Puts the host's console into single-character (non-canonical) mode.
 */
void console_init_main(struct emul *emul)
{
	int i, tra;

	if (console_initialized)
		return;

#ifdef _WIN32
	(void) i;
	(void) tra;
	win32_console_init();
#else
	tcgetattr(STDIN_FILENO, &console_oldtermios);
	memcpy(&console_curtermios, &console_oldtermios,
	    sizeof (struct termios));

	console_curtermios.c_lflag &= ~ICANON;
	console_curtermios.c_cc[VTIME] = 0;
	console_curtermios.c_cc[VMIN] = 1;

	console_curtermios.c_lflag &= ~ECHO;

	/*
	 *  Most guest OSes seem to work ok without ~ICRNL, but Linux on
	 *  DECstation requires it to be usable.  Unfortunately, clearing
	 *  out ICRNL makes tracing with '-t ... |more' akward, as you
	 *  might need to use CTRL-J instead of the enter key.  Hence,
	 *  this bit is only cleared if we're not tracing:
	 */
	tra = 0;
	for (i=0; i<emul->n_machines; i++)
		if (emul->machines[i]->show_trace_tree ||
		    emul->machines[i]->instruction_trace ||
		    emul->machines[i]->register_dump)
			tra = 1;
	if (!tra)
		console_curtermios.c_iflag &= ~ICRNL;

	tcsetattr(STDIN_FILENO, TCSANOW, &console_curtermios);
#endif

	console_stdout_pending = 1;
	console_handles[MAIN_CONSOLE].fifo_head = 0;
	console_handles[MAIN_CONSOLE].fifo_tail = 0;

	console_mouse_dx = 0;
	console_mouse_dy = 0;
	console_mouse_buttons = 0;

	telnet_show_ports(emul);

	console_initialized = 1;
}


/*
 *  console_debug_dump():
 *
 *  Dump debug info, if verbose >= 2.
 */
void console_debug_dump(struct machine *machine)
{
	int i, iadd = 1, listed_main = 0;

	if (verbose < 2)
		return;

	debug("console slaves (telnet): %s\n", allow_slaves?
	    "yes" : "no");

	debug("console handles:\n");
	debug_indentation(iadd);

	for (i=0; i<n_console_handles; i++) {
		if (!console_handles[i].in_use)
			continue;
		debug("%i: \"%s\"", i, console_handles[i].name);
		if (console_handles[i].using_telnet)
			debug(" [telnet port %i]", console_handles[i].port);
		if (console_handles[i].inputonly)
			debug(" [inputonly]");
		if (console_handles[i].outputonly)
			debug(" [outputonly]");
		if (i == machine->main_console_handle) {
			debug(" [MAIN CONSOLE]");
			listed_main = 1;
		}
		debug("\n");
	}

	debug_indentation(-iadd);

	if (!listed_main)
		fatal("WARNING! no main console handle?\n");
}


/*
 *  console_allow_slaves():
 *
 *  This function tells the console subsystem whether or not to give each
 *  emulated serial controller a TCP port of its own.
 */
void console_allow_slaves(int allow)
{
	allow_slaves = allow;

	if (allow_slaves) {
		for (int i = 0; i < n_console_handles; i++)
			if (i != MAIN_CONSOLE && console_handles[i].in_use &&
			    !console_handles[i].inputonly &&
			    !console_handles[i].using_telnet)
				telnet_listen(i);
	}
}



/*
 *  console_are_slaves_allowed():
 *
 *  Returns the value of allow_slaves.
 */
int console_are_slaves_allowed(void)
{
	return allow_slaves;
}


/*
 *  console_warn_if_slaves_are_needed():
 *
 *  Prints an error (during startup of the emulator) if slave terminals are needed
 *  (i.e. there is more than one console handle in use which is used for
 *  INPUT), but they are not currently allowed.
 *
 *  This function should be called during startup (with init = 1), and every
 *  time a console handle changes/upgrades its in_use_for_input from 0 to 1.
 *
 *  If init is non-zero, this function doesn't return if there was a warning.
 *
 *  If init is zero, no warning is printed. 1 is returned if there were more
 *  than one input, 0 otherwise.
 */
int console_warn_if_slaves_are_needed(int init)
{
	int i, n = 0;

	if (allow_slaves)
		return 0;

	for (i=MAIN_CONSOLE+1; i<n_console_handles; i++)
		if (console_handles[i].in_use &&
		    console_handles[i].in_use_for_input &&
		    !console_handles[i].using_telnet)
			n ++;

	if (n > 1) {
		if (init) {
			fatal("#\n#  ERROR! More than one console input is "
			    "in use,\n#  but console ports are not enabled.\n"
			    "#\n");
			fatal("#  Use -x to enable telnet ports for them.)\n#\n");
			for (i=MAIN_CONSOLE+1; i<n_console_handles; i++)
				if (console_handles[i].in_use &&
				    console_handles[i].in_use_for_input &&
				    !console_handles[i].using_telnet)
					fatal("#  console handle %i: '%s'\n",
					    i, console_handles[i].name);
			fatal("#\n");
			exit(1);
		}
		return 1;
	}

	return 0;
}


/*
 *  console_init():
 *
 *  This function should be called before any other console_*() function
 *  is used.
 */
void console_init(void)
{
	int handle;
	struct console_handle *chp;

	console_settings = settings_new();

	settings_add(global_settings, "console", 1,
            SETTINGS_TYPE_SUBSETTINGS, 0, console_settings);

	settings_add(console_settings, "allow_slaves", 0,
            SETTINGS_TYPE_INT, SETTINGS_FORMAT_YESNO, (void *)&allow_slaves);

	chp = console_new_handle("MAIN", &handle);
	if (handle != MAIN_CONSOLE) {
		printf("console_init(): fatal error: could not create"
		    " console 0: handle = %i\n", handle);
		exit(1);
	}

	chp->in_use_for_input = 1;
}


/*
 *  console_deinit():
 *
 *  Unregister settings registered by console_init().
 */
void console_deinit(void)
{
	settings_remove(console_settings, "allow_slaves");
	settings_remove(global_settings, "console");
}

