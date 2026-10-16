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
 *
 *
 *  What GXemul needs from POSIX and BSD, on Windows (mingw-w64).  See
 *  src/include/compat_win32.h.
 */

#ifdef _WIN32

#define	WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "compat_win32.h"


/*
 *  random(), srandom():
 *
 *  Windows' rand() only gives 15 bits, and random() should give 31.  This
 *  is a 64-bit xorshift generator, which is good enough for the emulator.
 */
static uint64_t random_state = 0x2545f4914f6cdd1dULL;

long random(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;

	return (long) (random_state >> 33);
}

void srandom(unsigned int seed)
{
	random_state = 0x2545f4914f6cdd1dULL ^ seed;
	if (random_state == 0)
		random_state = 1;
}


/*
 *  mmap(), munmap():
 *
 *  Only anonymous memory, which the emulator uses for RAM and page tables.
 *  VirtualAlloc() memory is zero-filled, and pages only become real when
 *  they are first used, as with mmap().
 */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
	void *p;

	if (!(flags & MAP_ANON) || fd != -1)
		return MAP_FAILED;

	p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	return p != NULL ? p : MAP_FAILED;
}

int munmap(void *addr, size_t len)
{
	return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}


/*
 *  win32_timer_*():
 *
 *  The timer framework (src/core/timer.c) uses these instead of SIGALRM
 *  and setitimer().  The tick function runs on a thread pool thread, so
 *  timer.c takes the lock when it changes its list of timers.
 */
static SRWLOCK timer_lock = SRWLOCK_INIT;
static HANDLE timer_handle = NULL;
static void (*timer_tick_function)(void);

static VOID CALLBACK win32_timer_callback(PVOID param, BOOLEAN fired)
{
	win32_timer_lock();
	timer_tick_function();
	win32_timer_unlock();
}

void win32_timer_start(void (*tick)(void), double hz)
{
	DWORD ms = (DWORD) (1000.0 / hz);

	if (ms < 1)
		ms = 1;

	timer_tick_function = tick;
	CreateTimerQueueTimer(&timer_handle, NULL, win32_timer_callback,
	    NULL, ms, ms, WT_EXECUTEDEFAULT);
}

void win32_timer_stop(void)
{
	if (timer_handle == NULL)
		return;

	/*  Waits for a running tick to finish:  */
	DeleteTimerQueueTimer(NULL, timer_handle, INVALID_HANDLE_VALUE);
	timer_handle = NULL;
}

void win32_timer_lock(void)
{
	AcquireSRWLockExclusive(&timer_lock);
}

void win32_timer_unlock(void)
{
	ReleaseSRWLockExclusive(&timer_lock);
}


/*
 *  win32_console_*(), win32_stdin_*(), win32_set_ctrl_c_handler():
 *
 *  The console is put in raw mode (no line editing or echo), with VT input
 *  (so arrow keys etc. arrive as escape sequences, as in a Unix terminal)
 *  and VT output (for the guest's escape sequences).
 *
 *  select() only works with sockets on Windows, so a thread reads the
 *  console's key presses into a buffer.  Ctrl-C arrives as a character
 *  (as ENABLE_PROCESSED_INPUT is off), and the thread calls the Ctrl-C
 *  handler for it, as SIGINT would on Unix.  Ctrl-Break, and Ctrl-C when
 *  stdin isn't a console, come through SetConsoleCtrlHandler().
 *
 *  When stdin isn't a console (a pipe or a file), it is read as such.
 */
#define	STDIN_BUF_LEN	4096

static HANDLE stdin_handle;
static int stdin_is_console;
static DWORD old_stdin_mode, old_stdout_mode;
static int console_modes_saved;
static void (*ctrl_c_handler)(int);

static SRWLOCK stdin_lock = SRWLOCK_INIT;
static unsigned char stdin_buf[STDIN_BUF_LEN];
static int stdin_head, stdin_tail;
static HANDLE stdin_thread;

static DWORD WINAPI stdin_thread_main(LPVOID param)
{
	INPUT_RECORD r;
	DWORD n;

	for (;;) {
		int repeat;
		unsigned char ch;

		if (!ReadConsoleInputA(stdin_handle, &r, 1, &n)) {
			Sleep(100);
			continue;
		}

		if (n == 0 || r.EventType != KEY_EVENT ||
		    !r.Event.KeyEvent.bKeyDown ||
		    r.Event.KeyEvent.uChar.AsciiChar == 0)
			continue;

		ch = r.Event.KeyEvent.uChar.AsciiChar;
		repeat = r.Event.KeyEvent.wRepeatCount;

		if (ch == 3) {
			if (ctrl_c_handler != NULL)
				ctrl_c_handler(SIGINT);
			continue;
		}

		AcquireSRWLockExclusive(&stdin_lock);
		while (repeat-- > 0 &&
		    (stdin_head + 1) % STDIN_BUF_LEN != stdin_tail) {
			stdin_buf[stdin_head] = ch;
			stdin_head = (stdin_head + 1) % STDIN_BUF_LEN;
		}
		ReleaseSRWLockExclusive(&stdin_lock);
	}

	return 0;
}

void win32_console_init(void)
{
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD mode;

	stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
	stdin_is_console = GetConsoleMode(stdin_handle, &old_stdin_mode);

	if (stdin_is_console) {
		mode = old_stdin_mode;
		mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
		    ENABLE_PROCESSED_INPUT);
		mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
		SetConsoleMode(stdin_handle, mode);

		if (stdin_thread == NULL)
			stdin_thread = CreateThread(NULL, 0,
			    stdin_thread_main, NULL, 0, NULL);
	}

	if (GetConsoleMode(out, &old_stdout_mode))
		SetConsoleMode(out, old_stdout_mode |
		    ENABLE_VIRTUAL_TERMINAL_PROCESSING);

	console_modes_saved = 1;
}

void win32_console_restore(void)
{
	if (!console_modes_saved)
		return;

	if (stdin_is_console)
		SetConsoleMode(stdin_handle, old_stdin_mode);
	SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), old_stdout_mode);
	console_modes_saved = 0;
}

int win32_stdin_avail(void)
{
	DWORD avail;
	int res;

	if (stdin_handle == NULL)
		stdin_handle = GetStdHandle(STD_INPUT_HANDLE);

	if (!stdin_is_console) {
		if (GetFileType(stdin_handle) == FILE_TYPE_PIPE)
			return !PeekNamedPipe(stdin_handle, NULL, 0, NULL,
			    &avail, NULL) || avail > 0;
		return 1;
	}

	AcquireSRWLockShared(&stdin_lock);
	res = stdin_head != stdin_tail;
	ReleaseSRWLockShared(&stdin_lock);
	return res;
}

int win32_stdin_read(unsigned char *buf, int len)
{
	DWORD n;
	int got = 0;

	if (!stdin_is_console) {
		if (!ReadFile(stdin_handle, buf, len, &n, NULL))
			return -1;
		return n;
	}

	AcquireSRWLockExclusive(&stdin_lock);
	while (got < len && stdin_tail != stdin_head) {
		buf[got++] = stdin_buf[stdin_tail];
		stdin_tail = (stdin_tail + 1) % STDIN_BUF_LEN;
	}
	ReleaseSRWLockExclusive(&stdin_lock);

	return got;
}

static BOOL WINAPI win32_ctrl_handler(DWORD type)
{
	if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
		return FALSE;

	ctrl_c_handler(SIGINT);
	return TRUE;
}

void win32_set_ctrl_c_handler(void (*handler)(int))
{
	if (ctrl_c_handler == NULL)
		SetConsoleCtrlHandler(win32_ctrl_handler, TRUE);
	ctrl_c_handler = handler;
}


/*
 *  win32_sockets_init():
 */
void win32_sockets_init(void)
{
	WSADATA wsadata;

	if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
		fprintf(stderr, "Could not start Winsock\n");
		exit(1);
	}
}

#endif	/*  _WIN32  */
