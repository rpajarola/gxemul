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
 *  Machine configurations (see machine_config.h), and their JSON files.
 *
 *  A machine.json file looks like this; every key is optional, and has the
 *  default shown, except for machine.type:
 *
 *	{
 *	  "gxemul": 1,
 *	  "name": "DECstation 5000/133 with Ultrix",
 *	  "machine": { "type": "dec", "model": "3min", "cpu": "",
 *	               "cpus": 0, "memory_mb": 0, "emulated_hz": 0,
 *	               "byte_order": "", "random_memory": false,
 *	               "random_bootstrap_cpu": false, "serial_nr": 0 },
 *	  "boot": { "prom_emulation": true, "rom": "", "load": [],
 *	            "bootname": "", "bootarg": null, "netboot": false,
 *	            "start_paused": false, "nvram": "",
 *	            "skip_failing_tests": false },
 *	  "drives": [ { "file": "disk.img", "kind": "disk", "bus": "default",
 *	                "id": -1, "readonly": false, "overlay": false,
 *	                "vendor": "", "product": "", "revision": "",
 *	                "boot": false } ],
 *	  "display": { "enabled": false, "scale": 0, "cards": -1 },
 *	  "serial": { "telnet": false },
 *	  "slots": { "0": "PMAG-BA.bin" },
 *	  "network": { "enabled": true, "ipv4": "10.0.0.0", "ipv4len": 8,
 *	               "local_port": 0, "remotes": [], "tapdev": "",
 *	               "l2tp": { "enabled": false, "session": 1,
 *	                         "cookie": "", "counter": true } },
 *	  "devices": []
 *	}
 *
 *  File names are relative to the directory the file is in, unless they
 *  are absolute.  "rom" is the system ROM image, loaded where the machine
 *  has its ROM (only known for some machines, see machine_config_rom_
 *  address()); "load" are other files to load, which may be prefixed with
 *  an address, as on the command line (e.g. "0xbfc00000:rom.bin").  On a
 *  machine with a known ROM address, "rom" is used only without PROM
 *  emulation, and "load", "bootname", "bootarg", and "netboot" only with it.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "console.h"
#include "diskimage.h"
#include "emul.h"
#include "machine.h"
#include "machine_config.h"
#include "misc.h"
#include "net.h"
#include "thirdparty/cJSON.h"


extern struct machine_entry *first_machine_entry;

#define	CONFIG_VERSION		1


static char *xstrdup(const char *s)
{
	char *p;
	CHECK_ALLOCATION(p = strdup(s == NULL ? "" : s));
	return p;
}


/*  Returns a string from printf-style arguments (for error messages):  */
static char *xprintf(const char *fmt, ...)
{
	va_list ap;
	char buf[2000];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return xstrdup(buf);
}


void machine_config_set(char **field, const char *value)
{
	char *old = *field;
	*field = value == NULL ? NULL : xstrdup(value);
	free(old);
}


void machine_config_add_string(int *np, char ***listp, const char *s)
{
	CHECK_ALLOCATION(*listp = (char **) realloc(*listp,
	    sizeof(char *) * (*np + 1)));
	(*listp)[(*np)++] = xstrdup(s);
}


void machine_config_remove_string(int *np, char ***listp, int i)
{
	if (i < 0 || i >= *np)
		return;
	free((*listp)[i]);
	memmove(&(*listp)[i], &(*listp)[i + 1], sizeof(char *) * (*np - i - 1));
	(*np) --;
}


void machine_config_add_drive(struct machine_config *c, const char *file,
	int kind, int id)
{
	struct mc_drive *d;

	CHECK_ALLOCATION(c->drives = (struct mc_drive *) realloc(c->drives,
	    sizeof(struct mc_drive) * (c->n_drives + 1)));
	d = &c->drives[c->n_drives ++];
	memset(d, 0, sizeof(*d));
	d->file = xstrdup(file);
	d->kind = kind;
	d->bus = MC_BUS_DEFAULT;
	d->id = id;
	d->vendor = xstrdup("");
	d->product = xstrdup("");
	d->revision = xstrdup("");
}

static void free_drive(struct mc_drive *d)
{
	free(d->file);
	free(d->vendor);
	free(d->product);
	free(d->revision);
}


void machine_config_remove_drive(struct machine_config *c, int i)
{
	if (i < 0 || i >= c->n_drives)
		return;
	free_drive(&c->drives[i]);
	memmove(&c->drives[i], &c->drives[i + 1],
	    sizeof(struct mc_drive) * (c->n_drives - i - 1));
	c->n_drives --;
}


struct machine_config *machine_config_new(void)
{
	struct machine_config *c;

	CHECK_ALLOCATION(c = (struct machine_config *) malloc(sizeof(*c)));
	memset(c, 0, sizeof(*c));

	c->name = xstrdup("");
	c->type = xstrdup("");
	c->model = xstrdup("");
	c->cpu = xstrdup("");
	c->byte_order = xstrdup("");
	c->prom_emulation = true;
	c->rom = xstrdup("");
	c->bootname = xstrdup("");
	c->nvram = xstrdup("");
	c->scale = 0;
	c->gfx_cards = -1;
	c->net = true;
	c->net_ipv4 = xstrdup(NET_DEFAULT_IPV4_MASK);
	c->net_ipv4len = NET_DEFAULT_IPV4_LEN;
	c->net_tapdev = xstrdup("");
	c->net_l2tp_session = 1;
	c->net_l2tp_cookie = xstrdup("");
	c->net_l2tp_counter = true;

	return c;
}


void machine_config_free(struct machine_config *c)
{
	int i;

	if (c == NULL)
		return;

	free(c->path); free(c->dir); free(c->name);
	free(c->type); free(c->model); free(c->cpu); free(c->byte_order);
	free(c->rom); free(c->bootname); free(c->bootarg); free(c->nvram);
	for (i = 0; i < c->n_load; i++)
		free(c->load[i]);
	free(c->load);
	for (i = 0; i < c->n_drives; i++)
		free_drive(&c->drives[i]);
	free(c->drives);
	for (i = 0; i < MC_MAX_SLOTS; i++)
		free(c->slot_rom[i]);
	free(c->net_ipv4);
	for (i = 0; i < c->n_net_remotes; i++)
		free(c->net_remotes[i]);
	free(c->net_remotes);
	free(c->net_tapdev); free(c->net_l2tp_cookie);
	for (i = 0; i < c->n_devices; i++)
		free(c->devices[i]);
	free(c->devices);
	free(c);
}


struct machine_config *machine_config_copy(const struct machine_config *src)
{
	struct machine_config *c;
	int i;

	CHECK_ALLOCATION(c = (struct machine_config *) malloc(sizeof(*c)));
	*c = *src;

	c->path = src->path ? xstrdup(src->path) : NULL;
	c->dir = src->dir ? xstrdup(src->dir) : NULL;
	c->name = xstrdup(src->name);
	c->type = xstrdup(src->type);
	c->model = xstrdup(src->model);
	c->cpu = xstrdup(src->cpu);
	c->byte_order = xstrdup(src->byte_order);
	c->rom = xstrdup(src->rom);
	c->bootname = xstrdup(src->bootname);
	c->bootarg = src->bootarg ? xstrdup(src->bootarg) : NULL;
	c->nvram = xstrdup(src->nvram);

	c->n_load = 0; c->load = NULL;
	for (i = 0; i < src->n_load; i++)
		machine_config_add_string(&c->n_load, &c->load, src->load[i]);

	c->n_drives = 0; c->drives = NULL;
	for (i = 0; i < src->n_drives; i++) {
		machine_config_add_drive(c, src->drives[i].file,
		    src->drives[i].kind, src->drives[i].id);
		free_drive(&c->drives[i]);
		c->drives[i] = src->drives[i];
		c->drives[i].file = xstrdup(src->drives[i].file);
		c->drives[i].vendor = xstrdup(src->drives[i].vendor);
		c->drives[i].product = xstrdup(src->drives[i].product);
		c->drives[i].revision = xstrdup(src->drives[i].revision);
	}

	for (i = 0; i < MC_MAX_SLOTS; i++)
		c->slot_rom[i] = src->slot_rom[i] ? xstrdup(src->slot_rom[i])
		    : NULL;

	c->net_ipv4 = xstrdup(src->net_ipv4);
	c->n_net_remotes = 0; c->net_remotes = NULL;
	for (i = 0; i < src->n_net_remotes; i++)
		machine_config_add_string(&c->n_net_remotes, &c->net_remotes,
		    src->net_remotes[i]);
	c->net_tapdev = xstrdup(src->net_tapdev);
	c->net_l2tp_cookie = xstrdup(src->net_l2tp_cookie);

	c->n_devices = 0; c->devices = NULL;
	for (i = 0; i < src->n_devices; i++)
		machine_config_add_string(&c->n_devices, &c->devices,
		    src->devices[i]);

	return c;
}


/*****************************************************************************
 *
 *  File names
 */

static bool is_absolute(const char *p)
{
#ifdef _WIN32
	if (isalpha((unsigned char) p[0]) && p[1] == ':')
		return true;
	if (p[0] == '\\')
		return true;
#endif
	return p[0] == '/';
}


static char *dirname_of(const char *path)
{
	const char *slash = strrchr(path, '/');
#ifdef _WIN32
	const char *bslash = strrchr(path, '\\');
	if (bslash != NULL && (slash == NULL || bslash > slash))
		slash = bslash;
#endif
	if (slash == NULL)
		return xstrdup(".");
	if (slash == path)
		return xstrdup("/");
	return xprintf("%.*s", (int)(slash - path), path);
}


/*  Renames a file, replacing the file that has the new name, if any:  */
static bool replace_file(const char *from, const char *to)
{
#ifdef _WIN32
	/*  (Windows' rename() doesn't replace files.)  */
	return win32_replace_file(from, to);
#else
	return rename(from, to) == 0;
#endif
}


/*
 *  machine_config_absolute():
 *
 *  Returns a file name as an absolute one (relative to the current
 *  directory).  The caller frees it.
 */
char *machine_config_absolute(const char *p)
{
	char cwd[4096];

	if (is_absolute(p) || getcwd(cwd, sizeof(cwd)) == NULL)
		return xstrdup(p);
	return xprintf("%s/%s", cwd, p);
}


/*
 *  machine_config_resolve():
 *
 *  Returns a file name from the configuration as a name that can be
 *  opened (relative names are relative to the configuration's directory).
 *  The caller frees it.
 */
char *machine_config_resolve(const struct machine_config *c, const char *p)
{
	if (p == NULL || p[0] == '\0' || is_absolute(p) || c->dir == NULL)
		return xstrdup(p);
	return xprintf("%s/%s", c->dir, p);
}


static bool file_exists(const char *path);


/*
 *  machine_config_clean_path():
 *
 *  Returns a file name as an absolute one (relative to the current
 *  directory), tidied up: "." and ".." resolved, no repeated slashes, and
 *  on Windows, slashes instead of backslashes.  The caller frees it.
 */
char *machine_config_clean_path(const char *path)
{
	char *abs = machine_config_absolute(path), *out;
	const char *p;
	size_t root = 0, n;

#ifdef _WIN32
	for (out = abs; *out; out++)
		if (*out == '\\')
			*out = '/';
	if (isalpha((unsigned char) abs[0]) && abs[1] == ':')
		root = abs[2] == '/' ? 3 : 2;
	else
#endif
	if (abs[0] == '/')
		root = 1;

	CHECK_ALLOCATION(out = (char *) malloc(strlen(abs) + 2));
	memcpy(out, abs, root);
	n = root;
	for (p = abs + root; *p; ) {
		const char *e = strchr(p, '/');
		size_t l = e != NULL ? (size_t) (e - p) : strlen(p);

		if (l == 0 || (l == 1 && p[0] == '.'))
			;
		else if (l == 2 && p[0] == '.' && p[1] == '.') {
			while (n > root && out[n - 1] != '/')
				n --;
			if (n > root)
				n --;
		} else {
			if (n > root)
				out[n++] = '/';
			memcpy(out + n, p, l);
			n += l;
		}
		p += l;
		if (*p == '/')
			p ++;
	}
	out[n] = '\0';
	free(abs);
	return out;
}


/*  The part of a clean path after a clean directory, if it is in it:  */
static const char *inside(const char *dir, const char *path)
{
	size_t n = strlen(dir);

	if (n > 0 && dir[n - 1] == '/')
		n --;
#ifdef _WIN32
	if (strncasecmp(path, dir, n) == 0 && path[n] == '/')
#else
	if (strncmp(path, dir, n) == 0 && path[n] == '/')
#endif
		return path + n + 1;
	return NULL;
}


/*
 *  A file name as it is kept in a configuration in dir: relative to dir if
 *  it is in it (without ".."), or for a ROM image, relative to the ROM
 *  directory it is in, or else absolute.  abs is a clean path.
 */
static char *shorten(const char *abs, const char *dir, bool rom)
{
	const char *rest = NULL;
	char *d, **dirs, *r;
	int n, i;

	if (dir != NULL) {
		d = machine_config_clean_path(dir);
		rest = inside(d, abs);
		r = rest != NULL ? xstrdup(rest) : NULL;
		free(d);
		if (r != NULL)
			return r;
	}
	if (rom) {
		r = NULL;
		n = appdirs_rom_dirs(&dirs);
		for (i = 0; i < n; i++) {
			d = machine_config_clean_path(dirs[i]);
			if (r == NULL && (rest = inside(d, abs)) != NULL)
				r = xstrdup(rest);
			free(d);
			free(dirs[i]);
		}
		free(dirs);
		if (r != NULL)
			return r;
	}
	return xstrdup(abs);
}


/*
 *  machine_config_resolve_rom():
 *
 *  Like machine_config_resolve(), for a ROM image: a relative name which
 *  isn't in the configuration's directory is looked for in the ROM
 *  directories (see appdirs_rom_dirs()).  The caller frees it.
 */
char *machine_config_resolve_rom(const struct machine_config *c,
	const char *p)
{
	char *f, **dirs, *found = NULL;
	int n, i;

	f = machine_config_resolve(c, p);
	if (p == NULL || p[0] == '\0' || is_absolute(p) || file_exists(f))
		return f;

	n = appdirs_rom_dirs(&dirs);
	for (i = 0; i < n; i++) {
		char *g = xprintf("%s/%s", dirs[i], p);
		if (found == NULL && file_exists(g))
			found = g;
		else
			free(g);
		free(dirs[i]);
	}
	free(dirs);
	if (found != NULL) {
		free(f);
		return found;
	}
	return f;
}


/*
 *  machine_config_normalize():
 *
 *  Returns a file name (e.g. one chosen in a file chooser) as the
 *  configuration keeps it: see shorten().  Until the configuration has a
 *  directory, relative names are kept as they are.  The caller frees it.
 */
char *machine_config_normalize(const struct machine_config *c,
	const char *p, bool rom)
{
	char *f, *abs, *r;

	if (p == NULL || p[0] == '\0')
		return xstrdup("");
	if (!is_absolute(p) && c->dir == NULL)
		return xstrdup(p);
	f = rom ? machine_config_resolve_rom(c, p) :
	    machine_config_resolve(c, p);
	abs = machine_config_clean_path(f);
	r = shorten(abs, c->dir, rom);
	free(f);
	free(abs);
	return r;
}


/*  For a file name with an "address:" prefix, the length of the prefix:  */
static size_t load_prefix_len(const char *spec)
{
	const char *p = spec;
	size_t len = 0;

	while (isdigit((unsigned char) p[0])) {
		const char *colon = strchr(p, ':');
		const char *q;
		if (colon == NULL)
			break;
		for (q = p; q < colon; q++)
			if (!isxdigit((unsigned char) *q) && *q != 'x' &&
			    *q != 'X')
				return len;
		len += (colon - p) + 1;
		p = colon + 1;
	}
	return len;
}


static char *resolve_load(const struct machine_config *c, const char *spec)
{
	size_t n = load_prefix_len(spec);
	char *f = machine_config_resolve(c, spec + n);
	char *r = xprintf("%.*s%s", (int) n, spec, f);
	free(f);
	return r;
}


/*
 *  Rewrites the configuration's file names for a (new) directory, as
 *  shorten() keeps them.  Relative names in a configuration without a
 *  directory yet stay relative (to the new one).
 */
static void rebase_one(const struct machine_config *c, const char *newdir,
	char **field, bool is_load, bool rom)
{
	size_t n;
	char *f, *abs, *rel;

	if (*field == NULL || (*field)[0] == '\0')
		return;
	n = is_load ? load_prefix_len(*field) : 0;
	if (c->dir == NULL && !is_absolute(*field + n))
		return;
	f = rom ? machine_config_resolve_rom(c, *field + n) :
	    machine_config_resolve(c, *field + n);
	abs = machine_config_clean_path(f);
	rel = shorten(abs, newdir, rom);
	free(f);
	free(abs);
	f = xprintf("%.*s%s", (int) n, *field, rel);
	free(rel);
	free(*field);
	*field = f;
}

static void rebase_paths(struct machine_config *c, const char *newdir)
{
	int i;

	rebase_one(c, newdir, &c->rom, false, true);
	for (i = 0; i < c->n_load; i++)
		rebase_one(c, newdir, &c->load[i], true, false);
	for (i = 0; i < c->n_drives; i++)
		rebase_one(c, newdir, &c->drives[i].file, false, false);
	for (i = 0; i < MC_MAX_SLOTS; i++)
		rebase_one(c, newdir, &c->slot_rom[i], false, true);
	rebase_one(c, newdir, &c->nvram, false, false);

	machine_config_set(&c->dir, newdir);
}


/*****************************************************************************
 *
 *  JSON
 */

static const char *kind_names[] = { "disk", "cdrom", "tape", "floppy" };
static const char *bus_names[] = { "default", "scsi", "ide" };


/*  Lookup helpers, which record the first type error in *errp:  */
static void get_str(cJSON *o, const char *key, char **field, char **errp)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	if (v == NULL || cJSON_IsNull(v))
		return;
	if (!cJSON_IsString(v)) {
		if (*errp == NULL)
			*errp = xprintf("\"%s\" should be a string", key);
		return;
	}
	machine_config_set(field, v->valuestring);
}

static void get_int(cJSON *o, const char *key, int *field, char **errp)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	if (v == NULL)
		return;
	if (!cJSON_IsNumber(v)) {
		if (*errp == NULL)
			*errp = xprintf("\"%s\" should be a number", key);
		return;
	}
	*field = v->valueint;
}

static void get_bool(cJSON *o, const char *key, bool *field, char **errp)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	if (v == NULL)
		return;
	if (!cJSON_IsBool(v)) {
		if (*errp == NULL)
			*errp = xprintf("\"%s\" should be true or false", key);
		return;
	}
	*field = cJSON_IsTrue(v);
}

static void get_list(cJSON *o, const char *key, int *np, char ***listp,
	char **errp)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key), *e;
	if (v == NULL)
		return;
	if (!cJSON_IsArray(v)) {
		if (*errp == NULL)
			*errp = xprintf("\"%s\" should be a list", key);
		return;
	}
	cJSON_ArrayForEach(e, v) {
		if (!cJSON_IsString(e)) {
			if (*errp == NULL)
				*errp = xprintf("\"%s\" should be a list of "
				    "strings", key);
			return;
		}
		machine_config_add_string(np, listp, e->valuestring);
	}
}

static int name_index(const char *s, const char **names, int n)
{
	int i;
	for (i = 0; i < n; i++)
		if (strcasecmp(s, names[i]) == 0)
			return i;
	return -1;
}

static cJSON *section(cJSON *root, const char *key, char **errp)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
	if (v != NULL && !cJSON_IsObject(v)) {
		if (*errp == NULL)
			*errp = xprintf("\"%s\" should be an object", key);
		return NULL;
	}
	return v;
}


static char *read_file(const char *path, char **errp)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long len;

	if (f == NULL) {
		*errp = xprintf("%s: %s", path, strerror(errno));
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len < 0 || len > 16 * 1048576) {
		fclose(f);
		*errp = xprintf("%s: not a configuration file", path);
		return NULL;
	}
	CHECK_ALLOCATION(buf = (char *) malloc(len + 1));
	if ((long) fread(buf, 1, len, f) != len) {
		fclose(f);
		free(buf);
		*errp = xprintf("%s: read error", path);
		return NULL;
	}
	buf[len] = '\0';
	fclose(f);
	return buf;
}


/*
 *  machine_config_is_config_file():
 *
 *  Returns true if a file looks like a machine configuration (a JSON object
 *  with a "gxemul" key), so that "gxemul machine.json" can tell it from a
 *  program to load.
 */
bool machine_config_is_config_file(const char *path)
{
	char *err = NULL, *text;
	FILE *f = fopen(path, "rb");
	cJSON *root;
	bool r;
	int ch;

	/*  A quick look first, so that a kernel isn't read for nothing:  */
	if (f == NULL)
		return false;
	do
		ch = fgetc(f);
	while (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
	fclose(f);
	if (ch != '{')
		return false;

	text = read_file(path, &err);
	free(err);
	if (text == NULL)
		return false;
	root = cJSON_Parse(text);
	free(text);
	r = cJSON_IsObject(root) &&
	    cJSON_GetObjectItemCaseSensitive(root, "gxemul") != NULL;
	cJSON_Delete(root);
	return r;
}


/*
 *  machine_config_load():
 *
 *  Reads a machine configuration file.  Returns NULL on error, with an
 *  error message (which the caller frees) in *errp.
 */
struct machine_config *machine_config_load(const char *path, char **errp)
{
	struct machine_config *c;
	char *text, *abs;

	*errp = NULL;
	text = read_file(path, errp);
	if (text == NULL)
		return NULL;

	abs = machine_config_clean_path(path);
	c = machine_config_parse(text, abs, errp);
	free(text);
	free(abs);
	return c;
}


/*
 *  machine_config_parse():
 *
 *  Makes a machine configuration from JSON text.  path is the file it came
 *  from (relative file names are relative to its directory), or NULL (then
 *  they are relative to the current directory).  Returns NULL on error,
 *  with an error message in *errp.
 */
struct machine_config *machine_config_parse(const char *text,
	const char *path, char **errp)
{
	char *err = NULL, *s = NULL;
	cJSON *root, *o, *v, *e;
	struct machine_config *c;
	int version = 0;

	if (path == NULL)
		path = "(built-in)";
	*errp = NULL;
	root = cJSON_Parse(text);
	if (root == NULL) {
		const char *where = cJSON_GetErrorPtr();
		int line = 1;
		const char *p;
		for (p = text; where != NULL && p < where && *p; p++)
			if (*p == '\n')
				line ++;
		*errp = xprintf("%s, line %i: not valid JSON", path, line);
		return NULL;
	}

	if (!cJSON_IsObject(root) ||
	    cJSON_GetObjectItemCaseSensitive(root, "gxemul") == NULL) {
		*errp = xprintf("%s: not a GXemul machine configuration (no "
		    "\"gxemul\" key)", path);
		cJSON_Delete(root);
		return NULL;
	}

	c = machine_config_new();
	if (path[0] == '/' || is_absolute(path)) {
		c->path = xstrdup(path);
		c->dir = dirname_of(c->path);
	}

	get_int(root, "gxemul", &version, &err);
	if (err == NULL && version > CONFIG_VERSION)
		err = xprintf("written by a newer GXemul (version %i)",
		    version);
	get_str(root, "name", &c->name, &err);

	if ((o = section(root, "machine", &err)) != NULL) {
		int hz = 0;
		get_str(o, "type", &c->type, &err);
		get_str(o, "model", &c->model, &err);
		get_str(o, "cpu", &c->cpu, &err);
		get_int(o, "cpus", &c->ncpus, &err);
		get_int(o, "memory_mb", &c->memory_mb, &err);
		get_int(o, "emulated_hz", &hz, &err);
		c->emulated_hz = hz;
		get_str(o, "byte_order", &c->byte_order, &err);
		get_bool(o, "random_memory", &c->random_memory, &err);
		get_bool(o, "random_bootstrap_cpu",
		    &c->random_bootstrap_cpu, &err);
		get_int(o, "serial_nr", &c->serial_nr, &err);
	}

	if ((o = section(root, "boot", &err)) != NULL) {
		get_bool(o, "prom_emulation", &c->prom_emulation, &err);
		get_str(o, "rom", &c->rom, &err);
		get_list(o, "load", &c->n_load, &c->load, &err);
		get_str(o, "bootname", &c->bootname, &err);
		v = cJSON_GetObjectItemCaseSensitive(o, "bootarg");
		if (v != NULL && !cJSON_IsNull(v))
			get_str(o, "bootarg", &c->bootarg, &err);
		get_bool(o, "netboot", &c->force_netboot, &err);
		get_bool(o, "start_paused", &c->start_paused, &err);
		get_str(o, "nvram", &c->nvram, &err);
		get_bool(o, "skip_failing_tests", &c->skip_failing_tests,
		    &err);
	}

	v = cJSON_GetObjectItemCaseSensitive(root, "drives");
	if (v != NULL && !cJSON_IsArray(v) && err == NULL)
		err = xstrdup("\"drives\" should be a list");
	else if (v != NULL) {
		cJSON_ArrayForEach(e, v) {
			struct mc_drive *d;
			if (!cJSON_IsObject(e)) {
				if (err == NULL)
					err = xstrdup("\"drives\" should be "
					    "a list of objects");
				break;
			}
			machine_config_add_drive(c, "", MC_DRIVE_DISK, -1);
			d = &c->drives[c->n_drives - 1];
			get_str(e, "file", &d->file, &err);
			s = xstrdup("disk");
			get_str(e, "kind", &s, &err);
			if ((d->kind = name_index(s, kind_names, 4)) < 0) {
				if (err == NULL)
					err = xprintf("unknown drive kind "
					    "\"%s\"", s);
				d->kind = MC_DRIVE_DISK;
			}
			machine_config_set(&s, "default");
			get_str(e, "bus", &s, &err);
			if ((d->bus = name_index(s, bus_names, 3)) < 0) {
				if (err == NULL)
					err = xprintf("unknown bus \"%s\"", s);
				d->bus = MC_BUS_DEFAULT;
			}
			free(s);
			s = NULL;
			get_int(e, "id", &d->id, &err);
			get_bool(e, "readonly", &d->readonly, &err);
			get_bool(e, "overlay", &d->overlay, &err);
			get_bool(e, "boot", &d->boot, &err);
			get_str(e, "vendor", &d->vendor, &err);
			get_str(e, "product", &d->product, &err);
			get_str(e, "revision", &d->revision, &err);
		}
	}

	if ((o = section(root, "display", &err)) != NULL) {
		get_bool(o, "enabled", &c->display, &err);
		get_int(o, "scale", &c->scale, &err);
		get_int(o, "cards", &c->gfx_cards, &err);

		/*  (Earlier, 1 = actual size, 2 = half, -2 = double.)  */
		if (c->scale == 1)
			c->scale = 100;
		else if (c->scale == 2)
			c->scale = 50;
		else if (c->scale == -2)
			c->scale = 200;
	}

	if ((o = section(root, "serial", &err)) != NULL)
		get_bool(o, "telnet", &c->serial_telnet, &err);

	if ((o = section(root, "slots", &err)) != NULL) {
		cJSON_ArrayForEach(e, o) {
			char *end;
			long slot = strtol(e->string, &end, 10);
			if (*end != '\0' || slot < 0 || slot >= MC_MAX_SLOTS ||
			    !cJSON_IsString(e)) {
				if (err == NULL)
					err = xprintf("bad slot \"%s\" (slots "
					    "are 0..%i, with a file name)",
					    e->string, MC_MAX_SLOTS - 1);
				continue;
			}
			machine_config_set(&c->slot_rom[slot], e->valuestring);
		}
	}

	if ((o = section(root, "network", &err)) != NULL) {
		int session = c->net_l2tp_session;
		get_bool(o, "enabled", &c->net, &err);
		get_str(o, "ipv4", &c->net_ipv4, &err);
		get_int(o, "ipv4len", &c->net_ipv4len, &err);
		get_int(o, "local_port", &c->net_local_port, &err);
		get_list(o, "remotes", &c->n_net_remotes, &c->net_remotes,
		    &err);
		get_str(o, "tapdev", &c->net_tapdev, &err);
		if ((v = section(o, "l2tp", &err)) != NULL) {
			get_bool(v, "enabled", &c->net_l2tp, &err);
			get_int(v, "session", &session, &err);
			get_str(v, "cookie", &c->net_l2tp_cookie, &err);
			get_bool(v, "counter", &c->net_l2tp_counter, &err);
		}
		c->net_l2tp_session = session;
	}

	get_list(root, "devices", &c->n_devices, &c->devices, &err);

	cJSON_Delete(root);

	if (err != NULL) {
		*errp = xprintf("%s: %s", path, err);
		free(err);
		machine_config_free(c);
		return NULL;
	}

	return c;
}


static void add_list(cJSON *o, const char *key, int n, char **list)
{
	cJSON *a = cJSON_AddArrayToObject(o, key);
	int i;
	for (i = 0; i < n; i++)
		cJSON_AddItemToArray(a, cJSON_CreateString(list[i]));
}


/*
 *  machine_config_save():
 *
 *  Writes a machine configuration file.  If it is written to another
 *  directory than the one it came from, its file names are rewritten so
 *  that they still refer to the same files.  Returns false on error, with
 *  an error message in *errp.
 */
bool machine_config_save(struct machine_config *c, const char *path,
	char **errp)
{
	char *abs = machine_config_clean_path(path), *dir = dirname_of(abs);
	char *text, *tmp;
	cJSON *root, *o, *a, *d, *l2;
	FILE *f;
	int i;

	*errp = NULL;

	/*  (Also when the directory is the same, so that the file names are
	    kept as shorten() keeps them.)  */
	rebase_paths(c, dir);

	root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "gxemul", CONFIG_VERSION);
	cJSON_AddStringToObject(root, "name", c->name);

	o = cJSON_AddObjectToObject(root, "machine");
	cJSON_AddStringToObject(o, "type", c->type);
	cJSON_AddStringToObject(o, "model", c->model);
	cJSON_AddStringToObject(o, "cpu", c->cpu);
	cJSON_AddNumberToObject(o, "cpus", c->ncpus);
	cJSON_AddNumberToObject(o, "memory_mb", c->memory_mb);
	cJSON_AddNumberToObject(o, "emulated_hz", (double) c->emulated_hz);
	cJSON_AddStringToObject(o, "byte_order", c->byte_order);
	cJSON_AddBoolToObject(o, "random_memory", c->random_memory);
	cJSON_AddBoolToObject(o, "random_bootstrap_cpu",
	    c->random_bootstrap_cpu);
	cJSON_AddNumberToObject(o, "serial_nr", c->serial_nr);

	o = cJSON_AddObjectToObject(root, "boot");
	cJSON_AddBoolToObject(o, "prom_emulation", c->prom_emulation);
	cJSON_AddStringToObject(o, "rom", c->rom);
	add_list(o, "load", c->n_load, c->load);
	cJSON_AddStringToObject(o, "bootname", c->bootname);
	if (c->bootarg != NULL)
		cJSON_AddStringToObject(o, "bootarg", c->bootarg);
	else
		cJSON_AddNullToObject(o, "bootarg");
	cJSON_AddBoolToObject(o, "netboot", c->force_netboot);
	cJSON_AddBoolToObject(o, "start_paused", c->start_paused);
	cJSON_AddStringToObject(o, "nvram", c->nvram);
	cJSON_AddBoolToObject(o, "skip_failing_tests", c->skip_failing_tests);

	a = cJSON_AddArrayToObject(root, "drives");
	for (i = 0; i < c->n_drives; i++) {
		struct mc_drive *dr = &c->drives[i];
		d = cJSON_CreateObject();
		cJSON_AddStringToObject(d, "file", dr->file);
		cJSON_AddStringToObject(d, "kind", kind_names[dr->kind]);
		cJSON_AddStringToObject(d, "bus", bus_names[dr->bus]);
		cJSON_AddNumberToObject(d, "id", dr->id);
		cJSON_AddBoolToObject(d, "readonly", dr->readonly);
		cJSON_AddBoolToObject(d, "overlay", dr->overlay);
		cJSON_AddBoolToObject(d, "boot", dr->boot);
		cJSON_AddStringToObject(d, "vendor", dr->vendor);
		cJSON_AddStringToObject(d, "product", dr->product);
		cJSON_AddStringToObject(d, "revision", dr->revision);
		cJSON_AddItemToArray(a, d);
	}

	o = cJSON_AddObjectToObject(root, "display");
	cJSON_AddBoolToObject(o, "enabled", c->display);
	cJSON_AddNumberToObject(o, "scale", c->scale);
	cJSON_AddNumberToObject(o, "cards", c->gfx_cards);

	o = cJSON_AddObjectToObject(root, "serial");
	cJSON_AddBoolToObject(o, "telnet", c->serial_telnet);

	o = cJSON_AddObjectToObject(root, "slots");
	for (i = 0; i < MC_MAX_SLOTS; i++)
		if (c->slot_rom[i] != NULL && c->slot_rom[i][0]) {
			char key[8];
			snprintf(key, sizeof(key), "%i", i);
			cJSON_AddStringToObject(o, key, c->slot_rom[i]);
		}

	o = cJSON_AddObjectToObject(root, "network");
	cJSON_AddBoolToObject(o, "enabled", c->net);
	cJSON_AddStringToObject(o, "ipv4", c->net_ipv4);
	cJSON_AddNumberToObject(o, "ipv4len", c->net_ipv4len);
	cJSON_AddNumberToObject(o, "local_port", c->net_local_port);
	add_list(o, "remotes", c->n_net_remotes, c->net_remotes);
	cJSON_AddStringToObject(o, "tapdev", c->net_tapdev);
	l2 = cJSON_AddObjectToObject(o, "l2tp");
	cJSON_AddBoolToObject(l2, "enabled", c->net_l2tp);
	cJSON_AddNumberToObject(l2, "session", c->net_l2tp_session);
	cJSON_AddStringToObject(l2, "cookie", c->net_l2tp_cookie);
	cJSON_AddBoolToObject(l2, "counter", c->net_l2tp_counter);

	add_list(root, "devices", c->n_devices, c->devices);

	text = cJSON_Print(root);
	cJSON_Delete(root);

	/*  Write to a temporary file first, so that a failed write doesn't
	    destroy the old file:  */
	tmp = xprintf("%s.tmp", abs);
	f = fopen(tmp, "w");
	if (f == NULL || fputs(text, f) < 0 || fputs("\n", f) < 0 ||
	    fclose(f) != 0 || !replace_file(tmp, abs)) {
		*errp = xprintf("%s: %s", abs, strerror(errno));
		remove(tmp);
		free(tmp); free(text); free(abs); free(dir);
		return false;
	}

	free(tmp);
	free(text);
	machine_config_set(&c->path, abs);
	free(abs);
	free(dir);
	return true;
}


/*****************************************************************************
 *
 *  Machine descriptions
 */

/*
 *  machine_config_machine_type():
 *
 *  The machine type (MACHINE_*) of a type name, or MACHINE_NONE.
 */
int machine_config_machine_type(const char *type)
{
	struct machine_entry *me;
	int i;

	for (me = first_machine_entry; me != NULL; me = me->next)
		for (i = 0; i < me->n_aliases; i++)
			if (strcasecmp(me->aliases[i], type) == 0)
				return me->machine_type;
	return MACHINE_NONE;
}


/*
 *  machine_config_has_ide():
 *
 *  False for machines that have only SCSI (where the drives' bus can't be
 *  chosen).
 */
bool machine_config_has_ide(const char *type)
{
	return diskimage_default_type(machine_config_machine_type(type)) !=
	    DISKIMAGE_SCSI;
}


/*
 *  machine_config_rom_address():
 *
 *  Returns where a machine type has its system ROM, or 0 if it isn't known
 *  (then a ROM image can only be given with an address, in "load").
 */
uint64_t machine_config_rom_address(const char *type)
{
	switch (machine_config_machine_type(type)) {
	case MACHINE_PMAX:	return 0xbfc00000ULL;
	}
	return 0;
}


/*
 *  machine_config_uses_rom():
 *
 *  True if the machine runs a system ROM image: without PROM emulation, on
 *  a machine type that has one.  It then boots from that alone; the files
 *  to load and the boot settings are for PROM emulation.
 */
bool machine_config_uses_rom(const struct machine_config *c)
{
	return !c->prom_emulation && machine_config_rom_address(c->type) != 0;
}


/*  Finds a machine type and model, without exiting on errors:  */
static bool lookup_machine(const char *type, const char *model,
	int *tp, int *stp, char **errp)
{
	struct machine_entry *me;
	int i, j, k;

	for (me = first_machine_entry; me != NULL; me = me->next)
		for (i = 0; i < me->n_aliases; i++) {
			if (strcasecmp(me->aliases[i], type) != 0)
				continue;
			*tp = me->machine_type;
			*stp = 0;
			if (me->n_subtypes == 0)
				return true;
			for (j = 0; j < me->n_subtypes; j++)
				for (k = 0; k < me->subtype[j]->n_aliases; k++)
					if (strcasecmp(model, me->subtype[j]->
					    aliases[k]) == 0) {
						*stp = me->subtype[j]->
						    machine_subtype;
						return true;
					}
			*errp = model[0] ? xprintf("unknown model \"%s\" for "
			    "machine type \"%s\"", model, type) : xprintf(
			    "machine type \"%s\" needs a model", type);
			return false;
		}

	*errp = type[0] ? xprintf("unknown machine type \"%s\"", type) :
	    xstrdup("no machine type");
	return false;
}


/*
 *  machine_config_max_gfx_cards():
 *
 *  How many graphics cards a machine can have (with the display on): up to
 *  3 on the TURBOchannel DECstations, otherwise 1 (or none).
 */
int machine_config_max_gfx_cards(const struct machine_config *c)
{
	int type, subtype;
	char *err = NULL;

	if (!lookup_machine(c->type, c->model, &type, &subtype, &err)) {
		free(err);
		return 1;
	}
	if (type == MACHINE_PMAX && (subtype == MACHINE_DEC_3MAX_5000 ||
	    subtype == MACHINE_DEC_3MIN_5000 ||
	    subtype == MACHINE_DEC_3MAXPLUS_5000 ||
	    subtype == MACHINE_DEC_MAXINE_5000))
		return 3;
	return 1;
}


/*
 *  machine_config_slots():
 *
 *  Lists the TURBOchannel slots that have a card in them, which can be
 *  given an option ROM image (for a real system ROM to use the card), as
 *  machine_pmax.c sets them up: graphics cards (one, or "cards", with the
 *  framebuffer on), and on the 5000/200, the SCSI and Ethernet interfaces.
 *  Returns how many there are (at most MC_MAX_SLOTS), or -1 if the machine
 *  has no TURBOchannel.
 */
int machine_config_slots(const struct machine_config *c,
	struct mc_slot *slots)
{
	int type, subtype, cards, n = 0, i;
	char *err = NULL;

	if (!lookup_machine(c->type, c->model, &type, &subtype, &err)) {
		free(err);
		return -1;
	}
	if (type != MACHINE_PMAX)
		return -1;

	cards = !c->display ? 0 : c->gfx_cards < 0 ? 1 : c->gfx_cards;
	switch (subtype) {
	case MACHINE_DEC_3MAX_5000:
	case MACHINE_DEC_3MIN_5000:
	case MACHINE_DEC_3MAXPLUS_5000:
		for (i = 0; i < cards && i < 3; i++) {
			slots[n].slot = i;
			slots[n++].device = "PMAG-BA framebuffer";
		}
		if (subtype == MACHINE_DEC_3MAX_5000) {
			slots[n].slot = 5;
			slots[n++].device = "PMAZ-AA SCSI";
			slots[n].slot = 6;
			slots[n++].device = "PMAD-AA Ethernet";
		}
		break;
	case MACHINE_DEC_MAXINE_5000:
		/*  The first card is the built-in one, in slot 2:  */
		for (i = 1; i < cards && i < 3; i++) {
			slots[n].slot = i - 1;
			slots[n++].device = "PMAG-BA framebuffer";
		}
		slots[n].slot = 2;
		slots[n++].device = "PMAG-DV framebuffer (built in)";
		break;
	default:
		return -1;
	}
	return n;
}


static bool file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}


/*
 *  machine_config_check(), machine_config_check_ex():
 *
 *  Returns NULL if the configuration can be started, or a description of
 *  what's wrong with it (which the caller frees).  With missing_disks_ok,
 *  disk images that don't exist yet are not a problem (they are created
 *  when a new machine is saved).
 */
char *machine_config_check(const struct machine_config *c)
{
	return machine_config_check_ex(c, false);
}

char *machine_config_check_ex(const struct machine_config *c,
	bool missing_disks_ok)
{
	char *err = NULL, *f;
	int t, st, i, j;

	if (!lookup_machine(c->type, c->model, &t, &st, &err))
		return err;

	if (c->memory_mb < 0)
		return xstrdup("the memory size can't be negative");
	if (c->scale != 0 && (c->scale < 10 || c->scale > 800))
		return xstrdup("the window scale must be a percentage from 10 "
		    "to 800, or 0 (fit the screen)");
	if (c->gfx_cards > machine_config_max_gfx_cards(c))
		return xprintf("this machine can have at most %i graphics "
		    "card%s", machine_config_max_gfx_cards(c),
		    machine_config_max_gfx_cards(c) == 1 ? "" : "s");
	if (c->ncpus < 0)
		return xstrdup("the number of CPUs can't be negative");

	if (machine_config_uses_rom(c) && c->rom[0]) {
		f = machine_config_resolve_rom(c, c->rom);
		if (!file_exists(f)) {
			err = xprintf("ROM image %s not found", f);
			free(f);
			return err;
		}
		free(f);
	}

	for (i = 0; !machine_config_uses_rom(c) && i < c->n_load; i++) {
		f = machine_config_resolve(c, c->load[i] +
		    load_prefix_len(c->load[i]));
		if (!file_exists(f)) {
			err = xprintf("%s not found", f);
			free(f);
			return err;
		}
		free(f);
	}

	for (i = 0; i < c->n_drives; i++) {
		const struct mc_drive *d = &c->drives[i];
		if (d->bus == MC_BUS_IDE && !machine_config_has_ide(c->type))
			return xprintf("drive %i is on the IDE bus, but this "
			    "machine has only SCSI", i + 1);
		if (strlen(d->vendor) > 8 || strlen(d->product) > 16 ||
		    strlen(d->revision) > 4)
			return xprintf("drive %i: the vendor, product, and "
			    "revision can be at most 8, 16, and 4 characters",
			    i + 1);
		if (d->file[0] == '\0') {
			if (d->kind != MC_DRIVE_CDROM &&
			    d->kind != MC_DRIVE_TAPE)
				return xprintf("drive %i has no image file",
				    i + 1);
		} else {
			f = machine_config_resolve(c, d->file);
			if (!file_exists(f) && !(missing_disks_ok &&
			    d->kind == MC_DRIVE_DISK)) {
				err = xprintf("drive image %s not found", f);
				free(f);
				return err;
			}
			free(f);
		}
		if (d->id > 7)
			return xprintf("drive %i: IDs are 0..7", i + 1);
		for (j = 0; j < i; j++)
			if (d->id >= 0 && c->drives[j].id == d->id &&
			    c->drives[j].bus == d->bus &&
			    (d->kind == MC_DRIVE_FLOPPY) ==
			    (c->drives[j].kind == MC_DRIVE_FLOPPY))
				return xprintf("drives %i and %i both have "
				    "ID %i", j + 1, i + 1, d->id);
	}

	for (i = 0; machine_config_uses_rom(c) && i < MC_MAX_SLOTS; i++)
		if (c->slot_rom[i] != NULL && c->slot_rom[i][0]) {
			struct mc_slot slots[MC_MAX_SLOTS];
			int n = machine_config_slots(c, slots), k;
			bool found = false;
			for (k = 0; k < n; k++)
				if (slots[k].slot == i)
					found = true;
			if (!found)
				return xprintf("slot %i has an option ROM, but "
				    "no card (a framebuffer is there only with "
				    "the display on, and enough cards)", i);
			f = machine_config_resolve_rom(c, c->slot_rom[i]);
			if (!file_exists(f)) {
				err = xprintf("slot %i: option ROM %s not "
				    "found", i, f);
				free(f);
				return err;
			}
			free(f);
		}

	if (machine_config_uses_rom(c) && c->rom[0] == '\0')
		return xstrdup("without PROM emulation, a system ROM image "
		    "is needed (under Firmware)");

	if (!machine_config_uses_rom(c) && c->n_load == 0 &&
	    c->n_drives == 0)
		return xstrdup("nothing to boot: add a drive, or a file to "
		    "load");

	if (!c->prom_emulation && !machine_config_uses_rom(c) &&
	    c->n_load == 0)
		return xstrdup("without PROM emulation, a file to load is "
		    "needed");

	return NULL;
}


/*
 *  machine_config_summary():
 *
 *  A few lines describing a configuration, e.g. for the list of machines.
 *  The caller frees it.
 */
char *machine_config_summary(const struct machine_config *c)
{
	char buf[4000], *p;
	int i;

	snprintf(buf, sizeof(buf), "%s %s", c->type, c->model);
	if (c->memory_mb > 0)
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
		    ", %i MB", c->memory_mb);
	snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", %s",
	    machine_config_uses_rom(c) ? "real ROM" :
	    c->prom_emulation ? "PROM emulation" :
	    "no PROM emulation");
	if (c->display)
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
		    ", framebuffer");
	for (i = 0; i < c->n_drives; i++) {
		const char *f = c->drives[i].file;
		p = strrchr(f, '/');
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
		    "\n%s", kind_names[c->drives[i].kind]);
		if (c->drives[i].id >= 0)
			snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
			    " %i", c->drives[i].id);
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
		    ": %s", f[0] ? (p ? p + 1 : f) : "(empty)");
	}
	return xstrdup(buf);
}


/*
 *  machine_config_create_disk():
 *
 *  Creates an empty disk image of a size in bytes (a sparse file, where
 *  the host supports that).  Returns false, with an error message in *errp,
 *  if it couldn't.
 */
bool machine_config_create_disk(const char *path, int64_t size, char **errp)
{
	FILE *f;
	char zero = 0;

	*errp = NULL;
	if (size < 512) {
		*errp = xstrdup("the size must be at least one sector");
		return false;
	}
	if (file_exists(path)) {
		*errp = xprintf("%s already exists", path);
		return false;
	}
	f = fopen(path, "wb");
#ifdef _WIN32
	if (f == NULL || _fseeki64(f, (__int64) size - 1,
#else
	if (f == NULL || fseeko(f, (off_t) size - 1,
#endif
	    SEEK_SET) != 0 || fwrite(&zero, 1, 1, f) != 1 || fclose(f) != 0) {
		*errp = xprintf("%s: %s", path, strerror(errno));
		if (f != NULL)
			remove(path);
		return false;
	}
	return true;
}


/*****************************************************************************
 *
 *  Setting up the emulation
 */

/*
 *  ROM self-tests that fail in gxemul, only because it doesn't emulate what
 *  they check (see experiments/patch_rom_tests.py, which makes patched
 *  copies by hand):
 *
 *	KN02-BA V5.7j (5000/1xx system ROM):  3/cache/seg and 3/cache/isol
 *	    need a real R3000 data cache, and 3/rtc/time real-time I/O timing.
 *	PMAG-BA V5.3a (color frame buffer option ROM):  0/curs and 0/ptrn
 *	    read the Bt459's signature registers, which aren't computed.
 *
 *  With "skip_failing_tests", a copy of such a ROM image is loaded instead,
 *  in which the first two instructions of these tests' functions are
 *  "jr ra; move v0,zero", so that they pass at once.  A ROM is recognized
 *  by its name and version strings, and the tests' first instructions
 *  (other versions are left alone).
 */
struct rom_test {
	uint32_t	addr;		/*  where the ROM runs the function  */
	uint32_t	first;		/*  its first instruction  */
};
static const struct {
	const char	*name, *version;
	uint32_t	base;		/*  ROM offset of the code at 0xa0020000  */
	struct rom_test	tests[4];
} failing_tests[] = {
	{ "KN02-BA", "V5.7j", 0x194a4, { { 0xa00212a0, 0x27bdffb8 },
	    { 0xa0021650, 0x27bdffd8 }, { 0xa0021ef0, 0x27bdffc8 } } },
	{ "PMAG-BA", "V5.3a", 0x1594, { { 0xa00204c0, 0x3c0ea002 },
	    { 0xa0021b80, 0x3c02a002 } } },
};

#define	JR_RA		0x03e00008
#define	MOVE_V0_ZERO	0x00001021

static bool contains(const unsigned char *buf, size_t len, const char *s)
{
	size_t n = strlen(s), i;

	for (i = 0; i + n <= len; i++)
		if (memcmp(buf + i, s, n) == 0)
			return true;
	return false;
}

static uint32_t get_le32(const unsigned char *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void put_le32(unsigned char *p, uint32_t v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/*
 *  Returns the name of a copy of a ROM image with its failing tests
 *  patched, or NULL if it isn't a ROM with such tests (or they are
 *  patched already).  The caller frees it.
 */
static char *rom_without_failing_tests(const char *path)
{
	unsigned char *buf;
	char *copy = NULL, *err = NULL;
	const char *base;
	size_t len, r, t, i;
	int patched = 0;
	FILE *f;

	f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = (size_t) ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len == 0 || len > 4 * 1048576) {
		fclose(f);
		return NULL;
	}
	CHECK_ALLOCATION(buf = (unsigned char *) malloc(len));
	if (fread(buf, 1, len, f) != len) {
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);

	for (r = 0; r < sizeof(failing_tests) / sizeof(failing_tests[0]); r++) {
		if (!contains(buf, len, failing_tests[r].name) ||
		    !contains(buf, len, failing_tests[r].version))
			continue;
		for (t = 0; t < 4 && failing_tests[r].tests[t].addr; t++) {
			size_t off = failing_tests[r].base +
			    failing_tests[r].tests[t].addr - 0xa0020000;
			uint32_t w;
			if (off + 8 > len)
				break;
			w = get_le32(buf + off);
			if (w == failing_tests[r].tests[t].first) {
				put_le32(buf + off, JR_RA);
				put_le32(buf + off + 4, MOVE_V0_ZERO);
				patched ++;
			} else if (w != JR_RA)
				break;		/*  another version  */
		}
		if (t < 4 && failing_tests[r].tests[t].addr) {
			/*  (Undo a partial patch of another version.)  */
			patched = 0;
		}
		break;
	}

	if (patched > 0) {
		base = path;
		for (i = 0; path[i]; i++)
			if (path[i] == '/' || path[i] == '\\')
				base = path + i + 1;
		copy = xprintf("%s/patched-%s", appdirs_state_dir(), base);
		f = fopen(copy, "wb");
		if (f == NULL || fwrite(buf, 1, len, f) != len) {
			err = copy;
			copy = NULL;
		}
		if (f != NULL)
			fclose(f);
		if (err != NULL) {
			fprintf(stderr, "Could not write %s; using the ROM "
			    "image as it is\n", err);
			free(err);
		} else
			debug("[ %s: %i failing self-test%s skipped ]\n",
			    path, patched, patched == 1 ? "" : "s");
	}
	free(buf);
	return copy;
}


/*  The file to load for a ROM image: the patched copy, if asked for:  */
static char *rom_to_load(const struct machine_config *c, const char *name)
{
	char *f = machine_config_resolve_rom(c, name), *p;

	if (c->skip_failing_tests &&
	    (p = rom_without_failing_tests(f)) != NULL) {
		free(f);
		return p;
	}
	return f;
}


/*
 *  machine_config_apply():
 *
 *  Sets up machine m (already added to emul) as the configuration says,
 *  and the emul's network.  Options set on m before (e.g. tracing from the
 *  command line) are kept.  Returns false, with an error message printed,
 *  if the machine could not be set up.
 */
bool machine_config_apply(struct machine_config *c, struct emul *emul,
	struct machine *m)
{
	char *err = machine_config_check(c);
	char **loads = NULL, *s;
	int n_loads = 0, i;
	bool ok;

	if (err != NULL) {
		fprintf(stderr, "%s%s%s\n", c->path ? c->path : "",
		    c->path ? ": " : "", err);
		free(err);
		return false;
	}

	if (c->name[0]) {
		free(m->name);
		m->name = xstrdup(c->name);
	}
	if (c->path != NULL)
		m->config_path = xstrdup(c->path);

	lookup_machine(c->type, c->model, &m->machine_type,
	    &m->machine_subtype, &err);
	if (c->cpu[0])
		m->cpu_name = xstrdup(c->cpu);
	if (c->ncpus > 0)
		m->ncpus = c->ncpus;
	if (c->memory_mb > 0)
		m->physical_ram_in_mb = c->memory_mb;
	if (c->emulated_hz > 0)
		m->emulated_hz = (int) c->emulated_hz;
	if (strncasecmp(c->byte_order, "big", 3) == 0)
		m->byte_order_override = EMUL_BIG_ENDIAN;
	else if (strncasecmp(c->byte_order, "little", 6) == 0)
		m->byte_order_override = EMUL_LITTLE_ENDIAN;
	if (c->random_memory)
		m->random_mem_contents = 1;
	if (c->random_bootstrap_cpu)
		m->use_random_bootstrap_cpu = 1;
	if (c->serial_nr > 0) {
		m->serial_nr = c->serial_nr;
		emul->next_serial_nr = c->serial_nr + 1;
	}

	m->prom_emulation = c->prom_emulation;
	m->force_netboot = c->force_netboot && !machine_config_uses_rom(c);
	m->start_paused = c->start_paused;
	if (c->bootarg != NULL && !machine_config_uses_rom(c))
		m->boot_string_argument = xstrdup(c->bootarg);

	if (c->nvram[0])
		m->nvram_filename = machine_config_resolve(c, c->nvram);

	m->display_md.in_use = c->display;
	m->display_md.zoom = c->scale;
	if (c->gfx_cards >= 0)
		m->n_gfx_cards = c->gfx_cards;
	else if (!c->display)
		m->n_gfx_cards = 0;
	if (c->display || c->serial_telnet)
		console_allow_slaves(1);

	for (i = 0; machine_config_uses_rom(c) && i < MC_MAX_SLOTS &&
	    i < MACHINE_MAX_TC_SLOTS; i++)
		if (c->slot_rom[i] != NULL && c->slot_rom[i][0])
			m->tc_rom_filename[i] = rom_to_load(c,
			    c->slot_rom[i]);

	for (i = 0; i < c->n_drives; i++) {
		struct mc_drive *d = &c->drives[i];
		char prefix[20] = "", *f, *spec;

		if (d->id >= 0)
			snprintf(prefix, sizeof(prefix), "%i", d->id);
		strlcat(prefix, d->kind == MC_DRIVE_CDROM ? "c" :
		    d->kind == MC_DRIVE_TAPE ? "t" :
		    d->kind == MC_DRIVE_FLOPPY ? "f" : "d", sizeof(prefix));
		if (d->bus == MC_BUS_SCSI)
			strlcat(prefix, "s", sizeof(prefix));
		if (d->bus == MC_BUS_IDE)
			strlcat(prefix, "i", sizeof(prefix));
		if (d->readonly)
			strlcat(prefix, "r", sizeof(prefix));
		if (d->overlay)
			strlcat(prefix, "R", sizeof(prefix));
		if (d->boot)
			strlcat(prefix, "b", sizeof(prefix));

		f = machine_config_resolve(c, d->file);
		spec = xprintf("%s:%s", prefix, f);
		free(f);
		if (diskimage_add(m, spec) < 0) {
			fprintf(stderr, "Could not add drive %s\n", spec);
			free(spec);
			return false;
		}
		free(spec);

		/*  The drive just added is the last one:  */
		struct diskimage *di = m->first_diskimage;
		while (di->next != NULL)
			di = di->next;
		if (d->vendor[0])
			di->inq_vendor = xstrdup(d->vendor);
		if (d->product[0])
			di->inq_product = xstrdup(d->product);
		if (d->revision[0])
			di->inq_revision = xstrdup(d->revision);
	}

	if (c->net) {
		emul->net = net_init(emul, NET_INIT_FLAG_GATEWAY,
		    c->net_tapdev[0] ? c->net_tapdev : NULL,
		    c->net_ipv4, c->net_ipv4len, c->net_remotes,
		    c->n_net_remotes, c->net_local_port, NULL);
		if (emul->net == NULL) {
			fprintf(stderr, "Could not set up the network.\n");
			return false;
		}
		if (c->net_l2tp && !net_l2tp_init(emul->net,
		    c->net_l2tp_session, c->net_l2tp_cookie,
		    c->net_l2tp_counter)) {
			fprintf(stderr, "Bad L2TPv3 settings.\n");
			return false;
		}
	}

	/*  Files to load: the system ROM, or the others (with PROM
	    emulation):  */
	if (machine_config_uses_rom(c)) {
		char *f = rom_to_load(c, c->rom);
		s = xprintf("0x%llx:%s", (unsigned long long)
		    machine_config_rom_address(c->type), f);
		machine_config_add_string(&n_loads, &loads, s);
		free(s);
		free(f);
	}
	for (i = 0; !machine_config_uses_rom(c) && i < c->n_load; i++) {
		s = resolve_load(c, c->load[i]);
		machine_config_add_string(&n_loads, &loads, s);
		free(s);
	}

	/*  The boot file name; by default, that of the last file loaded:  */
	if (c->bootname[0] && !machine_config_uses_rom(c))
		m->boot_kernel_filename = xstrdup(c->bootname);
	else if (n_loads > 0 && !machine_config_uses_rom(c)) {
		const char *last = loads[n_loads - 1];
		const char *slash = strrchr(last, '/');
		m->boot_kernel_filename = xstrdup(slash ? slash + 1 :
		    last + load_prefix_len(last));
	}

	ok = emul_machine_setup(m, n_loads, loads, c->n_devices, c->devices);

	for (i = 0; i < n_loads; i++)
		free(loads[i]);
	free(loads);
	return ok;
}


/*
 *  machine_config_create_emul():
 *
 *  Creates an emulation with one machine, set up from a configuration.
 *  Returns NULL if that failed.
 */
struct emul *machine_config_create_emul(struct machine_config *c)
{
	struct emul *emul = emul_new(c->path);
	struct machine *m = emul_add_machine(emul, NULL);

	if (!machine_config_apply(c, emul, m)) {
		emul_destroy(emul);
		return NULL;
	}
	return emul;
}
