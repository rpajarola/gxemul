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
 *  GXemul's own state, kept between runs (the list of recently used machine
 *  configurations), and where new machines go by default:
 *
 *	macOS:    ~/Library/Application Support/gxemul/
 *	Windows:  %APPDATA%\gxemul\
 *	others:   $XDG_CONFIG_HOME/gxemul/ (~/.config/gxemul/)
 *
 *  recent.json there is a JSON list of configuration file names, most
 *  recently used first.  settings.json holds the directory last used in a
 *  file chooser ("last_dir"), and the directories searched for ROM images
 *  ("roms_dir", the common one, if not the default, and "rom_dirs", the
 *  others).  New machines are saved in
 *  ~/GXemul by default (Documents\GXemul on Windows), and ROM images go in
 *  its ROMs folder.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "machine_config.h"
#include "misc.h"
#include "thirdparty/cJSON.h"

#define	MAX_RECENT	20


/*  Creates a directory and its parents (like mkdir -p):  */
static void make_dirs(const char *path)
{
	char *p, *s;

	CHECK_ALLOCATION(s = strdup(path));
	for (p = s + 1; *p; p++)
		if (*p == '/' || *p == '\\') {
			char c = *p;
			*p = '\0';
#ifdef _WIN32
			mkdir(s);
#else
			mkdir(s, 0755);
#endif
			*p = c;
		}
#ifdef _WIN32
	mkdir(s);
#else
	mkdir(s, 0755);
#endif
	free(s);
}


static char *home_dir(void)
{
	const char *h = getenv("HOME");
#ifdef _WIN32
	if (h == NULL)
		h = getenv("USERPROFILE");
#endif
	return h != NULL ? (char *) h : (char *) ".";
}


/*
 *  appdirs_state_dir():
 *
 *  Returns GXemul's state directory (creating it if needed).
 */
const char *appdirs_state_dir(void)
{
	static char *dir = NULL;
	char buf[4096];

	if (dir != NULL)
		return dir;

#if defined(_WIN32)
	{
		const char *a = getenv("APPDATA");
		snprintf(buf, sizeof(buf), "%s\\gxemul", a ? a : home_dir());
	}
#elif defined(__APPLE__)
	snprintf(buf, sizeof(buf), "%s/Library/Application Support/gxemul",
	    home_dir());
#else
	{
		const char *x = getenv("XDG_CONFIG_HOME");
		if (x != NULL && x[0] == '/')
			snprintf(buf, sizeof(buf), "%s/gxemul", x);
		else
			snprintf(buf, sizeof(buf), "%s/.config/gxemul",
			    home_dir());
	}
#endif

	make_dirs(buf);
	CHECK_ALLOCATION(dir = strdup(buf));
	return dir;
}


/*
 *  appdirs_machines_dir():
 *
 *  Returns the directory where new machines are created by default.
 */
const char *appdirs_machines_dir(void)
{
	static char *dir = NULL;
	char buf[4096];

	if (dir != NULL)
		return dir;
#ifdef _WIN32
	snprintf(buf, sizeof(buf), "%s\\Documents\\GXemul", home_dir());
#else
	snprintf(buf, sizeof(buf), "%s/GXemul", home_dir());
#endif
	CHECK_ALLOCATION(dir = strdup(buf));
	return dir;
}


static char *recent_file(void)
{
	char *f;
	size_t n = strlen(appdirs_state_dir()) + 20;
	CHECK_ALLOCATION(f = (char *) malloc(n));
	snprintf(f, n, "%s/recent.json", appdirs_state_dir());
	return f;
}


/*
 *  appdirs_recent():
 *
 *  Returns the recently used configuration files (most recent first), as
 *  a list which the caller frees (with each string).  Files which no longer
 *  exist are left out.
 */
int appdirs_recent(char ***listp)
{
	char *fname = recent_file(), *text = NULL;
	FILE *f = fopen(fname, "rb");
	cJSON *root, *e;
	int n = 0;
	long len;

	*listp = NULL;
	free(fname);
	if (f == NULL)
		return 0;

	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len > 0 && len < 1048576) {
		CHECK_ALLOCATION(text = (char *) malloc(len + 1));
		if ((long) fread(text, 1, len, f) == len)
			text[len] = '\0';
		else
			text[0] = '\0';
	}
	fclose(f);
	if (text == NULL)
		return 0;

	root = cJSON_Parse(text);
	free(text);
	if (cJSON_IsArray(root)) {
		cJSON_ArrayForEach(e, root) {
			struct stat st;
			if (cJSON_IsString(e) &&
			    stat(e->valuestring, &st) == 0)
				machine_config_add_string(&n, listp,
				    e->valuestring);
		}
	}
	cJSON_Delete(root);
	return n;
}


static void write_recent(int n, char **list)
{
	char *fname = recent_file(), *text;
	cJSON *a = cJSON_CreateArray();
	FILE *f;
	int i;

	for (i = 0; i < n && i < MAX_RECENT; i++)
		cJSON_AddItemToArray(a, cJSON_CreateString(list[i]));
	text = cJSON_Print(a);
	cJSON_Delete(a);

	f = fopen(fname, "w");
	if (f != NULL) {
		fputs(text, f);
		fputs("\n", f);
		fclose(f);
	}
	free(text);
	free(fname);
}


/*
 *  appdirs_add_recent(), appdirs_remove_recent():
 *
 *  Puts a configuration file first in the list of recent ones, or removes
 *  it from the list.
 */
void appdirs_add_recent(const char *path)
{
	char **list, **newlist = NULL;
	int n = appdirs_recent(&list), nn = 0, i;

	machine_config_add_string(&nn, &newlist, path);
	for (i = 0; i < n; i++) {
		if (strcmp(list[i], path) != 0)
			machine_config_add_string(&nn, &newlist, list[i]);
		free(list[i]);
	}
	free(list);

	write_recent(nn, newlist);
	for (i = 0; i < nn; i++)
		free(newlist[i]);
	free(newlist);
}

void appdirs_remove_recent(const char *path)
{
	char **list, **newlist = NULL;
	int n = appdirs_recent(&list), nn = 0, i;

	for (i = 0; i < n; i++) {
		if (strcmp(list[i], path) != 0)
			machine_config_add_string(&nn, &newlist, list[i]);
		free(list[i]);
	}
	free(list);

	write_recent(nn, newlist);
	for (i = 0; i < nn; i++)
		free(newlist[i]);
	free(newlist);
}


/*
 *  Settings (settings.json):
 */

static cJSON *settings = NULL;

static char *settings_file(void)
{
	char *f;
	size_t n = strlen(appdirs_state_dir()) + 20;
	CHECK_ALLOCATION(f = (char *) malloc(n));
	snprintf(f, n, "%s/settings.json", appdirs_state_dir());
	return f;
}

static cJSON *settings_get(void)
{
	char *fname, *text = NULL;
	FILE *f;
	long len;

	if (settings != NULL)
		return settings;

	fname = settings_file();
	f = fopen(fname, "rb");
	free(fname);
	if (f != NULL) {
		fseek(f, 0, SEEK_END);
		len = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (len > 0 && len < 1048576) {
			CHECK_ALLOCATION(text = (char *) calloc(1, len + 1));
			if ((long) fread(text, 1, len, f) != len)
				text[0] = '\0';
		}
		fclose(f);
	}
	if (text != NULL)
		settings = cJSON_Parse(text);
	free(text);
	if (!cJSON_IsObject(settings)) {
		cJSON_Delete(settings);
		settings = cJSON_CreateObject();
	}
	return settings;
}

static void settings_write(void)
{
	char *fname = settings_file(), *text = cJSON_Print(settings_get());
	FILE *f = fopen(fname, "w");

	if (f != NULL) {
		fputs(text, f);
		fputs("\n", f);
		fclose(f);
	}
	free(text);
	free(fname);
}


/*
 *  appdirs_last_dir(), appdirs_set_last_dir():
 *
 *  The directory last used in a file chooser (NULL if none yet).
 */
const char *appdirs_last_dir(void)
{
	cJSON *d = cJSON_GetObjectItemCaseSensitive(settings_get(),
	    "last_dir");
	return cJSON_IsString(d) ? d->valuestring : NULL;
}

void appdirs_set_last_dir(const char *dir)
{
	const char *old = appdirs_last_dir();

	if (dir == NULL || dir[0] == '\0' ||
	    (old != NULL && strcmp(old, dir) == 0))
		return;
	cJSON_DeleteItemFromObjectCaseSensitive(settings_get(), "last_dir");
	cJSON_AddStringToObject(settings_get(), "last_dir", dir);
	settings_write();
}


/*
 *  appdirs_roms_dir(), appdirs_set_roms_dir():
 *
 *  The common directory for ROM images (it may not exist yet): ROMs in the
 *  machines' directory, unless another one is set (NULL for the default).
 */
const char *appdirs_roms_dir(void)
{
	static char *dir = NULL;
	cJSON *d = cJSON_GetObjectItemCaseSensitive(settings_get(),
	    "roms_dir");

	if (cJSON_IsString(d) && d->valuestring[0])
		return d->valuestring;
	if (dir == NULL) {
		size_t n = strlen(appdirs_machines_dir()) + 10;
		CHECK_ALLOCATION(dir = (char *) malloc(n));
#ifdef _WIN32
		snprintf(dir, n, "%s\\ROMs", appdirs_machines_dir());
#else
		snprintf(dir, n, "%s/ROMs", appdirs_machines_dir());
#endif
	}
	return dir;
}


void appdirs_set_roms_dir(const char *dir)
{
	cJSON_DeleteItemFromObjectCaseSensitive(settings_get(), "roms_dir");
	if (dir != NULL && dir[0])
		cJSON_AddStringToObject(settings_get(), "roms_dir", dir);
	settings_write();
}


/*
 *  appdirs_rom_dirs(), appdirs_set_rom_dirs():
 *
 *  The directories searched for ROM images: the common one first, then the
 *  others given (which appdirs_set_rom_dirs() sets; the list returned
 *  includes the common one, and is freed by the caller, with its strings).
 */
int appdirs_rom_dirs(char ***listp)
{
	cJSON *a = cJSON_GetObjectItemCaseSensitive(settings_get(),
	    "rom_dirs"), *e;
	int n = 0;

	*listp = NULL;
	machine_config_add_string(&n, listp, appdirs_roms_dir());
	if (cJSON_IsArray(a))
		cJSON_ArrayForEach(e, a)
			if (cJSON_IsString(e) && e->valuestring[0])
				machine_config_add_string(&n, listp,
				    e->valuestring);
	return n;
}

void appdirs_set_rom_dirs(int n, char **list)
{
	cJSON *a = cJSON_CreateArray();
	int i;

	for (i = 0; i < n; i++)
		if (list[i] != NULL && list[i][0])
			cJSON_AddItemToArray(a, cJSON_CreateString(list[i]));
	cJSON_DeleteItemFromObjectCaseSensitive(settings_get(), "rom_dirs");
	cJSON_AddItemToObject(settings_get(), "rom_dirs", a);
	settings_write();
}
