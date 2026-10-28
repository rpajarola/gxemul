#ifndef	CONTROL_H
#define	CONTROL_H

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
 *  The control socket, for programs to drive the emulator (see
 *  doc/control.html and src/debugger/control.c).
 */

#include "misc.h"

struct cpu;
struct emul;
struct machine;

/*  Set while a watch (or a log subscription) needs these hooks; checked
    before calling them, as they are on fast paths:  */
extern bool control_watch_exceptions;
extern bool control_watch_devices;
extern int control_log_level;		/*  -1: no log events  */

void control_init(struct emul *emul, const char *spec);
bool control_exception(struct cpu *cpu, const char *name, int code,
	uint64_t from_pc, uint64_t vaddr);
void control_device_access(struct cpu *cpu, const char *devname,
	uint64_t offset, const unsigned char *data, size_t len, int writeflag);
void control_log(struct cpu *cpu, const char *subsystem, const char *name,
	int verbosity, const char *msg);
void control_poll(bool force);
void control_note_stop(struct machine *m, const char *reason);
void control_shutdown(void);
int control_client_main(int argc, char *argv[]);

#endif	/*  CONTROL_H  */
