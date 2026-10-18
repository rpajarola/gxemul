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
 *  The window for choosing, creating, and editing machine configurations
 *  (see config_ui.h and machine_config.h).  It has two screens:
 *
 *	the list of machines (recently used configuration files), with
 *	    New, Open, Edit, Duplicate, Forget, and Start;
 *	the editor, with a page for each part of the configuration.
 *
 *  A new machine starts with the defaults of a model that is known to work
 *  (its preset, see machine_presets.c), and choosing another model gives it
 *  that model's defaults, until it is saved.  Only models with a preset are
 *  offered, unless "Show all machines" is checked.
 *
 *  The widgets are microui's (src/console/microui.c), drawn with SDL and the
 *  same font as the framebuffer windows' menus.  Coordinates are in output
 *  pixels (twice the window's points on a HiDPI screen).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config_ui.h"
#include "display.h"
#include "machine.h"
#include "machine_config.h"
#include "misc.h"


#ifndef	WITH_SDL



bool config_ui_available(void)
{
	return false;
}

struct machine_config *config_ui_run(const char *edit_path, bool running)
{
	(void) edit_path;
	(void) running;
	return NULL;
}

#else	/*  WITH_SDL  */


#define	SDL_MAIN_HANDLED
#include <SDL.h>

#include "display_font.h"
#include "thirdparty/microui.h"


extern struct machine_entry *first_machine_entry;

#define	WINDOW_W		940
#define	WINDOW_H		660
#define	EDIT_LEN		1024
#define	MAX_EDITS		400
#define	DEFAULT_DISK_MB		2048

enum screen { SCREEN_LIST, SCREEN_EDITOR, SCREEN_SETTINGS };
enum page { PAGE_GENERAL, PAGE_FIRMWARE, PAGE_DRIVES, PAGE_DISPLAY,
	PAGE_NETWORK, PAGE_ADVANCED, N_PAGES };

static const char *page_names[N_PAGES] = { "General", "Firmware", "Drives",
	"Display", "Network", "Advanced" };

static const char *kind_labels[] = { "Disk", "CD-ROM", "Tape", "Floppy" };
static const char *bus_labels[] = { "Default bus", "SCSI", "IDE" };
static const char *order_labels[] = { "Default", "Big-endian",
	"Little-endian" };
static const char *order_values[] = { "", "big", "little" };


/*  The text of an edited text field, for a field of the configuration:  */
struct edit {
	const void	*key;
	char		buf[EDIT_LEN];
};

static struct {
	SDL_Window	*window;
	SDL_Renderer	*renderer;
	SDL_Texture	*font;
	int		scale;
	int		font_w, font_h;
	int		out_w, out_h;
	mu_Context	*ctx;

	enum screen	screen;
	bool		quit;
	struct machine_config *result;
	char		*message;	/*  shown at the bottom  */
	bool		message_error;

	/*  The list of machines:  */
	int		n_recent;
	char		**recent;
	struct machine_config **recent_cfg;	/*  NULL if unreadable  */
	char		**recent_err;
	int		selected;

	/*  The editor:  */
	struct machine_config *cfg;
	bool		is_new;
	bool		fresh;		/*  new, and gets a model's defaults  */
	char		*auto_name;	/*  the name it got from them  */
	bool		show_all;	/*  also models without a preset  */
	bool		start_after_save;	/*  (when saved as)  */
	enum page	page;
	int		disk_mb[64];	/*  for creating missing disk images  */
	bool		custom[64];	/*  editing a drive's INQUIRY strings  */
	struct edit	edits[MAX_EDITS];
	int		n_edits;

	/*  Text typed or pasted, given to microui a little at a time:  */
	char		pending_text[4096];
	size_t		pending_len;

	/*  The open file chooser's target:  */
	char		**pick_field;
	bool		pick_rom;	/*  a ROM image (see picked())  */
	bool		pick_open;

	/*  The open drop-down list, if any:  */
	mu_Container	*open_list;

	/*  The settings: the ROM folders besides the common one:  */
	int		n_rom_dirs;
	char		**rom_dirs;

	/*  Editing the configuration of a running machine (only that):  */
	bool		running;
} ui;


/****************************************************************************/

/*
 *  Messages
 */

static void set_message(bool error, const char *fmt, ...)
{
	char buf[1000];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	free(ui.message);
	CHECK_ALLOCATION(ui.message = strdup(buf));
	ui.message_error = error;
}


/****************************************************************************/

/*
 *  Drawing: microui's commands, with SDL and the menu font.
 */

/*  Returns the next character of UTF-8 text, as Latin-1 (see display_sdl.c).  */
static int text_next(const char **textp, const char *end)
{
	const unsigned char *s = (const unsigned char *) *textp;
	unsigned long c;
	int more, i;

	if (s[0] < 0x80) {
		*textp += 1;
		return s[0];
	}
	if ((s[0] & 0xe0) == 0xc0) {
		c = s[0] & 0x1f;
		more = 1;
	} else if ((s[0] & 0xf0) == 0xe0) {
		c = s[0] & 0x0f;
		more = 2;
	} else if ((s[0] & 0xf8) == 0xf0) {
		c = s[0] & 0x07;
		more = 3;
	} else {
		*textp += 1;
		return s[0];
	}
	for (i = 1; i <= more; i++) {
		if ((const char *) s + i >= end || (s[i] & 0xc0) != 0x80) {
			*textp += 1;
			return s[0];
		}
		c = (c << 6) | (s[i] & 0x3f);
	}
	*textp += 1 + more;
	return c <= 0xff ? (int) c : '?';
}


static int text_width(mu_Font font, const char *str, int len)
{
	const char *end = str + (len < 0 ? (int) strlen(str) : len);
	int n = 0;

	(void) font;
	while (str < end && *str != '\0') {
		text_next(&str, end);
		n ++;
	}
	return n * ui.font_w;
}


static int text_height(mu_Font font)
{
	(void) font;
	return ui.font_h;
}


static void make_font(void)
{
	int window_w, window_h, width, height, zoom, bytes, c, row, col;
	SDL_Surface *surface;

	SDL_GetWindowSize(ui.window, &window_w, &window_h);
	if (SDL_GetRendererOutputSize(ui.renderer, &ui.out_w, &ui.out_h) != 0)
		ui.out_w = window_w, ui.out_h = window_h;
	int scale = window_w > 0 ? (ui.out_w + window_w / 2) / window_w : 1;
	if (scale < 1)
		scale = 1;
	if (ui.font != NULL && scale == ui.scale)
		return;
	ui.scale = scale;

	if (scale >= 2) {
		width = DISPLAY_FONT_16X32_WIDTH;
		height = DISPLAY_FONT_16X32_HEIGHT;
		zoom = scale / 2;
	} else {
		width = DISPLAY_FONT_8X16_WIDTH;
		height = DISPLAY_FONT_8X16_HEIGHT;
		zoom = 1;
	}
	bytes = (width + 7) / 8;

	if (ui.font != NULL)
		SDL_DestroyTexture(ui.font);
	ui.font = NULL;
	surface = SDL_CreateRGBSurfaceWithFormat(0, 16 * width, 14 * height, 32,
	    SDL_PIXELFORMAT_ARGB8888);
	if (surface != NULL) {
		SDL_LockSurface(surface);
		for (c = DISPLAY_FONT_FIRST; c <= DISPLAY_FONT_LAST; c++) {
			int i = c - DISPLAY_FONT_FIRST;
			const unsigned char *glyph = width ==
			    DISPLAY_FONT_16X32_WIDTH ? display_font_16x32[i] :
			    display_font_8x16[i];
			for (row = 0; row < height; row++) {
				Uint32 *pixels = (Uint32 *) ((Uint8 *)
				    surface->pixels + ((i / 16) * height + row)
				    * surface->pitch) + (i % 16) * width;
				for (col = 0; col < width; col++)
					pixels[col] = (glyph[row * bytes + col
					    / 8] & (0x80 >> (col % 8))) ?
					    0xffffffff : 0x00ffffff;
			}
		}
		SDL_UnlockSurface(surface);
		ui.font = SDL_CreateTextureFromSurface(ui.renderer, surface);
		SDL_FreeSurface(surface);
	}
	if (ui.font != NULL)
		SDL_SetTextureBlendMode(ui.font, SDL_BLENDMODE_BLEND);

	ui.font_w = width * zoom;
	ui.font_h = height * zoom;

	/*  Sizes of things, in output pixels:  */
	mu_Style *st = ui.ctx->style;
	st->size = mu_vec2(10 * ui.font_w, ui.font_h + 2 * scale);
	st->padding = 4 * scale;
	st->spacing = 4 * scale;
	st->indent = 2 * ui.font_w;
	st->title_height = ui.font_h + 8 * scale;
	st->scrollbar_size = 12 * scale;
	st->thumb_size = 8 * scale;
}


static void draw_char(int x, int y, int c)
{
	int width = ui.scale >= 2 ? DISPLAY_FONT_16X32_WIDTH :
	    DISPLAY_FONT_8X16_WIDTH;
	int height = ui.scale >= 2 ? DISPLAY_FONT_16X32_HEIGHT :
	    DISPLAY_FONT_8X16_HEIGHT;

	if (c < DISPLAY_FONT_FIRST || c > DISPLAY_FONT_LAST)
		c = '?';
	c -= DISPLAY_FONT_FIRST;

	SDL_Rect src = { (c % 16) * width, (c / 16) * height, width, height };
	SDL_Rect dst = { x, y, ui.font_w, ui.font_h };
	SDL_RenderCopy(ui.renderer, ui.font, &src, &dst);
}


static void draw_text(const char *text, int x, int y, mu_Color color)
{
	const char *end = text + strlen(text);

	if (ui.font == NULL)
		return;
	SDL_SetTextureColorMod(ui.font, color.r, color.g, color.b);
	while (text < end) {
		draw_char(x, y, text_next(&text, end));
		x += ui.font_w;
	}
}


static void draw_rect(mu_Rect r, mu_Color color)
{
	SDL_Rect rect = { r.x, r.y, r.w, r.h };
	SDL_SetRenderDrawColor(ui.renderer, color.r, color.g, color.b, color.a);
	SDL_RenderFillRect(ui.renderer, &rect);
}


static void draw_icon(int id, mu_Rect r, mu_Color color)
{
	const char *c = id == MU_ICON_CLOSE ? "x" : id == MU_ICON_COLLAPSED ?
	    "+" : id == MU_ICON_EXPANDED ? "-" : NULL;

	if (id == MU_ICON_MAX) {
		/*  The arrow of a drop-down list:  */
		int i, w = r.w / 2, x = r.x + (r.w - w) / 2;
		int y = r.y + (r.h - w / 2) / 2;
		for (i = 0; i < w / 2; i++)
			draw_rect(mu_rect(x + i, y + i, w - 2 * i, 1), color);
		return;
	}
	if (id == MU_ICON_CHECK) {
		int m = r.w / 4;
		draw_rect(mu_rect(r.x + m, r.y + m, r.w - 2 * m, r.h - 2 * m),
		    color);
		return;
	}
	if (c != NULL)
		draw_text(c, r.x + (r.w - ui.font_w) / 2,
		    r.y + (r.h - ui.font_h) / 2, color);
}


static void render(void)
{
	mu_Command *cmd = NULL;

	SDL_RenderSetClipRect(ui.renderer, NULL);
	SDL_SetRenderDrawColor(ui.renderer, 0xf0, 0xf0, 0xf0, 255);
	SDL_RenderClear(ui.renderer);

	while (mu_next_command(ui.ctx, &cmd)) {
		switch (cmd->type) {
		case MU_COMMAND_TEXT:
			draw_text(cmd->text.str, cmd->text.pos.x,
			    cmd->text.pos.y, cmd->text.color);
			break;
		case MU_COMMAND_RECT:
			draw_rect(cmd->rect.rect, cmd->rect.color);
			break;
		case MU_COMMAND_ICON:
			draw_icon(cmd->icon.id, cmd->icon.rect,
			    cmd->icon.color);
			break;
		case MU_COMMAND_CLIP: {
			mu_Rect r = cmd->clip.rect;
			SDL_Rect rect = { r.x, r.y, r.w, r.h };
			if (r.w >= 0x1000000)
				SDL_RenderSetClipRect(ui.renderer, NULL);
			else
				SDL_RenderSetClipRect(ui.renderer, &rect);
			break;
		}
		}
	}

	SDL_RenderPresent(ui.renderer);
}


static void set_style(void)
{
	static const struct { int id; unsigned rgb; } colors[] = {
		{ MU_COLOR_TEXT,	0x000000 },
		{ MU_COLOR_BORDER,	0x9a9a9a },
		{ MU_COLOR_WINDOWBG,	0xf0f0f0 },
		{ MU_COLOR_TITLEBG,	0xe0e0e0 },
		{ MU_COLOR_TITLETEXT,	0x000000 },
		{ MU_COLOR_PANELBG,	0xfafafa },
		{ MU_COLOR_BUTTON,	0xe2e2e2 },
		{ MU_COLOR_BUTTONHOVER,	0xd2d2d2 },
		{ MU_COLOR_BUTTONFOCUS,	0xc2c2c2 },
		{ MU_COLOR_BASE,	0xffffff },
		{ MU_COLOR_BASEHOVER,	0xf6f6f6 },
		{ MU_COLOR_BASEFOCUS,	0xfffbe6 },
		{ MU_COLOR_SCROLLBASE,	0xe6e6e6 },
		{ MU_COLOR_SCROLLTHUMB,	0xa8a8a8 },
	};
	size_t i;

	for (i = 0; i < sizeof(colors) / sizeof(colors[0]); i++)
		ui.ctx->style->colors[colors[i].id] = mu_color(
		    colors[i].rgb >> 16, (colors[i].rgb >> 8) & 0xff,
		    colors[i].rgb & 0xff, 255);
}


/****************************************************************************/

/*
 *  Widgets
 */

/*  Width of n characters, in output pixels:  */
#define	CH(n)		((n) * ui.font_w)

/*  The height of a row of widgets, with the space after it:  */
static int row_height(void)
{
	mu_Style *st = ui.ctx->style;
	return st->size.y + 2 * st->padding + st->spacing;
}


static void row(int n, const int *widths)
{
	mu_layout_row(ui.ctx, n, widths, 0);
}


static void edits_clear(void)
{
	ui.n_edits = 0;
}


static struct edit *edit_for(const void *key, const char *value)
{
	int i;

	for (i = 0; i < ui.n_edits; i++)
		if (ui.edits[i].key == key)
			return &ui.edits[i];
	if (ui.n_edits >= MAX_EDITS)
		edits_clear();
	struct edit *e = &ui.edits[ui.n_edits ++];
	e->key = key;
	snprintf(e->buf, sizeof(e->buf), "%s", value != NULL ? value : "");
	return e;
}


/*  A text field for a string of the configuration:  */
static void text_field(char **field)
{
	struct edit *e = edit_for(field, *field);

	if (mu_textbox_ex(ui.ctx, e->buf, sizeof(e->buf), 0) & MU_RES_CHANGE)
		machine_config_set(field, e->buf);
}


/*  A text field for a number:  */
static void int_field(int *field)
{
	char value[40];
	struct edit *e;

	snprintf(value, sizeof(value), "%i", *field);
	e = edit_for(field, value);
	if (mu_textbox_ex(ui.ctx, e->buf, sizeof(e->buf), 0) & MU_RES_CHANGE)
		*field = atoi(e->buf);
}


static void check(const char *label, bool *field)
{
	int v = *field;

	/*  (mu_checkbox() can't tell checkboxes apart by themselves.)  */
	mu_push_id(ui.ctx, &field, sizeof(field));
	if (mu_checkbox(ui.ctx, label, &v))
		*field = v != 0;
	mu_pop_id(ui.ctx);
}


/*
 *  A drop-down list: a button that shows the choice made, and opens the
 *  list of choices under it (scrolling, if there are many).  Returns true if
 *  a choice was made.  If *sel isn't one of them, "Choose..." is shown.
 */
static bool list_item(const void *id_data, int id_len, const char *label,
	bool selected);

#define	DROPDOWN_ROWS	14
static bool dropdown(const char *name, const char **labels, int n, int *sel)
{
	mu_Context *ctx = ui.ctx;
	mu_Container *cnt;
	mu_Rect r, tr;
	mu_Id id;
	bool changed = false;
	int i, step = row_height();

	mu_push_id(ctx, name, (int) strlen(name));
	id = mu_get_id(ctx, "!button", 7);
	r = mu_layout_next(ctx);
	mu_update_control(ctx, id, r, 0);
	cnt = mu_get_container(ctx, "!list");

	/*  (A container is created open; only the list opened here is.)  */
	if (cnt != ui.open_list)
		cnt->open = 0;

	if (ctx->mouse_pressed == MU_MOUSE_LEFT && ctx->focus == id) {
		if (cnt->open && ui.open_list == cnt)
			cnt->open = 0;
		else if (n > 0) {
			int h = (n < DROPDOWN_ROWS ? n : DROPDOWN_ROWS) * step +
			    ctx->style->padding * 2 - ctx->style->spacing;
			mu_open_popup(ctx, "!list");
			cnt->rect = mu_rect(r.x, r.y + r.h, r.w, h);
			if (cnt->rect.y + h > ui.out_h && r.y - h >= 0)
				cnt->rect.y = r.y - h;
			cnt->scroll = mu_vec2(0, *sel > 2 && *sel < n ?
			    (*sel - 2) * step : 0);
			ui.open_list = cnt;
		}
	}

	mu_draw_control_frame(ctx, id, r, MU_COLOR_BUTTON, 0);
	tr = r;
	tr.w -= r.h;
	mu_push_clip_rect(ctx, tr);
	mu_draw_control_text(ctx, *sel >= 0 && *sel < n ? labels[*sel] :
	    "Choose...", tr, MU_COLOR_TEXT, 0);
	mu_pop_clip_rect(ctx);
	mu_draw_icon(ctx, MU_ICON_MAX, mu_rect(r.x + r.w - r.h, r.y, r.h, r.h),
	    ctx->style->colors[MU_COLOR_TEXT]);

	if (mu_begin_window_ex(ctx, "!list", mu_rect(0, 0, 0, 0),
	    MU_OPT_POPUP | MU_OPT_NORESIZE | MU_OPT_NOTITLE | MU_OPT_CLOSED)) {
		int full[1] = { -1 };
		row(1, full);
		for (i = 0; i < n; i++)
			if (list_item(&labels[i], sizeof(labels[i]), labels[i],
			    i == *sel)) {
				*sel = i;
				changed = true;
				cnt->open = 0;
			}
		mu_end_window(ctx);
	}
	mu_pop_id(ctx);
	return changed;
}


/*  An item in a list, highlighted if selected.  Returns true if clicked.  */
static bool list_item(const void *id_data, int id_len, const char *label,
	bool selected)
{
	mu_Context *ctx = ui.ctx;
	mu_Id id = mu_get_id(ctx, id_data, id_len);
	mu_Rect r = mu_layout_next(ctx);
	bool clicked;

	mu_update_control(ctx, id, r, 0);
	clicked = ctx->mouse_pressed == MU_MOUSE_LEFT && ctx->focus == id;
	if (selected) {
		mu_Vec2 pos;
		mu_draw_rect(ctx, r, mu_color(0x2f, 0x5f, 0xb3, 255));
		pos.x = r.x + ctx->style->padding;
		pos.y = r.y + (r.h - ui.font_h) / 2;
		mu_push_clip_rect(ctx, r);
		mu_draw_text(ctx, NULL, label, -1, pos,
		    mu_color(255, 255, 255, 255));
		mu_pop_clip_rect(ctx);
	} else {
		if (ctx->hover == id)
			mu_draw_rect(ctx, r, mu_color(0xe4, 0xec, 0xf8, 255));
		mu_draw_control_text(ctx, label, r, MU_COLOR_TEXT, 0);
	}
	return clicked;
}


/****************************************************************************/

/*
 *  File choosers
 */

static void picked(struct machine *m, const char *path, void *data)
{
	(void) m; (void) data;
	ui.pick_open = false;
	if (path != NULL && ui.pick_field != NULL) {
		/*  (Relative to the machine's folder, or for a ROM image, to
		    a ROM folder, if it is in one.)  */
		char *p = machine_config_normalize(ui.cfg, path, ui.pick_rom);
		machine_config_set(ui.pick_field, p);
		free(p);
		edits_clear();
	}
	ui.pick_field = NULL;
}


/*  Where a file chooser starts: where the last one was used.  */
static const char *start_dir(void)
{
	if (appdirs_last_dir() != NULL)
		return appdirs_last_dir();
	if (ui.cfg != NULL && ui.cfg->dir != NULL)
		return ui.cfg->dir;
	return appdirs_machines_dir();
}


/*  Lets the user choose a file for a field of the configuration:  */
static void pick_file(char **field, const char *title, bool rom)
{
	ui.pick_field = field;
	ui.pick_rom = rom;
	ui.pick_open = true;
	display_choose_file(NULL, title, start_dir(), picked, NULL);
	if (!display_chooser_open())
		ui.pick_open = false;
}


/*  A file name field, with a Browse button:  */
/*  Says where a ROM image given by name was found, or that it wasn't:  */
static void rom_found(const char *name)
{
	struct stat st;
	char *f, buf[1200];

	if (name == NULL || name[0] == '\0')
		return;
	f = machine_config_resolve_rom(ui.cfg, name);
	if (stat(f, &st) == 0)
		snprintf(buf, sizeof(buf), "(%s)", f);
	else
		snprintf(buf, sizeof(buf), "(not found in the machine's folder, "
		    "or in the ROM folders under Settings)");
	free(f);
	int full[1] = { -1 };
	row(1, full);
	mu_label(ui.ctx, buf);
}


static void file_field(char **field, const char *title, bool clear_button,
	bool rom)
{
	int widths[3] = { -CH(clear_button ? 20 : 10), CH(9), CH(8) };

	mu_push_id(ui.ctx, &field, sizeof(field));
	row(clear_button ? 3 : 2, widths);
	text_field(field);
	if (mu_button(ui.ctx, "Browse..."))
		pick_file(field, title, rom);
	if (clear_button && mu_button(ui.ctx, "Clear")) {
		machine_config_set(field, "");
		edits_clear();
	}
	mu_pop_id(ui.ctx);
}


/****************************************************************************/

/*
 *  The list of machines
 */

static void list_free(void)
{
	int i;

	for (i = 0; i < ui.n_recent; i++) {
		free(ui.recent[i]);
		machine_config_free(ui.recent_cfg[i]);
		free(ui.recent_err[i]);
	}
	free(ui.recent);
	free(ui.recent_cfg);
	free(ui.recent_err);
	ui.recent = NULL;
	ui.recent_cfg = NULL;
	ui.recent_err = NULL;
	ui.n_recent = 0;
}


static void list_load(const char *select_path)
{
	int i;

	list_free();
	ui.n_recent = appdirs_recent(&ui.recent);
	CHECK_ALLOCATION(ui.recent_cfg = (struct machine_config **) calloc(
	    ui.n_recent + 1, sizeof(struct machine_config *)));
	CHECK_ALLOCATION(ui.recent_err = (char **) calloc(ui.n_recent + 1,
	    sizeof(char *)));
	for (i = 0; i < ui.n_recent; i++)
		ui.recent_cfg[i] = machine_config_load(ui.recent[i],
		    &ui.recent_err[i]);

	if (ui.selected >= ui.n_recent)
		ui.selected = ui.n_recent - 1;
	if (ui.selected < 0)
		ui.selected = 0;
	for (i = 0; select_path != NULL && i < ui.n_recent; i++)
		if (strcmp(ui.recent[i], select_path) == 0)
			ui.selected = i;
}


static void open_editor(struct machine_config *c, bool is_new)
{
	int i;

	machine_config_free(ui.cfg);
	ui.cfg = c;
	ui.is_new = is_new;
	ui.fresh = false;
	ui.open_list = NULL;

	ui.page = PAGE_GENERAL;
	ui.start_after_save = false;
	for (i = 0; i < 64; i++) {
		ui.disk_mb[i] = DEFAULT_DISK_MB;
		ui.custom[i] = false;
	}
	edits_clear();
	ui.screen = SCREEN_EDITOR;
	free(ui.message);
	ui.message = NULL;
}


static void opened(struct machine *m, const char *path, void *data)
{
	char *err;
	struct machine_config *c;

	(void) m; (void) data;
	ui.pick_open = false;
	if (path == NULL)
		return;
	c = machine_config_load(path, &err);
	if (c == NULL) {
		set_message(true, "%s", err);
		free(err);
		return;
	}
	appdirs_add_recent(c->path);
	list_load(c->path);
	machine_config_free(c);
	set_message(false, "Added %s", path);
}


/*  Starts a machine: checks it, and returns it from config_ui_run():  */
static void start(struct machine_config *c)
{
	char *err = machine_config_check(c);

	if (err != NULL) {
		set_message(true, "Can't start: %s", err);
		free(err);
		return;
	}
	if (c->path != NULL)
		appdirs_add_recent(c->path);
	ui.result = machine_config_copy(c);
}


static void new_machine(const char *path);
static void open_settings(void);

static void screen_list(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *sel = ui.selected < ui.n_recent ?
	    ui.recent_cfg[ui.selected] : NULL;
	int i;

	int cols[2] = { CH(40), -1 };
	mu_layout_row(ctx, 2, cols, -row_height() * 2 - ctx->style->padding);

	/*  The machines:  */
	mu_begin_panel(ctx, "machines");
	int full[1] = { -1 };
	row(1, full);
	if (ui.n_recent == 0) {
		mu_label(ctx, "No machines yet.");
		mu_text(ctx, "Create one with New..., or add an existing "
		    "machine.json with Open....");
	}
	for (i = 0; i < ui.n_recent; i++) {
		const char *name = ui.recent_cfg[i] != NULL &&
		    ui.recent_cfg[i]->name[0] ? ui.recent_cfg[i]->name :
		    ui.recent[i];
		if (list_item(&ui.recent[i], sizeof(ui.recent[i]), name,
		    i == ui.selected))
			ui.selected = i;
	}
	mu_end_panel(ctx);

	/*  Details of the selected one:  */
	mu_begin_panel(ctx, "details");
	row(1, full);
	if (ui.selected < ui.n_recent) {
		if (sel != NULL) {
			char *summary = machine_config_summary(sel);
			mu_label(ctx, sel->name[0] ? sel->name : "(no name)");
			mu_text(ctx, summary);
			free(summary);
		} else {
			mu_label(ctx, "Can't read this configuration:");
			mu_text(ctx, ui.recent_err[ui.selected]);
		}
		mu_label(ctx, "");
		mu_text(ctx, ui.recent[ui.selected]);
	}
	mu_end_panel(ctx);

	/*  The buttons:  */
	int bw[8] = { CH(10), CH(10), CH(8), CH(12), CH(9), CH(13), -CH(12),
	    -1 };
	row(8, bw);
	if (mu_button(ctx, "New..."))
		new_machine(NULL);
	if (mu_button(ctx, "Open...")) {
		ui.pick_open = true;
		display_choose_file(NULL, "Open a machine configuration "
		    "(machine.json)", appdirs_last_dir() != NULL ?
		    appdirs_last_dir() : appdirs_machines_dir(), opened, NULL);
		if (!display_chooser_open())
			ui.pick_open = false;
	}
	if (mu_button(ctx, "Edit") && sel != NULL)
		open_editor(machine_config_copy(sel), false);
	if (mu_button(ctx, "Duplicate") && sel != NULL) {
		struct machine_config *c = machine_config_copy(sel);
		char name[300];
		snprintf(name, sizeof(name), "%s (copy)", c->name);
		machine_config_set(&c->name, name);
		machine_config_set(&c->path, NULL);
		open_editor(c, true);
	}
	if (mu_button(ctx, "Forget") && ui.selected < ui.n_recent) {
		appdirs_remove_recent(ui.recent[ui.selected]);
		list_load(NULL);
		set_message(false, "Removed from the list (the files are "
		    "still there)");
	}
	if (mu_button(ctx, "Settings..."))
		open_settings();
	mu_label(ctx, "");
	if (mu_button(ctx, "Start") && sel != NULL)
		start(sel);
}


/****************************************************************************/

/*
 *  The settings: where ROM images are looked for
 */

static void rom_dirs_free(void)
{
	int i;

	for (i = 0; i < ui.n_rom_dirs; i++)
		free(ui.rom_dirs[i]);
	free(ui.rom_dirs);
	ui.rom_dirs = NULL;
	ui.n_rom_dirs = 0;
}

static void rom_dirs_save(void)
{
	appdirs_set_rom_dirs(ui.n_rom_dirs, ui.rom_dirs);
}

static void open_settings(void)
{
	char **dirs;
	int n = appdirs_rom_dirs(&dirs), i;

	/*  (The first is the common folder, which isn't edited here.)  */
	rom_dirs_free();
	for (i = 0; i < n; i++) {
		if (i > 0)
			machine_config_add_string(&ui.n_rom_dirs,
			    &ui.rom_dirs, dirs[i]);
		free(dirs[i]);
	}
	free(dirs);
	edits_clear();
	free(ui.message);
	ui.message = NULL;
	ui.screen = SCREEN_SETTINGS;
}

/*  A ROM folder was chosen: for ROM folder data - 1, a new one (data 0),
    or the common one (data -1):  */
static void rom_dir_picked(struct machine *m, const char *path, void *data)
{
	int i = (int) (intptr_t) data - 1;

	(void) m;
	ui.pick_open = false;
	if (path == NULL)
		return;
	if (i == -2) {
		appdirs_set_roms_dir(path);
		return;
	}
	if (i >= 0 && i < ui.n_rom_dirs)
		machine_config_set(&ui.rom_dirs[i], path);
	else
		machine_config_add_string(&ui.n_rom_dirs, &ui.rom_dirs, path);
	edits_clear();
	rom_dirs_save();
}

static void pick_rom_dir(int i)
{
	ui.pick_open = true;
	display_choose_folder(NULL, "Choose a folder with ROM images",
	    NULL, rom_dir_picked, (void *) (intptr_t) (i + 1));
	if (!display_chooser_open())
		ui.pick_open = false;
}

static void screen_settings(void)
{
	mu_Context *ctx = ui.ctx;
	int full[1] = { -1 };
	int i;

	mu_layout_row(ctx, 1, full, -row_height() * 2 - ctx->style->padding);
	mu_begin_panel(ctx, "settings");
	row(1, full);
	mu_label(ctx, "ROM images");
	mu_text(ctx, "A ROM image given by name (e.g. a system ROM, or an "
	    "option ROM) is looked for in the machine's folder, then in the "
	    "common ROM folder, and then in the other folders listed here.  "
	    "A ROM image chosen in one of these folders is kept by its name, "
	    "so machines can share them.");
	mu_label(ctx, "");
	int cw[4] = { CH(16), -CH(22), CH(11), CH(9) };
	row(4, cw);
	mu_label(ctx, "Common folder:");
	mu_label(ctx, appdirs_roms_dir());
	if (mu_button(ctx, "Browse..."))
		pick_rom_dir(-2);
	if (mu_button(ctx, "Default"))
		appdirs_set_roms_dir(NULL);

	for (i = 0; i < ui.n_rom_dirs; i++) {
		int w[4] = { CH(16), -CH(22), CH(11), CH(9) };
		mu_push_id(ctx, &i, sizeof(i));
		row(4, w);
		mu_label(ctx, i == 0 ? "Other folders:" : "");
		{
			struct edit *e = edit_for(&ui.rom_dirs[i],
			    ui.rom_dirs[i]);
			if (mu_textbox_ex(ctx, e->buf, sizeof(e->buf), 0) &
			    MU_RES_CHANGE) {
				machine_config_set(&ui.rom_dirs[i], e->buf);
				rom_dirs_save();
			}
		}
		if (mu_button(ctx, "Browse..."))
			pick_rom_dir(i);
		if (mu_button(ctx, "Remove")) {
			machine_config_remove_string(&ui.n_rom_dirs,
			    &ui.rom_dirs, i);
			edits_clear();
			rom_dirs_save();
			mu_pop_id(ctx);
			break;
		}
		mu_pop_id(ctx);
	}
	int aw[2] = { CH(16), CH(14) };
	row(2, aw);
	mu_label(ctx, "");
	if (mu_button(ctx, "Add folder..."))
		pick_rom_dir(-1);
	mu_end_panel(ctx);

	int bw[1] = { CH(10) };
	row(1, bw);
	if (mu_button(ctx, "Back")) {
		rom_dirs_save();
		ui.screen = SCREEN_LIST;
	}
}


/****************************************************************************/

/*
 *  The editor
 */

static void make_dir(const char *path)
{
	char *s, *p;

	CHECK_ALLOCATION(s = strdup(path));
	for (p = s + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
#ifdef _WIN32
			mkdir(s);
#else
			mkdir(s, 0755);
#endif
			*p = '/';
		}
#ifdef _WIN32
	mkdir(s);
#else
	mkdir(s, 0755);
#endif
	free(s);
}


/*  The real drive that a drive says it is, if any:  */
static const struct mc_drive_model *drive_model(const struct mc_drive *d)
{
	const struct mc_drive_model *models;
	int n = machine_drive_models(ui.cfg->type, &models), i;

	for (i = 0; i < n; i++)
		if (models[i].kind == d->kind &&
		    strcmp(models[i].vendor, d->vendor) == 0 &&
		    strcmp(models[i].product, d->product) == 0 &&
		    strcmp(models[i].revision, d->revision) == 0)
			return &models[i];
	return NULL;
}

/*  The size of a new disk image: the real drive's, or the one given:  */
static int64_t disk_size(int i)
{
	const struct mc_drive_model *model = drive_model(&ui.cfg->drives[i]);

	if (model != NULL && model->sectors > 0)
		return model->sectors * 512;
	return (int64_t) ui.disk_mb[i < 64 ? i : 63] * 1048576;
}


/*  Creates the disk images that don't exist yet; returns false on error:  */
static bool create_missing_disks(void)
{
	struct machine_config *c = ui.cfg;
	struct stat st;
	char *err, *f;
	int i;

	for (i = 0; i < c->n_drives; i++) {
		if (c->drives[i].kind != MC_DRIVE_DISK ||
		    c->drives[i].file[0] == '\0')
			continue;
		f = machine_config_resolve(c, c->drives[i].file);
		if (stat(f, &st) != 0 && !machine_config_create_disk(f,
		    disk_size(i), &err)) {
			set_message(true, "%s", err);
			free(err);
			free(f);
			return false;
		}
		free(f);
	}
	return true;
}


/*  Saves the configuration in a file, and creates its missing disks:  */
static bool save_to(const char *path)
{
	struct machine_config *c = ui.cfg;
	char *err;

	if (!machine_config_save(c, path, &err)) {
		set_message(true, "%s", err);
		free(err);
		return false;
	}

	edits_clear();
	if (!create_missing_disks())
		return false;
	appdirs_add_recent(c->path);
	list_load(c->path);
	ui.is_new = false;
	ui.fresh = false;
	set_message(false, "Saved %s", c->path);
	return true;
}


/*
 *  A new machine's disk images are created where it is saved: if one with
 *  the same name is there already (another machine's), the new one gets
 *  another name (e.g. disk-2.img), instead of using it.
 */
static void new_disk_names(const char *file)
{
	struct machine_config *c = ui.cfg;
	struct stat st;
	char *dir, *slash;
	int i, k;

	if (c->dir != NULL)
		return;
	CHECK_ALLOCATION(dir = strdup(file));
	slash = strrchr(dir, '/');
	if (slash == NULL) {
		free(dir);
		return;
	}
	*slash = '\0';

	for (i = 0; i < c->n_drives; i++) {
		struct mc_drive *d = &c->drives[i];
		char path[2000], name[1000];
		const char *dot;

		if (d->kind != MC_DRIVE_DISK || d->file[0] == '\0' ||
		    d->file[0] == '/' || strchr(d->file, ':') != NULL)
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, d->file);
		if (stat(path, &st) != 0)
			continue;
		dot = strrchr(d->file, '.');
		if (dot == NULL || strchr(dot, '/') != NULL)
			dot = d->file + strlen(d->file);
		for (k = 2; k < 1000; k++) {
			snprintf(name, sizeof(name), "%.*s-%i%s",
			    (int) (dot - d->file), d->file, k, dot);
			snprintf(path, sizeof(path), "%s/%s", dir, name);
			if (stat(path, &st) != 0)
				break;
		}
		machine_config_set(&d->file, name);
	}
	free(dir);
}


/*
 *  Where a new machine was chosen to be saved: a folder for it (with its
 *  machine.json, and the disk images created for it), or a .json file.
 */
static void saved_as(struct machine *m, const char *path, void *data)
{
	struct stat st;
	char *file;
	size_t n;

	(void) m; (void) data;
	ui.pick_open = false;
	if (path == NULL)
		return;

	n = strlen(path);
	if (n > 5 && strcasecmp(path + n - 5, ".json") == 0) {
		CHECK_ALLOCATION(file = strdup(path));
	} else {
		if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
			set_message(true, "%s is a file; choose a name for a "
			    "folder for the machine, or a .json file", path);
			return;
		}
		make_dir(path);
		CHECK_ALLOCATION(file = (char *) malloc(n + 20));
		sprintf(file, "%s/machine.json", path);
		if (stat(file, &st) == 0) {
			set_message(true, "%s already has a machine.json; "
			    "choose another name", path);
			free(file);
			return;
		}
	}

	new_disk_names(file);
	if (save_to(file) && ui.start_after_save)
		start(ui.cfg);
	free(file);
}


/*
 *  Saves the configuration.  A new one is saved where the user chooses
 *  (later, so this returns false); then, with then_start, it is started.
 */
static bool save(bool then_start)
{
	struct machine_config *c = ui.cfg;
	char name[200];
	const char *dir = appdirs_last_dir();
	size_t i;

	if (c->path != NULL)
		return save_to(c->path);

	/*  The machine's name, as a file name:  */
	snprintf(name, sizeof(name), "%s", c->name[0] ? c->name : "machine");
	for (i = 0; name[i] != '\0'; i++)
		if (name[i] == '/' || name[i] == '\\' || name[i] == ':')
			name[i] = '-';

	if (dir == NULL) {
		make_dir(appdirs_machines_dir());
		dir = appdirs_machines_dir();
	}
	ui.start_after_save = then_start;
	ui.pick_open = true;
	display_choose_new_file(NULL, "Save the machine as (a folder for it, "
	    "or a .json file)", dir, name, saved_as, NULL);
	if (!display_chooser_open())
		ui.pick_open = false;
	return false;
}


/*
 *  The machine types, by architecture, and their presets:
 */

#define	N_ARCHS		10
static const char *arch_names[N_ARCHS] = { "Other", "MIPS", "PowerPC", NULL,
	"Alpha", "ARM", "SuperH", "Motorola 88K", "i960", "RISC-V" };

static int entry_arch(const struct machine_entry *me)
{
	int a = me->machine_type / 1000;
	return a > 0 && a < N_ARCHS && arch_names[a] != NULL ? a : 0;
}

static bool has_alias(char **aliases, int n, const char *name)
{
	int i;
	for (i = 0; i < n; i++)
		if (strcasecmp(aliases[i], name) == 0)
			return true;
	return false;
}

/*  The preset of a machine type and model (sub is NULL if it has none):  */
static const struct machine_preset *preset_for(const struct machine_entry *me,
	const struct machine_entry_subtype *sub)
{
	const struct machine_preset *presets;
	int n = machine_presets(&presets), i;

	for (i = 0; i < n; i++)
		if (has_alias(me->aliases, me->n_aliases, presets[i].type) &&
		    (sub == NULL ? presets[i].model[0] == '\0' :
		    has_alias(sub->aliases, sub->n_aliases, presets[i].model)))
			return &presets[i];
	return NULL;
}

static bool sub_shown(const struct machine_entry *me,
	const struct machine_entry_subtype *sub,
	const struct machine_entry_subtype *cur_sub)
{
	return ui.show_all || sub == cur_sub || preset_for(me, sub) != NULL;
}

static bool entry_shown(const struct machine_entry *me,
	const struct machine_entry *cur)
{
	int i;

	if (ui.show_all || me == cur || preset_for(me, NULL) != NULL)
		return true;
	for (i = 0; i < me->n_subtypes; i++)
		if (preset_for(me, me->subtype[i]) != NULL)
			return true;
	return false;
}

/*  The model chosen with a machine type: the first one with a preset:  */
static struct machine_entry_subtype *default_sub(struct machine_entry *me)
{
	int i;

	for (i = 0; i < me->n_subtypes; i++)
		if (preset_for(me, me->subtype[i]) != NULL)
			return me->subtype[i];
	return me->n_subtypes > 0 ? me->subtype[0] : NULL;
}

/*  The first machine type of an architecture, preferably with a preset:  */
static struct machine_entry *default_entry(int arch)
{
	struct machine_entry *me, *any = NULL;

	for (me = first_machine_entry; me != NULL; me = me->next) {
		if (entry_arch(me) != arch || !entry_shown(me, NULL))
			continue;
		if (preset_for(me, default_sub(me)) != NULL)
			return me;
		if (any == NULL)
			any = me;
	}
	return any;
}

/*
 *  Chooses a machine type and model.  A new machine also gets its defaults
 *  (from the preset, or none), keeping a name that the user typed.
 */
static void select_machine(struct machine_entry *me,
	struct machine_entry_subtype *sub)
{
	struct machine_config *c = ui.cfg;

	if (ui.fresh) {
		const struct machine_preset *p = preset_for(me, sub);
		struct machine_config *n;
		char *err = NULL;

		n = machine_config_parse(p != NULL ? p->json :
		    "{ \"gxemul\": 1 }", NULL, &err);
		free(err);
		if (n != NULL) {
			if (ui.auto_name != NULL &&
			    strcmp(c->name, ui.auto_name) != 0)
				machine_config_set(&n->name, c->name);
			else if (p == NULL)
				machine_config_set(&n->name, sub != NULL ?
				    sub->name : me->name);
			machine_config_set(&ui.auto_name, n->name);
			machine_config_set(&n->path, c->path);
			machine_config_set(&n->dir, c->dir);
			machine_config_free(c);
			ui.cfg = c = n;
		}
	}
	machine_config_set(&c->type, me->aliases[0]);
	machine_config_set(&c->model, sub != NULL ? sub->aliases[0] : "");
	edits_clear();
}

/*  Starts a new machine (to be saved at path, or in a directory of its
    own if path is NULL), with the defaults of the first preset:  */
static void new_machine(const char *path)
{
	const struct machine_preset *presets;
	struct machine_config *c;
	char *err = NULL, *abs;

	/*  (With an absolute path, file names are relative to its
	    directory.)  */
	machine_presets(&presets);
	abs = path != NULL ? machine_config_clean_path(path) : NULL;
	c = machine_config_parse(presets[0].json, abs, &err);
	free(abs);
	if (c == NULL) {
		set_message(true, "%s", err);
		free(err);
		return;
	}
	open_editor(c, path == NULL);
	ui.fresh = true;
	machine_config_set(&ui.auto_name, c->name);
}


static void page_general(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	struct machine_entry *me, *cur = NULL;
	struct machine_entry_subtype *cur_sub = NULL;
	const struct machine_preset *preset = NULL;
	const char *labels[200];
	void *items[200];
	int i, n, sel;

	/*  The machine type and model chosen:  */
	for (me = first_machine_entry; me != NULL; me = me->next)
		if (has_alias(me->aliases, me->n_aliases, c->type))
			cur = me;
	for (i = 0; cur != NULL && i < cur->n_subtypes; i++)
		if (has_alias(cur->subtype[i]->aliases,
		    cur->subtype[i]->n_aliases, c->model))
			cur_sub = cur->subtype[i];
	if (cur != NULL)
		preset = preset_for(cur, cur_sub);

	int lw[2] = { CH(16), CH(48) };
	row(2, lw);
	mu_label(ctx, "Name");
	text_field(&c->name);

	/*  Architecture:  */
	mu_label(ctx, "Architecture");
	n = 0;
	sel = -1;
	for (i = 1; i <= N_ARCHS; i++) {
		int a = i % N_ARCHS;	/*  "Other" last  */
		bool any = false;
		if (arch_names[a] == NULL)
			continue;
		for (me = first_machine_entry; me != NULL; me = me->next)
			if (entry_arch(me) == a && entry_shown(me, cur))
				any = true;
		if (!any)
			continue;
		if (cur != NULL && entry_arch(cur) == a)
			sel = n;
		items[n] = (void *) (size_t) a;
		labels[n++] = arch_names[a];
	}
	if (dropdown("arch", labels, n, &sel)) {
		me = default_entry((int) (size_t) items[sel]);
		if (me != NULL && me != cur) {
			select_machine(me, default_sub(me));
			return;
		}
	}

	/*  Machine type, of that architecture:  */
	mu_label(ctx, "Machine");
	n = 0;
	sel = -1;
	for (me = first_machine_entry; me != NULL && n < 200; me = me->next) {
		if (cur == NULL || entry_arch(me) != entry_arch(cur) ||
		    !entry_shown(me, cur))
			continue;
		if (me == cur)
			sel = n;
		items[n] = me;
		labels[n++] = me->name;
	}
	if (dropdown("type", labels, n, &sel) && items[sel] != cur) {
		me = (struct machine_entry *) items[sel];
		select_machine(me, default_sub(me));
		return;
	}

	/*  Model, if the machine type has models:  */
	mu_label(ctx, "Model");
	n = 0;
	sel = -1;
	for (i = 0; cur != NULL && i < cur->n_subtypes && n < 200; i++) {
		if (!sub_shown(cur, cur->subtype[i], cur_sub))
			continue;
		if (cur->subtype[i] == cur_sub)
			sel = n;
		items[n] = cur->subtype[i];
		labels[n++] = cur->subtype[i]->name;
	}
	if (n == 0)
		mu_label(ctx, cur == NULL ? "" : "(this machine has no models)");
	else if (dropdown("model", labels, n, &sel) && items[sel] != cur_sub) {
		select_machine(cur, (struct machine_entry_subtype *)
		    items[sel]);
		return;
	}

	mu_label(ctx, "");
	check("Show all machines, also untested ones", &ui.show_all);

	int lw2[2] = { CH(16), -1 };
	row(2, lw2);
	mu_label(ctx, "Memory (MB)");
	int_field(&c->memory_mb);
	mu_label(ctx, "");
	mu_label(ctx, "0 = the machine's default");
	mu_label(ctx, "CPU");
	text_field(&c->cpu);
	mu_label(ctx, "");
	mu_label(ctx, "empty = the machine's default (see gxemul -H)");
	mu_label(ctx, "CPUs");
	int_field(&c->ncpus);

	int full[1] = { -1 };
	row(1, full);
	mu_label(ctx, "");
	if (preset != NULL)
		mu_text(ctx, preset->description);
	else if (cur != NULL)
		mu_text(ctx, "This machine has no preset: it is untested here, "
		    "and may not work.");
	if (ui.fresh)
		mu_text(ctx, "Choosing another machine or model gives this "
		    "configuration its defaults (until it is saved).");
}


/*
 *  Option ROM images for the TURBOchannel cards, which a real system ROM
 *  needs to use them; also those that have no card (any more), to clear.
 */
static void option_roms(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	struct mc_slot slots[MC_MAX_SLOTS];
	int n = machine_config_slots(c, slots), i, k;
	int full[1] = { -1 };

	row(1, full);
	mu_label(ctx, "");
	if (n == 0)
		mu_label(ctx, "No TURBOchannel cards need option ROM images "
		    "(a framebuffer needs the display on).");
	else if (n > 0)
		mu_label(ctx, "Option ROM images of the TURBOchannel cards "
		    "(empty = gxemul's built-in one):");

	for (i = 0; i < MC_MAX_SLOTS; i++) {
		const char *device = NULL;
		char label[100];

		for (k = 0; k < n; k++)
			if (slots[k].slot == i)
				device = slots[k].device;
		if (device == NULL && (c->slot_rom[i] == NULL ||
		    c->slot_rom[i][0] == '\0'))
			continue;

		row(1, full);
		snprintf(label, sizeof(label), "Slot %i: %s", i, device != NULL ?
		    device : "no card (clear this)");
		mu_label(ctx, label);
		if (c->slot_rom[i] == NULL)
			machine_config_set(&c->slot_rom[i], "");
		file_field(&c->slot_rom[i], "Choose an option ROM image",
		    true, true);
		rom_found(c->slot_rom[i]);
	}
}


/*
 *  The file that keeps the RTC's NVRAM (the firmware's settings), and a
 *  way to reset it: without the file, the machine starts with gxemul's
 *  defaults, and the file is made when the firmware changes them.
 */
static void nvram_field(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	struct stat st;
	char *f;
	int full[1] = { -1 };
	int w[3] = { CH(16), -CH(18), CH(17) };

	row(1, full);
	mu_label(ctx, "");
	row(3, w);
	mu_label(ctx, "NVRAM file");
	text_field(&c->nvram);
	f = c->nvram[0] ? machine_config_resolve(c, c->nvram) : NULL;
	if (f != NULL && c->dir != NULL && stat(f, &st) == 0) {
		if (mu_button(ctx, "Reset NVRAM")) {
			if (remove(f) == 0)
				set_message(false, "The NVRAM is reset: the "
				    "machine starts with the defaults%s",
				    ui.running ? " when it is restarted" : "");
			else
				set_message(true, "Could not remove %s", f);
		}
	} else
		mu_label(ctx, c->nvram[0] ? "(not made yet)" : "(not kept)");
	free(f);
	row(1, full);
	mu_label(ctx, "The firmware's settings (e.g. the PROM's boot and "
	    "console variables) are kept in this file; empty = not kept.");
}


static void page_firmware(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	int i;
	int full[1] = { -1 };
	int lw[2] = { CH(16), -1 };

	/*  A machine with a known system ROM runs either that, or PROM
	    emulation (and then boots a kernel itself):  */
	if (machine_config_rom_address(c->type) != 0) {
		row(1, full);
		check("PROM emulation (gxemul provides the firmware, and boots "
		    "a kernel itself)", &c->prom_emulation);
		mu_label(ctx, "");
	}
	if (machine_config_uses_rom(c)) {
		row(1, full);
		mu_label(ctx, "System ROM image:");
		file_field(&c->rom, "Choose the system ROM image", true, true);
		rom_found(c->rom);
		row(1, full);
		check("Skip the self-tests that fail only in gxemul (KN02-BA "
		    "V5.7j: cache, rtc/time; PMAG-BA V5.3a: curs, ptrn)",
		    &c->skip_failing_tests);
		option_roms();
		nvram_field();
		row(1, full);
		mu_label(ctx, "");
		check("Start paused, in the debugger", &c->start_paused);
		return;
	}

	row(1, full);
	mu_label(ctx, "Files to load (a kernel or program; prefix "
	    "\"address:\" for raw images):");
	for (i = 0; i < c->n_load; i++) {
		int w[3] = { -CH(20), CH(9), CH(8) };
		mu_push_id(ctx, &i, sizeof(i));
		row(3, w);
		text_field(&c->load[i]);
		if (mu_button(ctx, "Browse..."))
			pick_file(&c->load[i], "Choose a file to load", false);
		if (mu_button(ctx, "Remove")) {
			machine_config_remove_string(&c->n_load, &c->load, i);
			edits_clear();
			mu_pop_id(ctx);
			break;
		}
		mu_pop_id(ctx);
	}
	int aw[1] = { CH(14) };
	row(1, aw);
	if (mu_button(ctx, "Add file")) {
		machine_config_add_string(&c->n_load, &c->load, "");
		edits_clear();
	}

	row(2, lw);
	mu_label(ctx, "");
	mu_label(ctx, "");
	mu_label(ctx, "Boot file name");
	text_field(&c->bootname);
	mu_label(ctx, "");
	mu_label(ctx, "e.g. vmunix or netbsd (PROM emulation); empty = default");

	bool has_arg = c->bootarg != NULL;
	row(1, full);
	check("Boot argument:", &has_arg);
	if (has_arg && c->bootarg == NULL)
		machine_config_set(&c->bootarg, "");
	if (!has_arg && c->bootarg != NULL) {
		machine_config_set(&c->bootarg, NULL);
		edits_clear();
	}
	if (has_arg) {
		row(2, lw);
		mu_label(ctx, "");
		text_field(&c->bootarg);
	}

	row(1, full);
	check("Boot from the network (DECstation, SGI, ARC)",
	    &c->force_netboot);
	nvram_field();
	row(1, full);
	mu_label(ctx, "");
	check("Start paused, in the debugger", &c->start_paused);
}


/*
 *  What a drive says it is: gxemul's default, a real one (which
 *  also gives a new disk image its size), or strings given.
 */
static void drive_identity(int i)
{
	struct machine_config *c = ui.cfg;
	struct mc_drive *d = &c->drives[i];
	const struct mc_drive_model *models, *model = drive_model(d);
	int n = machine_drive_models(c->type, &models), k, nl = 0, sel;
	const char *labels[64];
	const struct mc_drive_model *items[64];
	static char names[64][40];
	char name[40];

	labels[nl] = "Default";
	items[nl++] = NULL;
	for (k = 0; k < n && nl < 62; k++) {
		if (models[k].kind != d->kind)
			continue;
		if (models[k].sectors > 0)
			snprintf(names[nl], sizeof(names[nl]), "%s, %lli MB",
			    models[k].name, (long long) models[k].sectors *
			    512 / 1000000);
		else
			snprintf(names[nl], sizeof(names[nl]), "%s",
			    models[k].name);
		labels[nl] = names[nl];
		items[nl++] = &models[k];
	}
	labels[nl++] = "Other...";

	bool custom = i < 64 && ui.custom[i];
	sel = 0;
	if (custom || (model == NULL && (d->vendor[0] || d->product[0] ||
	    d->revision[0])))
		sel = nl - 1;
	else
		for (k = 1; k < nl - 1; k++)
			if (items[k] == model)
				sel = k;

	snprintf(name, sizeof(name), "model%i", i);
	if (dropdown(name, labels, nl, &sel)) {
		if (sel == nl - 1) {
			if (i < 64)
				ui.custom[i] = true;
		} else {
			const struct mc_drive_model *m = items[sel];
			machine_config_set(&d->vendor, m ? m->vendor : "");
			machine_config_set(&d->product, m ? m->product : "");
			machine_config_set(&d->revision, m ? m->revision : "");
			if (i < 64)
				ui.custom[i] = false;
			edits_clear();
		}
	}
}


static void page_drives(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	bool has_ide = machine_config_has_ide(c->type);
	struct stat st;
	int i;
	int full[1] = { -1 };

	for (i = 0; i < c->n_drives; i++) {
		struct mc_drive *d = &c->drives[i];
		const struct mc_drive_model *model;
		char name[40], *f;
		bool missing, scsi;

		mu_push_id(ctx, &i, sizeof(i));
		int w1[6] = { CH(10), CH(3), CH(5), -CH(22), CH(11), CH(9) };
		row(6, w1);
		snprintf(name, sizeof(name), "kind%i", i);
		dropdown(name, kind_labels, 4, &d->kind);
		mu_label(ctx, "ID");
		{
			char idtext[12];
			struct edit *e;
			snprintf(idtext, sizeof(idtext), "%i", d->id);
			e = edit_for(&d->id, d->id < 0 ? "" : idtext);
			if (mu_textbox_ex(ctx, e->buf, sizeof(e->buf), 0) &
			    MU_RES_CHANGE)
				d->id = e->buf[0] ? atoi(e->buf) : -1;
		}
		text_field(&d->file);
		if (mu_button(ctx, "Browse..."))
			pick_file(&d->file, "Choose a drive image", false);
		if (mu_button(ctx, "Remove")) {
			machine_config_remove_drive(c, i);
			memmove(&ui.custom[i < 63 ? i : 63],
			    &ui.custom[i < 63 ? i + 1 : 63],
			    sizeof(bool) * (i < 63 ? 63 - i : 0));
			edits_clear();
			mu_pop_id(ctx);
			break;
		}

		f = machine_config_resolve(c, d->file);
		missing = d->kind == MC_DRIVE_DISK && d->file[0] != '\0' &&
		    stat(f, &st) != 0;
		free(f);

		/*  The bus (only where there is a choice), what the drive
		    says it is (on SCSI), and its flags:  */
		scsi = d->kind != MC_DRIVE_FLOPPY && (d->bus == MC_BUS_SCSI ||
		    (d->bus == MC_BUS_DEFAULT && !has_ide));
		int w2[6] = { CH(10), has_ide || d->bus == MC_BUS_IDE ?
		    CH(14) : CH(6), CH(24), CH(13), CH(11), CH(8) };
		row(6, w2);
		mu_label(ctx, "");
		snprintf(name, sizeof(name), "bus%i", i);
		if (has_ide || d->bus == MC_BUS_IDE)
			dropdown(name, bus_labels, 3, &d->bus);
		else
			mu_label(ctx, d->kind == MC_DRIVE_FLOPPY ? "" : "SCSI");
		if (scsi)
			drive_identity(i);
		else
			mu_label(ctx, "");
		check("Read-only", &d->readonly);
		check("Overlay", &d->overlay);
		check("Boot", &d->boot);

		/*  The strings, if not those of the usual or a real drive:  */
		model = drive_model(d);
		if (scsi && ((i < 64 && ui.custom[i]) || (model == NULL &&
		    (d->vendor[0] || d->product[0] || d->revision[0])))) {
			int w3[7] = { CH(10), CH(7), CH(10), CH(8), CH(18),
			    CH(9), CH(6) };
			row(7, w3);
			mu_label(ctx, "");
			mu_label(ctx, "Vendor");
			text_field(&d->vendor);
			mu_label(ctx, "Product");
			text_field(&d->product);
			mu_label(ctx, "Revision");
			text_field(&d->revision);
		}

		/*  A disk image to create:  */
		if (missing && i < 64) {
			int w4[4] = { CH(10), CH(30), CH(10), CH(14) };
			row(4, w4);
			mu_label(ctx, "");
			if (model != NULL && model->sectors > 0) {
				char size[60];
				snprintf(size, sizeof(size), "New image: %lli "
				    "sectors", (long long) model->sectors);
				mu_label(ctx, size);
				mu_label(ctx, "");
			} else {
				mu_label(ctx, "New image, size (MB):");
				int_field(&ui.disk_mb[i]);
			}
			if (c->dir == NULL)
				mu_label(ctx, "(created when saved)");
			else if (mu_button(ctx, "Create now")) {
				char *err;
				make_dir(c->dir);
				f = machine_config_resolve(c, d->file);
				if (machine_config_create_disk(f,
				    disk_size(i), &err))
					set_message(false, "Created %s", f);
				else {
					set_message(true, "%s", err);
					free(err);
				}
				free(f);
			}
		}
		row(1, full);
		mu_label(ctx, "");
		mu_pop_id(ctx);
	}

	int bw[4] = { CH(12), CH(14), CH(12), CH(14) };
	row(4, bw);
	if (mu_button(ctx, "Add disk")) {
		machine_config_add_drive(c, "", MC_DRIVE_DISK, -1);
		edits_clear();
	}
	if (mu_button(ctx, "Add CD-ROM")) {
		machine_config_add_drive(c, "", MC_DRIVE_CDROM, -1);
		edits_clear();
	}
	if (mu_button(ctx, "Add tape")) {
		machine_config_add_drive(c, "", MC_DRIVE_TAPE, -1);
		edits_clear();
	}
	if (mu_button(ctx, "Add floppy")) {
		machine_config_add_drive(c, "", MC_DRIVE_FLOPPY, -1);
		edits_clear();
	}

	row(1, full);
	mu_label(ctx, "");
	mu_text(ctx, "An empty ID is the next free one.  A CD-ROM or tape drive "
	    "may start empty, and media can be changed in the running "
	    "machine's Media menu.  Overlay keeps the image unchanged: writes "
	    "go to a temporary file.  A drive can say it is a real one (the "
	    "SCSI INQUIRY vendor, product, and revision), which for a disk "
	    "also gives a new image that drive's size.  A disk image that "
	    "doesn't exist yet is created when the machine is saved.");
}


static void page_display(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	int full[1] = { -1 };
	int lw[2] = { CH(22), CH(34) };
	static const char *scale_labels[] = { "Fit the screen",
	    "Actual size (100%)", "Half size (50%)", "Double size (200%)" };
	static const int scale_values[] = { 0, 100, 50, 200 };
	static const char *card_labels[] = { "1", "2", "3" };
	int i, sel, max_cards = machine_config_max_gfx_cards(c);

	row(1, full);
	check("Framebuffer window (graphics, keyboard and mouse)", &c->display);
	if (c->display) {
		row(2, lw);
		mu_label(ctx, "Window size");
		sel = -1;
		for (i = 0; i < 4; i++)
			if (c->scale == scale_values[i])
				sel = i;
		if (dropdown("scale", scale_labels, 4, &sel))
			c->scale = scale_values[sel];

		if (max_cards > 1) {
			mu_label(ctx, "Graphics cards");
			sel = (c->gfx_cards < 1 ? 1 : c->gfx_cards) - 1;
			if (dropdown("cards", card_labels, max_cards, &sel))
				c->gfx_cards = sel + 1;
			mu_label(ctx, "");
			mu_label(ctx, "(each with a window of its own)");
		}
		row(1, full);
		mu_label(ctx, "");
		mu_text(ctx, "Fit the screen: the largest of double, actual, "
		    "half, and quarter size that fits the screen.");
	}
	row(1, full);
	mu_label(ctx, "");
	check("Serial ports on telnet ports (localhost, 4000 and up)",
	    &c->serial_telnet);
}


static void page_network(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	int full[1] = { -1 };
	int lw[2] = { CH(24), -1 };
	int i;

	row(1, full);
	check("Network (a simulated network with a gateway and nameserver)",
	    &c->net);
	if (!c->net)
		return;
	row(2, lw);
	mu_label(ctx, "Network address");
	text_field(&c->net_ipv4);
	mu_label(ctx, "Prefix length");
	int_field(&c->net_ipv4len);
	mu_label(ctx, "Tap device");
	text_field(&c->net_tapdev);
	mu_label(ctx, "Local UDP port");
	int_field(&c->net_local_port);

	row(1, full);
	mu_label(ctx, "Remote hosts (host:port), for connecting emulators:");
	for (i = 0; i < c->n_net_remotes; i++) {
		int w[2] = { -CH(10), -1 };
		mu_push_id(ctx, &i, sizeof(i));
		row(2, w);
		text_field(&c->net_remotes[i]);
		if (mu_button(ctx, "Remove")) {
			machine_config_remove_string(&c->n_net_remotes,
			    &c->net_remotes, i);
			edits_clear();
			mu_pop_id(ctx);
			break;
		}
		mu_pop_id(ctx);
	}
	int aw[1] = { CH(14) };
	row(1, aw);
	if (mu_button(ctx, "Add remote")) {
		machine_config_add_string(&c->n_net_remotes, &c->net_remotes,
		    "localhost:15001");
		edits_clear();
	}

	row(1, full);
	check("L2TPv3 tunnel", &c->net_l2tp);
	if (c->net_l2tp) {
		int session = (int) c->net_l2tp_session;
		row(2, lw);
		mu_label(ctx, "Session ID");
		int_field(&session);
		c->net_l2tp_session = (uint32_t) session;
		mu_label(ctx, "Cookie");
		text_field(&c->net_l2tp_cookie);
		row(1, full);
		check("Sequence numbers", &c->net_l2tp_counter);
	}
}


static void page_advanced(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	int full[1] = { -1 };
	int lw[2] = { CH(24), CH(20) };
	int sel = 0, i;

	row(2, lw);
	mu_label(ctx, "Emulated clock (Hz)");
	{
		int hz = (int) c->emulated_hz;
		int_field(&hz);
		c->emulated_hz = hz > 0 ? (uint64_t) hz : 0;
	}
	mu_label(ctx, "Byte order");
	for (i = 0; i < 3; i++)
		if (strcasecmp(c->byte_order, order_values[i]) == 0)
			sel = i;
	if (dropdown("byteorder", order_labels, 3, &sel))
		machine_config_set(&c->byte_order, order_values[sel]);
	mu_label(ctx, "Serial number");
	int_field(&c->serial_nr);

	row(1, full);
	check("Random memory contents at startup", &c->random_memory);
	check("Random bootstrap CPU", &c->random_bootstrap_cpu);

	mu_label(ctx, "");
	mu_label(ctx, "Extra devices (as for the debugger's device add):");
	for (i = 0; i < c->n_devices; i++) {
		int w[2] = { -CH(10), -1 };
		mu_push_id(ctx, &i, sizeof(i));
		row(2, w);
		text_field(&c->devices[i]);
		if (mu_button(ctx, "Remove")) {
			machine_config_remove_string(&c->n_devices,
			    &c->devices, i);
			edits_clear();
			mu_pop_id(ctx);
			break;
		}
		mu_pop_id(ctx);
	}
	int aw[1] = { CH(14) };
	row(1, aw);
	if (mu_button(ctx, "Add device")) {
		machine_config_add_string(&c->n_devices, &c->devices, "");
		edits_clear();
	}
}


static void screen_editor(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	char *problem;
	int i;

	/*  The pages:  */
	int tw[N_PAGES];
	for (i = 0; i < N_PAGES; i++)
		tw[i] = CH((int) strlen(page_names[i]) + 4);
	row(N_PAGES, tw);
	for (i = 0; i < N_PAGES; i++) {
		mu_push_id(ctx, &i, sizeof(i));
		if (list_item(page_names[i], (int) strlen(page_names[i]),
		    page_names[i], (int) ui.page == i))
			ui.page = (enum page) i;
		mu_pop_id(ctx);
	}

	int full[1] = { -1 };
	mu_layout_row(ctx, 1, full, -row_height() * 2 - ctx->style->padding);
	mu_begin_panel(ctx, page_names[ui.page]);
	switch (ui.page) {
	case PAGE_GENERAL:	page_general(); break;
	case PAGE_FIRMWARE:		page_firmware(); break;
	case PAGE_DRIVES:	page_drives(); break;
	case PAGE_DISPLAY:	page_display(); break;
	case PAGE_NETWORK:	page_network(); break;
	case PAGE_ADVANCED:	page_advanced(); break;
	default:		break;
	}
	mu_end_panel(ctx);

	/*  (Choosing another model for a new machine replaces it.)  */
	c = ui.cfg;

	/*  What's wrong, if anything:  */
	/*  A message, or what's wrong, if anything:  */
	row(1, full);
	problem = machine_config_check_ex(c, true);
	if (ui.pick_open)
		mu_label(ctx, "Waiting for the file chooser...");
	else if (ui.message != NULL)
		mu_label(ctx, ui.message);
	else if (problem != NULL) {
		char buf[600];
		snprintf(buf, sizeof(buf), "Not ready: %s", problem);
		mu_label(ctx, buf);
	} else {
		char *p2 = machine_config_check(c);
		mu_label(ctx, p2 == NULL ? "Ready" : "Ready (missing disk "
		    "images are created when it is saved)");
		free(p2);
	}
	free(problem);

	int bw[5] = { CH(10), -CH(30), CH(10), -1 };
	row(4, bw);
	if (mu_button(ctx, ui.running ? "Close" : "Cancel")) {
		ui.screen = SCREEN_LIST;
		free(ui.message);
		ui.message = NULL;
		if (ui.running)
			ui.quit = true;
	}
	mu_label(ctx, c->path != NULL ? c->path : "(not saved yet)");
	if (ui.running) {
		/*  (Starting it again would use the same disk images.)  */
		mu_label(ctx, "");
		if (mu_button(ctx, "Save") && save(false))
			set_message(false, "Saved; the changes take effect "
			    "when the machine is restarted");
		return;
	}
	if (mu_button(ctx, "Save"))
		save(false);
	if (mu_button(ctx, "Save and Start") && save(true))
		start(c);
}


/****************************************************************************/

/*
 *  The window
 */

static void frame(void)
{
	mu_Context *ctx = ui.ctx;
	const char *title = ui.screen == SCREEN_LIST ? "GXemul: Machines" :
	    ui.screen == SCREEN_SETTINGS ? "Settings" :
	    "Machine configuration";
	mu_Container *cnt;

	/*  Text typed or pasted, a little at a time:  */
	if (ui.pending_len > 0) {
		char chunk[32];
		size_t n = ui.pending_len < sizeof(chunk) - 1 ?
		    ui.pending_len : sizeof(chunk) - 1;
		memcpy(chunk, ui.pending_text, n);
		chunk[n] = '\0';
		mu_input_text(ctx, chunk);
		memmove(ui.pending_text, ui.pending_text + n,
		    ui.pending_len - n);
		ui.pending_len -= n;
	}

	mu_begin(ctx);
	cnt = mu_get_container(ctx, "main");
	cnt->rect = mu_rect(0, 0, ui.out_w, ui.out_h);
	if (mu_begin_window_ex(ctx, "main", cnt->rect, MU_OPT_NOTITLE |
	    MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME |
	    MU_OPT_NOSCROLL)) {
		int full[1] = { -1 };
		row(1, full);
		mu_label(ctx, ui.screen == SCREEN_EDITOR && ui.cfg != NULL &&
		    ui.cfg->name[0] ? ui.cfg->name : title);

		switch (ui.screen) {
		case SCREEN_LIST:	screen_list(); break;
		case SCREEN_EDITOR:	screen_editor(); break;
		case SCREEN_SETTINGS:	screen_settings(); break;
		}

		if (ui.screen != SCREEN_EDITOR) {
			row(1, full);
			if (ui.pick_open)
				mu_label(ctx, "Waiting for the file chooser...");
			else if (ui.message != NULL)
				mu_label(ctx, ui.message);
			else
				mu_label(ctx, "");
		}

		mu_end_window(ctx);
	}
	mu_end(ctx);
}


static void handle_event(const SDL_Event *ev)
{
	mu_Context *ctx = ui.ctx;
	int s = ui.scale;

	switch (ev->type) {
	case SDL_QUIT:
		ui.quit = true;
		break;
	case SDL_WINDOWEVENT:
		if (ev->window.event == SDL_WINDOWEVENT_CLOSE)
			ui.quit = true;
		if (ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
		    ev->window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)
			make_font();
		break;
	case SDL_MOUSEMOTION:
		mu_input_mousemove(ctx, ev->motion.x * s, ev->motion.y * s);
		break;
	case SDL_MOUSEWHEEL:
		mu_input_scroll(ctx, 0, -ev->wheel.y * ui.font_h * 2);
		break;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP: {
		int b = ev->button.button == SDL_BUTTON_LEFT ? MU_MOUSE_LEFT :
		    ev->button.button == SDL_BUTTON_RIGHT ? MU_MOUSE_RIGHT :
		    MU_MOUSE_MIDDLE;
		if (ev->type == SDL_MOUSEBUTTONDOWN)
			mu_input_mousedown(ctx, ev->button.x * s,
			    ev->button.y * s, b);
		else
			mu_input_mouseup(ctx, ev->button.x * s,
			    ev->button.y * s, b);
		break;
	}
	case SDL_TEXTINPUT: {
		size_t n = strlen(ev->text.text);
		if (ui.pending_len + n < sizeof(ui.pending_text)) {
			memcpy(ui.pending_text + ui.pending_len, ev->text.text,
			    n);
			ui.pending_len += n;
		}
		break;
	}
	case SDL_KEYDOWN:
	case SDL_KEYUP: {
		int key = 0;
		SDL_Keycode k = ev->key.keysym.sym;
		if (k == SDLK_BACKSPACE) key = MU_KEY_BACKSPACE;
		if (k == SDLK_RETURN || k == SDLK_KP_ENTER) key = MU_KEY_RETURN;
		if (k == SDLK_LSHIFT || k == SDLK_RSHIFT) key = MU_KEY_SHIFT;
		if (k == SDLK_LCTRL || k == SDLK_RCTRL) key = MU_KEY_CTRL;
		if (k == SDLK_LALT || k == SDLK_RALT) key = MU_KEY_ALT;
		if (key != 0) {
			if (ev->type == SDL_KEYDOWN)
				mu_input_keydown(ctx, key);
			else
				mu_input_keyup(ctx, key);
		}

		/*  Paste (Ctrl+V, or Command+V on macOS):  */
		if (ev->type == SDL_KEYDOWN && k == SDLK_v &&
		    (ev->key.keysym.mod & (KMOD_CTRL | KMOD_GUI))) {
			char *t = SDL_GetClipboardText();
			if (t != NULL) {
				size_t n = strlen(t);
				if (ui.pending_len + n <
				    sizeof(ui.pending_text)) {
					memcpy(ui.pending_text +
					    ui.pending_len, t, n);
					ui.pending_len += n;
				}
				SDL_free(t);
			}
		}

		/*  Escape closes a drop-down list, or leaves the editor:  */
		if (ev->type == SDL_KEYDOWN && k == SDLK_ESCAPE &&
		    ui.open_list != NULL && ui.open_list->open)
			ui.open_list->open = 0;
		else if (ev->type == SDL_KEYDOWN && k == SDLK_ESCAPE &&
		    ui.screen != SCREEN_LIST && !ui.pick_open) {
			ui.screen = SCREEN_LIST;
			if (ui.running)
				ui.quit = true;
		}
		break;
	}
	}
}


bool config_ui_available(void)
{
	return display_backend_available();
}


struct machine_config *config_ui_run(const char *edit_path, bool running)
{
	SDL_Event ev;

	memset(&ui, 0, sizeof(ui));

	SDL_SetMainReady();
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "Couldn't initialize SDL: %s\n",
		    SDL_GetError());
		return NULL;
	}

	ui.window = SDL_CreateWindow("GXemul", SDL_WINDOWPOS_CENTERED,
	    SDL_WINDOWPOS_CENTERED, WINDOW_W, WINDOW_H,
	    SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (ui.window != NULL)
		ui.renderer = SDL_CreateRenderer(ui.window, -1,
		    SDL_RENDERER_PRESENTVSYNC);
	if (ui.renderer == NULL) {
		fprintf(stderr, "Couldn't open a window: %s\n",
		    SDL_GetError());
		if (ui.window != NULL)
			SDL_DestroyWindow(ui.window);
		return NULL;
	}
	SDL_SetWindowMinimumSize(ui.window, 640, 420);
	SDL_StartTextInput();

	CHECK_ALLOCATION(ui.ctx = (mu_Context *) malloc(sizeof(mu_Context)));
	mu_init(ui.ctx);
	ui.ctx->text_width = text_width;
	ui.ctx->text_height = text_height;
	set_style();
	make_font();

	list_load(NULL);

	if (edit_path != NULL) {
		char *err;
		struct stat st;
		struct machine_config *c = NULL;

		if (stat(edit_path, &st) == 0) {
			c = machine_config_load(edit_path, &err);
			if (c == NULL) {
				fprintf(stderr, "%s\n", err);
				free(err);
				if (running)
					return NULL;
			} else
				open_editor(c, false);
		} else {
			/*  A new machine, to be saved at edit_path:  */
			new_machine(edit_path);
		}
	}
	if (running && ui.screen == SCREEN_EDITOR) {
		ui.running = true;
		set_message(false, "The machine is running: the changes take "
		    "effect when it is restarted");
	}

	while (!ui.quit && ui.result == NULL) {
		if (SDL_WaitEventTimeout(&ev, 50)) {
			handle_event(&ev);
			while (SDL_PollEvent(&ev))
				handle_event(&ev);
		}
		display_files_poll();
		frame();
		render();
	}

	list_free();
	rom_dirs_free();
	machine_config_free(ui.cfg);
	free(ui.message);
	free(ui.ctx);
	if (ui.font != NULL)
		SDL_DestroyTexture(ui.font);
	SDL_DestroyRenderer(ui.renderer);
	SDL_DestroyWindow(ui.window);

	return ui.result;
}

#endif	/*  WITH_SDL  */
