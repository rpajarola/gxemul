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
 *  The VNC server: --vnc tcp:[host:]port or --vnc unix:path shows the
 *  first framebuffer window to VNC clients (RFB 3.3, 3.7 and 3.8, with
 *  VNC Authentication when GXEMUL_VNC_PASSWORD is set, and otherwise
 *  without a password), instead of in a host window.
 *
 *  vnc_poll() is called with the display's other events, on the
 *  emulator's own thread.  Keys typed in a client go to the machine's
 *  console, like keys typed in an SDL window, and the mouse moves the
 *  emulated mouse by as much as the client's pointer moved.
 *
 *  Updates are sent as Raw rectangles, or, when there is zlib, as ZRLE
 *  (which makes text screens tiny) or Zlib, whichever the client lists
 *  first (macOS's Screen Sharing wants Zlib).  display_update() marks
 *  64x64 tiles as dirty, and the tiles which differ from what a client
 *  was sent last are sent when it asks for an update, at most every
 *  VNC_UPDATE_INTERVAL_MS, and only when what it was sent before has
 *  gone out.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "sockets.h"
#ifndef _WIN32
#include <netinet/tcp.h>
#include <sys/un.h>
#endif

#include "console.h"
#include "display.h"
#include "emul.h"
#include "machine.h"
#include "misc.h"
#include "vnc.h"

#ifdef WITH_ZLIB
#include <zlib.h>
#endif


#define	VNC_MAX_CLIENTS		4
#define	VNC_TILE		64
#define	VNC_UPDATE_INTERVAL_MS	20
#define	VNC_MAX_MESSAGE		(4 + 4 * 65536)

/*  Encodings:  */
#define	ENC_RAW			0
#define	ENC_ZLIB		6
#define	ENC_ZRLE		16
#define	ENC_DESKTOP_SIZE	(-223)
#define	ENC_DESKTOP_NAME	(-307)

enum vnc_state {
	VNC_VERSION,		/*  waiting for the client's version  */
	VNC_SECURITY,		/*  ... for its security type  */
	VNC_AUTH,		/*  ... for its answer to the challenge  */
	VNC_CLIENT_INIT,	/*  ... for ClientInit  */
	VNC_WAIT_FB,		/*  ... for a framebuffer window to show  */
	VNC_NORMAL
};

struct vnc_client {
	int		fd;		/*  -1 for a free slot  */
	enum vnc_state	state;
	int		minor;		/*  RFB 3.minor: 3, 7 or 8  */

	unsigned char	*in;
	size_t		inlen;
	size_t		skip;		/*  of ClientCutText, to discard  */

	unsigned char	*out;
	size_t		outlen, outsize;

	/*  The client's pixel format:  */
	int		bytes;		/*  per pixel  */
	bool		big_endian;
	uint32_t	rtab[256], gtab[256], btab[256];
	int		cbytes;		/*  per ZRLE CPIXEL  */
	bool		chigh;		/*  CPIXELs are the high 3 bytes  */

	int32_t		encoding;	/*  ENC_RAW, ENC_ZLIB or ENC_ZRLE  */
	bool		desktop_size;
	bool		desktop_name;
#ifdef WITH_ZLIB
	z_stream	zs[2];		/*  ZRLE's and Zlib's  */
	bool		zs_init[2];
#endif

	/*  What the client shows:  */
	struct fb_window *win;
	int		xsize, ysize;
	char		*name;
	int		tiles_x, tiles_y;
	unsigned char	*dirty;		/*  per tile  */
	uint32_t	*shadow;	/*  what the client was sent  */
	bool		update_requested;
	bool		resized;
	uint32_t	last_update;	/*  ms ticks  */

	int		buttons;
	int		mouse_x, mouse_y;
	bool		mouse_known;
	bool		ctrl_l, ctrl_r;

	unsigned char	challenge[16];
	bool		closing;	/*  once what is sent has gone  */
};

static int listen_fd = -1;
static char *password = NULL;		/*  NULL for none  */
static bool wrong_password = false;
static bool trace = false;		/*  GXEMUL_VNC_DEBUG  */
static uint32_t wrong_password_at;	/*  ms ticks  */
#ifndef _WIN32
static char *unix_path = NULL;
#endif
static struct vnc_client clients[VNC_MAX_CLIENTS];


/*  The protocol, as it happens, with GXEMUL_VNC_DEBUG set:  */
#define	TRACE(c, ...)	do { if (trace) {				\
		fprintf(stderr, "vnc[%i]: ", (int) ((c) - clients));	\
		fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); }	\
	} while (0)


/*****************************************************************************/

/*
 *  Sending:  everything goes to the client's out buffer, which flush()
 *  sends as far as the socket takes it.
 */

static void disconnect(struct vnc_client *c, const char *why)
{
	if (c->fd < 0)
		return;

	if (why != NULL)
		debugmsg(SUBSYS_DISPLAY, "vnc", VERBOSITY_WARNING,
		    "dropping a VNC client: %s", why);
	TRACE(c, "disconnected%s%s", why != NULL ? ": " : "",
	    why != NULL ? why : "");

	socket_close(c->fd);
	c->fd = -1;
	free(c->in);
	free(c->out);
	free(c->name);
	free(c->dirty);
	free(c->shadow);
#ifdef WITH_ZLIB
	for (int i = 0; i < 2; i++)
		if (c->zs_init[i])
			deflateEnd(&c->zs[i]);
#endif
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}


static void reserve(struct vnc_client *c, size_t n)
{
	if (c->outlen + n <= c->outsize)
		return;

	while (c->outlen + n > c->outsize)
		c->outsize = c->outsize == 0 ? 65536 : c->outsize * 2;
	CHECK_ALLOCATION(c->out = (unsigned char *) realloc(c->out,
	    c->outsize));
}


static void put(struct vnc_client *c, const void *data, size_t n)
{
	reserve(c, n);
	memcpy(c->out + c->outlen, data, n);
	c->outlen += n;
}


static void put8(struct vnc_client *c, int v)
{
	unsigned char b = v;
	put(c, &b, 1);
}


static void put16(struct vnc_client *c, int v)
{
	unsigned char b[2] = { (unsigned char) (v >> 8), (unsigned char) v };
	put(c, b, 2);
}


static void put32(struct vnc_client *c, uint32_t v)
{
	unsigned char b[4] = { (unsigned char) (v >> 24),
	    (unsigned char) (v >> 16), (unsigned char) (v >> 8),
	    (unsigned char) v };
	put(c, b, 4);
}


static void flush(struct vnc_client *c)
{
#ifdef MSG_NOSIGNAL
	int flags = MSG_NOSIGNAL;
#else
	int flags = 0;
#endif
	size_t done = 0;

	if (c->fd < 0)
		return;

	while (c->fd >= 0 && done < c->outlen) {
		ssize_t n = send(c->fd, (const char *) c->out + done,
		    c->outlen - done, flags);
		if (n > 0) {
			done += n;
			continue;
		}

		int e = socket_error();
		if (n < 0 && e == SOCKET_EINTR)
			continue;
		if (n < 0 && socket_would_block(e))
			break;

		disconnect(c, NULL);
		return;
	}

	memmove(c->out, c->out + done, c->outlen - done);
	c->outlen -= done;
}


/*****************************************************************************/

/*
 *  Pixels:  the window's 0x00RRGGBB pixels, in the client's format.
 */

static void set_pixel_format(struct vnc_client *c, int bpp, int depth,
	bool big_endian, int rmax, int gmax, int bmax, int rshift,
	int gshift, int bshift)
{
	c->bytes = bpp / 8;
	c->big_endian = big_endian;

	for (int i = 0; i < 256; i++) {
		c->rtab[i] = (uint32_t) ((i * rmax + 127) / 255) << rshift;
		c->gtab[i] = (uint32_t) ((i * gmax + 127) / 255) << gshift;
		c->btab[i] = (uint32_t) ((i * bmax + 127) / 255) << bshift;
	}

	/*  ZRLE sends 32-bit pixels which fit in 3 bytes as 3 bytes:  */
	uint32_t mask = c->rtab[255] | c->gtab[255] | c->btab[255];
	c->cbytes = c->bytes;
	c->chigh = false;
	if (bpp == 32 && depth <= 24) {
		if ((mask & 0xff000000) == 0)
			c->cbytes = 3;
		else if ((mask & 0xff) == 0) {
			c->cbytes = 3;
			c->chigh = true;
		}
	}
}


static inline uint32_t convert(const struct vnc_client *c, uint32_t rgb)
{
	return c->rtab[(rgb >> 16) & 0xff] | c->gtab[(rgb >> 8) & 0xff] |
	    c->btab[rgb & 0xff];
}


/*  Writes a pixel of n bytes (from the low or high end of v):  */
static unsigned char *pixel_bytes(const struct vnc_client *c,
	unsigned char *p, uint32_t v, int n, bool high)
{
	if (high)
		v >>= 8;

	if (c->big_endian)
		for (int i = n - 1; i >= 0; i--)
			*p++ = v >> (8 * i);
	else
		for (int i = 0; i < n; i++)
			*p++ = v >> (8 * i);

	return p;
}


static void rect_header(struct vnc_client *c, int x, int y, int w, int h,
	int32_t encoding)
{
	put16(c, x);
	put16(c, y);
	put16(c, w);
	put16(c, h);
	put32(c, (uint32_t) encoding);
}


static void send_raw(struct vnc_client *c, int x, int y, int w, int h)
{
	rect_header(c, x, y, w, h, ENC_RAW);

	reserve(c, (size_t) w * h * c->bytes);
	unsigned char *p = c->out + c->outlen;
	for (int yy = y; yy < y + h; yy++) {
		const uint32_t *src = c->shadow + (size_t) yy * c->xsize + x;
		for (int xx = 0; xx < w; xx++)
			p = pixel_bytes(c, p, convert(c, src[xx]), c->bytes,
			    false);
	}
	c->outlen = p - c->out;
}


#ifdef WITH_ZLIB

/*
 *  Compressed encodings:  Zlib is the pixels of a rectangle, as for Raw,
 *  compressed with the client's zlib stream for Zlib.  In ZRLE, a
 *  rectangle is cut into 64x64 tiles, each of which is sent as one
 *  colour, a packed palette, run lengths (of colours or of palette
 *  indices), or raw, whichever is the smallest; all the tiles together
 *  are then compressed with the client's zlib stream for ZRLE.
 */

#define	ZRLE_MAX_PALETTE	127

/*  Where a rectangle's tiles are put together:  */
static unsigned char *scratch = NULL;
static size_t scratch_size = 0;

/*  Sends data compressed with zlib stream i, after its length:  */
static void put_deflated(struct vnc_client *c, int i, unsigned char *data,
	size_t n)
{
	z_stream *zs = &c->zs[i];

	if (!c->zs_init[i]) {
		if (deflateInit(zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
			fprintf(stderr, "vnc: deflateInit failed\n");
			exit(1);
		}
		c->zs_init[i] = true;
	}

	size_t lenpos = c->outlen;
	put32(c, 0);

	zs->next_in = data;
	zs->avail_in = n;
	do {
		reserve(c, 65536);
		zs->next_out = c->out + c->outlen;
		zs->avail_out = c->outsize - c->outlen;
		deflate(zs, Z_SYNC_FLUSH);
		c->outlen = c->outsize - zs->avail_out;
	} while (zs->avail_out == 0);

	uint32_t len = c->outlen - lenpos - 4;
	c->out[lenpos] = len >> 24;
	c->out[lenpos + 1] = len >> 16;
	c->out[lenpos + 2] = len >> 8;
	c->out[lenpos + 3] = len;
}


struct zrle_palette {
	int		n;
	uint32_t	colors[ZRLE_MAX_PALETTE];
	struct {
		uint32_t	color;
		int		index;	/*  -1 if unused  */
	} hash[256];
};


static unsigned char *scratch_reserve(size_t n)
{
	if (n > scratch_size) {
		scratch_size = n;
		CHECK_ALLOCATION(scratch = (unsigned char *) realloc(scratch,
		    scratch_size));
	}
	return scratch;
}


static int palette_index(struct zrle_palette *pal, uint32_t color)
{
	unsigned h = (color * 2654435761u) >> 24;

	for (;;) {
		if (pal->hash[h].index < 0) {
			if (pal->n >= ZRLE_MAX_PALETTE)
				return -1;
			pal->hash[h].color = color;
			pal->hash[h].index = pal->n;
			pal->colors[pal->n] = color;
			return pal->n++;
		}
		if (pal->hash[h].color == color)
			return pal->hash[h].index;
		h = (h + 1) & 255;
	}
}


static unsigned char *put_cpixel(const struct vnc_client *c,
	unsigned char *p, uint32_t v)
{
	return pixel_bytes(c, p, v, c->cbytes, c->chigh);
}


/*  A run length, as ZRLE writes it:  */
static unsigned char *put_run(unsigned char *p, int len)
{
	len --;
	while (len >= 255) {
		*p++ = 255;
		len -= 255;
	}
	*p++ = len;
	return p;
}


static unsigned char *zrle_tile(const struct vnc_client *c,
	unsigned char *p, int x0, int y0, int w, int h)
{
	static uint32_t px[VNC_TILE * VNC_TILE];
	static unsigned char idx[VNC_TILE * VNC_TILE];
	struct zrle_palette pal;
	int n = w * h, cb = c->cbytes;
	size_t plain_rle = 1, palette_rle = 1;
	bool overflow = false;

	for (int y = 0; y < h; y++) {
		const uint32_t *src = c->shadow + (size_t) (y0 + y) * c->xsize
		    + x0;
		for (int x = 0; x < w; x++)
			px[y * w + x] = convert(c, src[x]);
	}

	/*  Count the colours, and what each kind of run lengths takes:  */
	pal.n = 0;
	for (int i = 0; i < 256; i++)
		pal.hash[i].index = -1;
	for (int i = 0; i < n; ) {
		int j = i + 1;
		while (j < n && px[j] == px[i])
			j++;
		int len = j - i, lenbytes = (len - 1) / 255 + 1;
		plain_rle += cb + lenbytes;
		palette_rle += len == 1 ? 1 : 1 + lenbytes;
		if (!overflow) {
			int k = palette_index(&pal, px[i]);
			if (k < 0)
				overflow = true;
			else
				memset(idx + i, k, len);
		}
		i = j;
	}

	if (!overflow && pal.n == 1) {
		*p++ = 1;
		return put_cpixel(c, p, px[0]);
	}

	size_t raw = 1 + (size_t) n * cb, best = raw;
	int bits = 0;
	enum { RAW, PACKED, PLAIN_RLE, PALETTE_RLE } kind = RAW;

	if (plain_rle < best) {
		best = plain_rle;
		kind = PLAIN_RLE;
	}
	if (!overflow) {
		palette_rle += (size_t) pal.n * cb;
		if (palette_rle < best) {
			best = palette_rle;
			kind = PALETTE_RLE;
		}
		if (pal.n <= 16) {
			bits = pal.n <= 2 ? 1 : pal.n <= 4 ? 2 : 4;
			size_t packed = 1 + (size_t) pal.n * cb +
			    (size_t) h * ((w * bits + 7) / 8);
			if (packed < best) {
				best = packed;
				kind = PACKED;
			}
		}
	}

	switch (kind) {

	case RAW:
		*p++ = 0;
		for (int i = 0; i < n; i++)
			p = put_cpixel(c, p, px[i]);
		break;

	case PACKED:
		*p++ = pal.n;
		for (int i = 0; i < pal.n; i++)
			p = put_cpixel(c, p, pal.colors[i]);
		for (int y = 0; y < h; y++) {
			int byte = 0, nbits = 0;
			for (int x = 0; x < w; x++) {
				byte = (byte << bits) | idx[y * w + x];
				nbits += bits;
				if (nbits == 8) {
					*p++ = byte;
					byte = nbits = 0;
				}
			}
			if (nbits > 0)
				*p++ = byte << (8 - nbits);
		}
		break;

	case PLAIN_RLE:
		*p++ = 128;
		for (int i = 0; i < n; ) {
			int j = i + 1;
			while (j < n && px[j] == px[i])
				j++;
			p = put_cpixel(c, p, px[i]);
			p = put_run(p, j - i);
			i = j;
		}
		break;

	case PALETTE_RLE:
		*p++ = 128 + pal.n;
		for (int i = 0; i < pal.n; i++)
			p = put_cpixel(c, p, pal.colors[i]);
		for (int i = 0; i < n; ) {
			int j = i + 1;
			while (j < n && px[j] == px[i])
				j++;
			if (j - i == 1)
				*p++ = idx[i];
			else {
				*p++ = idx[i] | 128;
				p = put_run(p, j - i);
			}
			i = j;
		}
		break;
	}

	return p;
}


static void send_zrle(struct vnc_client *c, int x, int y, int w, int h)
{
	/*  (The worst case for a tile is raw, plus its subencoding byte.)  */
	size_t tiles = (size_t) ((w + VNC_TILE - 1) / VNC_TILE) *
	    ((h + VNC_TILE - 1) / VNC_TILE);
	unsigned char *start = scratch_reserve(tiles *
	    (1 + VNC_TILE * VNC_TILE * 4));
	unsigned char *p = start;

	for (int ty = y; ty < y + h; ty += VNC_TILE)
		for (int tx = x; tx < x + w; tx += VNC_TILE)
			p = zrle_tile(c, p, tx, ty,
			    x + w - tx < VNC_TILE ? x + w - tx : VNC_TILE,
			    y + h - ty < VNC_TILE ? y + h - ty : VNC_TILE);

	rect_header(c, x, y, w, h, ENC_ZRLE);
	put_deflated(c, 0, start, p - start);
}


static void send_zlib(struct vnc_client *c, int x, int y, int w, int h)
{
	unsigned char *start = scratch_reserve((size_t) w * h * 4);
	unsigned char *p = start;

	for (int yy = y; yy < y + h; yy++) {
		const uint32_t *src = c->shadow + (size_t) yy * c->xsize + x;
		for (int xx = 0; xx < w; xx++)
			p = pixel_bytes(c, p, convert(c, src[xx]), c->bytes,
			    false);
	}

	rect_header(c, x, y, w, h, ENC_ZLIB);
	put_deflated(c, 1, start, p - start);
}

#endif	/*  WITH_ZLIB  */


/*****************************************************************************/

/*
 *  Updates:
 */

/*  The window that VNC clients show:  the first one there is.  */
static struct fb_window *shown_window(struct emul *emul)
{
	for (int i = 0; i < emul->n_machines; i++)
		if (emul->machines[i]->display_md.n_fb_windows > 0)
			return emul->machines[i]->display_md.fb_windows[0];
	return NULL;
}


/*  Takes on the window's size, with everything to be sent:  */
static void take_size(struct vnc_client *c)
{
	c->xsize = c->win->xsize;
	c->ysize = c->win->ysize;
	c->tiles_x = (c->xsize + VNC_TILE - 1) / VNC_TILE;
	c->tiles_y = (c->ysize + VNC_TILE - 1) / VNC_TILE;

	free(c->dirty);
	free(c->shadow);
	CHECK_ALLOCATION(c->dirty = (unsigned char *) malloc(
	    (size_t) c->tiles_x * c->tiles_y));
	memset(c->dirty, 2, (size_t) c->tiles_x * c->tiles_y);
	CHECK_ALLOCATION(c->shadow = (uint32_t *) calloc(
	    (size_t) c->xsize * c->ysize, sizeof(uint32_t)));
}


static void server_init(struct vnc_client *c, struct fb_window *win)
{
	c->win = win;
	take_size(c);
	CHECK_ALLOCATION(c->name = strdup(win->name));

	put16(c, c->xsize);
	put16(c, c->ysize);

	/*  32 bits per pixel, depth 24, little-endian, true colour:  */
	put8(c, 32);
	put8(c, 24);
	put8(c, 0);
	put8(c, 1);
	put16(c, 255);
	put16(c, 255);
	put16(c, 255);
	put8(c, 16);
	put8(c, 8);
	put8(c, 0);
	put8(c, 0);
	put16(c, 0);
	set_pixel_format(c, 32, 24, false, 255, 255, 255, 16, 8, 0);

	put32(c, strlen(c->name));
	put(c, c->name, strlen(c->name));
	TRACE(c, "ServerInit: %ix%i, \"%s\"", c->xsize, c->ysize, c->name);

	c->state = VNC_NORMAL;
}


/*
 *  vnc_update():
 *
 *  Called by display_update() for a changed part of a window.
 */
void vnc_update(struct fb_window *win, int x, int y, int w, int h)
{
	for (int i = 0; i < VNC_MAX_CLIENTS; i++) {
		struct vnc_client *c = &clients[i];
		if (c->fd < 0 || c->win != win || c->xsize != win->xsize ||
		    c->ysize != win->ysize)
			continue;

		for (int ty = y / VNC_TILE; ty <= (y + h - 1) / VNC_TILE;
		    ty++)
			for (int tx = x / VNC_TILE; tx <= (x + w - 1) /
			    VNC_TILE; tx++)
				if (c->dirty[ty * c->tiles_x + tx] == 0)
					c->dirty[ty * c->tiles_x + tx] = 1;
	}
}


/*
 *  Copies the dirty tiles which changed into the shadow image.  A tile
 *  is 1 when dirty, and 2 when it is to be sent even if it looks the
 *  same; afterwards, the ones to send are 3, and the rest 0.  Returns the
 *  number of tiles to send.
 */
static int changed_tiles(struct vnc_client *c)
{
	int n = 0;

	for (int ty = 0; ty < c->tiles_y; ty++)
		for (int tx = 0; tx < c->tiles_x; tx++) {
			unsigned char *d = &c->dirty[ty * c->tiles_x + tx];
			if (*d == 0)
				continue;

			int x = tx * VNC_TILE, y = ty * VNC_TILE;
			int w = c->xsize - x < VNC_TILE ? c->xsize - x :
			    VNC_TILE;
			int h = c->ysize - y < VNC_TILE ? c->ysize - y :
			    VNC_TILE;
			bool changed = *d == 2;
			for (int yy = y; yy < y + h; yy++) {
				size_t o = (size_t) yy * c->xsize + x;
				if (!changed && memcmp(c->shadow + o,
				    c->win->screen + o, w * 4) == 0)
					continue;
				memcpy(c->shadow + o, c->win->screen + o,
				    w * 4);
				changed = true;
			}

			*d = changed ? 3 : 0;
			n += changed;
		}

	return n;
}


static void send_update(struct vnc_client *c)
{
	struct fb_window *win = c->win;

	if (win->xsize != c->xsize || win->ysize != c->ysize) {
		if (!c->desktop_size) {
			disconnect(c, "the screen size changed, and the "
			    "client can't follow");
			return;
		}
		take_size(c);
		c->resized = true;
	}

	bool rename = c->desktop_name && strcmp(c->name, win->name) != 0;
	int n = changed_tiles(c);
	if (n == 0 && !c->resized && !rename)
		return;

	put8(c, 0);
	put8(c, 0);
	size_t countpos = c->outlen;
	put16(c, 0);
	int nrects = 0;

	if (c->resized) {
		rect_header(c, 0, 0, c->xsize, c->ysize, ENC_DESKTOP_SIZE);
		nrects ++;
		c->resized = false;
	}

	if (rename) {
		free(c->name);
		CHECK_ALLOCATION(c->name = strdup(win->name));
		rect_header(c, 0, 0, 0, 0, ENC_DESKTOP_NAME);
		put32(c, strlen(c->name));
		put(c, c->name, strlen(c->name));
		nrects ++;
	}

	/*  A rectangle per run of tiles in a row:  */
	for (int ty = 0; ty < c->tiles_y; ty++)
		for (int tx = 0; tx < c->tiles_x; ) {
			if (c->dirty[ty * c->tiles_x + tx] != 3) {
				tx++;
				continue;
			}

			int tx2 = tx;
			while (tx2 < c->tiles_x &&
			    c->dirty[ty * c->tiles_x + tx2] == 3)
				c->dirty[ty * c->tiles_x + tx2++] = 0;

			int x = tx * VNC_TILE, y = ty * VNC_TILE;
			int x2 = tx2 * VNC_TILE, y2 = y + VNC_TILE;
			if (x2 > c->xsize)
				x2 = c->xsize;
			if (y2 > c->ysize)
				y2 = c->ysize;

#ifdef WITH_ZLIB
			if (c->encoding == ENC_ZRLE)
				send_zrle(c, x, y, x2 - x, y2 - y);
			else if (c->encoding == ENC_ZLIB)
				send_zlib(c, x, y, x2 - x, y2 - y);
			else
#endif
				send_raw(c, x, y, x2 - x, y2 - y);
			nrects ++;
			tx = tx2;
		}

	c->out[countpos] = nrects >> 8;
	c->out[countpos + 1] = nrects;
	TRACE(c, "update: %i rectangles, %i tiles, %zu bytes (%s)", nrects,
	    n, c->outlen, c->encoding == ENC_ZRLE ? "ZRLE" :
	    c->encoding == ENC_ZLIB ? "Zlib" : "Raw");

	c->update_requested = false;
	c->last_update = display_ticks();
}


/*****************************************************************************/

/*
 *  VNC Authentication:  the server sends 16 random bytes, which the
 *  client encrypts with DES, with the password (up to 8 characters, each
 *  byte's bits reversed) as the key.
 */

static const unsigned char des_ip[64] = {
	58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
	62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
	57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
	61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7 };
static const unsigned char des_fp[64] = {
	40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
	38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
	36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
	34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25 };
static const unsigned char des_e[48] = {
	32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13,
	12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21, 22, 23,
	24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1 };
static const unsigned char des_p[32] = {
	16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
	2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25 };
static const unsigned char des_pc1[56] = {
	57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18,
	10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
	63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
	14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4 };
static const unsigned char des_pc2[48] = {
	14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10,
	23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
	41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
	44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32 };
static const unsigned char des_shifts[16] = {
	1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };
static const unsigned char des_s[8][64] = {
	{ 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
	  0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
	  4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
	  15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
	{ 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
	  3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
	  0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
	  13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
	{ 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
	  13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
	  13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
	  1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
	{ 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
	  13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
	  10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
	  3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
	{ 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
	  14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
	  4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
	  11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
	{ 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
	  10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
	  9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
	  4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
	{ 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
	  13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
	  1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
	  6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
	{ 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
	  1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
	  7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
	  2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 } };


/*  Bit i (1 = the most significant) of the output is bit table[i-1] of
    the input.  */
static uint64_t des_permute(uint64_t in, int inbits,
	const unsigned char *table, int n)
{
	uint64_t out = 0;

	for (int i = 0; i < n; i++)
		out = (out << 1) | ((in >> (inbits - table[i])) & 1);
	return out;
}


static uint64_t des_encrypt(uint64_t key, uint64_t block)
{
	uint64_t k = des_permute(key, 64, des_pc1, 56);
	uint32_t c = k >> 28, d = k & 0xfffffff;
	uint64_t b = des_permute(block, 64, des_ip, 64);
	uint32_t l = b >> 32, r = (uint32_t) b;

	for (int round = 0; round < 16; round++) {
		for (int i = 0; i < des_shifts[round]; i++) {
			c = ((c << 1) | (c >> 27)) & 0xfffffff;
			d = ((d << 1) | (d >> 27)) & 0xfffffff;
		}
		uint64_t subkey = des_permute(((uint64_t) c << 28) | d, 56,
		    des_pc2, 48);

		uint64_t x = des_permute(r, 32, des_e, 48) ^ subkey;
		uint32_t f = 0;
		for (int i = 0; i < 8; i++) {
			int six = (x >> (42 - 6 * i)) & 63;
			f = (f << 4) | des_s[i][(six & 0x20) | ((six & 1) << 4) |
			    ((six >> 1) & 15)];
		}
		f = des_permute(f, 32, des_p, 32);

		uint32_t t = l ^ f;
		l = r;
		r = t;
	}

	return des_permute(((uint64_t) r << 32) | l, 64, des_fp, 64);
}


/*  Is the client's answer the challenge, encrypted with the password?  */
static bool password_ok(const struct vnc_client *c, const unsigned char *p)
{
	uint64_t key = 0;
	unsigned char diff = 0;

	for (int i = 0; i < 8; i++) {
		unsigned char ch = i < (int) strlen(password) ?
		    password[i] : 0, rev = 0;
		for (int bit = 0; bit < 8; bit++)
			if (ch & (1 << bit))
				rev |= 0x80 >> bit;
		key = (key << 8) | rev;
	}

	for (int half = 0; half < 2; half++) {
		uint64_t block = 0;
		for (int i = 0; i < 8; i++)
			block = (block << 8) | c->challenge[half * 8 + i];
		uint64_t want = des_encrypt(key, block);
		for (int i = 0; i < 8; i++)
			diff |= p[half * 8 + i] ^ (unsigned char)
			    (want >> (56 - 8 * i));
	}

	return diff == 0;
}


static void send_challenge(struct vnc_client *c)
{
	FILE *f = fopen("/dev/urandom", "rb");

	if (f == NULL || fread(c->challenge, 1, 16, f) != 16)
		for (int i = 0; i < 16; i++)
			c->challenge[i] = random() >> 8;
	if (f != NULL)
		fclose(f);

	put(c, c->challenge, 16);
	c->state = VNC_AUTH;
}


/*****************************************************************************/

/*
 *  Input:
 */

/*  X11 keysyms (the others are in display.c):  */
#define	XK_Control_L	0xffe3
#define	XK_Control_R	0xffe4


static void key_event(struct vnc_client *c, bool down, uint32_t key)
{
	if (key == XK_Control_L)
		c->ctrl_l = down;
	if (key == XK_Control_R)
		c->ctrl_r = down;
	if (down)
		display_keysym(c->win->machine, key, c->ctrl_l || c->ctrl_r);
}


/*
 *  The emulated mouse moves by as much as the client's pointer moved (in
 *  window pixels, which are scaledown emulated pixels).
 */
static void pointer_event(struct vnc_client *c, int buttons, int x, int y)
{
	struct fb_window *win = c->win;

	if (c->mouse_known && (x != c->mouse_x || y != c->mouse_y))
		console_mouse_coordinate_update(
		    (x - c->mouse_x) * win->scaledown,
		    (y - c->mouse_y) * win->scaledown, win->fb_number);
	c->mouse_x = x;
	c->mouse_y = y;
	c->mouse_known = true;

	/*  Bits 0..2 are the left, middle and right buttons:  */
	for (int b = 0; b < 3; b++)
		if ((buttons ^ c->buttons) & (1 << b))
			console_mouse_button(b + 1, (buttons >> b) & 1);
	c->buttons = buttons;
}


static uint32_t get16(const unsigned char *p)
{
	return (p[0] << 8) | p[1];
}


static uint32_t get32(const unsigned char *p)
{
	return ((uint32_t) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}


/*
 *  Handles the message (or handshake step) at the start of p.  Returns
 *  how many bytes it was, 0 if it isn't all there yet, or -1 if the
 *  client is to be dropped.
 */
static long client_message(struct vnc_client *c, const unsigned char *p,
	size_t n)
{
	switch (c->state) {

	case VNC_VERSION:
		if (n < 12)
			return 0;
		if (memcmp(p, "RFB 003.", 8) != 0 || p[11] != '\n') {
			disconnect(c, "not a VNC client");
			return -1;
		}
		c->minor = atoi((const char *) p + 8);
		TRACE(c, "client version %.11s", (const char *) p);
		if (c->minor >= 8)
			c->minor = 8;
		else if (c->minor != 7)
			c->minor = 3;

		/*  The security type is VNC Authentication (2) with a
		    password, and None (1) without:  */
		if (c->minor == 3) {
			put32(c, password != NULL ? 2 : 1);
			if (password != NULL)
				send_challenge(c);
			else
				c->state = VNC_CLIENT_INIT;
		} else {
			put8(c, 1);
			put8(c, password != NULL ? 2 : 1);
			c->state = VNC_SECURITY;
		}
		return 12;

	case VNC_SECURITY:
		TRACE(c, "security type %i", p[0]);
		if (p[0] != (password != NULL ? 2 : 1)) {
			disconnect(c, "it wants another security type");
			return -1;
		}
		if (password != NULL)
			send_challenge(c);
		else {
			if (c->minor == 8)
				put32(c, 0);
			c->state = VNC_CLIENT_INIT;
		}
		return 1;

	case VNC_AUTH:
		if (n < 16)
			return 0;
		TRACE(c, "password %s", password_ok(c, p) ? "ok" : "wrong");
		if (password_ok(c, p)) {
			put32(c, 0);
			c->state = VNC_CLIENT_INIT;
			return 16;
		}

		debugmsg(SUBSYS_DISPLAY, "vnc", VERBOSITY_WARNING,
		    "a VNC client gave the wrong password");
		put32(c, 1);
		if (c->minor == 8) {
			const char *why = "Wrong password";
			put32(c, strlen(why));
			put(c, why, strlen(why));
		}
		c->closing = true;

		/*  (Guessing gets slow.)  */
		wrong_password = true;
		wrong_password_at = display_ticks();
		return 16;

	case VNC_CLIENT_INIT:
		TRACE(c, "ClientInit: shared %i", p[0]);
		/*  Not shared:  the other clients go.  */
		if (p[0] == 0)
			for (int i = 0; i < VNC_MAX_CLIENTS; i++)
				if (&clients[i] != c)
					disconnect(&clients[i], NULL);
		c->state = VNC_WAIT_FB;
		return 1;

	case VNC_WAIT_FB:
		/*  (Nothing more is expected before ServerInit.)  */
		return 0;

	case VNC_NORMAL:
		break;
	}

	switch (p[0]) {

	case 0:		/*  SetPixelFormat  */
		if (n < 20)
			return 0;
		if (p[4] != 8 && p[4] != 16 && p[4] != 32) {
			disconnect(c, "unsupported pixel format");
			return -1;
		}
		if (!p[7]) {
			disconnect(c, "it wants a colour map, which isn't "
			    "supported");
			return -1;
		}
		TRACE(c, "SetPixelFormat: %i bpp, depth %i, %s endian, "
		    "true colour %i, max %i/%i/%i, shift %i/%i/%i", p[4], p[5],
		    p[6] ? "big" : "little", p[7], get16(p + 8), get16(p + 10),
		    get16(p + 12), p[14], p[15], p[16]);
		set_pixel_format(c, p[4], p[5], p[6] != 0, get16(p + 8),
		    get16(p + 10), get16(p + 12), p[14] & 31, p[15] & 31,
		    p[16] & 31);
		memset(c->dirty, 2, (size_t) c->tiles_x * c->tiles_y);
		return 20;

	case 2: {	/*  SetEncodings  */
		if (n < 4)
			return 0;
		size_t len = 4 + 4 * get16(p + 2);
		if (n < len)
			return 0;

		c->encoding = ENC_RAW;
		c->desktop_size = c->desktop_name = false;
		if (trace) {
			fprintf(stderr, "vnc[%i]: SetEncodings:",
			    (int) (c - clients));
			for (size_t i = 4; i < len; i += 4)
				fprintf(stderr, " %i", (int32_t) get32(p + i));
			fputc('\n', stderr);
		}
		/*  The first encoding there is, in the client's order:  */
		bool chosen = false;
		for (size_t i = 4; i < len; i += 4) {
			int32_t e = (int32_t) get32(p + i);
#ifdef WITH_ZLIB
			if (!chosen && (e == ENC_ZRLE || e == ENC_ZLIB))
				c->encoding = e;
#endif
			if (e == ENC_RAW || e == ENC_ZRLE || e == ENC_ZLIB)
				chosen = true;
			if (e == ENC_DESKTOP_SIZE)
				c->desktop_size = true;
			if (e == ENC_DESKTOP_NAME)
				c->desktop_name = true;
		}
		return len;
	}

	case 3:		/*  FramebufferUpdateRequest  */
		if (n < 10)
			return 0;
		TRACE(c, "FramebufferUpdateRequest: %s %i,%i %ix%i",
		    p[1] ? "incremental" : "full", get16(p + 2), get16(p + 4),
		    get16(p + 6), get16(p + 8));
		if (!p[1]) {
			/*  Not incremental:  all of the requested area.  */
			int x = get16(p + 2), y = get16(p + 4);
			int w = get16(p + 6), h = get16(p + 8);
			for (int ty = y / VNC_TILE; ty < c->tiles_y &&
			    ty <= (y + h - 1) / VNC_TILE; ty++)
				for (int tx = x / VNC_TILE; tx < c->tiles_x &&
				    tx <= (x + w - 1) / VNC_TILE; tx++)
					c->dirty[ty * c->tiles_x + tx] = 2;
		}
		c->update_requested = true;
		return 10;

	case 4:		/*  KeyEvent  */
		if (n < 8)
			return 0;
		TRACE(c, "KeyEvent: %s 0x%x", p[1] ? "down" : "up",
		    (unsigned) get32(p + 4));
		key_event(c, p[1] != 0, get32(p + 4));
		return 8;

	case 5:		/*  PointerEvent  */
		if (n < 6)
			return 0;
		pointer_event(c, p[1], get16(p + 2), get16(p + 4));
		return 6;

	case 6:		/*  ClientCutText, which is ignored  */
		if (n < 8)
			return 0;
		c->skip = get32(p + 4);
		TRACE(c, "ClientCutText: %zu bytes", c->skip);
		return 8;

	default: {
		char why[40];
		snprintf(why, sizeof(why), "unknown message type %i", p[0]);
		disconnect(c, why);
		return -1;
	}
	}
}


static void read_client(struct vnc_client *c)
{
	char buf[16384];

	while (c->fd >= 0) {
		ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
		if (n <= 0) {
			int e = socket_error();
			if (n == 0 || (!socket_would_block(e) &&
			    e != SOCKET_EINTR))
				disconnect(c, NULL);
			return;
		}

		const char *p = buf;
		if (c->skip > 0) {
			size_t k = (size_t) n < c->skip ? (size_t) n : c->skip;
			c->skip -= k;
			p += k;
			n -= k;
		}
		if (n == 0)
			continue;

		if (c->inlen + n > VNC_MAX_MESSAGE) {
			disconnect(c, "message too long");
			return;
		}
		memcpy(c->in + c->inlen, p, n);
		c->inlen += n;

		size_t done = 0;
		while (c->fd >= 0 && done < c->inlen) {
			if (c->skip > 0) {
				size_t k = c->inlen - done < c->skip ?
				    c->inlen - done : c->skip;
				c->skip -= k;
				done += k;
				continue;
			}
			long k = client_message(c, c->in + done,
			    c->inlen - done);
			if (k <= 0)
				break;
			done += k;
		}
		if (c->fd < 0)
			return;

		memmove(c->in, c->in + done, c->inlen - done);
		c->inlen -= done;
	}
}


static void accept_clients(void)
{
	int s, one = 1;

	while ((s = accept(listen_fd, NULL, NULL)) >= 0) {
#ifdef FD_CLOEXEC
		fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
#ifdef SO_NOSIGPIPE
		setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (void *) &one,
		    sizeof(one));
#endif
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (void *) &one,
		    sizeof(one));

		struct vnc_client *c = NULL;
		for (int i = 0; i < VNC_MAX_CLIENTS; i++)
			if (clients[i].fd < 0) {
				c = &clients[i];
				break;
			}
		if (c == NULL) {
			debugmsg(SUBSYS_DISPLAY, "vnc", VERBOSITY_WARNING,
			    "too many VNC clients");
			socket_close(s);
			continue;
		}

		socket_set_nonblocking(s);
		c->fd = s;
		TRACE(c, "connected");
		c->state = VNC_VERSION;
		CHECK_ALLOCATION(c->in = (unsigned char *) malloc(
		    VNC_MAX_MESSAGE));
		put(c, "RFB 003.008\n", 12);
	}
}


/*
 *  vnc_poll():
 *
 *  Accepts clients, handles what they sent, and sends them updates.
 */
void vnc_poll(struct emul *emul)
{
	if (listen_fd < 0)
		return;

	uint32_t now = display_ticks();
	if (wrong_password && now - wrong_password_at >= 1000)
		wrong_password = false;
	if (!wrong_password)
		accept_clients();

	for (int i = 0; i < VNC_MAX_CLIENTS; i++) {
		struct vnc_client *c = &clients[i];

		if (!c->closing)
			read_client(c);
		if (c->fd < 0)
			continue;

		if (c->state == VNC_WAIT_FB) {
			struct fb_window *win = shown_window(emul);
			if (win != NULL)
				server_init(c, win);
		}

		if (c->state == VNC_NORMAL && c->update_requested &&
		    c->outlen == 0 && now - c->last_update >=
		    VNC_UPDATE_INTERVAL_MS)
			send_update(c);

		flush(c);
		if (c->closing && c->outlen == 0)
			disconnect(c, NULL);
	}
}


/*****************************************************************************/


#ifndef _WIN32
static void remove_unix_socket(void)
{
	if (unix_path != NULL)
		unlink(unix_path);
}
#endif


bool vnc_active(void)
{
	return listen_fd >= 0;
}


/*
 *  vnc_init():
 *
 *  Starts listening, as given by --vnc: "tcp:port", "tcp:host:port" or
 *  "unix:path".  Exits on errors.
 */
void vnc_init(const char *spec)
{
	int one = 1;

	for (int i = 0; i < VNC_MAX_CLIENTS; i++)
		clients[i].fd = -1;

	trace = getenv("GXEMUL_VNC_DEBUG") != NULL;

	const char *pw = getenv("GXEMUL_VNC_PASSWORD");
	if (pw != NULL && pw[0] != '\0') {
		CHECK_ALLOCATION(password = strdup(pw));
		if (strlen(password) > 8)
			fprintf(stderr, "gxemul: only the first 8 characters of "
			    "GXEMUL_VNC_PASSWORD count (VNC passwords are at "
			    "most 8)\n");
	}

	if (strncmp(spec, "tcp:", 4) == 0) {
		struct addrinfo hints, *ai;
		char host[256], port[16];
		const char *colon = strrchr(spec + 4, ':');

		/*  Without a host, only this computer can connect:  */
		if (colon == NULL) {
			strlcpy(host, "127.0.0.1", sizeof(host));
			strlcpy(port, spec + 4, sizeof(port));
		} else {
			const char *h = spec + 4;
			size_t len = colon - h;
			if (len >= 2 && h[0] == '[' && h[len - 1] == ']') {
				h++;
				len -= 2;
			}
			if (len >= sizeof(host)) {
				fprintf(stderr, "--vnc: bad host in '%s'\n",
				    spec);
				exit(1);
			}
			memcpy(host, h, len);
			host[len] = '\0';
			strlcpy(port, colon + 1, sizeof(port));
		}

		char *end;
		long portnr = strtol(port, &end, 10);
		if (port[0] == '\0' || *end != '\0' || portnr < 0 ||
		    portnr > 65535) {
			fprintf(stderr, "--vnc: bad port in '%s'\n", spec);
			exit(1);
		}

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;
		if (getaddrinfo(host, port, &hints, &ai) != 0) {
			fprintf(stderr, "--vnc: unknown host '%s'\n", host);
			exit(1);
		}

		listen_fd = socket(ai->ai_family, SOCK_STREAM, 0);
		if (listen_fd < 0) {
			perror("--vnc: socket");
			exit(1);
		}
		setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
		    (void *) &one, sizeof(one));

		/*  [::] is IPv4's addresses too (not the default on
		    BSDs and Windows):  */
		if (ai->ai_family == AF_INET6) {
			int zero = 0;
			setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY,
			    (void *) &zero, sizeof(zero));
		}
		if (bind(listen_fd, ai->ai_addr, ai->ai_addrlen) < 0) {
			fprintf(stderr, "--vnc: can't listen on %s port %s "
			    "(error %i)\n", host, port, socket_error());
			exit(1);
		}

		bool loopback = ai->ai_family == AF_INET6 ?
		    IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *)
		    ai->ai_addr)->sin6_addr) :
		    (ntohl(((struct sockaddr_in *) ai->ai_addr)->sin_addr.
		    s_addr) >> 24) == 127;
		freeaddrinfo(ai);

		struct sockaddr_storage ss;
		socklen_t sslen = sizeof(ss);
		getsockname(listen_fd, (struct sockaddr *) &ss, &sslen);
		int actual = ntohs(ss.ss_family == AF_INET6 ?
		    ((struct sockaddr_in6 *) &ss)->sin6_port :
		    ((struct sockaddr_in *) &ss)->sin_port);
		if (colon == NULL)
			fprintf(stderr, "gxemul: VNC server: tcp:%i\n", actual);
		else
			fprintf(stderr, "gxemul: VNC server: tcp:%s%s%s:%i\n",
			    strchr(host, ':') != NULL ? "[" : "", host,
			    strchr(host, ':') != NULL ? "]" : "", actual);
		if (!loopback && password == NULL)
			fprintf(stderr, "gxemul: anyone who can reach this "
			    "port can use the emulator (set "
			    "GXEMUL_VNC_PASSWORD for a password)\n");
	} else if (strncmp(spec, "unix:", 5) == 0) {
#ifdef _WIN32
		fprintf(stderr, "--vnc: unix: sockets aren't supported on "
		    "Windows; use tcp:port\n");
		exit(1);
#else
		struct sockaddr_un sun;
		struct stat st;
		const char *path = spec + 5;

		if (path[0] == '\0' || strlen(path) >= sizeof(sun.sun_path)) {
			fprintf(stderr, "--vnc: bad path in '%s'\n", spec);
			exit(1);
		}

		/*  Replace an old socket, but nothing else:  */
		if (lstat(path, &st) == 0) {
			if (!S_ISSOCK(st.st_mode)) {
				fprintf(stderr, "--vnc: %s exists, and isn't "
				    "a socket\n", path);
				exit(1);
			}
			unlink(path);
		}

		listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (listen_fd < 0) {
			perror("--vnc: socket");
			exit(1);
		}

		memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_UNIX;
		strlcpy(sun.sun_path, path, sizeof(sun.sun_path));
		if (bind(listen_fd, (struct sockaddr *) &sun,
		    sizeof(sun)) < 0) {
			fprintf(stderr, "--vnc: can't listen on %s: %s\n",
			    path, strerror(errno));
			exit(1);
		}

		CHECK_ALLOCATION(unix_path = strdup(path));
		atexit(remove_unix_socket);
		fprintf(stderr, "gxemul: VNC server: %s\n", spec);
#endif
	} else {
		fprintf(stderr, "--vnc: expected tcp:port, tcp:host:port or "
		    "unix:path, not '%s'\n", spec);
		exit(1);
	}

	if (listen(listen_fd, VNC_MAX_CLIENTS) < 0) {
		perror("--vnc: listen");
		exit(1);
	}

#ifdef FD_CLOEXEC
	/*  (A reset execs gxemul again, which listens anew.)  */
	fcntl(listen_fd, F_SETFD, FD_CLOEXEC);
#endif
	socket_set_nonblocking(listen_fd);

	if (password != NULL)
		fprintf(stderr, "gxemul: VNC clients need the password (from "
		    "GXEMUL_VNC_PASSWORD)\n");
}
