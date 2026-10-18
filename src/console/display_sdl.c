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
 *  SDL2 display backend: shows framebuffer windows (see display.c) as SDL
 *  windows, and turns SDL keyboard and mouse events into console input.
 *
 *  Each window has a menu bar at the top and a status line at the bottom
 *  (see display_menu.c), drawn in the Spleen font (display_font.h), with
 *  the screen image scaled in between (keeping the aspect ratio).  The
 *  machine keeps running while menus are open.  Clicking in the screen
 *  grabs the mouse, using SDL's relative mouse mode; Ctrl+Alt releases
 *  it.  When the mouse isn't grabbed, the menus work with the mouse, and
 *  F10 opens them for the keyboard.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "emul.h"
#include "machine.h"
#include "misc.h"
#include "display.h"


#ifdef	WITH_SDL


/*  gxemul has its own main(), and doesn't link with SDL2main:  */
#define	SDL_MAIN_HANDLED
#include <SDL.h>

#include "display_font.h"

/*
 *  CTRL+ALT ungrabs the mouse.  On macOS, Option is Alt, and Command
 *  works as well.
 */
#ifdef __APPLE__
#define	UNGRAB_KEYS	"Ctrl+Option"
#else
#define	UNGRAB_KEYS	"CTRL+ALT"
#endif

/*  Windows are redrawn at most this often:  */
#define	MIN_PRESENT_INTERVAL_MS		10

/*  The menus' colors:  */
#define	COLOR_BAR		0xe4e4e4
#define	COLOR_MENU		0xf6f6f6
#define	COLOR_TEXT		0x000000
#define	COLOR_DISABLED		0x8c8c8c
#define	COLOR_HIGHLIGHT		0x2f5fb3
#define	COLOR_HIGHLIGHT_TEXT	0xffffff
#define	COLOR_BORDER		0x8a8a8a
#define	COLOR_SEPARATOR		0xc8c8c8

/*  How deep menus can be open:  */
#define	MENU_DEPTH		4

/*  The character that shows that an item opens a submenu (Latin-1 "»"):  */
#define	MENU_ARROW		"\302\273"

/*  A framebuffer window's SDL state:  */
struct sdl_window {
	SDL_Window	*window;
	SDL_Renderer	*renderer;
	SDL_Texture	*texture;

	/*  The part of the screen image which the texture lacks:  */
	bool		dirty;
	int		dirty_x1, dirty_y1, dirty_x2, dirty_y2;

	/*  The texture has changed since the window was redrawn:  */
	bool		present_pending;
	Uint32		last_present;

	/*  The font, and the sizes of things, in output pixels:  */
	int		scale;		/*  output pixels per window point  */
	SDL_Texture	*font;
	int		font_w, font_h;
	int		pad;
	int		bar_h;		/*  the menu bar and status line  */

	/*  The menu and status generations drawn:  */
	unsigned long	menus_drawn, status_drawn;

	/*
	 *  The open menus: how many, the index of the item that opened
	 *  each one (in the menu bar for the first), and the index of the
	 *  highlighted item in each, or -1:
	 */
	int		menu_depth;
	int		menu_opener[MENU_DEPTH];
	int		menu_highlight[MENU_DEPTH];
};

#define	SWIN(win)	((struct sdl_window *) (win)->backend)

static bool sdl_initialized = false;
static struct fb_window *grabbed = NULL;


static bool init_sdl(void)
{
	if (sdl_initialized)
		return true;

	SDL_SetMainReady();

	/*  SIGINT is for gxemul's debugger, not for quitting:  */
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

	/*  Keep the pixels sharp when a window is scaled:  */
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		debugmsg(SUBSYS_DISPLAY, "init", VERBOSITY_ERROR,
		    "couldn't initialize SDL: %s", SDL_GetError());
		return false;
	}

	SDL_StartTextInput();
	sdl_initialized = true;
	return true;
}


/*  Returns the framebuffer window of an SDL window ID, or NULL.  */
static struct fb_window *fb_window_of(Uint32 window_id)
{
	SDL_Window *window = SDL_GetWindowFromID(window_id);

	return window == NULL ? NULL :
	    (struct fb_window *) SDL_GetWindowData(window, "fb_window");
}


/****************************************************************************/

/*
 *  Text and boxes:
 */

static void output_size(struct fb_window *win, int *w, int *h)
{
	if (SDL_GetRendererOutputSize(SWIN(win)->renderer, w, h) != 0)
		*w = *h = 0;
}


static void fill(struct fb_window *win, int x, int y, int w, int h,
	Uint32 rgb)
{
	SDL_Rect r = { x, y, w, h };

	SDL_SetRenderDrawColor(SWIN(win)->renderer, rgb >> 16, (rgb >> 8) & 0xff,
	    rgb & 0xff, 255);
	SDL_RenderFillRect(SWIN(win)->renderer, &r);
}


/*
 *  Makes the font texture, and sets the sizes of things, for the window's
 *  scale.  HiDPI screens get the larger font, instead of the smaller one
 *  scaled up.
 */
static void font_init(struct fb_window *win)
{
	struct sdl_window *sw = SWIN(win);
	int window_w, window_h, out_w, out_h, width, height, zoom;

	SDL_GetWindowSize(sw->window, &window_w, &window_h);
	output_size(win, &out_w, &out_h);
	int scale = window_w > 0 ? (out_w + window_w / 2) / window_w : 1;
	if (scale < 1)
		scale = 1;
	if (sw->font != NULL && scale == sw->scale)
		return;
	sw->scale = scale;

	if (scale >= 2) {
		width = DISPLAY_FONT_16X32_WIDTH;
		height = DISPLAY_FONT_16X32_HEIGHT;
		zoom = scale / 2;
	} else {
		width = DISPLAY_FONT_8X16_WIDTH;
		height = DISPLAY_FONT_8X16_HEIGHT;
		zoom = 1;
	}
	int bytes = (width + 7) / 8;

	/*  The characters, in rows of 16, white where they're drawn and
	    transparent elsewhere, so that they can be drawn in any color:  */
	if (sw->font != NULL)
		SDL_DestroyTexture(sw->font);
	sw->font = NULL;
	SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, 16 * width,
	    14 * height, 32, SDL_PIXELFORMAT_ARGB8888);
	if (surface != NULL) {
		SDL_LockSurface(surface);
		for (int c = DISPLAY_FONT_FIRST; c <= DISPLAY_FONT_LAST; c++) {
			int i = c - DISPLAY_FONT_FIRST;
			const unsigned char *glyph = width ==
			    DISPLAY_FONT_16X32_WIDTH ? display_font_16x32[i] :
			    display_font_8x16[i];
			for (int row = 0; row < height; row++) {
				Uint32 *pixels = (Uint32 *) ((Uint8 *)
				    surface->pixels + ((i / 16) * height + row)
				    * surface->pitch) + (i % 16) * width;
				for (int col = 0; col < width; col++)
					pixels[col] = (glyph[row * bytes + col
					    / 8] & (0x80 >> (col % 8))) ?
					    0xffffffff : 0x00ffffff;
			}
		}
		SDL_UnlockSurface(surface);
		sw->font = SDL_CreateTextureFromSurface(sw->renderer, surface);
		SDL_FreeSurface(surface);
	}
	if (sw->font != NULL)
		SDL_SetTextureBlendMode(sw->font, SDL_BLENDMODE_BLEND);

	sw->font_w = width * zoom;
	sw->font_h = height * zoom;
	sw->pad = 3 * scale;
	sw->bar_h = sw->font_h + 2 * sw->pad;
	sw->bar_h += (scale - sw->bar_h % scale) % scale;
}


/*
 *  Returns the next character of UTF-8 text, as Latin-1.  Other characters
 *  are '?', and bytes that aren't UTF-8 are taken as Latin-1.
 */
static int text_next(const char **textp)
{
	const unsigned char *s = (const unsigned char *) *textp;
	unsigned long c;
	int more;

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
	for (int i = 1; i <= more; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			*textp += 1;
			return s[0];
		}
		c = (c << 6) | (s[i] & 0x3f);
	}
	*textp += 1 + more;
	return c <= 0xff ? (int) c : '?';
}


static int text_length(const char *text)
{
	int length = 0;

	if (text == NULL)
		return 0;
	for (; *text != '\0'; length++)
		text_next(&text);
	return length;
}


static void char_draw(struct fb_window *win, int x, int y, int c)
{
	struct sdl_window *sw = SWIN(win);
	int width = sw->scale >= 2 ? DISPLAY_FONT_16X32_WIDTH :
	    DISPLAY_FONT_8X16_WIDTH;
	int height = sw->scale >= 2 ? DISPLAY_FONT_16X32_HEIGHT :
	    DISPLAY_FONT_8X16_HEIGHT;

	if (c < DISPLAY_FONT_FIRST || c > DISPLAY_FONT_LAST)
		c = '?';
	c -= DISPLAY_FONT_FIRST;

	SDL_Rect src = { (c % 16) * width, (c / 16) * height, width, height };
	SDL_Rect dst = { x, y, sw->font_w, sw->font_h };
	SDL_RenderCopy(sw->renderer, sw->font, &src, &dst);
}


/*  Draws at most max characters of UTF-8 text; longer text ends in "...".  */
static void text_draw(struct fb_window *win, int x, int y, const char *text,
	Uint32 rgb, int max)
{
	struct sdl_window *sw = SWIN(win);

	if (text == NULL || max <= 0 || sw->font == NULL)
		return;

	SDL_SetTextureColorMod(sw->font, rgb >> 16, (rgb >> 8) & 0xff,
	    rgb & 0xff);
	int length = text_length(text);
	int count = length <= max ? length : max - 3;
	for (; count > 0 && *text != '\0'; count--) {
		char_draw(win, x, y, text_next(&text));
		x += sw->font_w;
	}
	if (length > max) {
		for (count = max < 3 ? max : 3; count > 0; count--) {
			char_draw(win, x, y, '.');
			x += sw->font_w;
		}
	}
}


/****************************************************************************/

/*
 *  Menus:
 */

static struct menu *menu_bar(struct fb_window *win)
{
	return display_menus(win->machine)->bar;
}


static struct menu_item *menu_nth(const struct menu *menu, int index)
{
	struct menu_item *item;

	if (menu == NULL || index < 0)
		return NULL;
	for (item = menu->items; item != NULL && index > 0; index--)
		item = item->next;
	return item;
}


static int menu_count(const struct menu *menu)
{
	int count = 0;

	for (struct menu_item *item = menu->items; item != NULL;
	    item = item->next)
		count ++;
	return count;
}


/*  Returns the menu open at a level, or NULL.  */
static struct menu *menu_level(struct fb_window *win, int level)
{
	struct menu *menu = menu_bar(win);

	for (int i = 0; i <= level; i++) {
		struct menu_item *item = menu_nth(menu,
		    SWIN(win)->menu_opener[i]);
		if (item == NULL || item->type != MENU_ITEM_SUBMENU)
			return NULL;
		menu = item->submenu;
	}
	return menu;
}


/*  Closes open menus that no longer exist (menus can be rebuilt).  */
static void menu_check(struct fb_window *win)
{
	struct sdl_window *sw = SWIN(win);
	int level;

	for (level = 0; level < sw->menu_depth; level++)
		if (menu_level(win, level) == NULL)
			break;
	sw->menu_depth = level;
}


static int item_height(struct fb_window *win, const struct menu_item *item)
{
	struct sdl_window *sw = SWIN(win);

	return item->type == MENU_ITEM_SEPARATOR ? sw->pad * 2 :
	    sw->font_h + sw->pad;
}


static int border(struct fb_window *win)
{
	return SWIN(win)->scale;
}


/*  Returns the x coordinate and width of a menu bar item.  */
static void bar_item_x(struct fb_window *win, int index, int *xp, int *wp)
{
	struct sdl_window *sw = SWIN(win);
	int x = sw->font_w / 2;

	*wp = 0;
	for (struct menu_item *item = menu_bar(win)->items; item != NULL;
	    item = item->next, index--) {
		*wp = (text_length(item->label) + 2) * sw->font_w;
		if (index == 0)
			break;
		x += *wp;
	}
	*xp = x;
}


/*  Returns the menu bar item at a point, or -1.  */
static int bar_hit(struct fb_window *win, int x, int y)
{
	int n = menu_count(menu_bar(win));

	if (y < 0 || y >= SWIN(win)->bar_h)
		return -1;
	for (int index = 0; index < n; index++) {
		int item_x, item_w;
		bar_item_x(win, index, &item_x, &item_w);
		if (x >= item_x && x < item_x + item_w)
			return index;
	}
	return -1;
}


/*  Returns the rectangle of the menu open at a level.  */
static void menu_rect(struct fb_window *win, int level, SDL_Rect *rect)
{
	struct sdl_window *sw = SWIN(win);
	const struct menu *menu = menu_level(win, level);
	int label = 0, arrow = 0, b = border(win), out_w, out_h;

	/*  Room for a check mark, the longest label, and a submenu arrow:  */
	rect->h = 2 * b + sw->pad;
	for (const struct menu_item *item = menu->items; item != NULL;
	    item = item->next) {
		int length = text_length(item->label);
		if (length > label)
			label = length;
		if (item->type == MENU_ITEM_SUBMENU)
			arrow = 2;
		rect->h += item_height(win, item);
	}
	rect->w = (2 + label + (arrow > 0 ? 1 + arrow : 0) + 1) * sw->font_w
	    + 2 * b;

	/*  The first menu is below its menu bar item, and a submenu is next
	    to the item that opened it:  */
	output_size(win, &out_w, &out_h);
	if (level == 0) {
		int w;
		bar_item_x(win, sw->menu_opener[0], &rect->x, &w);
		rect->y = sw->bar_h;
	} else {
		SDL_Rect parent;
		menu_rect(win, level - 1, &parent);
		rect->x = parent.x + parent.w - b;
		if (rect->x + rect->w > out_w)
			rect->x = parent.x - rect->w + b;
		rect->y = parent.y + sw->pad / 2;
		const struct menu_item *item = menu_level(win, level - 1)->items;
		for (int i = 0; item != NULL && i < sw->menu_opener[level];
		    i++, item = item->next)
			rect->y += item_height(win, item);
	}

	/*  Keep the menu inside the window:  */
	if (rect->x + rect->w > out_w)
		rect->x = out_w - rect->w;
	if (rect->x < 0)
		rect->x = 0;
	if (rect->y + rect->h > out_h)
		rect->y = out_h - rect->h;
	if (rect->y < 0)
		rect->y = 0;
}


/*
 *  Returns the open menu level at a point, or -1, and the index of the
 *  item there, or -1.
 */
static int menu_hit(struct fb_window *win, int x, int y, int *indexp)
{
	*indexp = -1;
	for (int level = SWIN(win)->menu_depth - 1; level >= 0; level--) {
		SDL_Rect r;
		menu_rect(win, level, &r);
		if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h)
			continue;

		int item_y = r.y + border(win) + SWIN(win)->pad / 2, index = 0;
		for (const struct menu_item *item = menu_level(win,
		    level)->items; item != NULL; item = item->next, index++) {
			int height = item_height(win, item);
			if (y >= item_y && y < item_y + height) {
				if (item->type != MENU_ITEM_SEPARATOR)
					*indexp = index;
				break;
			}
			item_y += height;
		}
		return level;
	}
	return -1;
}


static void menu_draw_level(struct fb_window *win, int level)
{
	struct sdl_window *sw = SWIN(win);
	int b = border(win), fw = sw->font_w, pad = sw->pad, index = 0;
	SDL_Rect r;

	menu_rect(win, level, &r);
	int columns = (r.w - 2 * b) / fw;
	fill(win, r.x, r.y, r.w, r.h, COLOR_BORDER);
	fill(win, r.x + b, r.y + b, r.w - 2 * b, r.h - 2 * b, COLOR_MENU);

	int y = r.y + b + pad / 2;
	for (const struct menu_item *item = menu_level(win, level)->items;
	    item != NULL; item = item->next, index++) {
		int height = item_height(win, item);
		Uint32 text;

		if (item->type == MENU_ITEM_SEPARATOR) {
			fill(win, r.x + b + fw / 2, y + height / 2,
			    r.w - 2 * b - fw, b, COLOR_SEPARATOR);
			y += height;
			continue;
		}

		/*  The highlighted item, or the one whose submenu is open:  */
		if (item->enabled && (index == sw->menu_highlight[level] ||
		    (level + 1 < sw->menu_depth &&
		    index == sw->menu_opener[level + 1]))) {
			fill(win, r.x + b, y, r.w - 2 * b, height,
			    COLOR_HIGHLIGHT);
			text = COLOR_HIGHLIGHT_TEXT;
		} else
			text = item->enabled ? COLOR_TEXT : COLOR_DISABLED;

		if (item->type == MENU_ITEM_CHECK && item->checked) {
			int mark = fw / 2;
			fill(win, r.x + b + fw - mark / 2, y + (height - mark)
			    / 2, mark, mark, text);
		}

		text_draw(win, r.x + b + 2 * fw, y + pad / 2, item->label,
		    text, columns - 3);
		if (item->type == MENU_ITEM_SUBMENU)
			text_draw(win, r.x + r.w - b - 2 * fw, y + pad / 2,
			    MENU_ARROW, text, 1);
		y += height;
	}
}


/*  Draws the menu bar, the status line, and any open menus.  */
static void menu_draw(struct fb_window *win)
{
	struct sdl_window *sw = SWIN(win);
	struct display_menus *menus = display_menus(win->machine);
	int out_w, out_h, bar = sw->bar_h, fw = sw->font_w, b = border(win);
	int index = 0;

	menu_check(win);
	output_size(win, &out_w, &out_h);

	fill(win, 0, 0, out_w, bar, COLOR_BAR);
	fill(win, 0, bar - b, out_w, b, COLOR_SEPARATOR);
	for (const struct menu_item *item = menus->bar->items; item != NULL;
	    item = item->next, index++) {
		int x, w;
		Uint32 text = COLOR_TEXT;
		bar_item_x(win, index, &x, &w);
		if (sw->menu_depth > 0 && sw->menu_opener[0] == index) {
			fill(win, x, 0, w, bar - b, COLOR_HIGHLIGHT);
			text = COLOR_HIGHLIGHT_TEXT;
		}
		text_draw(win, x + fw, sw->pad, item->label, text, 64);
	}

	/*
	 *  The status line has a message, or what to do with the mouse, at
	 *  the left, and what the media hold at the right.  If they don't
	 *  both fit, the media get at least half of the line.
	 */
	fill(win, 0, out_h - bar, out_w, bar, COLOR_BAR);
	fill(win, 0, out_h - bar, out_w, b, COLOR_SEPARATOR);
	const char *left = menus->message;
	if (left == NULL)
		left = grabbed == win ? UNGRAB_KEYS " releases the mouse" :
		    "Click in the screen to grab the mouse, F10 for the menus";
	const char *right = menus->media_status;
	int columns = out_w / fw - 2;
	int left_len = text_length(left), right_len = text_length(right);
	if (left_len + 4 + right_len > columns) {
		int half = columns / 2 > columns - left_len - 4 ?
		    columns / 2 : columns - left_len - 4;
		if (right_len > half)
			right_len = half;
		left_len = columns - right_len - 4;
	}
	text_draw(win, fw, out_h - bar + sw->pad, left, COLOR_TEXT, left_len);
	text_draw(win, out_w - (right_len + 1) * fw, out_h - bar + sw->pad,
	    right, COLOR_TEXT, right_len);

	for (index = 0; index < sw->menu_depth; index++)
		menu_draw_level(win, index);

	sw->menus_drawn = menus->generation;
	sw->status_drawn = menus->status_generation;
}


/*  Returns the first or last item in a menu that can be chosen, or -1.  */
static int menu_selectable(struct fb_window *win, int level, int from,
	int step)
{
	const struct menu *menu = menu_level(win, level);
	int count = menu_count(menu), index = from;

	for (int tries = 0; tries < count; tries++) {
		index = (index + step + count) % count;
		const struct menu_item *item = menu_nth(menu, index);
		if (item->enabled && item->type != MENU_ITEM_SEPARATOR)
			return index;
	}
	return -1;
}


static void ungrab(void);

static void menu_open_bar(struct fb_window *win, int index, bool keyboard)
{
	struct sdl_window *sw = SWIN(win);

	/*  The mouse goes back to the host while the menus are open:  */
	ungrab();
	sw->menu_depth = 1;
	sw->menu_opener[0] = index;
	sw->menu_highlight[0] = keyboard ? menu_selectable(win, 0, -1, 1) : -1;
}


/*  Opens the submenu of the highlighted item at a level, if it has one.  */
static bool menu_open_sub(struct fb_window *win, int level, bool keyboard)
{
	struct sdl_window *sw = SWIN(win);
	int index = sw->menu_highlight[level];
	const struct menu_item *item = menu_nth(menu_level(win, level), index);

	if (item == NULL || !item->enabled || item->type != MENU_ITEM_SUBMENU
	    || level + 1 >= MENU_DEPTH)
		return false;

	sw->menu_depth = level + 2;
	sw->menu_opener[level + 1] = index;
	sw->menu_highlight[level + 1] = keyboard ?
	    menu_selectable(win, level + 1, -1, 1) : -1;
	return true;
}


static void present(struct fb_window *win);

/*  Closes the menus, and does what the highlighted item at a level does.  */
static void menu_activate(struct fb_window *win, int level)
{
	struct sdl_window *sw = SWIN(win);
	struct menu_item *item = menu_nth(menu_level(win, level),
	    sw->menu_highlight[level]);

	if (item == NULL || !item->enabled || item->type == MENU_ITEM_SUBMENU
	    || item->type == MENU_ITEM_SEPARATOR)
		return;

	sw->menu_depth = 0;
	present(win);
	display_menu_activate(win->machine, item);
}


/*
 *  Handles a mouse event while the mouse isn't grabbed.  Returns true if
 *  the event was for the menus or the status line, and not a click in the
 *  screen (which grabs the mouse).
 */
static bool menu_mouse_event(struct fb_window *win, const SDL_Event *ev)
{
	struct sdl_window *sw = SWIN(win);
	int x, y, window_w, window_h, out_w, out_h, index;

	if (ev->type == SDL_MOUSEMOTION) {
		x = ev->motion.x;
		y = ev->motion.y;
	} else {
		x = ev->button.x;
		y = ev->button.y;
	}
	SDL_GetWindowSize(sw->window, &window_w, &window_h);
	output_size(win, &out_w, &out_h);
	if (window_w > 0 && window_h > 0) {
		x = x * out_w / window_w;
		y = y * out_h / window_h;
	}

	int bar_index = bar_hit(win, x, y);

	/*  When no menu is open, a press on a menu bar item opens its menu,
	    and other events in the bars are ignored:  */
	menu_check(win);
	if (sw->menu_depth == 0) {
		if (ev->type == SDL_MOUSEBUTTONDOWN && bar_index >= 0) {
			menu_open_bar(win, bar_index, false);
			present(win);
			return true;
		}
		return y < sw->bar_h || y >= out_h - sw->bar_h;
	}

	int level = menu_hit(win, x, y, &index);
	switch (ev->type) {

	/*  Moving over another menu bar item opens its menu, and moving
	    over an item highlights it, and opens its submenu:  */
	case SDL_MOUSEMOTION:
		if (bar_index >= 0) {
			if (bar_index != sw->menu_opener[0])
				menu_open_bar(win, bar_index, false);
		} else if (level >= 0) {
			sw->menu_depth = level + 1;
			sw->menu_highlight[level] = index;
			menu_open_sub(win, level, false);
		}
		break;

	/*  A press on the open menu's bar item, or outside of the menus,
	    closes them:  */
	case SDL_MOUSEBUTTONDOWN:
		if (bar_index >= 0) {
			if (bar_index == sw->menu_opener[0])
				sw->menu_depth = 0;
			else
				menu_open_bar(win, bar_index, false);
		} else if (level < 0)
			sw->menu_depth = 0;
		break;

	/*  A release on an item chooses it, both after a click on it and
	    after dragging to it from the menu bar:  */
	case SDL_MOUSEBUTTONUP:
		if (level >= 0 && index >= 0) {
			sw->menu_highlight[level] = index;
			menu_activate(win, level);
			return true;
		}
		break;
	}

	present(win);
	return true;
}


/*
 *  Handles a key press.  F10 opens the menus (unless the mouse is grabbed,
 *  when the guest gets it), and while they're open, they take all keys.
 *  Returns true if the key was for the menus.
 */
static bool menu_key_event(struct fb_window *win, const SDL_KeyboardEvent *key)
{
	struct sdl_window *sw = SWIN(win);
	int count = menu_count(menu_bar(win));

	menu_check(win);
	if (sw->menu_depth == 0) {
		if (key->keysym.sym != SDLK_F10 || grabbed != NULL || count == 0)
			return false;
		if (!key->repeat) {
			menu_open_bar(win, 0, true);
			present(win);
		}
		return true;
	}

	int level = sw->menu_depth - 1;
	int highlight = sw->menu_highlight[level];
	switch (key->keysym.sym) {

	case SDLK_F10:
		sw->menu_depth = 0;
		break;

	case SDLK_ESCAPE:
		sw->menu_depth = level;
		break;

	case SDLK_UP:
	case SDLK_DOWN:
		sw->menu_highlight[level] = menu_selectable(win, level,
		    highlight < 0 && key->keysym.sym == SDLK_UP ? 0 : highlight,
		    key->keysym.sym == SDLK_UP ? -1 : 1);
		break;

	case SDLK_RIGHT:
		if (!menu_open_sub(win, level, true))
			menu_open_bar(win, (sw->menu_opener[0] + 1) % count,
			    true);
		break;

	case SDLK_LEFT:
		if (level > 0)
			sw->menu_depth = level;
		else
			menu_open_bar(win, (sw->menu_opener[0] + count - 1)
			    % count, true);
		break;

	case SDLK_RETURN:
	case SDLK_KP_ENTER:
	case SDLK_SPACE:
		if (!menu_open_sub(win, level, true)) {
			menu_activate(win, level);
			return true;
		}
		break;
	}

	present(win);
	return true;
}


/****************************************************************************/


/*  Returns where the screen image goes: between the bars, keeping its
    aspect ratio.  */
static void screen_rect(struct fb_window *win, SDL_Rect *r)
{
	struct sdl_window *sw = SWIN(win);
	int out_w, out_h;

	output_size(win, &out_w, &out_h);
	int area_h = out_h - 2 * sw->bar_h;
	if (area_h < 1)
		area_h = 1;

	if ((int64_t) out_w * win->ysize <= (int64_t) area_h * win->xsize) {
		r->w = out_w;
		r->h = (int) ((int64_t) out_w * win->ysize / win->xsize);
	} else {
		r->h = area_h;
		r->w = (int) ((int64_t) area_h * win->xsize / win->ysize);
	}
	r->x = (out_w - r->w) / 2;
	r->y = sw->bar_h + (area_h - r->h) / 2;
}


/*  Redraws a window, from its texture and the menus.  */
static void present(struct fb_window *win)
{
	struct sdl_window *sw = SWIN(win);
	SDL_Rect r;

	font_init(win);
	screen_rect(win, &r);

	SDL_SetRenderDrawColor(sw->renderer, 0, 0, 0, 255);
	SDL_RenderClear(sw->renderer);
	SDL_RenderCopy(sw->renderer, sw->texture, NULL, &r);
	menu_draw(win);
	SDL_RenderPresent(sw->renderer);

	sw->present_pending = false;
	sw->last_present = SDL_GetTicks();
}


static void grab(struct fb_window *win)
{
	if (grabbed != NULL)
		return;

	if (SDL_SetRelativeMouseMode(SDL_TRUE) != 0) {
		debugmsg(SUBSYS_DISPLAY, "grab", VERBOSITY_WARNING,
		    "couldn't grab the mouse: %s", SDL_GetError());
		return;
	}

	debugmsg(SUBSYS_DISPLAY, "grab", VERBOSITY_DEBUG, "Grabbed mouse pointer.");
	grabbed = win;
	display_backend_set_title(win);
	present(win);
}


static void ungrab(void)
{
	if (grabbed == NULL)
		return;

	struct fb_window *win = grabbed;

	SDL_SetRelativeMouseMode(SDL_FALSE);
	grabbed = NULL;
	display_backend_set_title(win);
	present(win);

	debugmsg(SUBSYS_DISPLAY, "grab", VERBOSITY_DEBUG, "Releasing grab.");
}


/*  Sizes a window for its screen image and the bars.  */
/*
 *  Sizes a window for its screen image, at the machine's zoom: 50%, 100%,
 *  200%, ... of the image's size, in window points.  Zoom 0 is the largest
 *  of 200%, 100%, 50%, and 25% that fits the screen (leaving room for the
 *  window's title bar).
 */
static void set_window_size(struct fb_window *win)
{
	struct sdl_window *sw = SWIN(win);
	int zoom = win->machine->display_md.zoom, bars, display;
	static const int zooms[] = { 200, 100, 50, 25 };
	SDL_Rect usable;

	font_init(win);
	bars = 2 * sw->bar_h / sw->scale;

	if (zoom <= 0) {
		size_t i;
		display = SDL_GetWindowDisplayIndex(sw->window);
		if (SDL_GetDisplayUsableBounds(display < 0 ? 0 : display,
		    &usable) != 0)
			usable.w = usable.h = 0;
		zoom = 100;
		for (i = 0; usable.w > 0 && i < sizeof(zooms) / sizeof(zooms[0]);
		    i++) {
			zoom = zooms[i];
			if ((int64_t) win->xsize * zoom / 100 <= usable.w &&
			    (int64_t) win->ysize * zoom / 100 + bars + 40 <=
			    usable.h)
				break;
		}
	}

	SDL_SetWindowSize(sw->window, (int) ((int64_t) win->xsize * zoom / 100),
	    (int) ((int64_t) win->ysize * zoom / 100) + bars);
}


/*  (Re)creates a window's texture, for its current size.  */
static bool create_texture(struct fb_window *win)
{
	struct sdl_window *sw = SWIN(win);

	if (sw->texture != NULL)
		SDL_DestroyTexture(sw->texture);

	/*  SDL's RGB888 is 0x00RRGGBB, like the screen image:  */
	sw->texture = SDL_CreateTexture(sw->renderer, SDL_PIXELFORMAT_RGB888,
	    SDL_TEXTUREACCESS_STREAMING, win->xsize, win->ysize);
	if (sw->texture == NULL) {
		debugmsg(SUBSYS_DISPLAY, "texture", VERBOSITY_ERROR,
		    "couldn't create a texture: %s", SDL_GetError());
		return false;
	}

	display_backend_update(win, 0, 0, win->xsize, win->ysize);
	return true;
}


bool display_backend_available(void)
{
	return true;
}


/*
 *  display_backend_open():
 *
 *  Creates the SDL window for a new framebuffer window.
 */
bool display_backend_open(struct fb_window *win)
{
	struct sdl_window *sw;

	if (!init_sdl())
		return false;

	CHECK_ALLOCATION(sw = (struct sdl_window *) calloc(1, sizeof(*sw)));
	win->backend = sw;

	sw->window = SDL_CreateWindow(win->name, SDL_WINDOWPOS_UNDEFINED,
	    SDL_WINDOWPOS_UNDEFINED, win->xsize, win->ysize,
	    SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (sw->window == NULL) {
		debugmsg(SUBSYS_DISPLAY, "fb_init", VERBOSITY_ERROR,
		    "couldn't create a window: %s", SDL_GetError());
		return false;
	}
	SDL_SetWindowData(sw->window, "fb_window", win);

	/*  (Without vsync, which would make the emulator wait for it.)  */
	sw->renderer = SDL_CreateRenderer(sw->window, -1, 0);
	if (sw->renderer == NULL) {
		debugmsg(SUBSYS_DISPLAY, "fb_init", VERBOSITY_ERROR,
		    "couldn't create a renderer: %s", SDL_GetError());
		return false;
	}

	if (!create_texture(win))
		return false;

	set_window_size(win);
	display_backend_set_title(win);

	/*  Keep a window that grew on the screen:  */
	{
		SDL_Rect usable;
		int x, y, w, h, display = SDL_GetWindowDisplayIndex(sw->window);
		SDL_GetWindowPosition(sw->window, &x, &y);
		SDL_GetWindowSize(sw->window, &w, &h);
		if (SDL_GetDisplayUsableBounds(display < 0 ? 0 : display,
		    &usable) == 0 && (x + w > usable.x + usable.w ||
		    y + h > usable.y + usable.h))
			SDL_SetWindowPosition(sw->window,
			    SDL_WINDOWPOS_CENTERED_DISPLAY(display < 0 ? 0 :
			    display), SDL_WINDOWPOS_CENTERED_DISPLAY(
			    display < 0 ? 0 : display));
	}
	return true;
}


/*
 *  display_backend_resize():
 *
 *  Resizes a window, after the fb_window's images have been resized.
 */
void display_backend_resize(struct fb_window *win)
{
	set_window_size(win);
	if (!create_texture(win))
		exit(1);
}


/*
 *  display_backend_set_title():
 *
 *  Sets the title of a window, from its name and whether the mouse is
 *  grabbed.
 */
void display_backend_set_title(struct fb_window *win)
{
	size_t title_maxlen = strlen(win->name) + 100;
	char *title;
	CHECK_ALLOCATION(title = (char *) malloc(title_maxlen));

	snprintf(title, title_maxlen, "%s%s", win->name,
	    grabbed == win ? " (" UNGRAB_KEYS " to ungrab)" : "");
	SDL_SetWindowTitle(SWIN(win)->window, title);

	free(title);
}


/*
 *  display_backend_raise():
 *
 *  Brings a machine's first window to the front (after a file chooser).
 */
void display_backend_raise(struct machine *m)
{
	if (m->display_md.n_fb_windows > 0 &&
	    m->display_md.fb_windows[0]->backend != NULL)
		SDL_RaiseWindow(SWIN(m->display_md.fb_windows[0])->window);
}


void display_backend_update(struct fb_window *win, int x, int y, int w, int h)
{
	struct sdl_window *sw = SWIN(win);

	if (!sw->dirty) {
		sw->dirty = true;
		sw->dirty_x1 = x;
		sw->dirty_y1 = y;
		sw->dirty_x2 = x + w;
		sw->dirty_y2 = y + h;
		return;
	}

	if (x < sw->dirty_x1)		sw->dirty_x1 = x;
	if (y < sw->dirty_y1)		sw->dirty_y1 = y;
	if (x + w > sw->dirty_x2)	sw->dirty_x2 = x + w;
	if (y + h > sw->dirty_y2)	sw->dirty_y2 = y + h;
}


/*
 *  display_backend_flush():
 *
 *  Copies what has changed to the texture.  The window is redrawn now,
 *  or (if it was just redrawn) by display_backend_check_events().
 */
void display_backend_flush(struct fb_window *win)
{
	struct sdl_window *sw = SWIN(win);

	if (sw->dirty) {
		SDL_Rect r = { sw->dirty_x1, sw->dirty_y1,
		    sw->dirty_x2 - sw->dirty_x1, sw->dirty_y2 - sw->dirty_y1 };

		SDL_UpdateTexture(sw->texture, &r,
		    &win->screen[r.y * win->xsize + r.x],
		    win->xsize * sizeof(uint32_t));
		sw->dirty = false;
		sw->present_pending = true;
	}

	if (sw->present_pending &&
	    SDL_GetTicks() - sw->last_present >= MIN_PRESENT_INTERVAL_MS)
		present(win);
}


/*  Sends text from an SDL_TEXTINPUT event (UTF-8) as Latin-1.  */
static void text_input(struct machine *m, const char *text)
{
	const unsigned char *p = (const unsigned char *) text;

	while (*p != '\0') {
		if (*p < 0x80) {
			console_makeavail(m->main_console_handle, *p++);
		} else if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
			int c = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
			if (c < 256)
				console_makeavail(m->main_console_handle, c);
			p += 2;
		} else {
			/*  Not in Latin-1:  */
			p++;
			while ((*p & 0xc0) == 0x80)
				p++;
		}
	}
}


/*
 *  Handles a key press.  Printable characters arrive separately, as
 *  SDL_TEXTINPUT events; this sends everything else.
 *
 *  TODO:  It would be MUCH better if keys were converted into 'native
 *  scancodes', for example for the DECstation's keyboard or the PC-style
 *  8042 controller.
 */
static void key_down(struct fb_window *win, const SDL_Keysym *keysym)
{
	struct machine *m = win->machine;
	SDL_Keycode key = keysym->sym;
	bool numlock = (keysym->mod & KMOD_NUM) != 0;
	int ch = -1;

	switch (key) {
	case SDLK_RETURN:
	case SDLK_KP_ENTER:	ch = '\r'; break;
	case SDLK_BACKSPACE:	ch = '\b'; break;
	case SDLK_TAB:		ch = '\t'; break;
	case SDLK_ESCAPE:	ch = 27; break;
	case SDLK_DELETE:	ch = 127; break;

	case SDLK_F1:	case SDLK_F2:	case SDLK_F3:	case SDLK_F4:
	case SDLK_F5:	case SDLK_F6:	case SDLK_F7:	case SDLK_F8:
	case SDLK_F9:	case SDLK_F10:	case SDLK_F11:	case SDLK_F12:
		display_key(m, DISPLAY_KEY_F1 + key - SDLK_F1);
		break;

	/*  Cursor keys, and the numeric keypad's when Num Lock is off:  */
	case SDLK_UP:		display_key(m, DISPLAY_KEY_UP); break;
	case SDLK_DOWN:		display_key(m, DISPLAY_KEY_DOWN); break;
	case SDLK_LEFT:		display_key(m, DISPLAY_KEY_LEFT); break;
	case SDLK_RIGHT:	display_key(m, DISPLAY_KEY_RIGHT); break;
	case SDLK_HOME:		display_key(m, DISPLAY_KEY_HOME); break;
	case SDLK_END:		display_key(m, DISPLAY_KEY_END); break;
	case SDLK_PAGEUP:	display_key(m, DISPLAY_KEY_PAGE_UP); break;
	case SDLK_PAGEDOWN:	display_key(m, DISPLAY_KEY_PAGE_DOWN); break;
	case SDLK_KP_8:	if (!numlock) display_key(m, DISPLAY_KEY_UP); break;
	case SDLK_KP_2:	if (!numlock) display_key(m, DISPLAY_KEY_DOWN); break;
	case SDLK_KP_4:	if (!numlock) display_key(m, DISPLAY_KEY_LEFT); break;
	case SDLK_KP_6:	if (!numlock) display_key(m, DISPLAY_KEY_RIGHT); break;
	case SDLK_KP_7:	if (!numlock) display_key(m, DISPLAY_KEY_HOME); break;
	case SDLK_KP_1:	if (!numlock) display_key(m, DISPLAY_KEY_END); break;
	case SDLK_KP_9:	if (!numlock) display_key(m, DISPLAY_KEY_PAGE_UP); break;
	case SDLK_KP_3:	if (!numlock) display_key(m, DISPLAY_KEY_PAGE_DOWN); break;

	case SDLK_LCTRL:	case SDLK_RCTRL:
	case SDLK_LALT:		case SDLK_RALT:
	case SDLK_LGUI:		case SDLK_RGUI:
		if ((keysym->mod & KMOD_CTRL) &&
		    (keysym->mod & (KMOD_ALT | KMOD_GUI)))
			ungrab();
		break;

	default:
		/*  Ctrl+letter etc. give control characters:  */
		if ((keysym->mod & KMOD_CTRL) && ((key >= 'a' && key <= 'z') ||
		    (key >= '@' && key <= '_') || key == ' '))
			ch = key & 0x1f;
	}

	if (ch >= 0)
		console_makeavail(m->main_console_handle, ch);
}


static void handle_event(const SDL_Event *ev)
{
	struct fb_window *win;

	switch (ev->type) {

	case SDL_QUIT:
		printf("[ the framebuffer window was closed; exiting ]\n");
		exit(0);

	case SDL_WINDOWEVENT:
		if ((win = fb_window_of(ev->window.windowID)) == NULL)
			break;
		switch (ev->window.event) {
		case SDL_WINDOWEVENT_EXPOSED:
		case SDL_WINDOWEVENT_SIZE_CHANGED:
			present(win);
			break;
		case SDL_WINDOWEVENT_FOCUS_LOST:
			ungrab();
			break;
		}
		break;

	case SDL_KEYDOWN:
		if ((win = fb_window_of(ev->key.windowID)) != NULL &&
		    !menu_key_event(win, &ev->key))
			key_down(win, &ev->key.keysym);
		break;

	case SDL_TEXTINPUT:
		/*  (Control characters were sent by key_down(), and the menus
		    take all keys while they are open.)  */
		if ((win = fb_window_of(ev->text.windowID)) != NULL &&
		    !(SDL_GetModState() & KMOD_CTRL) && SWIN(win)->menu_depth == 0)
			text_input(win->machine, ev->text.text);
		break;

	case SDL_MOUSEMOTION:
		win = fb_window_of(ev->motion.windowID);
		if (win == NULL)
			break;
		if (grabbed != win) {
			menu_mouse_event(win, ev);
			break;
		}
		if (ev->motion.xrel != 0 || ev->motion.yrel != 0)
			console_mouse_coordinate_update(
			    ev->motion.xrel * win->scaledown,
			    ev->motion.yrel * win->scaledown, win->fb_number);
		break;

	/*  button = 1,2,3 = left,middle,right  */
	case SDL_MOUSEBUTTONDOWN:
		// TODO: console_mouse_button with multiple machines?!
		win = fb_window_of(ev->button.windowID);
		if (win == NULL)
			break;
		if (grabbed == win)
			console_mouse_button(ev->button.button, 1);
		else
			menu_mouse_event(win, ev);
		break;

	/*  A click in the screen grabs the mouse:  */
	case SDL_MOUSEBUTTONUP:
		win = fb_window_of(ev->button.windowID);
		if (win == NULL)
			break;
		if (grabbed == win)
			console_mouse_button(ev->button.button, 0);
		else if (!menu_mouse_event(win, ev))
			grab(win);
		break;
	}
}


/*
 *  display_backend_check_events():
 *
 *  Handles SDL events (for all windows, not only this machine's), and
 *  redraws this machine's windows if they were changed recently.
 */
void display_backend_check_events(struct machine *m)
{
	SDL_Event ev;

	if (!sdl_initialized)
		return;

	while (SDL_PollEvent(&ev))
		handle_event(&ev);

	/*  Redraw windows whose screen, menus or status line changed:  */
	struct display_menus *menus = display_menus(m);
	display_menus_update(m);
	for (int i = 0; i < m->display_md.n_fb_windows; i++) {
		struct fb_window *win = m->display_md.fb_windows[i];
		struct sdl_window *sw = SWIN(win);
		if (sw == NULL)
			continue;
		if (sw->present_pending || sw->menus_drawn != menus->generation
		    || sw->status_drawn != menus->status_generation)
			present(win);
	}
}

#endif	/*  WITH_SDL  */
