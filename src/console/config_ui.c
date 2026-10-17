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
 *  (see config_ui.h and machine_config.h).  It has three screens:
 *
 *	the list of machines (recently used configuration files), with
 *	    New, Open, Edit, Duplicate, Forget, and Start;
 *	the presets, for a new machine;
 *	the editor, with a page for each part of the configuration.
 *
 *  The widgets are microui's (src/console/microui.c), drawn with SDL and the
 *  same font as the framebuffer windows' menus.  Coordinates are in output
 *  pixels (twice the window's points on a HiDPI screen).
 */

#include <stdarg.h>
#include <stdbool.h>
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

struct machine_config *config_ui_run(const char *edit_path)
{
	(void) edit_path;
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

enum screen { SCREEN_LIST, SCREEN_PRESETS, SCREEN_EDITOR };
enum page { PAGE_GENERAL, PAGE_BOOT, PAGE_DRIVES, PAGE_DISPLAY,
	PAGE_EXPANSION, PAGE_NETWORK, PAGE_ADVANCED, N_PAGES };

static const char *page_names[N_PAGES] = { "General", "Boot", "Drives",
	"Display", "Expansion", "Network", "Advanced" };

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

	/*  The presets:  */
	int		preset;

	/*  The editor:  */
	struct machine_config *cfg;
	bool		is_new;
	bool		planned_dir;	/*  cfg->dir is where it will be saved  */
	bool		scroll_types;	/*  show the machine type selected  */
	char		*planned_name;
	bool		start_after_save;
	enum page	page;
	int		disk_mb[64];	/*  for creating missing disk images  */
	struct edit	edits[MAX_EDITS];
	int		n_edits;

	/*  Text typed or pasted, given to microui a little at a time:  */
	char		pending_text[4096];
	size_t		pending_len;

	/*  The open file chooser's target:  */
	char		**pick_field;
	bool		pick_open;
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
	return ui.ctx->style->size.y + ui.ctx->style->spacing;
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

	if (mu_checkbox(ui.ctx, label, &v))
		*field = v != 0;
}


/*
 *  A button that shows one of a few choices, and opens a menu of them.
 *  Returns true if a choice was made.
 */
static bool choice(const char *name, const char **labels, int n, int *sel)
{
	char popup[100];
	bool changed = false;
	int i, w;

	snprintf(popup, sizeof(popup), "choice:%s", name);
	mu_push_id(ui.ctx, name, (int) strlen(name));
	if (mu_button_ex(ui.ctx, labels[*sel >= 0 && *sel < n ? *sel : 0], 0,
	    MU_OPT_ALIGNCENTER))
		mu_open_popup(ui.ctx, popup);
	if (mu_begin_popup(ui.ctx, popup)) {
		w = 0;
		for (i = 0; i < n; i++)
			if ((int) strlen(labels[i]) + 4 > w)
				w = (int) strlen(labels[i]) + 4;
		int widths[1] = { CH(w) };
		row(1, widths);
		for (i = 0; i < n; i++)
			if (mu_button_ex(ui.ctx, labels[i], 0, 0)) {
				*sel = i;
				changed = true;
				mu_get_current_container(ui.ctx)->open = 0;
			}
		mu_end_popup(ui.ctx);
	}
	mu_pop_id(ui.ctx);
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
		machine_config_set(ui.pick_field, path);
		edits_clear();
	}
	ui.pick_field = NULL;
}


static const char *start_dir(void)
{
	if (ui.cfg != NULL && ui.cfg->dir != NULL)
		return ui.cfg->dir;
	return appdirs_machines_dir();
}


/*  Lets the user choose a file for a field of the configuration:  */
static void pick_file(char **field, const char *title)
{
	ui.pick_field = field;
	ui.pick_open = true;
	display_choose_file(NULL, title, start_dir(), picked, NULL);
	if (!display_chooser_open())
		ui.pick_open = false;
}


/*  A file name field, with a Browse button:  */
static void file_field(char **field, const char *title, bool clear_button)
{
	int widths[3] = { -CH(clear_button ? 20 : 10), CH(9), CH(8) };

	mu_push_id(ui.ctx, &field, sizeof(field));
	row(clear_button ? 3 : 2, widths);
	text_field(field);
	if (mu_button(ui.ctx, "Browse..."))
		pick_file(field, title);
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

	/*  A new machine from a preset has its files in the directory it
	    will be saved in (named after the machine):  */
	ui.planned_dir = is_new && c->path == NULL && c->dir == NULL;
	free(ui.planned_name);
	ui.planned_name = NULL;
	ui.page = PAGE_GENERAL;
	ui.scroll_types = true;
	ui.start_after_save = false;
	for (i = 0; i < 64; i++)
		ui.disk_mb[i] = DEFAULT_DISK_MB;
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
	int bw[7] = { CH(10), CH(10), CH(8), CH(12), CH(9), -CH(12), -1 };
	row(7, bw);
	if (mu_button(ctx, "New...")) {
		ui.screen = SCREEN_PRESETS;
		ui.preset = 0;
	}
	if (mu_button(ctx, "Open...")) {
		ui.pick_open = true;
		display_choose_file(NULL, "Open a machine configuration "
		    "(machine.json)", appdirs_machines_dir(), opened, NULL);
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
	mu_label(ctx, "");
	if (mu_button(ctx, "Start") && sel != NULL)
		start(sel);
}


/****************************************************************************/

/*
 *  The presets
 */

static void screen_presets(void)
{
	mu_Context *ctx = ui.ctx;
	const struct machine_preset *presets;
	int n = machine_presets(&presets), i;

	int cols[2] = { CH(44), -1 };
	mu_layout_row(ctx, 2, cols, -row_height() * 2 - ctx->style->padding);

	mu_begin_panel(ctx, "presets");
	int full[1] = { -1 };
	row(1, full);
	for (i = 0; i < n; i++)
		if (list_item(&presets[i], sizeof(presets[i]),
		    presets[i].title, i == ui.preset))
			ui.preset = i;
	mu_end_panel(ctx);

	mu_begin_panel(ctx, "preset");
	row(1, full);
	mu_label(ctx, presets[ui.preset].title);
	mu_text(ctx, presets[ui.preset].description);
	mu_end_panel(ctx);

	int bw[3] = { CH(10), -CH(12), -1 };
	row(3, bw);
	if (mu_button(ctx, "Back"))
		ui.screen = SCREEN_LIST;
	mu_label(ctx, "");
	if (mu_button(ctx, "Create")) {
		char *err;
		struct machine_config *c = machine_config_parse(
		    presets[ui.preset].json, NULL, &err);
		if (c == NULL) {
			set_message(true, "%s", err);
			free(err);
		} else
			open_editor(c, true);
	}
}


/****************************************************************************/

/*
 *  The editor
 */

static void make_dir(const char *path);

/*  Where a new machine is saved (a directory of its own):  */
static char *new_machine_dir(const char *name)
{
	char safe[200], *dir;
	size_t i, n = 0;
	struct stat st;
	int k;

	for (i = 0; name[i] != '\0' && n < sizeof(safe) - 1; i++) {
		char c = name[i];
		if (c == '/' || c == '\\' || c == ':' || c < ' ')
			c = '-';
		safe[n++] = c;
	}
	safe[n] = '\0';
	if (n == 0)
		snprintf(safe, sizeof(safe), "machine");

	CHECK_ALLOCATION(dir = (char *) malloc(strlen(appdirs_machines_dir()) +
	    sizeof(safe) + 20));
	for (k = 1; ; k++) {
		if (k == 1)
			sprintf(dir, "%s/%s", appdirs_machines_dir(), safe);
		else
			sprintf(dir, "%s/%s %i", appdirs_machines_dir(), safe,
			    k);
		if (stat(dir, &st) != 0)
			break;
	}
	return dir;
}


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
		    ui.disk_mb[i < 64 ? i : 63], &err)) {
			set_message(true, "%s", err);
			free(err);
			free(f);
			return false;
		}
		free(f);
	}
	return true;
}


static bool save(void)
{
	struct machine_config *c = ui.cfg;
	char *err, *path;

	if (c->path == NULL) {
		/*  A new machine gets a directory of its own:  */
		char *dir;
		if (ui.planned_dir && c->dir != NULL) {
			CHECK_ALLOCATION(dir = strdup(c->dir));
		} else
			dir = new_machine_dir(c->name);
		make_dir(dir);
		CHECK_ALLOCATION(path = (char *) malloc(strlen(dir) + 20));
		sprintf(path, "%s/machine.json", dir);
		if (!machine_config_save(c, path, &err)) {
			set_message(true, "%s", err);
			free(err); free(path); free(dir);
			return false;
		}
		free(path);
		free(dir);
	} else if (!machine_config_save(c, c->path, &err)) {
		set_message(true, "%s", err);
		free(err);
		return false;
	}

	edits_clear();
	if (!create_missing_disks())
		return false;
	appdirs_add_recent(c->path);
	ui.is_new = false;
	ui.planned_dir = false;
	set_message(false, "Saved %s", c->path);
	return true;
}


static void page_general(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	struct machine_entry *me, *cur = NULL;
	int i, j;

	int lw[2] = { CH(16), -1 };
	row(2, lw);
	mu_label(ctx, "Name");
	text_field(&c->name);

	/*  Machine type and model, as two lists:  */
	for (me = first_machine_entry; me != NULL; me = me->next)
		for (i = 0; i < me->n_aliases; i++)
			if (strcasecmp(me->aliases[i], c->type) == 0)
				cur = me;

	int cols[2] = { CH(42), -1 };
	mu_layout_row(ctx, 2, cols, row_height() * 9);
	mu_begin_panel(ctx, "types");
	int full[1] = { -1 };
	row(1, full);
	if (ui.scroll_types) {
		/*  Scroll the type selected into view, once:  */
		int index = 0;
		for (me = first_machine_entry; me != NULL && me != cur;
		    me = me->next)
			index ++;
		if (cur != NULL)
			mu_get_current_container(ctx)->scroll.y = (index - 2) *
			    (ctx->style->size.y + ctx->style->spacing);
		ui.scroll_types = false;
	}
	for (me = first_machine_entry; me != NULL; me = me->next)
		if (list_item(&me, sizeof(me), me->name, me == cur) &&
		    me != cur) {
			machine_config_set(&c->type, me->aliases[0]);
			machine_config_set(&c->model, me->n_subtypes > 0 ?
			    me->subtype[0]->aliases[0] : "");
			edits_clear();
		}
	mu_end_panel(ctx);

	mu_begin_panel(ctx, "models");
	row(1, full);
	if (cur == NULL || cur->n_subtypes == 0)
		mu_label(ctx, cur == NULL ? "Choose a machine type" :
		    "(no models)");
	for (i = 0; cur != NULL && i < cur->n_subtypes; i++) {
		bool is_cur = false;
		for (j = 0; j < cur->subtype[i]->n_aliases; j++)
			if (strcasecmp(cur->subtype[i]->aliases[j],
			    c->model) == 0)
				is_cur = true;
		if (list_item(&cur->subtype[i], sizeof(cur->subtype[i]),
		    cur->subtype[i]->name, is_cur))
			machine_config_set(&c->model,
			    cur->subtype[i]->aliases[0]);
	}
	mu_end_panel(ctx);

	row(2, lw);
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
}


static void page_boot(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	int i;
	int full[1] = { -1 };
	int lw[2] = { CH(16), -1 };

	row(1, full);
	check("PROM emulation (gxemul provides the firmware)",
	    &c->prom_emulation);

	if (machine_config_rom_address(c->type) != 0) {
		row(1, full);
		mu_label(ctx, "System ROM image (instead of PROM emulation):");
		file_field(&c->rom, "Choose the system ROM image", true);
	}

	row(1, full);
	mu_label(ctx, "");
	mu_label(ctx, "Files to load (a kernel or program; prefix "
	    "\"address:\" for raw images):");
	for (i = 0; i < c->n_load; i++) {
		int w[3] = { -CH(20), CH(9), CH(8) };
		mu_push_id(ctx, &i, sizeof(i));
		row(3, w);
		text_field(&c->load[i]);
		if (mu_button(ctx, "Browse..."))
			pick_file(&c->load[i], "Choose a file to load");
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
	check("Start paused, in the debugger", &c->start_paused);
}


static void page_drives(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	struct stat st;
	int i;
	int full[1] = { -1 };

	for (i = 0; i < c->n_drives; i++) {
		struct mc_drive *d = &c->drives[i];
		char name[40], *f;
		bool missing;

		mu_push_id(ctx, &i, sizeof(i));
		int w1[6] = { CH(10), CH(3), CH(5), -CH(22), CH(11), CH(9) };
		row(6, w1);
		snprintf(name, sizeof(name), "kind%i", i);
		choice(name, kind_labels, 4, &d->kind);
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
			pick_file(&d->file, "Choose a drive image");
		if (mu_button(ctx, "Remove")) {
			machine_config_remove_drive(c, i);
			edits_clear();
			mu_pop_id(ctx);
			break;
		}

		f = machine_config_resolve(c, d->file);
		missing = d->kind == MC_DRIVE_DISK && d->file[0] != '\0' &&
		    stat(f, &st) != 0;
		free(f);

		int w2[7] = { CH(10), CH(13), CH(13), CH(11), CH(8),
		    CH(8), CH(14) };
		row(missing ? 7 : 5, w2);
		mu_label(ctx, "");
		snprintf(name, sizeof(name), "bus%i", i);
		choice(name, bus_labels, 3, &d->bus);
		check("Read-only", &d->readonly);
		check("Overlay", &d->overlay);
		check("Boot", &d->boot);
		if (missing && i < 64) {
			int_field(&ui.disk_mb[i]);
			if (mu_button(ctx, "Create (MB)")) {
				char *err;
				if (c->dir != NULL)
					make_dir(c->dir);
				f = machine_config_resolve(c, d->file);
				if (machine_config_create_disk(f,
				    ui.disk_mb[i], &err))
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
	    "go to a temporary file.  A disk image that doesn't exist yet can "
	    "be created with the size given (it is also created when a new "
	    "machine is saved).");
}


static void page_display(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	int full[1] = { -1 };
	int lw[2] = { CH(22), CH(10) };

	row(1, full);
	check("Framebuffer window (graphics, keyboard and mouse)", &c->display);
	row(2, lw);
	mu_label(ctx, "Window scale");
	int_field(&c->scale);
	mu_label(ctx, "Graphics cards");
	int_field(&c->gfx_cards);
	row(1, full);
	mu_text(ctx, "Scale: 1 = actual size, 2 = half size, -2 = double size.  "
	    "Graphics cards: -1 = the machine's default (DECstations: up to "
	    "3, in slots 0..2).");
	mu_label(ctx, "");
	check("Serial ports on telnet ports (localhost, 4000 and up)",
	    &c->serial_telnet);
}


static void page_expansion(void)
{
	mu_Context *ctx = ui.ctx;
	struct machine_config *c = ui.cfg;
	int n = machine_config_n_slots(c->type, c->model), i;
	int full[1] = { -1 };

	row(1, full);
	if (n == 0) {
		mu_text(ctx, "This machine type has no expansion slots that "
		    "can be configured here.");
		return;
	}
	mu_text(ctx, "Option ROM images for the TURBOchannel slots.  Slots "
	    "0..2 hold the graphics cards (with the framebuffer on); a real "
	    "ROM needs the cards' option ROMs to use them.  Empty = gxemul's "
	    "built-in ROM.");
	for (i = 0; i < n; i++) {
		char label[40];
		row(1, full);
		snprintf(label, sizeof(label), "Slot %i:", i);
		mu_label(ctx, label);
		if (c->slot_rom[i] == NULL)
			machine_config_set(&c->slot_rom[i], "");
		file_field(&c->slot_rom[i], "Choose an option ROM image",
		    true);
	}
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
	if (choice("byteorder", order_labels, 3, &sel))
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

	/*  A new machine's directory follows its name, until it is saved:  */
	if (ui.planned_dir && (ui.planned_name == NULL ||
	    strcmp(ui.planned_name, c->name) != 0)) {
		char *dir = new_machine_dir(c->name);
		machine_config_set(&c->dir, dir);
		free(dir);
		machine_config_set(&ui.planned_name, c->name);
	}

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
	case PAGE_BOOT:		page_boot(); break;
	case PAGE_DRIVES:	page_drives(); break;
	case PAGE_DISPLAY:	page_display(); break;
	case PAGE_EXPANSION:	page_expansion(); break;
	case PAGE_NETWORK:	page_network(); break;
	case PAGE_ADVANCED:	page_advanced(); break;
	default:		break;
	}
	mu_end_panel(ctx);

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
	if (mu_button(ctx, "Cancel")) {
		ui.screen = SCREEN_LIST;
		free(ui.message);
		ui.message = NULL;
	}
	mu_label(ctx, c->path != NULL ? c->path : "(not saved yet)");
	if (mu_button(ctx, "Save") && save())
		list_load(c->path);
	if (mu_button(ctx, "Save and Start") && save()) {
		list_load(c->path);
		start(c);
	}
}


/****************************************************************************/

/*
 *  The window
 */

static void frame(void)
{
	mu_Context *ctx = ui.ctx;
	const char *title = ui.screen == SCREEN_LIST ? "GXemul: Machines" :
	    ui.screen == SCREEN_PRESETS ? "New machine: choose a preset" :
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
		case SCREEN_PRESETS:	screen_presets(); break;
		case SCREEN_EDITOR:	screen_editor(); break;
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

		/*  Escape leaves the editor and the presets:  */
		if (ev->type == SDL_KEYDOWN && k == SDLK_ESCAPE &&
		    ui.screen != SCREEN_LIST && !ui.pick_open)
			ui.screen = SCREEN_LIST;
		break;
	}
	}
}


bool config_ui_available(void)
{
	return display_backend_available();
}


struct machine_config *config_ui_run(const char *edit_path)
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
			} else
				open_editor(c, false);
		} else {
			/*  A new machine, to be saved at edit_path:  */
			c = machine_config_parse("{ \"gxemul\": 1 }", NULL,
			    &err);
			machine_config_set(&c->path, edit_path);
			open_editor(c, false);
		}
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
	machine_config_free(ui.cfg);
	free(ui.planned_name);
	free(ui.message);
	free(ui.ctx);
	if (ui.font != NULL)
		SDL_DestroyTexture(ui.font);
	SDL_DestroyRenderer(ui.renderer);
	SDL_DestroyWindow(ui.window);

	return ui.result;
}

#endif	/*  WITH_SDL  */
