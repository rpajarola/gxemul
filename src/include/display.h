#ifndef	DISPLAY_H
#define	DISPLAY_H

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
 *  Headerfile for src/console/display.c, and for the display backend
 *  (src/console/display_sdl.c).
 *
 *  A framebuffer window shows an image of 0x00RRGGBB pixels, which the
 *  framebuffer device draws into fb_window->pixels.  display.c puts the
 *  emulated "hardware" cursor on top of that, in fb_window->screen, and
 *  the backend shows fb_window->screen in a host window.
 */

#include <stdbool.h>
#include <stdint.h>
#include "misc.h"

struct emul;
struct machine;


#define N_GRAYCOLORS            16
#define	CURSOR_COLOR_TRANSPARENT	-1
#define	CURSOR_COLOR_INVERT		-2
#define	CURSOR_MAXY		64
#define	CURSOR_MAXX		64

/*  Grayscale i (0 .. N_GRAYCOLORS-1) as a pixel:  */
#define	DISPLAY_GRAY(i)		((uint32_t) (i) * 0x111111)

/*  Framebuffer windows:  */
struct fb_window {
	int		fb_number;
	char		*name;
	struct machine	*machine;

	/*  xsize > 0 for a valid fb_window  */
	int		xsize, ysize;
	int		scaledown;

	uint32_t	*pixels;	/*  drawn by the framebuffer device  */
	uint32_t	*screen;	/*  pixels, with the cursor on top  */

	/*  -1 means transparent, 0 and up are grayscales  */
	int		cursor_pixels[CURSOR_MAXY][CURSOR_MAXX];
	int		cursor_x;
	int		cursor_y;
	int		cursor_xsize;
	int		cursor_ysize;
	int		cursor_on;

	/*  The cursor as it is drawn in screen:  */
	int		shown_cursor_x;
	int		shown_cursor_y;
	int		shown_cursor_xsize;
	int		shown_cursor_ysize;
	int		shown_cursor_on;

	bool		need_flush;

	/*  Owned by the backend:  */
	void		*backend;
};


/*  Keys which display_key() turns into escape sequences:  */
enum display_key {
	DISPLAY_KEY_F1,		/*  .. DISPLAY_KEY_F1 + 11 for F12  */
	DISPLAY_KEY_UP = DISPLAY_KEY_F1 + 12,
	DISPLAY_KEY_DOWN,
	DISPLAY_KEY_RIGHT,
	DISPLAY_KEY_LEFT,
	DISPLAY_KEY_HOME,
	DISPLAY_KEY_END,
	DISPLAY_KEY_PAGE_UP,
	DISPLAY_KEY_PAGE_DOWN
};


/*
 *  Menus (display_menu.c):
 *
 *  Each machine has a menu bar, of submenu items, which the backend
 *  draws in its windows and works with the mouse and keyboard; what the
 *  items do is in display_menu.c.  The menus can be rebuilt whenever
 *  display_menus_update() is called, so the backend must not keep
 *  pointers to items.  The generation numbers change whenever the menus
 *  or the status line (a message, and what the removable media hold)
 *  change, so the backend knows when to draw them again.
 */
enum menu_item_type {
	MENU_ITEM_ACTION,
	MENU_ITEM_CHECK,
	MENU_ITEM_SUBMENU,
	MENU_ITEM_SEPARATOR
};

struct menu;
struct menu_item;
typedef void (*menu_func)(struct machine *, struct menu_item *);

struct menu_item {
	struct menu_item	*next;
	enum menu_item_type	type;
	char			*label;
	bool			enabled;
	bool			checked;	/*  check items  */
	struct menu		*submenu;	/*  submenu items  */
	menu_func		func;
	void			*data;
};

struct menu {
	struct menu_item	*items;
};

struct display_menus {
	struct menu		*bar;
	unsigned long		generation;

	struct menu		*media;
	unsigned long		media_generation;	/*  shown  */
	struct menu_item	*record;
	struct menu_item	*debugger;

	char			*message;
	uint32_t		message_expires;	/*  ms ticks  */
	char			*media_status;
	unsigned long		status_generation;
};

struct display_menus *display_menus(struct machine *);
void display_menus_update(struct machine *);
void display_menu_activate(struct machine *, struct menu_item *);
void display_message(struct machine *, const char *fmt, ...);

/*
 *  File choosers (display_files.c):  display_choose_file() opens the
 *  desktop's file chooser, and the emulator keeps running while it is
 *  open.  display_files_poll() calls done with the path chosen, or NULL
 *  if the chooser was canceled.  display_choose_folder() chooses a folder.
 *  Without a start_dir, a chooser starts in the folder last chosen from.
 */
typedef void (*files_done_func)(struct machine *, const char *path,
	void *data);
bool display_can_choose_files(void);
void display_choose_file(struct machine *, const char *title,
	const char *start_dir, files_done_func done, void *data);
void display_choose_new_file(struct machine *, const char *title,
	const char *start_dir, const char *save_name, files_done_func done,
	void *data);
void display_choose_folder(struct machine *, const char *title,
	const char *start_dir, files_done_func done, void *data);
bool display_chooser_open(void);
void display_files_poll(void);


/*  display.c:  */
void display_init(struct machine *);
struct fb_window *display_fb_init(int xsize, int ysize, const char *name,
	int scaledown, struct machine *);
void display_fb_resize(struct fb_window *win, int new_xsize, int new_ysize);
void display_set_name(struct fb_window *win, const char *name);
void display_update(struct fb_window *win, int x1, int y1, int x2, int y2);
void display_update_cursor(struct fb_window *win);
void display_flush(struct fb_window *win);
void display_check_event(struct emul *emul);
void display_key(struct machine *m, int key);
int display_screenshot(struct machine *, const char *file);
int display_video_start(struct machine *, const char *file);
void display_video_stop(void);
bool display_video_recording(void);
uint32_t display_ticks(void);


/*
 *  The backend (display_sdl.c; display.c has dummy functions if there is
 *  none):
 *
 *  display_backend_open() creates a host window for a new fb_window, and
 *  returns false if that isn't possible.  display_backend_update() shows
 *  a rectangle of fb_window->screen (and may wait for
 *  display_backend_flush() to do so).  display_backend_check_events()
 *  handles input for a machine's windows.
 */
bool display_backend_available(void);
bool display_backend_open(struct fb_window *win);
void display_backend_resize(struct fb_window *win);
void display_backend_set_title(struct fb_window *win);
void display_backend_update(struct fb_window *win, int x, int y,
	int w, int h);
void display_backend_flush(struct fb_window *win);
void display_backend_check_events(struct machine *m);
void display_backend_raise(struct machine *m);


#endif	/*  DISPLAY_H  */
