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
 *  The menus of a machine's framebuffer windows, and its status line (see
 *  display.h).  The backend draws them; what the items do is here:
 *
 *	Machine:  Save Screenshot, Record Video, Enter Debugger, Edit
 *		  Configuration (for a machine set up from a machine.json),
 *		  Quit
 *	Media:    for each CD-ROM and tape drive, Load... and Eject
 *
 *  Load... opens the desktop's file chooser (display_files.c), and the
 *  machine keeps running while it is open.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "debugger.h"
#include "diskimage.h"
#include "machine.h"
#include "machine_config.h"
#include "misc.h"
#include "display.h"


extern bool single_step;

/*  How long a message stays in the status line:  */
#define	MESSAGE_MS		5000


static struct menu *menu_new(void)
{
	struct menu *menu;

	CHECK_ALLOCATION(menu = (struct menu *) calloc(1, sizeof(*menu)));
	return menu;
}


static void menu_clear(struct menu *menu)
{
	while (menu->items != NULL) {
		struct menu_item *item = menu->items;
		menu->items = item->next;
		if (item->submenu != NULL) {
			menu_clear(item->submenu);
			free(item->submenu);
		}
		free(item->label);
		free(item);
	}
}


/*  Appends an item to a menu.  A submenu item gets an empty submenu.  */
static struct menu_item *menu_append(struct menu *menu,
	enum menu_item_type type, const char *label, menu_func func,
	void *data)
{
	struct menu_item *item, **p;

	CHECK_ALLOCATION(item = (struct menu_item *) calloc(1, sizeof(*item)));
	item->type = type;
	if (label != NULL)
		CHECK_ALLOCATION(item->label = strdup(label));
	item->enabled = type != MENU_ITEM_SEPARATOR;
	item->func = func;
	item->data = data;
	if (type == MENU_ITEM_SUBMENU)
		item->submenu = menu_new();

	for (p = &menu->items; *p != NULL; p = &(*p)->next)
		;
	*p = item;
	return item;
}


static void menus_changed(struct display_menus *menus)
{
	menus->generation ++;
}


static void status_changed(struct display_menus *menus)
{
	menus->status_generation ++;
}


/*
 *  display_message():
 *
 *  Shows a message in the status line of a machine's windows for a while.
 */
void display_message(struct machine *m, const char *fmt, ...)
{
	struct display_menus *menus = display_menus(m);
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	free(menus->message);
	CHECK_ALLOCATION(menus->message = strdup(buf));
	menus->message_expires = display_ticks() + MESSAGE_MS;
	status_changed(menus);
}


/*  Machine menu:  */

static void menu_screenshot(struct machine *m, struct menu_item *item)
{
	display_screenshot(m, NULL);
}


static void menu_record(struct machine *m, struct menu_item *item)
{
	if (display_video_recording())
		display_video_stop();
	else
		display_video_start(m, NULL);
}


static void menu_debugger(struct machine *m, struct menu_item *item)
{
	debugger_activate(0);
}


/*
 *  Edit Configuration: the machine stops, and gxemul starts again with
 *  --edit (see main()).
 */
extern char *progname;

/*
 *  Opens the configuration window for a machine's configuration file, in
 *  another gxemul ("--edit file --no-start"), while the machine keeps
 *  running.  The other gxemul gets no terminal input (it shares this one's
 *  terminal, whose settings it must not touch), and on POSIX hosts, it is
 *  a grandchild, so that it isn't left a zombie when it exits.
 */
static bool spawn_editor(const char *path)
{
#ifdef _WIN32
	const char *args[] = { "--edit", path, "--no-start", NULL };
	return win32_spawn_self(args, true);
#else
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return false;
	if (pid == 0) {
		if (fork() == 0) {
			int fd = open("/dev/null", O_RDONLY);
			if (fd >= 0)
				dup2(fd, 0);
			setsid();
			execlp(progname, progname, "--edit", path,
			    "--no-start", (char *) NULL);
			_exit(1);
		}
		_exit(0);
	}
	waitpid(pid, &status, 0);
	return true;
#endif
}

static void menu_edit_config(struct machine *m, struct menu_item *item)
{
	(void) item;
	if (spawn_editor(m->config_path))
		display_message(m, "Editing the configuration in another "
		    "window; changes take effect when the machine is "
		    "restarted");
	else
		display_message(m, "Could not open the configuration window");
}


extern bool emul_shutdown;
extern bool restart_machine;

/*  View menu:  sizes the machine's windows (zoom in percent, 0 = fit):  */

static const char *zoom_labels[4] = { "Fit the Screen", "Actual Size",
	"Half Size", "Double Size" };
static const int zoom_values[4] = { 0, 100, 50, 200 };

static void menu_zoom(struct machine *m, struct menu_item *item)
{
	int i;

	m->display_md.zoom = (int) (intptr_t) item->data;
	for (i = 0; i < m->display_md.n_fb_windows; i++)
		display_backend_zoom(m->display_md.fb_windows[i]);
	display_menus_update(m);
}


/*  Stops the machine, and starts it again (see restart() in main.c):  */
static void menu_reset(struct machine *m, struct menu_item *item)
{
	(void) item;
	display_message(m, "Resetting...");
	restart_machine = true;
	emul_shutdown = true;
}


static void menu_quit(struct machine *m, struct menu_item *item)
{
	printf("[ Quit was chosen in the menu; exiting ]\n");
	exit(0);
}


/*  Media menu:  */

static const char *basename_of(const char *path)
{
	const char *p = strrchr(path, '/');
#ifdef _WIN32
	const char *q = strrchr(path, '\\');
	if (q != NULL && (p == NULL || q > p))
		p = q;
#endif
	return p == NULL ? path : p + 1;
}


static void media_loaded(struct machine *m, const char *path, void *data)
{
	struct diskimage *d = (struct diskimage *) data;
	char name[40], err[200];

	if (path == NULL)
		return;

	diskimage_device_name(d, name, sizeof(name));
	if (diskimage_load_medium(d, path, err, sizeof(err))) {
		printf("[ %s: loaded %s ]\n", name, path);
		display_message(m, "%s: loaded %s", name, basename_of(path));
	} else {
		printf("[ %s: could not load %s: %s ]\n", name, path, err);
		display_message(m, "Could not load %s: %s", basename_of(path),
		    err);
	}
}


static void menu_media_load(struct machine *m, struct menu_item *item)
{
	struct diskimage *d = (struct diskimage *) item->data;
	char name[40], title[80];

	diskimage_device_name(d, name, sizeof(name));
	snprintf(title, sizeof(title), "Load %s", name);

	/*  Start in the folder last chosen from, or else in that of the last
	    medium (relative names are in the current directory):  */
	char *start;
	const char *base = basename_of(d->fname);
	if (appdirs_last_dir() != NULL) {
		CHECK_ALLOCATION(start = strdup(appdirs_last_dir()));
	} else if (base != d->fname) {
		CHECK_ALLOCATION(start = strdup(d->fname));
		start[base - d->fname - (base - d->fname > 1)] = '\0';
	} else
		CHECK_ALLOCATION(start = strdup("."));

	display_choose_file(m, title, start, media_loaded, d);
	free(start);
}


static void menu_media_eject(struct machine *m, struct menu_item *item)
{
	struct diskimage *d = (struct diskimage *) item->data;
	char name[40];

	diskimage_device_name(d, name, sizeof(name));
	printf("[ %s: ejected %s ]\n", name, d->fname);
	diskimage_eject_medium(d);
}


/*  Rebuilds the Media menu and the media status, if the media changed.  */
static void media_update(struct machine *m, struct display_menus *menus)
{
	struct menu_item *item;
	size_t status_len = 1;
	char *status;

	if (menus->media_generation == diskimage_media_generation &&
	    menus->media_status != NULL)
		return;
	menus->media_generation = diskimage_media_generation;

	menu_clear(menus->media);
	CHECK_ALLOCATION(status = (char *) calloc(1, status_len));

	for (struct diskimage *d = m->first_diskimage; d != NULL; d = d->next) {
		char name[40], label[300];

		if (!diskimage_is_removable(d))
			continue;

		diskimage_device_name(d, name, sizeof(name));
		snprintf(label, sizeof(label), "%s: %s", name,
		    d->no_medium ? "empty" : basename_of(d->fname));

		item = menu_append(menus->media, MENU_ITEM_SUBMENU, label,
		    NULL, NULL);
		struct menu *submenu = item->submenu;
		item = menu_append(submenu, MENU_ITEM_ACTION, "Load...",
		    menu_media_load, d);
		item->enabled = display_can_choose_files();
		item = menu_append(submenu, MENU_ITEM_ACTION, "Eject",
		    menu_media_eject, d);
		item->enabled = !d->no_medium;

		status_len += strlen(label) + 4;
		CHECK_ALLOCATION(status = (char *) realloc(status, status_len));
		if (status[0] != '\0')
			strcat(status, "    ");
		strcat(status, label);
	}

	if (menus->media->items == NULL) {
		item = menu_append(menus->media, MENU_ITEM_ACTION,
		    "No CD-ROM or tape drives", NULL, NULL);
		item->enabled = false;
	}

	free(menus->media_status);
	menus->media_status = status;
	menus_changed(menus);
	status_changed(menus);
}


/*
 *  display_menus():
 *
 *  Returns a machine's menus, making them the first time.
 */
struct display_menus *display_menus(struct machine *m)
{
	struct display_menus *menus = m->display_md.menus;
	struct menu_item *item;

	if (menus != NULL)
		return menus;

	CHECK_ALLOCATION(menus = (struct display_menus *)
	    calloc(1, sizeof(*menus)));
	m->display_md.menus = menus;
	menus->bar = menu_new();

	item = menu_append(menus->bar, MENU_ITEM_SUBMENU, "Machine", NULL, NULL);
	struct menu *machine = item->submenu;
	menu_append(machine, MENU_ITEM_ACTION, "Save Screenshot",
	    menu_screenshot, NULL);
	menus->record = menu_append(machine, MENU_ITEM_CHECK, "Record Video",
	    menu_record, NULL);
	menu_append(machine, MENU_ITEM_SEPARATOR, NULL, NULL, NULL);
	menus->debugger = menu_append(machine, MENU_ITEM_ACTION,
	    "Enter Debugger", menu_debugger, NULL);
	menu_append(machine, MENU_ITEM_SEPARATOR, NULL, NULL, NULL);
	menu_append(machine, MENU_ITEM_ACTION, "Reset", menu_reset, NULL);
	if (m->config_path != NULL)
		menu_append(machine, MENU_ITEM_ACTION,
		    "Edit Configuration...", menu_edit_config, NULL);
	menu_append(machine, MENU_ITEM_ACTION, "Quit", menu_quit, NULL);

	item = menu_append(menus->bar, MENU_ITEM_SUBMENU, "View", NULL, NULL);
	for (int i = 0; i < 4; i++)
		menus->zoom[i] = menu_append(item->submenu, MENU_ITEM_CHECK,
		    zoom_labels[i], menu_zoom, (void *) (intptr_t)
		    zoom_values[i]);

	item = menu_append(menus->bar, MENU_ITEM_SUBMENU, "Media", NULL, NULL);
	menus->media = item->submenu;

	display_menus_update(m);
	return menus;
}


/*
 *  display_menus_update():
 *
 *  Brings a machine's menus and status line up to date.  (The media, the
 *  recording and the debugger can also change from debugger commands.)
 */
void display_menus_update(struct machine *m)
{
	struct display_menus *menus = display_menus(m);

	media_update(m, menus);

	if (menus->record->checked != display_video_recording()) {
		menus->record->checked = display_video_recording();
		menus_changed(menus);
	}
	for (int i = 0; i < 4; i++)
		if (menus->zoom[i]->checked !=
		    (m->display_md.zoom == zoom_values[i])) {
			menus->zoom[i]->checked = !menus->zoom[i]->checked;
			menus_changed(menus);
		}
	if (menus->debugger->enabled == single_step) {
		menus->debugger->enabled = !single_step;
		menus_changed(menus);
	}

	if (menus->message != NULL &&
	    (int32_t) (display_ticks() - menus->message_expires) >= 0) {
		free(menus->message);
		menus->message = NULL;
		status_changed(menus);
	}
}


/*
 *  display_menu_activate():
 *
 *  Does what an item does, when it's chosen.
 */
void display_menu_activate(struct machine *m, struct menu_item *item)
{
	if (!item->enabled || item->func == NULL)
		return;

	item->func(m, item);
	display_menus_update(m);
}
