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
 *  Framebuffer windows, independent of the host's window system.
 *
 *  Framebuffer devices draw 0x00RRGGBB pixels into fb_window->pixels, and
 *  tell display_update() which part changed.  That part is copied to
 *  fb_window->screen, with the emulated cursor on top, and handed to the
 *  backend (display_sdl.c) to show, or to the VNC server (vnc.c) when
 *  there is one, instead.  (Windows without a backend window have a NULL
 *  fb_window->backend.)
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>

#include "console.h"
#include "emul.h"
#include "machine.h"
#include "misc.h"
#include "display.h"
#include "vnc.h"

#ifdef WITH_ZLIB
#include <zlib.h>
#endif


#ifndef	WITH_DISPLAY

/*  Without a backend, there are no windows:  */
bool display_backend_available(void) { return false; }
bool display_backend_open(struct fb_window *win) { return false; }
void display_backend_resize(struct fb_window *win) { }
void display_backend_zoom(struct fb_window *win) { }
void display_backend_set_title(struct fb_window *win) { }
void display_backend_update(struct fb_window *win, int x, int y,
	int w, int h) { }
void display_backend_flush(struct fb_window *win) { }
void display_backend_check_events(struct machine *m) { }
void display_backend_raise(struct machine *m) { }

#endif


/*
 *  display_init():
 *
 *  Initializes a machine's display (but doesn't create any windows).
 *
 *  It is then up to individual drivers, for example framebuffer devices,
 *  to initialize their own windows.
 */
void display_init(struct machine *m)
{
	m->display_md.n_fb_windows = 0;

	if (m->display_md.n_display_names > 0)
		debugmsg(SUBSYS_DISPLAY, "init", VERBOSITY_WARNING,
		    "X11 display names (add_x11_display or -z) are ignored");
}


/*  Allocates a window's (black) images.  */
static void alloc_images(struct fb_window *win, int xsize, int ysize)
{
	size_t n = (size_t) xsize * ysize;

	free(win->pixels);
	free(win->screen);
	CHECK_ALLOCATION(win->pixels = (uint32_t *) calloc(n, sizeof(uint32_t)));
	CHECK_ALLOCATION(win->screen = (uint32_t *) calloc(n, sizeof(uint32_t)));

	win->xsize = xsize;
	win->ysize = ysize;
}


/*
 *  display_fb_init():
 *
 *  Initializes a framebuffer window.  Returns NULL if there is no way to
 *  show one.
 */
struct fb_window *display_fb_init(int xsize, int ysize, const char *name,
	int scaledown, struct machine *m)
{
	struct fb_window *win;

	if (!display_backend_available() && !vnc_active())
		return NULL;

	CHECK_ALLOCATION(win = (struct fb_window *) calloc(1, sizeof(*win)));

	win->fb_number = m->display_md.n_fb_windows;
	win->machine = m;
	win->scaledown = scaledown;
	CHECK_ALLOCATION(win->name = strdup(name));
	alloc_images(win, xsize, ysize);

	/*  Fill the 64x64 "hardware" cursor with white pixels:  */
	for (int y=0; y<CURSOR_MAXY; y++)
		for (int x=0; x<CURSOR_MAXX; x++)
			win->cursor_pixels[y][x] = N_GRAYCOLORS-1;

	if (!vnc_active() && !display_backend_open(win)) {
		debugmsg(SUBSYS_DISPLAY, "fb_init", VERBOSITY_ERROR,
		    "couldn't open a window for '%s'", name);
		exit(1);
	}

	CHECK_ALLOCATION(m->display_md.fb_windows = (struct fb_window **)
	    realloc(m->display_md.fb_windows, sizeof(struct fb_window *) *
	    (m->display_md.n_fb_windows + 1)));
	m->display_md.fb_windows[m->display_md.n_fb_windows ++] = win;

	display_update(win, 0, 0, xsize - 1, ysize - 1);
	display_flush(win);

	return win;
}


/*
 *  display_fb_resize():
 *
 *  Sets a new size for a framebuffer window.  The image is cleared.
 */
void display_fb_resize(struct fb_window *win, int new_xsize, int new_ysize)
{
	if (win == NULL) {
		debugmsg(SUBSYS_DISPLAY, "resize", VERBOSITY_ERROR, "win == NULL");
		return;
	}

	alloc_images(win, new_xsize, new_ysize);
	if (win->backend != NULL)
		display_backend_resize(win);
}


/*
 *  display_set_name():
 *
 *  Sets the name (title) of a framebuffer window.
 */
void display_set_name(struct fb_window *win, const char *name)
{
	free(win->name);
	CHECK_ALLOCATION(win->name = strdup(name));
	if (win->backend != NULL)
		display_backend_set_title(win);
}


/*
 *  Draws the part of the shown cursor which is inside the rectangle
 *  x1,y1 .. x2,y2 (exclusive) onto the screen image.
 */
static void draw_cursor(struct fb_window *win, int x1, int y1, int x2, int y2)
{
	int sd = win->scaledown;
	int x0 = win->shown_cursor_x / sd;
	int y0 = win->shown_cursor_y / sd;
	uint32_t white = DISPLAY_GRAY(N_GRAYCOLORS-1);
	uint32_t black = DISPLAY_GRAY(0);

	for (int y = 0; y < win->shown_cursor_ysize; y += sd) {
		for (int x = 0; x < win->shown_cursor_xsize; x += sd) {
			int px = x0 + x/sd;
			int py = y0 + y/sd;
			int p = 0, n = 0, c = 0;

			if (px < x1 || py < y1 || px >= x2 || py >= y2)
				continue;

			for (int suby = 0; suby < sd && y+suby < CURSOR_MAXY;
			    suby++)
				for (int subx = 0; subx < sd &&
				    x+subx < CURSOR_MAXX; subx++) {
					c = win->cursor_pixels[y+suby][x+subx];
					if (c >= 0) {
						p += c;
						n++;
					}
				}
			if (n > 0)
				p /= n;
			else
				p = c;

			uint32_t *pixel = &win->screen[py * win->xsize + px];
			switch (p) {
			case CURSOR_COLOR_TRANSPARENT:
				break;
			case CURSOR_COLOR_INVERT:
				*pixel = *pixel != white ? white : black;
				break;
			default:	/*  Normal grayscale:  */
				*pixel = DISPLAY_GRAY(p);
			}
		}
	}
}


/*
 *  display_update():
 *
 *  Shows a changed part of a framebuffer window's pixels, x1,y1 .. x2,y2
 *  (inclusive, in window coordinates).
 */
void display_update(struct fb_window *win, int x1, int y1, int x2, int y2)
{
	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x2 >= win->xsize)
		x2 = win->xsize - 1;
	if (y2 >= win->ysize)
		y2 = win->ysize - 1;
	if (x2 < x1 || y2 < y1)
		return;

	for (int y = y1; y <= y2; y++)
		memcpy(&win->screen[y * win->xsize + x1],
		    &win->pixels[y * win->xsize + x1],
		    (x2 - x1 + 1) * sizeof(uint32_t));

	if (win->shown_cursor_on)
		draw_cursor(win, x1, y1, x2 + 1, y2 + 1);

	if (win->backend != NULL)
		display_backend_update(win, x1, y1, x2 - x1 + 1, y2 - y1 + 1);
	vnc_update(win, x1, y1, x2 - x1 + 1, y2 - y1 + 1);
	win->need_flush = true;
}


/*  Updates the part of the screen that the shown cursor covers.  */
static void update_cursor_area(struct fb_window *win)
{
	int sd = win->scaledown;
	int x = win->shown_cursor_x / sd, y = win->shown_cursor_y / sd;

	display_update(win, x, y, x + win->shown_cursor_xsize / sd,
	    y + win->shown_cursor_ysize / sd);
}


/*
 *  display_update_cursor():
 *
 *  Shows the cursor, if it has been changed (cursor_x etc.) since it was
 *  last shown.  (The cursor_on value can be changed to another non-zero
 *  value, to show a new cursor_pixels image.)
 */
void display_update_cursor(struct fb_window *win)
{
	if (win->cursor_on == win->shown_cursor_on &&
	    win->cursor_x == win->shown_cursor_x &&
	    win->cursor_y == win->shown_cursor_y &&
	    win->cursor_xsize == win->shown_cursor_xsize &&
	    win->cursor_ysize == win->shown_cursor_ysize)
		return;

	/*  Remove the old cursor, if any, and then draw the new:  */
	bool was_on = win->shown_cursor_on;
	win->shown_cursor_on = 0;
	if (was_on)
		update_cursor_area(win);

	win->shown_cursor_on = win->cursor_on;
	win->shown_cursor_x = win->cursor_x;
	win->shown_cursor_y = win->cursor_y;
	win->shown_cursor_xsize = win->cursor_xsize;
	win->shown_cursor_ysize = win->cursor_ysize;
	if (win->shown_cursor_on)
		update_cursor_area(win);
}


/*
 *  display_flush():
 *
 *  Makes sure that what has been updated is shown.
 */
void display_flush(struct fb_window *win)
{
	if (!win->need_flush)
		return;

	if (win->backend != NULL)
		display_backend_flush(win);
	win->need_flush = false;
}


/*
 *  display_key():
 *
 *  Sends a key (enum display_key) to a machine's console, as the escape
 *  sequence an xterm sends:  F1..F4 send ESC O P..S, F5..F12 send
 *  ESC [ n ~, and the cursor keys ESC [ A..D.
 */
void display_key(struct machine *m, int key)
{
	static const char *f5_f12[] = { "15", "17", "18", "19",
	    "20", "21", "23", "24" };
	char seq[8];

	if (key >= DISPLAY_KEY_F1 && key < DISPLAY_KEY_F1 + 4)
		snprintf(seq, sizeof(seq), "\033O%c", 'P' + key - DISPLAY_KEY_F1);
	else if (key >= DISPLAY_KEY_F1 + 4 && key < DISPLAY_KEY_F1 + 12)
		snprintf(seq, sizeof(seq), "\033[%s~",
		    f5_f12[key - DISPLAY_KEY_F1 - 4]);
	else {
		switch (key) {
		case DISPLAY_KEY_UP:		strlcpy(seq, "\033[A", sizeof(seq)); break;
		case DISPLAY_KEY_DOWN:		strlcpy(seq, "\033[B", sizeof(seq)); break;
		case DISPLAY_KEY_RIGHT:		strlcpy(seq, "\033[C", sizeof(seq)); break;
		case DISPLAY_KEY_LEFT:		strlcpy(seq, "\033[D", sizeof(seq)); break;
		case DISPLAY_KEY_HOME:		strlcpy(seq, "\033[H", sizeof(seq)); break;
		case DISPLAY_KEY_END:		strlcpy(seq, "\033[F", sizeof(seq)); break;
		case DISPLAY_KEY_PAGE_UP:	strlcpy(seq, "\033[5~", sizeof(seq)); break;
		case DISPLAY_KEY_PAGE_DOWN:	strlcpy(seq, "\033[6~", sizeof(seq)); break;
		case DISPLAY_KEY_INSERT:	strlcpy(seq, "\033[2~", sizeof(seq)); break;
		default:			return;
		}
	}

	for (const char *c = seq; *c != '\0'; c++)
		console_makeavail(m->main_console_handle, *c);
}


/*  X11 keysyms:  */
#define	XK_BackSpace	0xff08
#define	XK_Tab		0xff09
#define	XK_Return	0xff0d
#define	XK_Escape	0xff1b
#define	XK_Home		0xff50
#define	XK_Left		0xff51
#define	XK_Up		0xff52
#define	XK_Right	0xff53
#define	XK_Down		0xff54
#define	XK_Prior	0xff55
#define	XK_Next		0xff56
#define	XK_End		0xff57
#define	XK_Insert	0xff63
#define	XK_KP_Enter	0xff8d
#define	XK_KP_Home	0xff95
#define	XK_KP_Left	0xff96
#define	XK_KP_Up	0xff97
#define	XK_KP_Right	0xff98
#define	XK_KP_Down	0xff99
#define	XK_KP_Prior	0xff9a
#define	XK_KP_Next	0xff9b
#define	XK_KP_End	0xff9c
#define	XK_KP_Insert	0xff9e
#define	XK_KP_Delete	0xff9f
#define	XK_KP_Multiply	0xffaa
#define	XK_KP_9		0xffb9
#define	XK_F1		0xffbe
#define	XK_F12		0xffc9
#define	XK_Delete	0xffff


/*
 *  display_keysym():
 *
 *  Types the key with an X11 keysym (as VNC clients send them) on the
 *  machine's main console, with Control held if ctrl is set: a character,
 *  or the escape sequence of a function or editing key. Returns false if
 *  the key types nothing.
 */
bool display_keysym(struct machine *m, uint32_t key, bool ctrl)
{
	int ch = -1;

	/*  Unicode keysyms, for Latin-1:  */
	if (key >= 0x01000000 && key < 0x01000100)
		key -= 0x01000000;

	switch (key) {
	case XK_Return:
	case XK_KP_Enter:	ch = '\r'; break;
	case XK_BackSpace:	ch = '\b'; break;
	case XK_Tab:		ch = '\t'; break;
	case XK_Escape:		ch = 27; break;
	case XK_Delete:
	case XK_KP_Delete:	ch = 127; break;

	case XK_Up:	case XK_KP_Up:	  display_key(m, DISPLAY_KEY_UP); break;
	case XK_Down:	case XK_KP_Down:  display_key(m, DISPLAY_KEY_DOWN); break;
	case XK_Left:	case XK_KP_Left:  display_key(m, DISPLAY_KEY_LEFT); break;
	case XK_Right:	case XK_KP_Right: display_key(m, DISPLAY_KEY_RIGHT); break;
	case XK_Home:	case XK_KP_Home:  display_key(m, DISPLAY_KEY_HOME); break;
	case XK_End:	case XK_KP_End:	  display_key(m, DISPLAY_KEY_END); break;
	case XK_Prior:	case XK_KP_Prior: display_key(m, DISPLAY_KEY_PAGE_UP); break;
	case XK_Next:	case XK_KP_Next:  display_key(m, DISPLAY_KEY_PAGE_DOWN); break;
	case XK_Insert:	case XK_KP_Insert: display_key(m, DISPLAY_KEY_INSERT); break;

	default:
		if (key >= XK_F1 && key <= XK_F12)
			display_key(m, DISPLAY_KEY_F1 + key - XK_F1);
		else if (key >= XK_KP_Multiply && key <= XK_KP_9)
			/*  * + , - . / and 0..9 on the numeric keypad:  */
			ch = key - XK_KP_Multiply + '*';
		else if ((key >= 0x20 && key < 0x7f) ||
		    (key >= 0xa0 && key < 0x100)) {
			ch = key;
			/*  Ctrl+letter etc. give control characters:  */
			if (ctrl && ((key >= 'a' && key <= 'z') ||
			    (key >= '@' && key <= '_') || key == ' '))
				ch = key & 0x1f;
		} else
			return false;
	}

	if (ch >= 0)
		console_makeavail(m->main_console_handle, ch);
	return true;
}


/*
 *  Screen capture:
 *
 *  display_screenshot() saves a screenshot of a framebuffer window, and
 *  display_video_start() and display_video_stop() record one into a video
 *  file, both at the size of the window's image, including the emulated
 *  cursor.  Screenshots are PNG files, compressed with zlib if it is
 *  available, and display_screendump() saves one of any of a machine's
 *  windows, for the control socket.  Recordings are MP4 files written by
 *  ffmpeg at a constant frame rate, repeating the current image when
 *  nothing changes, so they play back in real time.
 */

#define	VIDEO_FPS	10

static struct fb_window *video_fbwin = NULL;
static FILE *video_pipe = NULL;
static char *video_file = NULL;
static int video_width, video_height;
static struct timeval video_start;
static unsigned long video_frames;


/*  Returns a new file name in the current directory, from the date and time.  */
static char *capture_name(const char *extension)
{
	char name[64], stamp[32];
	time_t now = time(NULL);

	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
	snprintf(name, sizeof(name), "gxemul-%s.%s", stamp, extension);
	for (int suffix = 2; access(name, F_OK) == 0; suffix++)
		snprintf(name, sizeof(name), "gxemul-%s-%d.%s", stamp,
		    suffix, extension);
	return strdup(name);
}


/*
 *  ffmpeg runs through the shell (cmd.exe on Windows), which is sent the
 *  raw image data through a pipe.  File names are quoted with QUOTE.
 */
#ifdef _WIN32
#define	QUOTE		"\""
#define	PIPE_WRITE	"wb"
#define	FIND_FFMPEG	"where ffmpeg >NUL 2>&1"
#else
#define	QUOTE		"'"
#define	PIPE_WRITE	"w"
#define	FIND_FFMPEG	"command -v ffmpeg >/dev/null 2>&1"
#endif

static bool have_ffmpeg(void)
{
	return system(FIND_FFMPEG) == 0;
}


static void ignore_sigpipe(void)
{
#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif
}


/*
 *  Returns a new 24-bit RGB image of what a framebuffer window shows,
 *  or NULL if that isn't possible.
 */
static unsigned char *capture_rgb(struct fb_window *fbwin, int *wp, int *hp)
{
	if (fbwin == NULL || fbwin->xsize <= 0)
		return NULL;

	int w = fbwin->xsize, h = fbwin->ysize;
	unsigned char *rgb = (unsigned char *) malloc((size_t) w * h * 3);
	if (rgb == NULL)
		return NULL;

	for (size_t i = 0; i < (size_t) w * h; i++) {
		uint32_t pixel = fbwin->screen[i];
		rgb[i*3 + 0] = pixel >> 16;
		rgb[i*3 + 1] = pixel >> 8;
		rgb[i*3 + 2] = pixel;
	}

	*wp = w;
	*hp = h;
	return rgb;
}


/*  Returns the first framebuffer window of a machine, or NULL.  */
static struct fb_window *capture_fbwin(struct machine *m)
{
	if (m == NULL || m->display_md.n_fb_windows < 1 ||
	    m->display_md.fb_windows[0]->xsize <= 0) {
		printf("this machine has no framebuffer window\n");
		return NULL;
	}
	return m->display_md.fb_windows[0];
}


/*  PNG files are big-endian, and their chunks end with a CRC-32:  */
static void png_be32(unsigned char *p, uint32_t v)
{
	p[0] = v >> 24;  p[1] = v >> 16;  p[2] = v >> 8;  p[3] = v;
}

static uint32_t png_crc(uint32_t crc, const unsigned char *p, size_t len)
{
	static uint32_t table[256];

	if (table[1] == 0)
		for (uint32_t n = 0; n < 256; n++) {
			uint32_t c = n;
			for (int k = 0; k < 8; k++)
				c = c & 1 ? 0xedb88320 ^ (c >> 1) : c >> 1;
			table[n] = c;
		}

	crc = ~crc;
	while (len-- > 0)
		crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static bool png_chunk(FILE *f, const char *type, const unsigned char *data,
	size_t len)
{
	unsigned char buf[4];
	uint32_t crc = png_crc(0, (const unsigned char *) type, 4);

	if (len > 0)
		crc = png_crc(crc, data, len);
	png_be32(buf, len);
	if (fwrite(buf, 4, 1, f) != 1 || fwrite(type, 4, 1, f) != 1 ||
	    (len > 0 && fwrite(data, len, 1, f) != 1))
		return false;
	png_be32(buf, crc);
	return fwrite(buf, 4, 1, f) == 1;
}


/*
 *  Returns data as a zlib stream, compressed with zlib if it is available,
 *  or otherwise in stored (uncompressed) blocks.
 */
static unsigned char *png_deflate(const unsigned char *data, size_t len,
	size_t *lenp)
{
	unsigned char *z;
#ifdef WITH_ZLIB
	uLongf zlen = compressBound(len);

	if ((z = (unsigned char *) malloc(zlen)) == NULL)
		return NULL;
	if (compress2(z, &zlen, data, len, Z_BEST_COMPRESSION) != Z_OK) {
		free(z);
		return NULL;
	}
	*lenp = zlen;
#else
	size_t out = 0, offset = 0;
	uint32_t a = 1, b = 0;

	z = (unsigned char *) malloc(2 + len + 5 * (len / 65535 + 1) + 4);
	if (z == NULL)
		return NULL;
	z[out++] = 0x78;
	z[out++] = 0x01;
	do {
		size_t block = len - offset < 65535 ? len - offset : 65535;
		z[out++] = offset + block == len;
		z[out++] = block;
		z[out++] = block >> 8;
		z[out++] = ~block;
		z[out++] = ~block >> 8;
		memcpy(z + out, data + offset, block);
		out += block;
		offset += block;
	} while (offset < len);
	for (size_t i = 0; i < len; i++) {
		a = (a + data[i]) % 65521;
		b = (b + a) % 65521;
	}
	png_be32(z + out, (b << 16) | a);
	*lenp = out + 4;
#endif
	return z;
}


/*  Writes a 24-bit RGB image as a PNG file.  Returns false, with errno set,
    if it couldn't.  */
static bool write_png(const char *name, const unsigned char *rgb, int w, int h)
{
	static const unsigned char signature[8] =
	    { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	size_t row = (size_t) w * 3, rawlen = (1 + row) * h, zlen;
	unsigned char ihdr[13], *raw, *z;

	/*  Each row is a filter type byte (0, none), then its pixels:  */
	if ((raw = (unsigned char *) malloc(rawlen)) == NULL)
		return false;
	for (int y = 0; y < h; y++) {
		raw[y * (1 + row)] = 0;
		memcpy(raw + y * (1 + row) + 1, rgb + y * row, row);
	}
	z = png_deflate(raw, rawlen, &zlen);
	free(raw);
	if (z == NULL) {
		errno = ENOMEM;
		return false;
	}

	png_be32(ihdr, w);
	png_be32(ihdr + 4, h);
	ihdr[8] = 8;		/*  bits per sample  */
	ihdr[9] = 2;		/*  RGB  */
	ihdr[10] = ihdr[11] = ihdr[12] = 0;

	FILE *f = fopen(name, "wb");
	bool ok = f != NULL && fwrite(signature, 8, 1, f) == 1 &&
	    png_chunk(f, "IHDR", ihdr, sizeof(ihdr)) &&
	    png_chunk(f, "IDAT", z, zlen) && png_chunk(f, "IEND", NULL, 0);
	int saved_errno = errno;
	if (f != NULL && fclose(f) != 0 && ok) {
		ok = false;
		saved_errno = errno;
	}
	free(z);
	if (!ok) {
		if (f != NULL)
			remove(name);
		errno = saved_errno != 0 ? saved_errno : EIO;
	}
	return ok;
}


/*
 *  display_screendump():
 *
 *  Saves what framebuffer window n of a machine shows as a PNG file,
 *  file, or if file is NULL, a new file in the current directory.  Sets
 *  *namep to the file's name (to be freed), and *wp and *hp to the
 *  image's size.  Returns 0, ENODEV if there is no such window, or an
 *  errno value for why the file couldn't be written.
 */
int display_screendump(struct machine *m, int n, const char *file,
	char **namep, int *wp, int *hp)
{
	struct fb_window *fbwin;
	unsigned char *rgb;

	*namep = NULL;
	if (m == NULL || n < 0 || n >= m->display_md.n_fb_windows)
		return ENODEV;
	fbwin = m->display_md.fb_windows[n];
	if ((rgb = capture_rgb(fbwin, wp, hp)) == NULL)
		return fbwin->xsize <= 0 ? ENODEV : ENOMEM;

	CHECK_ALLOCATION(*namep = file != NULL ? strdup(file) :
	    capture_name("png"));
	int rc = write_png(*namep, rgb, *wp, *hp) ? 0 : errno;
	free(rgb);
	return rc;
}


/*
 *  display_screenshot():
 *
 *  Saves a screenshot of a machine's first framebuffer window.
 */
int display_screenshot(struct machine *m, const char *file)
{
	char *name;
	int w, h;

	int rc = display_screendump(m, 0, file, &name, &w, &h);
	if (rc == ENODEV)
		printf("this machine has no framebuffer window\n");
	else if (rc == 0) {
		printf("saved screenshot %s\n", name);
		display_message(m, "Saved screenshot %s", name);
	} else {
		printf("couldn't save screenshot %s: %s\n", name,
		    strerror(rc));
		display_message(m, "Could not save screenshot %s", name);
	}
	free(name);
	return rc == 0 ? 0 : -1;
}


/*  Sends the frames that are due by now to ffmpeg.  */
static void video_tick(void)
{
	struct timeval now;
	unsigned char *rgb;
	int w, h;

	if (video_pipe == NULL)
		return;

	gettimeofday(&now, NULL);
	unsigned long due = ((now.tv_sec - video_start.tv_sec) * 1000000L +
	    (now.tv_usec - video_start.tv_usec)) * VIDEO_FPS / 1000000L + 1;
	if (due <= video_frames)
		return;

	/*  After a long stall, skip most of the missed frames:  */
	unsigned long count = due - video_frames;
	if (count > 5 * VIDEO_FPS)
		count = 5 * VIDEO_FPS;
	video_frames = due;

	rgb = capture_rgb(video_fbwin, &w, &h);
	bool ok = rgb != NULL && w == video_width && h == video_height;
	for (; ok && count > 0; count--)
		ok = fwrite(rgb, (size_t) w * 3, h, video_pipe) == (size_t) h;
	ok = ok && fflush(video_pipe) == 0;
	free(rgb);

	if (!ok) {
		printf("[ video stopped: the screen changed size, or "
		    "ffmpeg failed ]\n");
		display_video_stop();
	}
}


/*
 *  display_video_start():
 *
 *  Starts recording a machine's framebuffer window.
 */
int display_video_start(struct machine *m, const char *file)
{
	struct fb_window *fbwin;
	unsigned char *rgb;
	char cmd[1024];

	if (video_pipe != NULL) {
		printf("already recording %s\n", video_file);
		return -1;
	}
	if (!have_ffmpeg()) {
		printf("recording needs ffmpeg, which wasn't found\n");
		display_message(m, "Recording needs ffmpeg, which wasn't found");
		return -1;
	}
	if ((fbwin = capture_fbwin(m)) == NULL ||
	    (rgb = capture_rgb(fbwin, &video_width, &video_height)) == NULL)
		return -1;
	free(rgb);

	char *name = file != NULL ? strdup(file) : capture_name("mp4");
	if (strchr(name, QUOTE[0]) != NULL) {
		printf("file names with quotes are not supported\n");
		free(name);
		return -1;
	}

	/*  An odd frame size is padded to an even one, which the H.264
	    encoder needs:  */
	snprintf(cmd, sizeof(cmd), "ffmpeg -loglevel error -y"
	    " -f rawvideo -pixel_format rgb24 -video_size %dx%d"
	    " -framerate %d -i -"
	    " -vf " QUOTE "pad=ceil(iw/2)*2:ceil(ih/2)*2" QUOTE
	    " -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p "
	    QUOTE "%s" QUOTE, video_width, video_height, VIDEO_FPS, name);
	ignore_sigpipe();
	if ((video_pipe = popen(cmd, PIPE_WRITE)) == NULL) {
		printf("couldn't start ffmpeg\n");
		free(name);
		return -1;
	}

	video_fbwin = fbwin;
	video_file = name;
	gettimeofday(&video_start, NULL);
	video_frames = 0;
	video_tick();
	if (video_pipe == NULL)
		return -1;

	printf("recording to %s\n", name);
	display_message(m, "Recording to %s", name);
	return 0;
}


/*
 *  display_video_stop():
 *
 *  Stops recording, and waits for ffmpeg to finish the file.
 */
void display_video_stop(void)
{
	if (video_pipe == NULL) {
		printf("not recording\n");
		return;
	}

	FILE *f = video_pipe;
	struct machine *m = video_fbwin->machine;
	video_tick();
	if (video_pipe != NULL)
		f = video_pipe;
	video_pipe = NULL;

	if (pclose(f) == 0) {
		printf("saved video %s\n", video_file);
		display_message(m, "Saved video %s", video_file);
	} else {
		printf("video %s failed\n", video_file);
		display_message(m, "Video %s failed", video_file);
	}
	free(video_file);
	video_file = NULL;
	video_fbwin = NULL;
}


/*
 *  display_video_recording():
 *
 *  Returns true while a framebuffer is being recorded.
 */
bool display_video_recording(void)
{
	return video_pipe != NULL;
}


/*
 *  display_ticks():
 *
 *  Returns a millisecond counter, for timing things in the display.
 */
uint32_t display_ticks(void)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	return (uint32_t) now.tv_sec * 1000 + now.tv_usec / 1000;
}


/*
 *  display_check_event():
 *
 *  Checks for host window system events.
 */
void display_check_event(struct emul *emul)
{
	for (int i=0; i<emul->n_machines; i++)
		display_backend_check_events(emul->machines[i]);

	vnc_poll(emul);

	/*  If a framebuffer is being recorded, send any frames that are due:  */
	video_tick();

	/*  Is a file chooser done?  */
	display_files_poll();
}

