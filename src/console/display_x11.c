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
 *  X11 display backend: shows framebuffer windows (see display.c) as X11
 *  windows, and turns X11 keyboard and mouse events into console input.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "emul.h"
#include "machine.h"
#include "misc.h"
#include "display.h"


#ifndef	WITH_X11


/*  Dummy functions:  */
bool display_backend_available(void) { return false; }
bool display_backend_open(struct fb_window *win) { return false; }
void display_backend_resize(struct fb_window *win) { }
void display_backend_set_title(struct fb_window *win) { }
void display_backend_update(struct fb_window *win, int x, int y,
	int w, int h) { }
void display_backend_flush(struct fb_window *win) { }
void display_backend_check_events(struct machine *m) { }


#else	/*  WITH_X11  */


#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

/*  A framebuffer window's X11 state:  */
struct x11_window {
	Display		*display;
	int		screen;
	int		depth;
	Window		window;
	GC		gc;

	/*
	 *  If direct is true, image's data is the fb_window's screen
	 *  image.  Otherwise, image has its own data, which updates are
	 *  converted into.
	 */
	XImage		*image;
	bool		direct;
	bool		truecolor;
	unsigned long	masks[3];
	int		shifts[3];
	int		bits[3];
	unsigned long	graycolor[N_GRAYCOLORS];

	/*  Host's X11 cursor:  */
	Cursor		host_cursor;
	Pixmap		host_cursor_pixmap;
};

#define	XWIN(win)	((struct x11_window *) (win)->backend)

/*
 *  CTRL+ALT ungrabs the mouse.  On macOS (XQuartz), Option is Alt (or
 *  Mode_switch, depending on the XQuartz settings), and Command is
 *  Meta; any of these work as the "ALT" key, on either side.
 */
#ifdef __APPLE__
#define	UNGRAB_KEYS	"Ctrl+Option"
#else
#define	UNGRAB_KEYS	"CTRL+ALT"
#endif

static bool ctrl_down = false;
static bool alt_down = false;

static bool is_ungrab_alt(KeySym key)
{
	return key == XK_Alt_L || key == XK_Alt_R ||
	    key == XK_Meta_L || key == XK_Meta_R ||
	    key == XK_Mode_switch;
}

static struct fb_window *grabbed = NULL;
static bool mouseExplicityMoved = false;
static int mouseXbeforeGrab = 0;
static int mouseYbeforeGrab = 0;
static int mouseXofLastEvent = 0;
static int mouseYofLastEvent = 0;

static bool mouseCursorHidden = false;


static void x11_unhide_cursor()
{
	if (!mouseCursorHidden)
		return;

	struct x11_window *xw = XWIN(grabbed);

	/*  Remove the old X11 host cursor:  */
	XUndefineCursor(xw->display, xw->window);
	XFreeCursor(xw->display, xw->host_cursor);
	xw->host_cursor = 0;

	mouseCursorHidden = false;
}


static void x11_hide_cursor()
{
	if (mouseCursorHidden)
		return;

	struct x11_window *xw = XWIN(grabbed);
	XColor black;

	memset(&black, 0, sizeof(black));

	/*  Create a new "empy" X11 host cursor:  */
	if (xw->host_cursor_pixmap != 0) {
		XFreePixmap(xw->display, xw->host_cursor_pixmap);
		xw->host_cursor_pixmap = 0;
	}

	xw->host_cursor_pixmap = XCreatePixmap(xw->display, xw->window, 1, 1, 1);

	GC tmpgc = XCreateGC(xw->display, xw->host_cursor_pixmap, 0,0);

	XDrawPoint(xw->display, xw->host_cursor_pixmap, tmpgc, 0, 0);

	XFreeGC(xw->display, tmpgc);

	xw->host_cursor = XCreatePixmapCursor(xw->display,
	    xw->host_cursor_pixmap, xw->host_cursor_pixmap,
	    &black, &black, 0, 0);

	// For testing:
	// xw->host_cursor = XCreateFontCursor(xw->display, XC_coffee_mug);

	if (xw->host_cursor != 0)
		XDefineCursor(xw->display, xw->window, xw->host_cursor);

	mouseCursorHidden = true;
}


static void setMousePointerCoordinates(struct fb_window *fbwin, int x, int y)
{
	struct x11_window *xw = XWIN(fbwin);

	XWarpPointer(xw->display, None,
	    DefaultRootWindow(xw->display), 0, 0, 0, 0, x, y);
	XFlush(xw->display);

	mouseExplicityMoved = true;
}


/*
 *  While grabbed, the (hidden) host mouse pointer is kept inside the
 *  framebuffer window. If it were elsewhere on the screen, clicks could
 *  reach other windows. (With a rootless X server, such as XQuartz on
 *  macOS, the host window system acts on those clicks despite the grab,
 *  and the emulator loses focus.)
 */
static void moveMouseToCenterOfWindow(struct fb_window *fbwin)
{
	struct x11_window *xw = XWIN(fbwin);

	XWarpPointer(xw->display, None, xw->window, 0, 0, 0, 0,
	    fbwin->xsize / 2, fbwin->ysize / 2);
	XFlush(xw->display);

	mouseExplicityMoved = true;
}

static void grab(struct fb_window *fbwin)
{
	if (grabbed != NULL)
		return;

	struct x11_window *xw = XWIN(fbwin);
	Window xqpWindow;
	int rootx, rooty, x, y;
	unsigned int mask;
	int res = XQueryPointer(xw->display,
	    RootWindow(xw->display, DefaultScreen(xw->display)),
	    &xqpWindow,
            &xqpWindow, &rootx, &rooty, &x, &y,
            &mask);
        if (res != True)
        	return;

	debugmsg(SUBSYS_X11, "grab", VERBOSITY_DEBUG, "Mouse coordinates "
	    "before grab: %i, %i", rootx, rooty);
	mouseXbeforeGrab = rootx;
	mouseYbeforeGrab = rooty;

	res = XGrabPointer(xw->display, xw->window, False,
	    ButtonPressMask | ButtonReleaseMask | PointerMotionMask | FocusChangeMask |
	    EnterWindowMask | LeaveWindowMask,
	    GrabModeAsync, GrabModeAsync,
	    xw->window, None, CurrentTime);

	if (res == GrabSuccess)
		grabbed = fbwin;

	debugmsg(SUBSYS_X11, "grab", VERBOSITY_DEBUG, "Grab mouse pointer: %s",
	    res == GrabSuccess ? "success" : "FAILURE");

	if (res != GrabSuccess)
		return;

	x11_hide_cursor();

	moveMouseToCenterOfWindow(fbwin);

	display_backend_set_title(fbwin);
}


static void ungrab()
{
	if (grabbed == NULL)
		return;

	struct fb_window *fbwin = grabbed;

	x11_unhide_cursor();

	grabbed = NULL;

	display_backend_set_title(fbwin);

	debugmsg(SUBSYS_X11, "grab", VERBOSITY_DEBUG, "Releasing grab.");

	XUngrabPointer(XWIN(fbwin)->display, CurrentTime);

	setMousePointerCoordinates(fbwin, mouseXbeforeGrab, mouseYbeforeGrab);
}


bool display_backend_available(void)
{
	return true;
}


/*
 *  Creates the XImage for a window's current size.  If the X server
 *  can show 0x00RRGGBB pixels as they are, the XImage uses the
 *  fb_window's screen image.
 */
static void create_image(struct fb_window *win)
{
	struct x11_window *xw = XWIN(win);
	int pad = xw->depth > 16 ? 32 : (xw->depth > 8 ? 16 : 8);
	unsigned int one = 1;

	if (xw->image != NULL) {
		if (xw->direct)
			xw->image->data = NULL;
		XDestroyImage(xw->image);
	}

	xw->direct = xw->truecolor && xw->depth == 24 &&
	    xw->masks[0] == 0xff0000 && xw->masks[1] == 0xff00 &&
	    xw->masks[2] == 0xff;

	xw->image = XCreateImage(xw->display, CopyFromParent, xw->depth,
	    ZPixmap, 0, NULL, win->xsize, win->ysize, pad, 0);
	CHECK_ALLOCATION(xw->image);

	if (xw->direct && xw->image->bits_per_pixel == 32) {
		/*  Xlib swaps the bytes, if the X server's order differs:  */
		xw->image->byte_order = *(unsigned char *) &one ?
		    LSBFirst : MSBFirst;
		xw->image->data = (char *) win->screen;
	} else {
		xw->direct = false;
		CHECK_ALLOCATION(xw->image->data = (char *) calloc(
		    win->ysize, xw->image->bytes_per_line));
	}
}


/*  Converts part of the screen image into a non-direct XImage.  */
static void convert_image(struct fb_window *win, int x1, int y1, int w, int h)
{
	struct x11_window *xw = XWIN(win);

	for (int y = y1; y < y1 + h; y++)
		for (int x = x1; x < x1 + w; x++) {
			uint32_t p = win->screen[y * win->xsize + x];
			int rgb[3] = { (int) (p >> 16) & 255,
			    (int) (p >> 8) & 255, (int) p & 255 };
			unsigned long color = 0;

			if (xw->truecolor) {
				for (int c = 0; c < 3; c++)
					color |= ((unsigned long) rgb[c] >>
					    (8 - xw->bits[c])) << xw->shifts[c];
			} else
				color = xw->graycolor[15 * (rgb[0] + rgb[1] +
				    rgb[2]) / (255 * 3)];

			XPutPixel(xw->image, x, y, color);
		}
}


/*
 *  display_backend_open():
 *
 *  Creates the X11 window for a new framebuffer window.
 */
bool display_backend_open(struct fb_window *win)
{
	struct machine *m = win->machine;
	struct x11_window *xw;
	Display *x11_display;
	char *display_name;
	Visual *visual;

	/*  Which display name?  */
	display_name = NULL;
	if (m->display_md.n_display_names > 0) {
		display_name = m->display_md.display_names[
		    m->display_md.current_display_name_nr];
		m->display_md.current_display_name_nr ++;
		m->display_md.current_display_name_nr %= m->display_md.n_display_names;
	}

	if (display_name != NULL)
		debugmsg(SUBSYS_X11, "fb_init", VERBOSITY_INFO,
		    "framebuffer window %i, %ix%i, DISPLAY"
		    "=%s", win->fb_number, win->xsize, win->ysize,
		    display_name);

	x11_display = XOpenDisplay(display_name);

	if (x11_display == NULL) {
		debugmsg(SUBSYS_X11, "fb_init", VERBOSITY_ERROR,
		    "couldn't open display '%s'", display_name != NULL ?
		    display_name : "");
		return false;
	}

	CHECK_ALLOCATION(xw = (struct x11_window *) calloc(1, sizeof(*xw)));
	win->backend = xw;

	xw->display = x11_display;
	xw->screen = DefaultScreen(x11_display);
	xw->depth = DefaultDepth(x11_display, xw->screen);
	visual = DefaultVisual(x11_display, xw->screen);

#if defined(__cplusplus) || defined(c_plusplus)
	xw->truecolor = visual->c_class == TrueColor;
#else
	xw->truecolor = visual->class == TrueColor;
#endif
	xw->masks[0] = visual->red_mask;
	xw->masks[1] = visual->green_mask;
	xw->masks[2] = visual->blue_mask;
	for (int c = 0; c < 3; c++) {
		while (xw->masks[c] != 0 && !((xw->masks[c] >> xw->shifts[c]) & 1))
			xw->shifts[c]++;
		while (xw->bits[c] < 8 && ((xw->masks[c] >> xw->shifts[c]) >>
		    xw->bits[c]) & 1)
			xw->bits[c]++;
	}

	if (!xw->truecolor) {
		debugmsg(SUBSYS_X11, "fb_init", VERBOSITY_WARNING,
		    "the X11 display isn't TrueColor; "
		    "using only 16 grayscales instead");

		for (int i=0; i<N_GRAYCOLORS; i++) {
			XColor color;
			color.red = color.green = color.blue = i * 0x1111;
			color.flags = DoRed | DoGreen | DoBlue;
			XAllocColor(x11_display, DefaultColormap(x11_display,
			    xw->screen), &color);
			xw->graycolor[i] = color.pixel;
		}
	}

	xw->window = XCreateWindow(
	    x11_display, DefaultRootWindow(x11_display),
	    0, 0, win->xsize, win->ysize,
	    0, CopyFromParent, InputOutput, CopyFromParent, 0,0);
	XSetWindowBackground(x11_display, xw->window,
	    BlackPixel(x11_display, xw->screen));

	display_backend_set_title(win);

	XSelectInput(x11_display, xw->window,
	    StructureNotifyMask | ExposureMask | ButtonPressMask | FocusChangeMask |
	    ButtonReleaseMask | PointerMotionMask | KeyPressMask | KeyReleaseMask);
	xw->gc = XCreateGC(x11_display, xw->window, 0,0);

	/*  Make sure the window is mapped:  */
	XMapRaised(x11_display, xw->window);

	create_image(win);

	return true;
}


/*
 *  display_backend_resize():
 *
 *  Resizes a window, after the fb_window's images have been resized.
 */
void display_backend_resize(struct fb_window *win)
{
	struct x11_window *xw = XWIN(win);

	create_image(win);
	XResizeWindow(xw->display, xw->window, win->xsize, win->ysize);
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
	    grabbed != NULL ? " (" UNGRAB_KEYS " to ungrab)" : "");

	XSetStandardProperties(XWIN(win)->display, XWIN(win)->window, title,
	    "GXemul " VERSION, None, NULL, 0, NULL);

	free(title);
}


void display_backend_update(struct fb_window *win, int x, int y, int w, int h)
{
	struct x11_window *xw = XWIN(win);

	if (!xw->direct)
		convert_image(win, x, y, w, h);

	XPutImage(xw->display, xw->window, xw->gc, xw->image, x, y, x, y, w, h);
}


void display_backend_flush(struct fb_window *win)
{
	XFlush(XWIN(win)->display);
}


/*
 *  display_backend_check_events():
 *
 *  Check for X11 events on a specific machine.
 */
void display_backend_check_events(struct machine *m)
{
	int fb_nr;

	for (fb_nr = 0; fb_nr < m->display_md.n_fb_windows; fb_nr ++) {
		struct fb_window *fbwin = m->display_md.fb_windows[fb_nr];
		struct x11_window *xw = XWIN(fbwin);
		XEvent event;
		bool need_redraw = false;

		while (XPending(xw->display)) {
			XNextEvent(xw->display, &event);

			if (event.type == ConfigureNotify) {
				need_redraw = true;
			}

			/*  Ignore focus changes caused by grabs:  */
			if (event.type == FocusOut &&
			    event.xfocus.mode == NotifyNormal)
				ungrab();

			if (event.type == Expose && event.xexpose.count == 0) {
				/*
				 *  TODO:  the xexpose struct has x,y,width,
				 *  height. Those could be used to only redraw
				 *  the part of the framebuffer that was
				 *  exposed.
				 */
				need_redraw = true;
			}

			if (event.type == MotionNotify && mouseExplicityMoved) {
				// debugmsg(SUBSYS_X11, "event", VERBOSITY_WARNING,
				//    "mouse explicitly moved to screen center; ignored.");

				mouseExplicityMoved = false;

				mouseXofLastEvent = event.xmotion.x;
				mouseYofLastEvent = event.xmotion.y;

			} else if (event.type == MotionNotify) {
				// debugmsg(SUBSYS_X11, "event", VERBOSITY_WARNING,
				//    "mouse moved to %i, %i",
				//    event.xmotion.x, event.xmotion.y);

				int dx = event.xmotion.x - mouseXofLastEvent;
				int dy = event.xmotion.y - mouseYofLastEvent;

				mouseXofLastEvent = event.xmotion.x;
				mouseYofLastEvent = event.xmotion.y;

				if (grabbed == fbwin && (dx != 0 || dy != 0)) {
					// debugmsg(SUBSYS_X11, "event", VERBOSITY_WARNING,
					//    "mouse moved dx %i, %i", dx, dy);

					dx *= fbwin->scaledown;
					dy *= fbwin->scaledown;
					console_mouse_coordinate_update(dx, dy, fb_nr);

					// Keep the mouse pointer away from the
					// edges of the window, so that it can
					// always move further.
					int w = fbwin->xsize;
					int h = fbwin->ysize;
					int x = event.xmotion.x;
					int y = event.xmotion.y;

					if (x < w * 1 / 5 || y < h * 1 / 5 ||
					    x >= w * 4 / 5 || y >= h * 4 / 5)
						moveMouseToCenterOfWindow(fbwin);
				}
			}

			if (event.type == ButtonPress) {
				/*  debug("[ X11 ButtonPress: %i ]\n",
				    event.xbutton.button);  */
				/*  button = 1,2,3 = left,middle,right  */

				// TODO: console_mouse_button with multiple machines?!
				if (grabbed == fbwin)
					console_mouse_button(event.xbutton.button, 1);
			}

			if (event.type == ButtonRelease) {
				/*  debug("[ X11 ButtonRelease: %i ]\n",
				    event.xbutton.button);  */
				/*  button = 1,2,3 = left,middle,right  */

				// TODO: console_mouse_button with multiple machines?!
				if (grabbed == fbwin)
					console_mouse_button(event.xbutton.button, 0);

				grab(fbwin);
			}

			if (event.type == KeyRelease) {
				KeySym key = XLookupKeysym(&event.xkey, 0);

				if (key == XK_Control_L || key == XK_Control_R)
					ctrl_down = false;
				if (is_ungrab_alt(key))
					alt_down = false;
			}

			if (event.type == KeyPress) {
				char text[15];
				KeySym key;

				memset(text, 0, sizeof(text));

				/*
				 *  Keys are recognized by their keysyms, not
				 *  their keycodes, since keycodes differ
				 *  between X servers (XQuartz on macOS uses
				 *  the Mac's own key numbers, so for example
				 *  its Right Shift has the keycode that F2
				 *  has on Linux).
				 */
				if (XLookupString(&event.xkey, text,
				    sizeof(text), &key, 0) == 1) {
					console_makeavail(
					    m->main_console_handle, text[0]);
				} else {
					/*
					 *  TODO:  It would be MUCH better if
					 *  these were converted into 'native
					 *  scancodes', for example for the
					 *  DECstation's keyboard or the
					 *  PC-style 8042 controller.
					 */
					switch (key) {
					case XK_Escape:
						console_makeavail(m->
						    main_console_handle, 27);
						break;
					/*  F1..F4 send ESC O P..S, and F5..F12
					    send ESC [ n ~, as on an xterm:  */
					case XK_F1:
					case XK_F2:
					case XK_F3:
					case XK_F4:
						console_makeavail(m->
						    main_console_handle, 27);
						console_makeavail(m->
						    main_console_handle, 'O');
						console_makeavail(m->
						    main_console_handle, 'P' +
						    key - XK_F1);
						break;
					case XK_F5:
					case XK_F6:
					case XK_F7:
					case XK_F8:
					case XK_F9:
					case XK_F10:
					case XK_F11:
					case XK_F12:
						{
							static const char *codes[] = {
							    "15", "17", "18", "19",
							    "20", "21", "23", "24" };
							const char *c =
							    codes[key - XK_F5];

							console_makeavail(m->
							    main_console_handle, 27);
							console_makeavail(m->
							    main_console_handle, '[');
							for (; *c != '\0'; c++)
								console_makeavail(m->
								    main_console_handle, *c);
							console_makeavail(m->
							    main_console_handle, '~');
						}
						break;
					/*  Cursor keys, and the numeric
					    keypad's:  */
					case XK_Up:
					case XK_KP_Up:
					case XK_Down:
					case XK_KP_Down:
					case XK_Left:
					case XK_KP_Left:
					case XK_Right:
					case XK_KP_Right:
						console_makeavail(m->
						    main_console_handle, 27);
						console_makeavail(m->
						    main_console_handle, '[');
						console_makeavail(m->
						    main_console_handle,
						    (key == XK_Up || key == XK_KP_Up)? 'A' : (
						    (key == XK_Down || key == XK_KP_Down)? 'B' : (
						    (key == XK_Right || key == XK_KP_Right)? 'C' : (
						    'D'))));
						break;
					case XK_Home:
					case XK_KP_Home:
						console_makeavail(m->
						    main_console_handle, 27);
						console_makeavail(m->
						    main_console_handle, '[');
						console_makeavail(m->
						    main_console_handle, 'H');
						break;
					case XK_End:
					case XK_KP_End:
						console_makeavail(m->
						    main_console_handle, 27);
						console_makeavail(m->
						    main_console_handle, '[');
						console_makeavail(m->
						    main_console_handle, 'F');
						break;
					case XK_Page_Up:
					case XK_KP_Page_Up:
						console_makeavail(m->
						    main_console_handle, 27);
						console_makeavail(m->
						    main_console_handle, '[');
						console_makeavail(m->
						    main_console_handle, '5');
						console_makeavail(m->
						    main_console_handle, '~');
						break;
					case XK_Page_Down:
					case XK_KP_Page_Down:
						console_makeavail(m->
						    main_console_handle, 27);
						console_makeavail(m->
						    main_console_handle, '[');
						console_makeavail(m->
						    main_console_handle, '6');
						console_makeavail(m->
						    main_console_handle, '~');
						break;

					case XK_Control_L:
					case XK_Control_R:
						ctrl_down = true;
						if (ctrl_down && alt_down)
							ungrab();
						break;

					default:
						if (is_ungrab_alt(key)) {
							alt_down = true;
							if (ctrl_down && alt_down)
								ungrab();
						}
						break;
					}
				}
			}
		}


		if (need_redraw) {
			XPutImage(xw->display, xw->window, xw->gc, xw->image,
			    0, 0, 0, 0, fbwin->xsize, fbwin->ysize);
			XFlush(xw->display);
		}
	}
}

#endif	/*  WITH_X11  */
