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
 *  Each window's screen image is kept in a streaming texture, which is
 *  scaled to the window's size (keeping the aspect ratio).  Clicking in a
 *  window grabs the mouse, using SDL's relative mouse mode; Ctrl+Alt
 *  releases it.
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


/*  Redraws a window from its texture.  */
static void present(struct fb_window *win)
{
	struct sdl_window *sw = SWIN(win);

	SDL_SetRenderDrawColor(sw->renderer, 0, 0, 0, 255);
	SDL_RenderClear(sw->renderer);
	SDL_RenderCopy(sw->renderer, sw->texture, NULL, NULL);
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
}


static void ungrab(void)
{
	if (grabbed == NULL)
		return;

	struct fb_window *win = grabbed;

	SDL_SetRelativeMouseMode(SDL_FALSE);
	grabbed = NULL;
	display_backend_set_title(win);

	debugmsg(SUBSYS_DISPLAY, "grab", VERBOSITY_DEBUG, "Releasing grab.");
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

	SDL_RenderSetLogicalSize(sw->renderer, win->xsize, win->ysize);
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

	display_backend_set_title(win);
	return true;
}


/*
 *  display_backend_resize():
 *
 *  Resizes a window, after the fb_window's images have been resized.
 */
void display_backend_resize(struct fb_window *win)
{
	SDL_SetWindowSize(SWIN(win)->window, win->xsize, win->ysize);
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
		if ((win = fb_window_of(ev->key.windowID)) != NULL)
			key_down(win, &ev->key.keysym);
		break;

	case SDL_TEXTINPUT:
		/*  (Control characters were sent by key_down().)  */
		if ((win = fb_window_of(ev->text.windowID)) != NULL &&
		    !(SDL_GetModState() & KMOD_CTRL))
			text_input(win->machine, ev->text.text);
		break;

	case SDL_MOUSEMOTION:
		win = fb_window_of(ev->motion.windowID);
		if (win != NULL && grabbed == win &&
		    (ev->motion.xrel != 0 || ev->motion.yrel != 0))
			console_mouse_coordinate_update(
			    ev->motion.xrel * win->scaledown,
			    ev->motion.yrel * win->scaledown, win->fb_number);
		break;

	/*  button = 1,2,3 = left,middle,right  */
	case SDL_MOUSEBUTTONDOWN:
		// TODO: console_mouse_button with multiple machines?!
		win = fb_window_of(ev->button.windowID);
		if (win != NULL && grabbed == win)
			console_mouse_button(ev->button.button, 1);
		break;

	case SDL_MOUSEBUTTONUP:
		win = fb_window_of(ev->button.windowID);
		if (win == NULL)
			break;
		if (grabbed == win)
			console_mouse_button(ev->button.button, 0);
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

	for (int i = 0; i < m->display_md.n_fb_windows; i++) {
		struct fb_window *win = m->display_md.fb_windows[i];
		if (SWIN(win)->present_pending)
			present(win);
	}
}

#endif	/*  WITH_SDL  */
