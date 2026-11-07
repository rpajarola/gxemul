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
 *  COMMENT: DECpc AXP 150 / DEC 2000 Model 300 ("Jensen") workstation
 *
 *  Digital's first entry-level PC-architecture Alpha system, using the
 *  DECchip 21064 (EV4) CPU with an EISA bus and standard PC peripherals.
 *  The machine runs genuine SRM or ARC console firmware extracted from
 *  Alpha firmware CDs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "device.h"
#include "devices.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"


MACHINE_SETUP(jensen)
{
	char tmp[200];

	switch (machine->machine_subtype) {
	case MACHINE_JENSEN_300:
		machine->machine_name = strdup("DECpc AXP 150 / DEC 2000 Model 300 (Jensen)");
		if (machine->emulated_hz == 0)
			machine->emulated_hz = 150000000;
		break;
	default:
		machine->machine_name = strdup("DECpc AXP 150 (Jensen)");
		if (machine->emulated_hz == 0)
			machine->emulated_hz = 150000000;
		break;
	}

	if (machine->physical_ram_in_mb > 128) {
		fatal("The DECpc AXP 150 can have at most 128 MB of memory.\n");
		exit(1);
	}

	/*  The firmware's own PALcode runs on the CPU:  */
	cpu->cd.alpha.ev4_palcode = 1;

	snprintf(tmp, sizeof(tmp), "%s.cpu[%i]", machine->path,
	    machine->bootstrap_cpu);
	machine->md.jensen = dev_jensen_init(machine, machine->memory,
	    (uint64_t) machine->physical_ram_in_mb << 20, tmp);
}


/*
 *  jensen_load_rom():
 *
 *  Loads ROM images from a firmware update file (e.g. DEC2000_V1_3.EXE,
 *  .SYS, .BTP) or from a raw extracted 1 MB Flash ROM image.
 *  Returns 1 on success, 0 if not a Jensen ROM.
 */
int jensen_load_rom(struct machine *machine, const char *filename,
	uint64_t *entrypointp)
{
	struct jensen_data *d = machine->md.jensen;
	unsigned char *buf;
	FILE *f;
	long size, i;
	long rom_start = -1, rom_len = 0;

	if (d == NULL)
		return 0;

	f = fopen(filename, "rb");
	if (f == NULL)
		return 0;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size < 0x100) {
		fclose(f);
		return 0;
	}

	CHECK_ALLOCATION(buf = (unsigned char *) malloc(size + 1));
	if (fread(buf, 1, size, f) != (size_t) size) {
		fclose(f);
		free(buf);
		return 0;
	}
	fclose(f);

	/*
	 *  Scan for DEC ROM header (magic 55 00 aa ff, "DEC     ", "JENS_SYS"):
	 */
	for (i = 4; i + 0x20 <= size; i++) {
		long start = i - 4;
		if (buf[i] != 0x55 || buf[i+1] != 0 || buf[i+2] != 0xaa ||
		    buf[i+3] != 0xff)
			continue;
		if (memcmp(buf + start + 0x10, "DEC     ", 8) != 0)
			continue;
		if (memcmp(buf + start + 0x18, "JENS_SYS", 8) != 0)
			continue;

		rom_start = start;
		rom_len = size - start;
		if (rom_len > 1048576)
			rom_len = 1048576;
		break;
	}

	/*
	 *  If not found via magic, check if this is a raw ROM image
	 *  with header at offset 0, or raw 1 MB image:
	 */
	if (rom_start < 0) {
		if (size >= 0x20 && memcmp(buf + 0x18, "JENS_SYS", 8) == 0) {
			rom_start = 0;
			rom_len = size > 1048576 ? 1048576 : size;
		} else if (size == 1048576) {
			rom_start = 0;
			rom_len = 1048576;
		}
	}

	if (rom_start < 0) {
		free(buf);
		return 0;
	}

	debug("Jensen ROM at offset 0x%lx (size %ld bytes)\n", rom_start, rom_len);
	dev_jensen_set_rom(d, buf + rom_start, rom_len);
	free(buf);

	*entrypointp = dev_jensen_srom(d, machine->cpus[machine->bootstrap_cpu]);
	return 1;
}


MACHINE_DEFAULT_CPU(jensen)
{
	machine->cpu_name = strdup("21064");
}


MACHINE_DEFAULT_RAM(jensen)
{
	machine->physical_ram_in_mb = 64;
}


MACHINE_REGISTER(jensen)
{
	MR_DEFAULT(jensen, "DECpc AXP 150 / DEC 2000 Model 300", MACHINE_JENSEN);

	machine_entry_add_alias(me, "jensen");
	machine_entry_add_alias(me, "dec2000");
	machine_entry_add_alias(me, "axp150");

	machine_entry_add_subtype(me, "DECpc AXP 150", MACHINE_JENSEN_300,
	    "300", "axp150", "dec2000", "jensen", "", NULL);

	me->set_default_ram = machine_default_ram_jensen;
}
