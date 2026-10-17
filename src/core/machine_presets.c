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
 *  The presets offered for a new machine, as machine.json text (see
 *  machine_config.c).  Files that the user has to supply are left empty;
 *  "disk.img" is created in the machine's directory when it is saved.
 */

#include <stddef.h>

#include "machine_config.h"


static const struct machine_preset presets[] = {
	{
		"DECstation 5000/200, PROM emulation",
		"Boots NetBSD, OpenBSD, Ultrix, or Sprite from a disk or "
		"CD-ROM image, without a ROM image (gxemul emulates the PROM). "
		"A PMAG-BA framebuffer in slot 0.",
		"{ \"gxemul\": 1, \"name\": \"DECstation 5000/200\","
		"  \"machine\": { \"type\": \"decstation\", \"model\": \"3max\","
		"                 \"memory_mb\": 64 },"
		"  \"boot\": { \"prom_emulation\": true, \"bootarg\": \"-a\" },"
		"  \"drives\": [ { \"file\": \"disk.img\", \"kind\": \"disk\","
		"                  \"id\": 0 },"
		"                { \"file\": \"\", \"kind\": \"cdrom\", \"id\": 4 } ],"
		"  \"display\": { \"enabled\": true } }"
	},
	{
		"DECstation 5000/1xx (5000/133), real ROM",
		"Runs the real KN02-BA system ROM (choose its image under Boot). "
		"For a graphics console, add the PMAG-BA option ROM image in "
		"slot 0 (under Expansion).",
		"{ \"gxemul\": 1, \"name\": \"DECstation 5000/133\","
		"  \"machine\": { \"type\": \"decstation\", \"model\": \"3min\","
		"                 \"memory_mb\": 32 },"
		"  \"boot\": { \"prom_emulation\": false, \"rom\": \"\" },"
		"  \"drives\": [ { \"file\": \"disk.img\", \"kind\": \"disk\","
		"                  \"id\": 0 },"
		"                { \"file\": \"\", \"kind\": \"cdrom\", \"id\": 4 } ],"
		"  \"display\": { \"enabled\": true } }"
	},
	{
		"DECstation 5000/200, real ROM",
		"Runs the real KN02 system ROM (choose its image under Boot). "
		"To boot from a disk, add the PMAZ-AA (SCSI) option ROM image "
		"in slot 5, and for Ethernet the PMAD-AA one in slot 6 (under "
		"Expansion).",
		"{ \"gxemul\": 1, \"name\": \"DECstation 5000/200 (ROM)\","
		"  \"machine\": { \"type\": \"decstation\", \"model\": \"3max\","
		"                 \"memory_mb\": 64 },"
		"  \"boot\": { \"prom_emulation\": false, \"rom\": \"\" },"
		"  \"drives\": [ { \"file\": \"disk.img\", \"kind\": \"disk\","
		"                  \"id\": 0 },"
		"                { \"file\": \"\", \"kind\": \"cdrom\", \"id\": 4 } ],"
		"  \"display\": { \"enabled\": true } }"
	},
	{
		"DECstation 3100, PROM emulation",
		"The first MIPS DECstation, with PROM emulation, a serial "
		"console, and a disk and CD-ROM.",
		"{ \"gxemul\": 1, \"name\": \"DECstation 3100\","
		"  \"machine\": { \"type\": \"decstation\", \"model\": \"pmax\","
		"                 \"memory_mb\": 24 },"
		"  \"boot\": { \"prom_emulation\": true, \"bootarg\": \"-a\" },"
		"  \"drives\": [ { \"file\": \"disk.img\", \"kind\": \"disk\","
		"                  \"id\": 0 },"
		"                { \"file\": \"\", \"kind\": \"cdrom\", \"id\": 4 } ] }"
	},
	{
		"SGI O2 (IP32), PROM emulation",
		"An SGI O2, for NetBSD/sgimips or OpenBSD/sgi.",
		"{ \"gxemul\": 1, \"name\": \"SGI O2\","
		"  \"machine\": { \"type\": \"sgi\", \"model\": \"o2\","
		"                 \"memory_mb\": 128 },"
		"  \"drives\": [ { \"file\": \"disk.img\", \"kind\": \"disk\","
		"                  \"id\": 0 },"
		"                { \"file\": \"\", \"kind\": \"cdrom\", \"id\": 6 } ],"
		"  \"display\": { \"enabled\": true } }"
	},
	{
		"MIPS test machine",
		"gxemul's simple MIPS machine, for running a program of your "
		"own (add it under Boot, Files to load).",
		"{ \"gxemul\": 1, \"name\": \"MIPS test machine\","
		"  \"machine\": { \"type\": \"testmips\" } }"
	},
	{
		"Empty configuration",
		"Choose everything yourself.",
		"{ \"gxemul\": 1, \"name\": \"New machine\" }"
	},
};


/*
 *  machine_presets():
 *
 *  Returns the list of presets, and how many there are.
 */
int machine_presets(const struct machine_preset **listp)
{
	*listp = presets;
	return (int) (sizeof(presets) / sizeof(presets[0]));
}
