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
#include <windows.h>

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

#endif	/*  _WIN32  */
