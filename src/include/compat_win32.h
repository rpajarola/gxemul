#ifndef	COMPAT_WIN32_H
#define	COMPAT_WIN32_H

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
 *  What GXemul needs from POSIX and BSD, on Windows (mingw-w64).  The
 *  functions are in src/core/compat_win32.c.
 *
 *  This header doesn't include <windows.h>, as its macros (min, max, IN,
 *  OUT, ERROR, interface, ...) clash with names used in the emulator.
 */

#ifdef _WIN32

#include <io.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/*  u_char, u_short, u_int, u_long (the same as Winsock's):  */
#include <_bsd_types.h>

typedef unsigned int	uint;
typedef char		*caddr_t;

#define	fsync(fd)	_commit(fd)
#define	timegm(tm)	_mkgmtime(tm)

long random(void);
void srandom(unsigned int seed);

/*  Anonymous memory only (MAP_ANON | MAP_PRIVATE, fd -1):  */
#define	PROT_READ	1
#define	PROT_WRITE	2
#define	MAP_PRIVATE	2
#define	MAP_ANON	0x20
#define	MAP_FAILED	((void *) -1)
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t len);

/*  A periodic timer instead of SIGALRM; tick() runs on another thread,
    holding the lock:  */
void win32_timer_start(void (*tick)(void), double hz);
void win32_timer_stop(void);
void win32_timer_lock(void);
void win32_timer_unlock(void);

/*  The host's console, instead of termios and select() on stdin:  */
void win32_console_init(void);
void win32_console_restore(void);
int win32_stdin_avail(void);
int win32_stdin_read(unsigned char *buf, int len);

/*  Ctrl-C and Ctrl-Break call handler, on another thread (instead of
    SIGINT):  */
void win32_set_ctrl_c_handler(void (*handler)(int));

/*  Starts Winsock:  */
void win32_sockets_init(void);

/*  The host's (first) DNS server, as a string, or NULL:  */
const char *win32_nameserver(void);

/*  Opens a file chooser in a thread of its own, returning false if that
    isn't possible.  win32_choose_file_done() returns true when it has
    closed, with the path chosen (to be freed), or NULL:  */
bool win32_choose_file(const char *title, const char *start_dir);
bool win32_choose_file_done(char **path);

#endif	/*  _WIN32  */

#endif	/*  COMPAT_WIN32_H  */
