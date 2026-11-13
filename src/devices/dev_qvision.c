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
 *  COMMENT: Compaq QVision 1024/E EISA graphics controller
 *
 *  The QVision is a VGA with Compaq extensions.  This file has its own VGA
 *  core (dev_vga.c guesses modes from a few registers, which is not enough
 *  for firmware that tests the planes):
 *
 *	- 1 MB of video memory as four planes, with the VGA write modes
 *	  0..3, read modes 0..1, latches, set/reset, rotate, logical
 *	  operations and bit mask, and planar, odd/even and chain-4
 *	  addressing;
 *	- the misc output, sequencer, CRTC, graphics controller and
 *	  attribute controller registers, input status, and the DAC;
 *	- a display that is scanned out from the registers: text modes,
 *	  16-colour planar modes and 256-colour modes.
 *
 *  QVision extensions (from Compaq's ECU board file and the VGADOC notes):
 *
 *	- GC index 0Fh unlocks the extended graphics registers (05h);
 *	  GC 0Ch is the chip ID, GC 54h the memory size, GC 45h/46h the
 *	  bank (page) registers;
 *	- the EISA board ID CPQ3011 at zC80, and the board control
 *	  registers zC84 (enable) and zC85 (controller order, ROM bank);
 *	- 83C4h, the controller select;
 *	- the DAC's extra registers at 13C6h, 83C6h and 93C6h, a Brooktree
 *	  Bt485's: 83C6h bit 1 selects an 8-bit DAC, and the hardware cursor
 *	  (see qv_cursor_line());
 *	- the BitBLT registers at 23C0h, 33C0h and 63C0h (stored only);
 *	- the 32 KB option ROM window at C0000, banked from a 64 KB image.
 *
 *  Video memory layout: byte (offset * 4 + plane), so that chain-4 and
 *  packed-pixel addresses are linear byte addresses.
 *
 *  TODO: packed 4-bit and 15/24-bit modes, the line draw engine (83C0h),
 *  the video interrupt.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "device.h"
#include "devices.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"


#define	QV_VRAM_SIZE		(1024 * 1024)
#define	QV_PLANE_SIZE		(QV_VRAM_SIZE / 4)
#define	QV_ROM_SIZE		0x10000
#define	QV_ROM_WINDOW		0x8000

/*  GC 0Ch, bits 3..7: 06h is the original QVision 1024.  */
#define	QV_CHIP_ID		(0x06 << 3)

#define	QV_MAX_XSIZE		1280
#define	QV_MAX_YSIZE		1024

#define	QV_TICK_SHIFT		20

#ifndef QV_SENSE
#define	QV_SENSE		0x10
#endif

/*  Where the framebuffer device sits in the physical address space (out
    of the way of everything else on the Jensen):  */
#define	QV_FB_ADDR		0x1c00000000ULL

static const uint8_t qv_eisa_id[4] = { 0x0e, 0x11, 0x30, 0x11 };


struct qv_data {
	struct machine	*machine;
	int		subsys;
	int		slot;

	unsigned char	*vram;
	uint8_t		latch[4];

	uint8_t		misc;
	uint8_t		feature;
	uint8_t		vga_enable;		/*  3C3h  */
	uint8_t		vga_enable_46e8;
	uint8_t		pos102;

	uint8_t		seq_idx;
	uint8_t		seq[8];
	uint8_t		gc_idx;
	uint8_t		gc[256];
	uint8_t		crtc_idx;
	uint8_t		crtc[256];
	uint8_t		attr_idx;		/*  including bit 5, PAS  */
	int		attr_flipflop;
	uint8_t		attr[32];

	uint8_t		dac_mask;
	uint8_t		dac_read_idx, dac_write_idx;
	int		dac_read_sub, dac_write_sub;
	int		dac_state;		/*  0 = write, 3 = read  */
	uint8_t		dac_tmp[3];
	uint8_t		dac[256][3];
	uint8_t		dac_ext[3][4];		/*  13C6h, 83C6h, 93C6h  */

	/*  The DAC's hardware cursor (a Brooktree Bt485's):  */
	uint8_t		cursor_ram[1024];
	int		cursor_addr;
	uint8_t		cursor_color[4][3];	/*  0 = overscan, 1..3  */
	int		cursor_color_idx, cursor_color_sub;

	uint8_t		ctrl_select;		/*  83C4h  */
	uint8_t		board_ctrl;		/*  zC84  */
	uint8_t		board_ctrl2;		/*  zC85  */
	uint8_t		blt[3][16];		/*  23C0h, 33C0h, 63C0h  */

	/*  The current BitBLT operation:  */
	int		blt_mono, blt_transparent, blt_use_rop;
	uint8_t		blt_rop;
	uint8_t		pattern[8];		/*  33CAh..33CDh, twice  */
	int		pattern_half;
	int		blt_host;		/*  source data from the CPU  */
	int		blt_w, blt_h, blt_y;
	uint32_t	blt_dst;
	uint8_t		blt_line[QV_MAX_XSIZE + 8];
	int		blt_line_pos, blt_line_len, blt_skip;

	unsigned	is1_reads;

	int		have_rom;
	unsigned char	rom[QV_ROM_SIZE];

	struct vfb_data	*fb;
	int		fb_xsize, fb_ysize;
	unsigned char	*line;
	int		dirty;
	int		blink_counter;
	char		last_state[200];
	char		last_text[81];
};


static int qv_extended_unlocked(struct qv_data *d)
{
	return (d->gc[0x0f] & 0x0f) == 0x05;
}


/*****************************************************************************
 *
 *  Video memory
 */

/*
 *  qv_mem_decode():
 *
 *  Turns a CPU address in the A0000..BFFFF window into a plane offset and a
 *  set of planes, as the sequencer's memory mode says.  Returns 0 if the
 *  address is outside the window the graphics controller maps.
 */
static int qv_mem_decode(struct qv_data *d, uint32_t a, uint32_t *off,
	int *planes, int *readplane)
{
	uint32_t bank = 0;

	switch ((d->gc[6] >> 2) & 3) {
	case 0:	break;
	case 1:	if (a >= 0x10000)
			return 0;
		break;
	case 2:	if (a < 0x10000 || a >= 0x18000)
			return 0;
		a -= 0x10000;
		break;
	case 3:	if (a < 0x18000)
			return 0;
		a -= 0x18000;
		break;
	}

	/*  QVision bank (page) registers, in 4 KB units; with GC 40h bit 0,
	    page 1 is used for the upper 32 KB:  */
	if ((d->gc[0x40] & 1) && a >= 0x8000) {
		bank = d->gc[0x46] * 4096;
		a -= 0x8000;
	} else
		bank = d->gc[0x45] * 4096;
	a += bank;

	if (d->seq[4] & 0x08) {
		/*  Chain-4:  */
		*off = a >> 2;
		*planes = 1 << (a & 3);
		*readplane = a & 3;
	} else if (!(d->seq[4] & 0x04)) {
		/*  Odd/even:  */
		*off = a & ~1;
		*planes = (a & 1)? 0x0a : 0x05;
		*readplane = (a & 1) | (d->gc[4] & 2);
	} else {
		*off = a;
		*planes = 0x0f;
		*readplane = d->gc[4] & 3;
	}

	*off &= (QV_PLANE_SIZE - 1);
	return 1;
}


static uint8_t qv_mem_read(struct qv_data *d, uint32_t a)
{
	uint32_t off;
	int planes, readplane, p;

	if (!qv_mem_decode(d, a, &off, &planes, &readplane))
		return 0xff;

	for (p = 0; p < 4; p++)
		d->latch[p] = d->vram[off * 4 + p];

	if (d->gc[5] & 0x08) {
		/*  Read mode 1: colour compare.  */
		uint8_t result = 0xff;
		for (p = 0; p < 4; p++)
			if (d->gc[7] & (1 << p))
				result &= ~(d->latch[p] ^
				    ((d->gc[2] & (1 << p))? 0xff : 0x00));
		return result;
	}

	return d->latch[readplane];
}


static void qv_mem_write(struct qv_data *d, uint32_t a, uint8_t val)
{
	uint32_t off;
	int planes, readplane, p;
	int mode = d->gc[5] & 3, rot = d->gc[3] & 7, func = (d->gc[3] >> 3) & 3;
	uint8_t mask = d->gc[8], rotated;

	if (!qv_mem_decode(d, a, &off, &planes, &readplane))
		return;

	planes &= d->seq[2] & 0x0f;
	rotated = (val >> rot) | (val << (8 - rot));

	for (p = 0; p < 4; p++) {
		uint8_t v, m = mask, latch = d->latch[p];

		if (!(planes & (1 << p)))
			continue;

		switch (mode) {
		case 0:	v = (d->gc[1] & (1 << p))?
			    ((d->gc[0] & (1 << p))? 0xff : 0x00) : rotated;
			break;
		case 1:	d->vram[off * 4 + p] = latch;
			continue;
		case 2:	v = (val & (1 << p))? 0xff : 0x00;
			break;
		default:
			m &= rotated;
			v = (d->gc[0] & (1 << p))? 0xff : 0x00;
			break;
		}

		if (mode != 3) {
			switch (func) {
			case 1:	v &= latch; break;
			case 2:	v |= latch; break;
			case 3:	v ^= latch; break;
			}
		}

		d->vram[off * 4 + p] = (v & m) | (latch & ~m);
	}

	d->dirty = 1;
}


/*****************************************************************************
 *
 *  Display (scan-out)
 */

static void qv_dac_rgb(struct qv_data *d, int idx, unsigned char *rgb)
{
	int i, dac8 = d->dac_ext[1][0] & 0x02;

	idx &= d->dac_mask;
	for (i = 0; i < 3; i++) {
		int c = d->dac[idx][i];
		rgb[i] = dac8? c : (c & 0x3f) * 255 / 63;
	}
}


/*  A 4-bit pixel through the attribute controller to a DAC index:  */
static int qv_attr_to_dac(struct qv_data *d, int pixel)
{
	int pal = d->attr[(pixel & d->attr[0x12]) & 0x0f] & 0x3f;

	if (d->attr[0x10] & 0x80)
		return ((d->attr[0x14] & 0x0f) << 4) | (pal & 0x0f);
	return ((d->attr[0x14] & 0x0c) << 4) | pal;
}


static void qv_resize(struct qv_data *d, int xsize, int ysize)
{
	if (xsize == d->fb_xsize && ysize == d->fb_ysize)
		return;

	debugmsg(d->subsys, "qvision", VERBOSITY_INFO,
	    "display %ix%i\n", xsize, ysize);
	dev_fb_resize(d->fb, xsize, ysize);
	d->fb_xsize = xsize;
	d->fb_ysize = ysize;
}


/*  Puts one rendered line into the framebuffer, marking it if changed:  */
/*
 *  qv_cursor_line():
 *
 *  Draws line y of the DAC's hardware cursor (Bt485) over the rendered
 *  line.  13C9h (command register 2) bits 1..0 are the mode: 0 = off,
 *  1 = three colours, 2 = "XGA" (the two planes as the AND and XOR masks:
 *  colour 1, colour 2, transparent, inverted), 3 = X Windows (transparent,
 *  transparent, colour 1, colour 2).  93C8h/93C9h (X) and 93C6h/93C7h (Y)
 *  give the cursor's lower right corner.  13C6h (command register 3)
 *  bit 2 selects a 64x64 cursor instead of 32x32.  The cursor RAM holds
 *  plane 0, then plane 1, a row at a time with the leftmost pixel in the
 *  most significant bit.
 */
static void qv_cursor_line(struct qv_data *d, int y)
{
	int mode = d->dac_ext[0][3] & 3, size, x0, y0, row, x, dac8;
	int bytes_per_row, plane1;

	if (mode == 0 || !(d->attr[0x10] & 0x01))
		return;

	size = (d->dac_ext[0][0] & 0x04)? 64 : 32;
	bytes_per_row = size / 8;
	plane1 = size * bytes_per_row;
	x0 = (d->dac_ext[2][2] | ((d->dac_ext[2][3] & 0x0f) << 8)) - size;
	y0 = (d->dac_ext[2][0] | ((d->dac_ext[2][1] & 0x0f) << 8)) - size;
	row = y - y0;
	if (row < 0 || row >= size)
		return;

	dac8 = d->dac_ext[1][0] & 0x02;
	for (x = 0; x < size; x++) {
		int sx = x0 + x, bit = 7 - (x & 7), i, pix, c;
		int a = row * bytes_per_row + x / 8;
		unsigned char *p;

		if (sx < 0 || sx >= d->fb_xsize)
			continue;
		pix = ((d->cursor_ram[a] >> bit) & 1) |
		    (((d->cursor_ram[plane1 + a] >> bit) & 1) << 1);
		p = d->line + sx * 3;

		switch (mode) {
		case 1:	c = pix; break;
		case 2:	c = pix == 2? 0 : pix == 3? -1 : pix + 1; break;
		default:c = pix < 2? 0 : pix - 1;
		}
		if (c == 0)
			continue;
		for (i = 0; i < 3; i++) {
			int v = d->cursor_color[c > 0? c : 0][i];
			if (c < 0)
				p[i] = 255 - p[i];
			else
				p[i] = dac8? v : (v & 0x3f) * 255 / 63;
		}
	}
}


static void qv_put_line(struct qv_data *d, int y)
{
	size_t n = d->fb_xsize * 3;
	unsigned char *dst = d->fb->framebuffer + (size_t) y * d->fb->bytes_per_line;

	qv_cursor_line(d, y);

	if (memcmp(dst, d->line, n) == 0)
		return;

	memcpy(dst, d->line, n);
	d->fb->update_x1 = 0;
	d->fb->update_x2 = d->fb_xsize - 1;
	if (y < d->fb->update_y1 || d->fb->update_y2 == -1)
		d->fb->update_y1 = y;
	if (y > d->fb->update_y2)
		d->fb->update_y2 = y;
}


static void qv_render_text(struct qv_data *d, int cols, int lines,
	int charw, int charh, uint32_t start, uint32_t stride)
{
	int y, col;
	int map_a = ((d->seq[3] >> 3) & 4) | (d->seq[3] & 3);
	int map_b = ((d->seq[3] >> 2) & 4) | ((d->seq[3] >> 2) & 3);
	uint32_t cursor = (d->crtc[0x0e] << 8) | d->crtc[0x0f];
	int cur_start = d->crtc[0x0a] & 0x1f, cur_end = d->crtc[0x0b] & 0x1f;
	int cursor_on = !(d->crtc[0x0a] & 0x20) && (d->blink_counter & 8);
	int blink = d->attr[0x10] & 0x08;
	static const int map_offset[8] = { 0x0000, 0x4000, 0x8000, 0xc000,
	    0x2000, 0x6000, 0xa000, 0xe000 };

	for (y = 0; y < lines; y++) {
		int row = y / charh, scan = y % charh;
		unsigned char *p = d->line;

		for (col = 0; col < cols; col++) {
			uint32_t addr = start + row * stride + col;
			uint32_t off = (addr * 2) & (QV_PLANE_SIZE - 1);
			int ch = d->vram[off * 4 + 0];
			int at = d->vram[off * 4 + 1];
			int fg = at & 0x0f, bg = at >> 4, x;
			int map = ((at & 0x08) && map_a != map_b)? map_b : map_a;
			uint8_t bits = d->vram[(map_offset[map] + ch * 32 +
			    scan) * 4 + 2];
			unsigned char fgrgb[3], bgrgb[3];

			if (blink) {
				if ((bg & 8) && (d->blink_counter & 16))
					fg = bg & 7;
				bg &= 7;
			}
			if (cursor_on && addr == cursor && scan >= cur_start &&
			    scan <= cur_end)
				bits = 0xff;

			qv_dac_rgb(d, qv_attr_to_dac(d, fg), fgrgb);
			qv_dac_rgb(d, qv_attr_to_dac(d, bg), bgrgb);

			for (x = 0; x < charw; x++) {
				int on;
				if (x < 8)
					on = bits & (0x80 >> x);
				else
					on = (d->attr[0x10] & 0x04) &&
					    ch >= 0xc0 && ch <= 0xdf &&
					    (bits & 1);
				memcpy(p, on? fgrgb : bgrgb, 3);
				p += 3;
			}
		}

		qv_put_line(d, y);
	}
}


static void qv_render_planar(struct qv_data *d, int width, int lines,
	int rowrep, uint32_t start, uint32_t stride)
{
	int y, x;

	for (y = 0; y < lines; y++) {
		uint32_t base = start + (y / rowrep) * stride;
		unsigned char *p = d->line;

		for (x = 0; x < width; x++) {
			uint32_t off = (base + (x >> 3)) & (QV_PLANE_SIZE - 1);
			int bit = 7 - (x & 7), pix = 0, plane;

			for (plane = 0; plane < 4; plane++)
				pix |= ((d->vram[off * 4 + plane] >> bit) & 1)
				    << plane;
			qv_dac_rgb(d, qv_attr_to_dac(d, pix), p);
			p += 3;
		}

		qv_put_line(d, y);
	}
}


static void qv_render_256(struct qv_data *d, int width, int lines,
	int rowrep, uint32_t start, uint32_t stride)
{
	int y, x;

	for (y = 0; y < lines; y++) {
		uint32_t base = start + (y / rowrep) * stride;
		unsigned char *p = d->line;

		for (x = 0; x < width; x++) {
			int pix = d->vram[(base + x) & (QV_VRAM_SIZE - 1)];
			qv_dac_rgb(d, pix, p);
			p += 3;
		}

		qv_put_line(d, y);
	}
}


/*  GC 40h bit 0: the QVision's packed-pixel mode (QV.SYS sets it for its
    8-bit modes, without the VGA's 256-colour bit in GC 05h).  */
static int qv_packed(struct qv_data *d)
{
	return (d->gc[0x40] & 0x01) && (d->attr[0x10] & 0x01);
}


static void qv_render(struct qv_data *d)
{
	int hde = d->crtc[0x01] + 1;
	int vde = (d->crtc[0x12] | ((d->crtc[0x07] & 0x02) << 7) |
	    ((d->crtc[0x07] & 0x40) << 3)) + 1;
	int maxscan = (d->crtc[0x09] & 0x1f) + 1;
	int dblscan = (d->crtc[0x09] & 0x80)? 2 : 1;
	uint32_t start = (d->crtc[0x0c] << 8) | d->crtc[0x0d] |
	    (((d->gc[0x42] >> 2) & 7) << 16);
	uint32_t offset = d->crtc[0x13] | ((d->gc[0x42] & 3) << 8);
	int charw = (d->seq[1] & 0x01)? 8 : 9;
	int width, y;

	{
		/*  Log the display state when it changes:  */
		char state[200];
		snprintf(state, sizeof(state), "%s hde=%i vde=%i maxscan=%i "
		    "start=0x%x offset=0x%x seq1=0x%02x seq4=0x%02x "
		    "gc5=0x%02x gc6=0x%02x attr10=0x%02x pas=%i misc=0x%02x",
		    !(d->attr[0x10] & 1)? "text" : (d->gc[5] & 0x40)?
		    "256-colour" : "planar", hde, vde, maxscan, start, offset,
		    d->seq[1], d->seq[4], d->gc[5], d->gc[6], d->attr[0x10],
		    (d->attr_idx >> 5) & 1, d->misc);
		if (strcmp(state, d->last_state) != 0) {
			int i;
			debugmsg(d->subsys, "qvision", VERBOSITY_INFO,
			    "%s\n", state);
			for (i = 0; i < 16; i++)
				debugmsg(d->subsys, "qvision", VERBOSITY_DEBUG,
				    "  attr palette %2i = 0x%02x, DAC %2i = "
				    "%02x %02x %02x\n", i, d->attr[i], i,
				    d->dac[i][0], d->dac[i][1], d->dac[i][2]);
			debugmsg(d->subsys, "qvision", VERBOSITY_DEBUG,
			    "  attr12=0x%02x attr14=0x%02x dac_mask=0x%02x "
			    "cursor=0x%02x%02x seq2=0x%02x seq3=0x%02x\n",
			    d->attr[0x12], d->attr[0x14], d->dac_mask,
			    d->crtc[0x0e], d->crtc[0x0f], d->seq[2], d->seq[3]);
		}
		if (strcmp(state, d->last_state) != 0 ||
		    debugmsg_is_active(d->subsys, VERBOSITY_DEBUG)) {
			/*  The first text line, from planes 0 and 1:  */
			char text[81];
			int i;
			for (i = 0; i < 80; i++) {
				int ch = d->vram[((start + i) * 2 &
				    (QV_PLANE_SIZE - 1)) * 4];
				text[i] = (ch >= 0x20 && ch < 0x7f)? ch : '.';
			}
			text[80] = '\0';
			if (strcmp(text, d->last_text) != 0) {
				debugmsg(d->subsys, "qvision", VERBOSITY_DEBUG,
				    "  line 0: \"%s\" (attr 0x%02x)\n", text,
				    d->vram[((start * 2) & (QV_PLANE_SIZE - 1))
				    * 4 + 1]);
				strlcpy(d->last_text, text,
				    sizeof(d->last_text));
			}
			strlcpy(d->last_state, state, sizeof(d->last_state));
		}
	}

	if (hde < 10 || vde < 10)
		return;

	if (!(d->attr[0x10] & 0x01)) {
		/*  Text mode:  */
		width = hde * charw;
		if (width > QV_MAX_XSIZE)
			width = QV_MAX_XSIZE;
		if (vde > QV_MAX_YSIZE)
			vde = QV_MAX_YSIZE;
		qv_resize(d, width, vde);
		hde = width / charw;
	} else if (qv_packed(d)) {
		/*  QVision packed pixels: one byte per pixel, 8 pixels per
		    character clock.  */
		width = hde * 8;
		if (width > QV_MAX_XSIZE)
			width = QV_MAX_XSIZE;
		if (vde > QV_MAX_YSIZE)
			vde = QV_MAX_YSIZE;
		qv_resize(d, width, vde);
	} else if (d->gc[5] & 0x40) {
		/*  256 colours, one byte per pixel:  */
		width = hde * 8;
		if (d->attr[0x10] & 0x40)
			width /= 2;
		if (width > QV_MAX_XSIZE)
			width = QV_MAX_XSIZE;
		if (vde > QV_MAX_YSIZE)
			vde = QV_MAX_YSIZE;
		qv_resize(d, width, vde);
	} else {
		/*  16 colours, planar:  */
		width = hde * 8;
		if (width > QV_MAX_XSIZE)
			width = QV_MAX_XSIZE;
		if (vde > QV_MAX_YSIZE)
			vde = QV_MAX_YSIZE;
		qv_resize(d, width, vde);
	}

	/*  Screen off (sequencer) or palette not enabled (attribute
	    controller): the screen is black.  */
	if ((d->seq[1] & 0x20) || !(d->attr_idx & 0x20)) {
		memset(d->line, 0, d->fb_xsize * 3);
		for (y = 0; y < d->fb_ysize; y++)
			qv_put_line(d, y);
		return;
	}

	if (!(d->attr[0x10] & 0x01))
		qv_render_text(d, hde, vde, charw, maxscan * dblscan, start,
		    offset * 2);
	else if (qv_packed(d) || (d->gc[5] & 0x40))
		qv_render_256(d, width, vde, maxscan * dblscan, start * 4,
		    offset * 8);
	else
		qv_render_planar(d, width, vde, maxscan * dblscan, start,
		    offset * 2);
}


DEVICE_TICK(qvision)
{
	struct qv_data *d = (struct qv_data *) extra;
	int text = !(d->attr[0x10] & 0x01);

	d->blink_counter ++;

	/*  Text modes are redrawn now and then, for the cursor and blink:  */
	if (d->dirty || (text && (d->blink_counter & 7) == 0)) {
		d->dirty = 0;
		qv_render(d);
	}
}


/*****************************************************************************
 *
 *  Registers
 */

static int qv_crtc_port_ok(struct qv_data *d, int port)
{
	int mono = !(d->misc & 0x01);
	return mono? (port >= 0x3b0 && port < 0x3c0) :
	    (port >= 0x3d0 && port < 0x3e0);
}


static void qv_write_port(struct qv_data *d, int port, uint8_t v)
{
	d->dirty = 1;

	switch (port) {

	case 0x3c0:
		if (!d->attr_flipflop)
			d->attr_idx = v & 0x3f;
		else if ((d->attr_idx & 0x1f) < 0x15)
			d->attr[d->attr_idx & 0x1f] = v;
		d->attr_flipflop ^= 1;
		return;
	case 0x3c1:
		/*  Some VGAs also take attribute data at 3C1h:  */
		if ((d->attr_idx & 0x1f) < 0x15)
			d->attr[d->attr_idx & 0x1f] = v;
		return;
	case 0x3c2:
		d->misc = v;
		return;
	case 0x3c3:
		d->vga_enable = v;
		return;
	case 0x3c4:
		d->seq_idx = v;
		return;
	case 0x3c5:
		if (d->seq_idx < 8)
			d->seq[d->seq_idx] = v;
		else
			debugmsg(d->subsys, "qvision", VERBOSITY_DEBUG,
			    "write to sequencer register 0x%02x: 0x%02x\n",
			    d->seq_idx, v);
		return;
	case 0x3c6:
		d->dac_mask = v;
		return;
	case 0x3c7:
		d->dac_read_idx = v;
		d->dac_read_sub = 0;
		d->dac_state = 3;
		return;
	case 0x3c8:
		d->dac_write_idx = v;
		d->dac_write_sub = 0;
		d->dac_state = 0;
		/*  Also the cursor RAM address (bits 9..8 in command
		    register 3):  */
		d->cursor_addr = v | ((d->dac_ext[0][0] & 3) << 8);
		return;
	case 0x3c9:
		d->dac_tmp[d->dac_write_sub++] = v;
		if (d->dac_write_sub == 3) {
			memcpy(d->dac[d->dac_write_idx], d->dac_tmp, 3);
			d->dac_write_idx ++;
			d->dac_write_sub = 0;
		}
		return;
	case 0x3ce:
		d->gc_idx = v;
		return;
	case 0x3cf:
		if (d->gc_idx >= 0x10 && d->gc_idx != 0x0f &&
		    !qv_extended_unlocked(d)) {
			debugmsg(d->subsys, "qvision", VERBOSITY_DEBUG,
			    "write to locked GC register 0x%02x: 0x%02x\n",
			    d->gc_idx, v);
			return;
		}
		if (d->gc_idx == 0x0c)
			return;		/*  chip ID, read-only  */
		d->gc[d->gc_idx] = v;
		if (d->gc_idx > 0x0f)
			debugmsg(d->subsys, "qvision", VERBOSITY_DEBUG,
			    "GC 0x%02x = 0x%02x\n", d->gc_idx, v);
		return;
	case 0x3b4:
	case 0x3d4:
		if (qv_crtc_port_ok(d, port))
			d->crtc_idx = v;
		return;
	case 0x3b5:
	case 0x3d5:
		if (!qv_crtc_port_ok(d, port))
			return;
		/*  CR11 bit 7 protects CR00..CR07 (except CR07 bit 4):  */
		if ((d->crtc[0x11] & 0x80) && d->crtc_idx < 8) {
			if (d->crtc_idx == 7)
				d->crtc[7] = (d->crtc[7] & ~0x10) | (v & 0x10);
			return;
		}
		d->crtc[d->crtc_idx] = v;
		return;
	case 0x3ba:
	case 0x3da:
		if (qv_crtc_port_ok(d, port))
			d->feature = v;
		return;
	}

	debugmsg(d->subsys, "qvision", VERBOSITY_DEBUG,
	    "write to unimplemented port 0x%03x: 0x%02x\n", port, v);
}


static uint8_t qv_read_port(struct qv_data *d, int port)
{
	uint8_t v;

	switch (port) {

	case 0x3c0:
		return d->attr_idx;
	case 0x3c1:
		return d->attr[d->attr_idx & 0x1f];
	case 0x3c2:
		/*  Input status 0: bit 4 is the monitor sense.  */
		return QV_SENSE;
	case 0x3c3:
		return d->vga_enable;
	case 0x3c4:
		return d->seq_idx;
	case 0x3c5:
		return d->seq_idx < 8? d->seq[d->seq_idx] : 0xff;
	case 0x3c6:
		return d->dac_mask;
	case 0x3c7:
		return d->dac_state;
	case 0x3c8:
		return d->dac_write_idx;
	case 0x3c9:
		v = d->dac[d->dac_read_idx][d->dac_read_sub++];
		if (d->dac_read_sub == 3) {
			d->dac_read_idx ++;
			d->dac_read_sub = 0;
		}
		return v;
	case 0x3ca:
		return d->feature;
	case 0x3cc:
		return d->misc;
	case 0x3ce:
		return d->gc_idx;
	case 0x3cf:
		if (d->gc_idx == 0x0c)
			return QV_CHIP_ID;
		if (d->gc_idx >= 0x10 && !qv_extended_unlocked(d))
			return 0xff;
		return d->gc[d->gc_idx];
	case 0x3b4:
	case 0x3d4:
		return qv_crtc_port_ok(d, port)? d->crtc_idx : 0xff;
	case 0x3b5:
	case 0x3d5:
		return qv_crtc_port_ok(d, port)? d->crtc[d->crtc_idx] : 0xff;
	case 0x3ba:
	case 0x3da:
		if (!qv_crtc_port_ok(d, port))
			return 0xff;
		/*  Input status 1: reading it resets the attribute flip-flop.
		    Display enable (bit 0) toggles, and vertical retrace (bit
		    3) is on for one read in sixteen.  */
		d->attr_flipflop = 0;
		d->is1_reads ++;
		v = (d->is1_reads & 1)? 0x01 : 0x00;
		if ((d->is1_reads & 15) == 0)
			v |= 0x09;
		return v;
	}

	debugmsg(d->subsys, "qvision", VERBOSITY_DEBUG,
	    "read from unimplemented port 0x%03x\n", port);
	return 0xff;
}


/*  Standard VGA registers, ports 3B0h..3BBh and 3C0h..3DFh (3BCh..3BFh is
    the parallel port):  */
/*  46E8h bit 3 enables the card's VGA addresses (the BIOS turns it off while
    it checks for other video adapters):  */
static int qv_vga_enabled(struct qv_data *d)
{
	return d->vga_enable_46e8 & 0x08;
}


static void qv_vga_ports(struct qv_data *d, int port, unsigned char *data,
	size_t len, int writeflag)
{
	size_t i;

	if (!qv_vga_enabled(d)) {
		if (writeflag == MEM_READ)
			memset(data, 0xff, len);
		return;
	}

	for (i = 0; i < len; i++) {
		if (writeflag == MEM_WRITE)
			qv_write_port(d, port + i, data[i]);
		else
			data[i] = qv_read_port(d, port + i);
	}
}

DEVICE_ACCESS(qvision_vga_mono)
{
	qv_vga_ports((struct qv_data *) extra, 0x3b0 + relative_addr, data,
	    len, writeflag);
	return 1;
}

DEVICE_ACCESS(qvision_vga)
{
	qv_vga_ports((struct qv_data *) extra, 0x3c0 + relative_addr, data,
	    len, writeflag);
	return 1;
}


/*  The A0000..BFFFF video memory window:  */
DEVICE_ACCESS(qvision_mem)
{
	struct qv_data *d = (struct qv_data *) extra;
	size_t i;

	if (!qv_vga_enabled(d)) {
		if (writeflag == MEM_READ)
			memset(data, 0xff, len);
		return 1;
	}

	for (i = 0; i < len; i++) {
		if (writeflag == MEM_WRITE)
			qv_mem_write(d, relative_addr + i, data[i]);
		else
			data[i] = qv_mem_read(d, relative_addr + i);
	}
	return 1;
}


/*
 *  The linear framebuffer ("high address video buffer"): 1 MB of video
 *  memory at the EISA address in GC 48h (address bits 20..27) and GC 49h
 *  (bits 28..31), as QV.SYS programs it. Video memory is laid out so that
 *  a linear address is the byte offset (see the top of this file).
 */
#define	QV_LFB_START		0x100000
#define	QV_LFB_END		0x10000000

static void qv_blt_host_byte(struct qv_data *d, uint8_t b);

static uint32_t qv_lfb_base(struct qv_data *d)
{
	return (d->gc[0x48] << 20) | ((d->gc[0x49] & 0x0f) << 28);
}

DEVICE_ACCESS(qvision_lfb)
{
	struct qv_data *d = (struct qv_data *) extra;
	uint32_t a = QV_LFB_START + relative_addr, base = qv_lfb_base(d);
	size_t i;

	if (base < QV_LFB_START || a < base || a + len > base + QV_VRAM_SIZE) {
		if (writeflag == MEM_READ)
			memset(data, 0xff, len);
		return 1;
	}

	a -= base;
	if (writeflag == MEM_WRITE && d->blt_host) {
		/*  Source data for the BitBLT engine:  */
		for (i = 0; i < len && d->blt_host; i++)
			qv_blt_host_byte(d, data[i]);
		return 1;
	}
	if (writeflag == MEM_WRITE) {
		for (i = 0; i < len; i++)
			d->vram[a + i] = data[i];
		d->dirty = 1;
	} else
		for (i = 0; i < len; i++)
			data[i] = d->vram[a + i];
	return 1;
}


/*  The option ROM window at C0000: 32 KB of the 64 KB image, banked by
    zC85 bit 4 (1 = "Extended ROM").  */
DEVICE_ACCESS(qvision_rom)
{
	struct qv_data *d = (struct qv_data *) extra;
	uint32_t bank = (d->board_ctrl2 & 0x10)? QV_ROM_WINDOW : 0;
	size_t i;

	if (writeflag == MEM_WRITE)
		return 1;

	for (i = 0; i < len; i++) {
		uint32_t a = (bank + relative_addr + i) & (QV_ROM_SIZE - 1);
		data[i] = d->have_rom? d->rom[a] : 0xff;
	}
	return 1;
}


/*  EISA slot registers zC80..zC85:  */
DEVICE_ACCESS(qvision_slot)
{
	struct qv_data *d = (struct qv_data *) extra;
	size_t i;

	for (i = 0; i < len; i++) {
		uint64_t r = relative_addr + i;
		if (writeflag == MEM_WRITE) {
			if (r == 4)
				d->board_ctrl = data[i];
			else if (r == 5)
				d->board_ctrl2 = data[i];
			debugmsg(d->subsys, "qvision", VERBOSITY_DEBUG,
			    "slot register %i = 0x%02x\n", (int) r, data[i]);
		} else {
			data[i] = r < 4? qv_eisa_id[r] : r == 4?
			    d->board_ctrl : d->board_ctrl2;
		}
	}
	return 1;
}


/*  A single byte register:  */
static void qv_byte_reg(uint8_t *reg, unsigned char *data, size_t len,
	int writeflag)
{
	if (len < 1)
		return;
	if (writeflag == MEM_WRITE)
		*reg = data[0];
	else
		data[0] = *reg;
}

/*  83C4h, the controller select:  */
DEVICE_ACCESS(qvision_select)
{
	struct qv_data *d = (struct qv_data *) extra;
	qv_byte_reg(&d->ctrl_select, data, len, writeflag);
	return 1;
}

/*  46E8h, the VGA enable register of adapter cards:  */
DEVICE_ACCESS(qvision_46e8)
{
	struct qv_data *d = (struct qv_data *) extra;
	qv_byte_reg(&d->vga_enable_46e8, data, len, writeflag);
	return 1;
}

/*  102h, the VGA setup (POS) register:  */
DEVICE_ACCESS(qvision_102)
{
	struct qv_data *d = (struct qv_data *) extra;
	qv_byte_reg(&d->pos102, data, len, writeflag);
	return 1;
}


/*  The DAC's extra registers: 13C6h, 83C6h, 93C6h (4 bytes each).  */
static void qv_dac_ext(struct qv_data *d, int group, uint64_t r,
	unsigned char *data, size_t len, int writeflag)
{
	if (len < 1 || r > 3)
		return;
	if (writeflag == MEM_WRITE) {
		d->dac_ext[group][r] = data[0];
		d->dirty = 1;
		if (group == 0 && r == 1) {
			/*  Cursor RAM data:  */
			d->cursor_ram[d->cursor_addr & 0x3ff] = data[0];
			d->cursor_addr ++;
			return;
		}
		if (group == 1 && r == 2) {
			/*  Cursor colour write address:  */
			d->cursor_color_idx = data[0] & 3;
			d->cursor_color_sub = 0;
		}
		if (group == 1 && r == 3) {
			/*  Cursor colour data, red, green, blue:  */
			d->cursor_color[d->cursor_color_idx]
			    [d->cursor_color_sub] = data[0];
			if (++ d->cursor_color_sub == 3) {
				d->cursor_color_sub = 0;
				d->cursor_color_idx =
				    (d->cursor_color_idx + 1) & 3;
			}
		}
		debugmsg(d->subsys, "qvision", VERBOSITY_DEBUG,
		    "DAC extension %i.%i = 0x%02x\n", group, (int) r, data[0]);
	} else
		data[0] = d->dac_ext[group][r];
}

DEVICE_ACCESS(qvision_dac0)
{
	qv_dac_ext((struct qv_data *) extra, 0, relative_addr, data, len,
	    writeflag);
	return 1;
}

DEVICE_ACCESS(qvision_dac1)
{
	qv_dac_ext((struct qv_data *) extra, 1, relative_addr, data, len,
	    writeflag);
	return 1;
}

DEVICE_ACCESS(qvision_dac2)
{
	qv_dac_ext((struct qv_data *) extra, 2, relative_addr, data, len,
	    writeflag);
	return 1;
}


/*
 *  The BitBLT engine, as Windows NT's QV.DLL uses it (worked out from what
 *  it writes; there is no documentation):
 *
 *	23C2h, 23C4h	width and height, in pixels
 *	23CAh bit 0	linear addressing: the source (63C0h) and destination
 *			(63CCh) are 32-bit bit addresses; otherwise they are
 *			X (63C0h, 63CCh) and Y (63C2h, 63CEh) pixel coordinates
 *	GC 5Ah		bits 0..1, the source: 1 = video memory, 2 = the
 *			foreground colour (a fill), 0 = the CPU (see below);
 *			bit 4: transparent colour expansion (0 bits leave the
 *			destination); bits 6..7: combine with the destination
 *			by the ROP in 33C7h
 *	63CAh bit 4	the source has 1 bit per pixel, expanded to GC 43h
 *			(1 bits) and GC 44h (0 bits); otherwise 8 bits
 *	33C7h		ROP2, a truth table: bit ((S << 1) | D) is the result
 *	33CAh..33CDh	an 8x8 1-bit pattern for fills (1 bits are the
 *			foreground colour, 0 bits the background), written
 *			twice: lines 0..3, then lines 4..7
 *	33CEh		write 1 to start; bit 0 reads as busy (never); bit 6:
 *			backwards (screen-to-screen copies): the source and
 *			destination are the bottom right pixels, and the
 *			engine goes right to left, bottom to top. QV.DLL sets
 *			it to copy downwards
 *
 *  Lines of 8-bit pixels are 1024 pixels apart. The lines of a 1-bit
 *  source in video memory are padded to 32 bits (most significant bit
 *  first). From the CPU, the source comes as writes to the start of the
 *  linear framebuffer, line by line, in whole bytes (1-bit) or 32-bit
 *  words (8-bit) from the one that holds the source X (the pixels before
 *  it are skipped).
 */
#define	QV_BLT_PITCH		1024

static uint32_t qv_le16(const uint8_t *p)
{
	return p[0] | (p[1] << 8);
}

static uint32_t qv_le32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void qv_blt_put(struct qv_data *d, uint32_t daddr, uint8_t src)
{
	uint8_t dst, v = src;
	int bit;

	daddr &= QV_VRAM_SIZE - 1;
	dst = d->vram[daddr];
	if (d->blt_use_rop) {
		v = 0;
		for (bit = 0; bit < 8; bit++) {
			int sb = (src >> bit) & 1, db = (dst >> bit) & 1;
			v |= ((d->blt_rop >> ((sb << 1) | db)) & 1) << bit;
		}
	}
	d->vram[daddr] = v;
}

/*  One pixel of the source: a 1-bit pixel is expanded (or skipped if
    transparent), an 8-bit one is used as it is.  */
static void qv_blt_pixel(struct qv_data *d, uint32_t daddr, int pix)
{
	if (d->blt_mono) {
		if (!pix && d->blt_transparent)
			return;
		pix = pix? d->gc[0x43] : d->gc[0x44];
	}
	qv_blt_put(d, daddr, pix);
}

/*  A line of source data from the CPU is complete:  */
static void qv_blt_host_line(struct qv_data *d)
{
	uint32_t daddr = d->blt_dst + d->blt_y * QV_BLT_PITCH;
	int x;

	for (x = 0; x < d->blt_w; x++) {
		int sx = x + d->blt_skip;
		int pix = d->blt_mono? (d->blt_line[sx >> 3] >> (7 - (sx & 7)))
		    & 1 : d->blt_line[sx];
		qv_blt_pixel(d, daddr + x, pix);
	}

	d->blt_line_pos = 0;
	if (++ d->blt_y >= d->blt_h)
		d->blt_host = 0;
	d->dirty = 1;
}

/*  A byte of source data written by the CPU:  */
static void qv_blt_host_byte(struct qv_data *d, uint8_t b)
{
	d->blt_line[d->blt_line_pos ++] = b;
	if (d->blt_line_pos >= d->blt_line_len)
		qv_blt_host_line(d);
}

/*  The 16 bytes of a BitBLT register group, as hex, for debug messages.  */
static const char *qv_hex16(const uint8_t *r, int which)
{
	static char buf[3][33];
	int i;

	for (i = 0; i < 16; i++)
		snprintf(buf[which] + i * 2, 3, "%02x", r[i]);
	return buf[which];
}

static void qv_blt_start(struct qv_data *d)
{
	uint8_t *b0 = d->blt[0], *b1 = d->blt[1], *b2 = d->blt[2];
	int w = qv_le16(b0 + 2) & 0xfff, h = qv_le16(b0 + 4) & 0xfff;
	int source = d->gc[0x5a] & 3, linear = b0[0x0a] & 1, x, y;
	int backwards = (b1[0x0e] & 0x40) != 0;
	uint32_t src, dst;	/*  bits if linear, else pixels  */

	d->blt_host = 0;
	d->blt_mono = (b2[0x0a] & 0x10) != 0;
	d->blt_transparent = (d->gc[0x5a] & 0x10) != 0;
	d->blt_use_rop = (d->gc[0x5a] & 0xc0) != 0;
	d->blt_rop = b1[7] & 0x0f;

	if (linear) {
		src = qv_le32(b2 + 0);
		dst = qv_le32(b2 + 0x0c) / 8;
	} else {
		src = qv_le16(b2 + 2) * QV_BLT_PITCH + qv_le16(b2 + 0);
		dst = qv_le16(b2 + 0x0e) * QV_BLT_PITCH + qv_le16(b2 + 0x0c);
		if (d->blt_mono)
			src *= 8;
	}
	if (linear && !d->blt_mono)
		src /= 8;

	debugmsg(d->subsys, "qvision", VERBOSITY_DEBUG,
	    "BitBLT 0x%02x %ix%i src %s 0x%x%s dst 0x%x%s fg 0x%02x bg 0x%02x "
	    "rop 0x%x pattern %02x%02x%02x%02x%02x%02x%02x%02x\n", d->gc[0x5a],
	    w, h, source == 0? "cpu" : source == 2? "pattern" : "vram", src,
	    d->blt_mono? " (1-bit)" : "", dst, backwards? " backwards" : "",
	    d->gc[0x43], d->gc[0x44],
	    d->blt_rop, d->pattern[0], d->pattern[1], d->pattern[2],
	    d->pattern[3], d->pattern[4], d->pattern[5], d->pattern[6],
	    d->pattern[7]);
	debugmsg(d->subsys, "qvision", VERBOSITY_DEBUG,
	    "BitBLT registers: 23C0h %s 33C0h %s 63C0h %s GC 5Ah..5Fh "
	    "%02x %02x %02x %02x %02x %02x\n", qv_hex16(b0, 0),
	    qv_hex16(b1, 1), qv_hex16(b2, 2), d->gc[0x5a], d->gc[0x5b],
	    d->gc[0x5c], d->gc[0x5d], d->gc[0x5e], d->gc[0x5f]);

	if (w == 0 || h == 0 || w > QV_BLT_PITCH)
		return;

	switch (source) {
	case 0:	/*  From the CPU, through the framebuffer:  */
		d->blt_host = 1;
		d->blt_w = w;
		d->blt_h = h;
		d->blt_y = 0;
		d->blt_dst = dst;
		d->blt_line_pos = 0;
		/*  The CPU sends whole bytes (1-bit) or 32-bit words (8-bit)
		    of its bitmap, from the one that holds the source X;
		    the pixels before X are skipped:  */
		d->blt_skip = d->blt_mono? (src & 7) : (src & 3);
		d->blt_line_len = d->blt_mono? (d->blt_skip + w + 7) / 8 :
		    (d->blt_skip + w + 3) & ~3;
		return;
	case 1:	/*  From video memory:  */
		if (d->blt_mono) {
			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++) {
					uint32_t bit = src + y * ((w + 31) & ~31)
					    + x;
					qv_blt_pixel(d, dst + y * QV_BLT_PITCH + x,
					    (d->vram[(bit >> 3) & (QV_VRAM_SIZE
					    - 1)] >> (7 - (bit & 7))) & 1);
				}
		} else {
			/*  Backwards, the source and destination are the
			    last pixels (bottom right), and the engine goes
			    right to left, bottom to top; the driver chooses
			    the direction for overlapping copies.  */
			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++) {
					uint32_t o = y * QV_BLT_PITCH + x;
					if (backwards)
						o = -o;
					qv_blt_put(d, dst + o, d->vram[(src + o)
					    & (QV_VRAM_SIZE - 1)]);
				}
		}
		break;
	case 2:	/*  A fill with the pattern (aligned to the screen):  */
		for (y = 0; y < h; y++) {
			uint32_t a = dst + y * QV_BLT_PITCH;
			uint8_t pat = d->pattern[(a / QV_BLT_PITCH) & 7];
			for (x = 0; x < w; x++, a++)
				qv_blt_put(d, a, (pat >> (7 - (a & 7))) & 1?
				    d->gc[0x43] : d->gc[0x44]);
		}
		break;
	default:
		debugmsg(d->subsys, "qvision", VERBOSITY_WARNING,
		    "unimplemented BitBLT operation 0x%02x\n", d->gc[0x5a]);
		return;
	}

	d->dirty = 1;
}


/*  BitBLT registers: 23C0h, 33C0h, 63C0h (16 bytes each).  */
static void qv_blt(struct qv_data *d, int group, uint64_t r,
	unsigned char *data, size_t len, int writeflag)
{
	size_t i;

	for (i = 0; i < len && r + i < 16; i++) {
		if (writeflag == MEM_WRITE) {
			d->blt[group][r + i] = data[i];
			if (group == 1 && r + i >= 0x0a && r + i <= 0x0d) {
				d->pattern[d->pattern_half * 4 + r + i - 0x0a] =
				    data[i];
				if (r + i == 0x0d)
					d->pattern_half ^= 1;
			}
			if (group == 1 && r + i == 0x0e && (data[i] & 1))
				qv_blt_start(d);
		} else {
			data[i] = d->blt[group][r + i];
			/*  63CAh: the busy bits are never set.  */
			if (group == 2 && r + i == 0x0a)
				data[i] &= ~0xc0;
			/*  33CEh bit 0: busy (the driver waits for it to
			    clear before it starts an operation).  */
			if (group == 1 && r + i == 0x0e)
				data[i] &= ~0x01;
		}
	}
}

DEVICE_ACCESS(qvision_blt0)
{
	qv_blt((struct qv_data *) extra, 0, relative_addr, data, len,
	    writeflag);
	return 1;
}

DEVICE_ACCESS(qvision_blt1)
{
	qv_blt((struct qv_data *) extra, 1, relative_addr, data, len,
	    writeflag);
	return 1;
}

DEVICE_ACCESS(qvision_blt2)
{
	qv_blt((struct qv_data *) extra, 2, relative_addr, data, len,
	    writeflag);
	return 1;
}


static void qv_reset(struct qv_data *d)
{
	/*  An 80x25 colour text mode, as the card comes out of its BIOS
	    POST; the firmware sets its own mode anyway.  */
	static const uint8_t crtc[25] = {
	    0x5f, 0x4f, 0x50, 0x82, 0x55, 0x81, 0xbf, 0x1f, 0x00, 0x4f,
	    0x0d, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x9c, 0x8e, 0x8f, 0x28,
	    0x1f, 0x96, 0xb9, 0xa3, 0xff };
	static const uint8_t seq[5] = { 0x03, 0x00, 0x03, 0x00, 0x02 };
	static const uint8_t gc[9] = { 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x10, 0x0e, 0x00, 0xff };
	static const uint8_t attr[21] = { 0x00, 0x01, 0x02, 0x03, 0x04,
	    0x05, 0x14, 0x07, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
	    0x0c, 0x00, 0x0f, 0x08, 0x00 };
	int i;

	d->misc = 0x67;
	memcpy(d->crtc, crtc, sizeof(crtc));
	memcpy(d->seq, seq, sizeof(seq));
	memcpy(d->gc, gc, sizeof(gc));
	memcpy(d->attr, attr, sizeof(attr));
	d->attr_idx = 0x20;
	d->dac_mask = 0xff;

	/*  The 16 EGA colours in the DAC:  */
	for (i = 0; i < 64; i++) {
		int r = ((i >> 2) & 1) * 2 + ((i >> 5) & 1);
		int g = ((i >> 1) & 1) * 2 + ((i >> 4) & 1);
		int b = (i & 1) * 2 + ((i >> 3) & 1);
		d->dac[i][0] = r * 21;
		d->dac[i][1] = g * 21;
		d->dac[i][2] = b * 21;
	}

	d->vga_enable_46e8 = 0x0e;
	d->board_ctrl = 0x01;
	d->board_ctrl2 = 0x10;
	d->gc[0x54] = 0x04;		/*  1 MB  */
	memset(d->pattern, 0xff, sizeof(d->pattern));	/*  solid  */
	d->dirty = 1;
}


/*
 *  dev_qvision_init():
 *
 *  A QVision 1024/E in EISA slot "slot", with the standard VGA addresses on
 *  the ISA bus at isa_portbase (I/O) and isa_membase (memory).  rom_file
 *  is an image of the option ROM (64 KB, both banks; or 32 KB), or NULL.
 */
void dev_qvision_init(struct machine *machine, struct memory *mem,
	uint64_t isa_portbase, uint64_t isa_membase, int slot,
	const char *rom_file)
{
	struct qv_data *d;

	CHECK_ALLOCATION(d = (struct qv_data *) malloc(sizeof(struct qv_data)));
	memset(d, 0, sizeof(struct qv_data));
	d->machine = machine;
	d->slot = slot;
	d->subsys = debugmsg_register_subsystem("qvision");

	CHECK_ALLOCATION(d->vram = (unsigned char *) malloc(QV_VRAM_SIZE));
	memset(d->vram, 0, QV_VRAM_SIZE);
	CHECK_ALLOCATION(d->line = (unsigned char *) malloc(QV_MAX_XSIZE * 3));

	memset(d->rom, 0xff, sizeof(d->rom));
	if (rom_file != NULL && rom_file[0]) {
		FILE *f = fopen(rom_file, "rb");
		size_t n;
		if (f == NULL) {
			perror(rom_file);
			exit(1);
		}
		n = fread(d->rom, 1, sizeof(d->rom), f);
		fclose(f);
		if (n == QV_ROM_WINDOW)
			memcpy(d->rom + QV_ROM_WINDOW, d->rom, QV_ROM_WINDOW);
		else if (n != QV_ROM_SIZE) {
			fatal("%s: a QVision option ROM image should be 32 or "
			    "64 KB, not %i bytes\n", rom_file, (int) n);
			exit(1);
		}
		d->have_rom = 1;
	}

	qv_reset(d);

	memory_device_register(mem, "qvision_vga_mono", isa_portbase + 0x3b0,
	    0x0c, dev_qvision_vga_mono_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "qvision_vga", isa_portbase + 0x3c0,
	    0x20, dev_qvision_vga_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "qvision_mem", isa_membase + 0xa0000,
	    0x20000, dev_qvision_mem_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "qvision_rom", isa_membase + 0xc0000,
	    QV_ROM_WINDOW, dev_qvision_rom_access, d, DM_DEFAULT, NULL);

	memory_device_register(mem, "qvision_lfb", isa_membase + QV_LFB_START,
	    QV_LFB_END - QV_LFB_START, dev_qvision_lfb_access, d, DM_DEFAULT,
	    NULL);

	memory_device_register(mem, "qvision_slot",
	    isa_portbase + slot * 0x1000 + 0xc80, 6,
	    dev_qvision_slot_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "qvision_select", isa_portbase + 0x83c4,
	    1, dev_qvision_select_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "qvision_46e8", isa_portbase + 0x46e8,
	    1, dev_qvision_46e8_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "qvision_102", isa_portbase + 0x102,
	    1, dev_qvision_102_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "qvision_dac0", isa_portbase + 0x13c6,
	    4, dev_qvision_dac0_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "qvision_dac1", isa_portbase + 0x83c6,
	    4, dev_qvision_dac1_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "qvision_dac2", isa_portbase + 0x93c6,
	    4, dev_qvision_dac2_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "qvision_blt0", isa_portbase + 0x23c0,
	    16, dev_qvision_blt0_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "qvision_blt1", isa_portbase + 0x33c0,
	    16, dev_qvision_blt1_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "qvision_blt2", isa_portbase + 0x63c0,
	    16, dev_qvision_blt2_access, d, DM_DEFAULT, NULL);

	d->fb_xsize = 720;
	d->fb_ysize = 400;
	d->fb = dev_fb_init(machine, mem, QV_FB_ADDR, VFB_GENERIC,
	    d->fb_xsize, d->fb_ysize, d->fb_xsize, d->fb_ysize, 24, "QVision");

	machine_add_tickfunction(machine, dev_qvision_tick, d, QV_TICK_SHIFT);
}
