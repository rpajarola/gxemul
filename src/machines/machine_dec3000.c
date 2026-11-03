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
 *  COMMENT: DEC 3000 AXP (TURBOchannel Alpha) workstations
 *
 *  The machine runs its real console firmware: give the ROM as the file to
 *  load, either a firmware update file (Mx00_Vy_z.EXE, .SYS or .BTP, from
 *  the Alpha firmware CDs) or images extracted from one (see
 *  experiments/dec3000_extract_rom.py). The serial ROM is emulated: memory
 *  is configured, the ROMs are copied into memory, and the CPU starts in
 *  PALmode in the system ROM, as the serial ROM leaves it.
 *
 *  Implemented: 3000/400 ("Sandpiper"). See dev_dec3000.c for the devices,
 *  and doc/dec3000-plan.md.
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


MACHINE_SETUP(dec3000)
{
	char tmp[200];

	switch (machine->machine_subtype) {
	case MACHINE_DEC3000_400:
		machine->machine_name = strdup("DEC 3000/400 (Sandpiper)");
		if (machine->emulated_hz == 0)
			machine->emulated_hz = 133333333;
		break;
	default:fatal("Unimplemented DEC 3000 model %i\n",
		    machine->machine_subtype);
		exit(1);
	}

	if (machine->physical_ram_in_mb > 128) {
		fatal("The DEC 3000/400 can have at most 128 MB of memory.\n");
		exit(1);
	}
	if (machine->physical_ram_in_mb % 32 != 0) {
		fatal("DEC 3000 memory comes in banks of 32 MB.\n");
		exit(1);
	}

	/*  The firmware's own PALcode runs on the CPU:  */
	cpu->cd.alpha.ev4_palcode = 1;

	snprintf(tmp, sizeof(tmp), "%s.cpu[%i]", machine->path,
	    machine->bootstrap_cpu);
	machine->md.dec3000 = dev_dec3000_init(machine, machine->memory,
	    (uint64_t) machine->physical_ram_in_mb << 20, tmp);
}


/*
 *  dec3000_load_rom():
 *
 *  Called by the file loader. Loads the ROM images from a firmware update
 *  file, or from a single extracted image. Returns 1 if the file had DEC
 *  3000 ROM images in it.
 */
int dec3000_load_rom(struct machine *machine, const char *filename,
	uint64_t *entrypointp)
{
	struct dec3000_data *d = machine->md.dec3000;
	unsigned char *buf;
	const char *want = "SAND";
	FILE *f;
	long size, i;
	int n_sys = 0, n_io = 0;

	if (d == NULL)
		return 0;

	f = fopen(filename, "rb");
	if (f == NULL)
		return 0;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	CHECK_ALLOCATION(buf = (unsigned char *) malloc(size + 1));
	if (fread(buf, 1, size, f) != (size_t) size) {
		fclose(f);
		free(buf);
		return 0;
	}
	fclose(f);

	/*
	 *  Each image starts with the DEC 3000 ROM header (SPM 13.3), as a
	 *  byte copy: width, stride 4, size, slot size, 55 00 aa ff, then
	 *  revision, vendor and module name. Take the first system ROM and
	 *  the first I/O ROM image for this model; an image ends where the
	 *  next one starts.
	 */
	for (i = 4; i + 0x20 <= size; i++) {
		long start = i - 4, end, j;
		int sys;

		if (buf[i] != 0x55 || buf[i+1] != 0 || buf[i+2] != 0xaa ||
		    buf[i+3] != 0xff || buf[start + 1] != 4 ||
		    memcmp(buf + start + 0x10, "DEC     ", 8) != 0)
			continue;
		if (memcmp(buf + start + 0x18, want, 4) != 0)
			continue;
		sys = memcmp(buf + start + 0x1c, "_SYS", 4) == 0;
		if (!sys && memcmp(buf + start + 0x1c, "_IO ", 4) != 0)
			continue;

		end = size;
		for (j = i + 4; j + 4 <= size; j++)
			if (buf[j] == 0x55 && buf[j+1] == 0 &&
			    buf[j+2] == 0xaa && buf[j+3] == 0xff &&
			    j - 4 >= start + 0x100 &&
			    memcmp(buf + j + 0xc, "DEC     ", 8) == 0) {
				end = j - 4;
				break;
			}

		if ((sys && n_sys == 0) || (!sys && n_io == 0)) {
			debug("%s ROM %.8s %.8s at offset 0x%lx\n",
			    sys? "system" : "I/O", buf + start + 8,
			    buf + start + 0x18, start);
			dev_dec3000_set_rom(d, sys, buf + start, end - start);
		}
		if (sys)
			n_sys ++;
		else
			n_io ++;
	}

	free(buf);

	if (n_sys == 0 && n_io == 0)
		return 0;

	if (n_sys > 1)
		debug("(%i system ROM images; using the first)\n", n_sys);

	*entrypointp = dev_dec3000_srom(d, machine->cpus[machine->bootstrap_cpu]);
	return 1;
}


MACHINE_DEFAULT_CPU(dec3000)
{
	machine->cpu_name = strdup("21064");
}


MACHINE_DEFAULT_RAM(dec3000)
{
	machine->physical_ram_in_mb = 64;
}


MACHINE_REGISTER(dec3000)
{
	MR_DEFAULT(dec3000, "DEC 3000 AXP", MACHINE_DEC3000);

	machine_entry_add_alias(me, "dec3000");

	machine_entry_add_subtype(me, "DEC 3000/400", MACHINE_DEC3000_400,
	    "400", "3000/400", NULL);

	me->set_default_ram = machine_default_ram_dec3000;
}
