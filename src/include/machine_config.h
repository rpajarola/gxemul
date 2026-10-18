#ifndef	MACHINE_CONFIG_H
#define	MACHINE_CONFIG_H

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
 *  A machine configuration: everything needed to set up one emulated
 *  machine, as plain data.  It is read from and written to a JSON file
 *  (machine.json, usually in a directory of its own, with the machine's
 *  disk and ROM images), filled in from command line options, or edited
 *  in the configuration window.  machine_config_create_emul() then sets up
 *  the emulation.  See machine_config.c for the file format.
 */

#include <stdbool.h>
#include <stdint.h>

struct emul;
struct machine;


/*  Drive kinds and buses (for mc_drive):  */
#define	MC_DRIVE_DISK		0
#define	MC_DRIVE_CDROM		1
#define	MC_DRIVE_TAPE		2
#define	MC_DRIVE_FLOPPY		3

#define	MC_BUS_DEFAULT		0
#define	MC_BUS_SCSI		1
#define	MC_BUS_IDE		2

#define	MC_MAX_SLOTS		8	/*  = MACHINE_MAX_TC_SLOTS  */

struct mc_drive {
	char		*file;		/*  "" for a drive without a medium  */
	int		kind;		/*  MC_DRIVE_*  */
	int		bus;		/*  MC_BUS_*  */
	int		id;		/*  SCSI ID or IDE unit, -1 = next free  */
	bool		readonly;
	bool		overlay;	/*  writes go to a temporary overlay  */
	bool		boot;		/*  boot from this drive  */
};

struct machine_config {
	char		*path;		/*  the file it was read from, or NULL  */
	char		*dir;		/*  relative paths are relative to this  */

	char		*name;

	/*  Hardware:  */
	char		*type;		/*  e.g. "dec" (see gxemul -H)  */
	char		*model;		/*  e.g. "3min", or "" if none  */
	char		*cpu;		/*  "" = the machine's default  */
	int		ncpus;		/*  0 = default (1)  */
	int		memory_mb;	/*  0 = default  */
	uint64_t	emulated_hz;	/*  0 = default  */
	char		*byte_order;	/*  "", "big" or "little"  */
	bool		random_memory;
	bool		random_bootstrap_cpu;
	int		serial_nr;	/*  0 = automatic  */

	/*  Boot:  */
	bool		prom_emulation;
	char		*rom;		/*  system ROM image, "" = none  */
	int		n_load;
	char		**load;		/*  files to load, "[addr:]file"  */
	char		*bootname;	/*  "" = default  */
	char		*bootarg;	/*  NULL = default  */
	bool		force_netboot;
	bool		start_paused;

	/*  Drives:  */
	int		n_drives;
	struct mc_drive	*drives;

	/*  Display and console:  */
	bool		display;
	int		scale;		/*  1 = 1:1, 2 = half size, -2 = double  */
	int		gfx_cards;	/*  -1 = default  */
	bool		serial_telnet;	/*  serial ports on telnet ports  */

	/*  Expansion slots (e.g. TURBOchannel option ROM images):  */
	char		*slot_rom[MC_MAX_SLOTS];	/*  NULL = built-in  */

	/*  Network:  */
	bool		net;
	char		*net_ipv4;
	int		net_ipv4len;
	int		net_local_port;	/*  0 = no UDP tunnel  */
	int		n_net_remotes;
	char		**net_remotes;
	char		*net_tapdev;	/*  "" = none  */
	bool		net_l2tp;
	uint32_t	net_l2tp_session;
	char		*net_l2tp_cookie;
	bool		net_l2tp_counter;

	/*  Extra devices, as for the debugger's device add:  */
	int		n_devices;
	char		**devices;
};


/*  machine_config.c:  */
struct machine_config *machine_config_new(void);
struct machine_config *machine_config_copy(const struct machine_config *);
void machine_config_free(struct machine_config *);
struct machine_config *machine_config_load(const char *path, char **errp);
struct machine_config *machine_config_parse(const char *text,
	const char *path, char **errp);
bool machine_config_save(struct machine_config *, const char *path,
	char **errp);
bool machine_config_is_config_file(const char *path);
char *machine_config_resolve(const struct machine_config *, const char *);
char *machine_config_absolute(const char *path);
char *machine_config_check(const struct machine_config *);
char *machine_config_check_ex(const struct machine_config *,
	bool missing_disks_ok);
char *machine_config_summary(const struct machine_config *);
bool machine_config_create_disk(const char *path, int size_mb, char **errp);
void machine_config_add_drive(struct machine_config *, const char *file,
	int kind, int id);
void machine_config_remove_drive(struct machine_config *, int i);
void machine_config_add_string(int *np, char ***listp, const char *s);
void machine_config_remove_string(int *np, char ***listp, int i);
void machine_config_set(char **field, const char *value);
bool machine_config_apply(struct machine_config *, struct emul *,
	struct machine *);
struct emul *machine_config_create_emul(struct machine_config *);

/*  Machine descriptions (what the configuration window offers):  */
uint64_t machine_config_rom_address(const char *type);
int machine_config_n_slots(const char *type, const char *model);

/*  machine_presets.c:  */
struct machine_preset {
	const char	*type;		/*  as in machine.json  */
	const char	*model;		/*  "" for a type without models  */
	const char	*description;
	const char	*json;
};
int machine_presets(const struct machine_preset **listp);

/*  appdirs.c:  */
const char *appdirs_state_dir(void);
const char *appdirs_machines_dir(void);
int appdirs_recent(char ***listp);
void appdirs_add_recent(const char *path);
void appdirs_remove_recent(const char *path);

#endif	/*  MACHINE_CONFIG_H  */
