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
 *  The defaults for a new machine of a model that is known to work, as
 *  machine.json text (see machine_config.c).  The configuration window
 *  offers only the models listed here, unless asked to show all of them.
 *  Files that the user has to supply are left empty; "disk.img" is created
 *  in the machine's directory when it is saved.
 */

#include <stddef.h>
#include <stdio.h>

#include "machine.h"
#include "machine_config.h"


static const struct machine_preset presets[] = {
	{
		"decstation", "3max",
		"Runs the real KN02 system ROM: choose its image under "
		"Firmware, with the option ROM images of the SCSI (slot 5) "
		"and Ethernet (slot 6) interfaces, which it needs to boot "
		"from them. With PROM emulation instead (a checkbox there), "
		"no ROM image is needed, and NetBSD, OpenBSD, Ultrix, or "
		"Sprite boot from a disk or CD-ROM image.",
		"{ \"gxemul\": 1, \"name\": \"DECstation 5000/200\","
		"  \"machine\": { \"type\": \"decstation\", \"model\": \"3max\","
		"                 \"memory_mb\": 64 },"
		"  \"boot\": { \"prom_emulation\": false, \"bootarg\": \"-a\","
		"             \"nvram\": \"nvram.bin\", \"skip_failing_tests\": true },"
		"  \"drives\": [ { \"file\": \"disk.img\", \"kind\": \"disk\","
		"                  \"id\": 0 },"
		"                { \"file\": \"\", \"kind\": \"cdrom\", \"id\": 4 } ],"
		"  \"display\": { \"enabled\": true } }"
	},
	{
		"decstation", "3min",
		"Runs the real KN02-BA system ROM: choose its image under "
		"Firmware, and for a graphics console, the PMAG-BA option ROM "
		"image for slot 0 there too. With PROM emulation "
		"instead (a checkbox under Firmware), no ROM image is needed.",
		"{ \"gxemul\": 1, \"name\": \"DECstation 5000/1xx\","
		"  \"machine\": { \"type\": \"decstation\", \"model\": \"3min\","
		"                 \"memory_mb\": 32 },"
		"  \"boot\": { \"prom_emulation\": false, \"bootarg\": \"-a\","
		"             \"nvram\": \"nvram.bin\", \"skip_failing_tests\": true },"
		"  \"drives\": [ { \"file\": \"disk.img\", \"kind\": \"disk\","
		"                  \"id\": 0 },"
		"                { \"file\": \"\", \"kind\": \"cdrom\", \"id\": 4 } ],"
		"  \"display\": { \"enabled\": true } }"
	},
	{
		"decstation", "pmax",
		"The first MIPS DECstation, with a serial console, and a disk "
		"and CD-ROM. Runs the real KN01 system ROM (choose its image "
		"under Firmware), or PROM emulation (a checkbox there).",
		"{ \"gxemul\": 1, \"name\": \"DECstation 3100\","
		"  \"machine\": { \"type\": \"decstation\", \"model\": \"pmax\","
		"                 \"memory_mb\": 24 },"
		"  \"boot\": { \"prom_emulation\": false, \"bootarg\": \"-a\","
		"             \"nvram\": \"nvram.bin\", \"skip_failing_tests\": true },"
		"  \"drives\": [ { \"file\": \"disk.img\", \"kind\": \"disk\","
		"                  \"id\": 0 },"
		"                { \"file\": \"\", \"kind\": \"cdrom\", \"id\": 4 } ] }"
	},
	{
		"sgi", "o2",
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
		"testmips", "",
		"gxemul's simple MIPS machine, for running a program of your "
		"own (add it under Firmware, Files to load).",
		"{ \"gxemul\": 1, \"name\": \"MIPS test machine\","
		"  \"machine\": { \"type\": \"testmips\" } }"
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


/*
 *  DEC's SCSI drives, as DECstations know them.  The capacities are from
 *  DEC's rz(4) (Ultrix 4.4) and rz(7) (Tru64 4.0F) manual pages; Ultrix
 *  has a partition table for each of these disks.  (gxemul's DECstation
 *  disks are RZ58s, rev 2000, of any size, unless given.)
 */
static const struct mc_drive_model dec_drives[] = {
	{ MC_DRIVE_DISK, "RZ22",  "DEC", "RZ22     (C) DEC", "", 102431 },
	{ MC_DRIVE_DISK, "RZ23",  "DEC", "RZ23     (C) DEC", "", 204864 },
	{ MC_DRIVE_DISK, "RZ23L", "DEC", "RZ23L    (C) DEC", "", 237588 },
	{ MC_DRIVE_DISK, "RZ24",  "DEC", "RZ24     (C) DEC", "", 409792 },
	{ MC_DRIVE_DISK, "RZ24L", "DEC", "RZ24L    (C) DEC", "", 479350 },
	{ MC_DRIVE_DISK, "RZ25",  "DEC", "RZ25     (C) DEC", "0900", 832527 },
	{ MC_DRIVE_DISK, "RZ25L", "DEC", "RZ25L    (C) DEC", "", 1046206 },
	{ MC_DRIVE_DISK, "RZ26",  "DEC", "RZ26     (C) DEC", "", 2050860 },
	{ MC_DRIVE_DISK, "RZ26L", "DEC", "RZ26L    (C) DEC", "", 2050860 },
	{ MC_DRIVE_DISK, "RZ28",  "DEC", "RZ28     (C) DEC", "", 4110480 },
	{ MC_DRIVE_DISK, "RZ29B", "DEC", "RZ29B    (C) DEC", "", 8380080 },
	{ MC_DRIVE_DISK, "RZ55",  "DEC", "RZ55     (C) DEC", "", 649040 },
	{ MC_DRIVE_DISK, "RZ56",  "DEC", "RZ56     (C) DEC", "", 1299174 },
	{ MC_DRIVE_DISK, "RZ57",  "DEC", "RZ57     (C) DEC", "", 1954050 },
	{ MC_DRIVE_DISK, "RZ58",  "DEC", "RZ58     (C) DEC", "2000", 2698061 },
	{ MC_DRIVE_DISK, "RZ73",  "DEC", "RZ73     (C) DEC", "", 3907911 },
	{ MC_DRIVE_DISK, "RZ74",  "DEC", "RZ74     (C) DEC", "", 6976375 },
	{ MC_DRIVE_CDROM, "RRD40", "DEC", "RRD40   (C) DEC ", "", 0 },
	{ MC_DRIVE_CDROM, "RRD42", "DEC", "RRD42   (C) DEC ", "4.5d", 0 },
};


/*
 *  machine_drive_models():
 *
 *  Returns the real drives known for a machine type, and how many there
 *  are (0 if none).
 */
int machine_drive_models(const char *type,
	const struct mc_drive_model **listp)
{
	*listp = dec_drives;
	if (machine_config_machine_type(type) == MACHINE_PMAX)
		return (int) (sizeof(dec_drives) / sizeof(dec_drives[0]));
	return 0;
}
