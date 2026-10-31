/*
 *  Copyright (C) 2004-2009  Anders Gavare.  All rights reserved.
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
 *  COMMENT: PMAGB-BA (HX) TURBOchannel framebuffer, with the SFB ASIC
 *
 *  The HX is a 1280x1024, 8-bit color framebuffer.  Its slot holds the
 *  option ROM (at offset 0), the registers of the SFB ("smart frame
 *  buffer") ASIC (at 0x100000), two general purpose outputs (0x140000
 *  and 0x180000), a Bt459 RAMDAC (0x1c0000), and 2 MB of VRAM (0x200000).
 *
 *  Every access to the VRAM goes through the ASIC, and its mode decides
 *  what a write does: it stores pixels (simple mode), expands a 32-bit
 *  stipple into up to 32 pixels of the foreground (and background)
 *  color, copies up to 32 pixels, or draws up to 16 pixels of a line.
 *  A raster operation and a plane mask apply in all of these modes.
 *  Reads return what is in the VRAM, in any mode.
 *
 *  The screen shows the VRAM from the video base address on, as large as
 *  the video timing registers say when the video is started (1280x1024,
 *  as the option ROM sets them up), and the ASIC interrupts at every
 *  vertical retrace while its interrupt is enabled.
 *
 *  See chapter 6 of the "DEC 3000 300/400/500/600/700/800/900 AXP Models
 *  System Programmer's Manual" (EK-D3SYS-PM), where the same ASIC is the
 *  CXTurbo, and NetBSD's sys/dev/tc/sfb.c, which shows how stipples and
 *  copies are aligned, and their bit order.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "devices.h"
#include "interrupt.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "thirdparty/sfbreg.h"


/*  #define SFB_DEBUG  */

#define	SFB_TICK_SHIFT		14

/*  The video oscillator, for 1280x1024 at 72 Hz:  */
#define	SFB_PIXEL_CLOCK		130808000
#define	SFB_RETRACE_HZ		72

#define	SFB_XSIZE		1280
#define	SFB_YSIZE		1024

#define	SFB_REGS_LENGTH		0x20000		/*  repeated every 0x80  */
#define	SFB_GP_OFFSET		0x140000	/*  GP0, and GP1 at 0x180000  */
#define	SFB_GP_LENGTH		0x80000
#define	SFB_VRAM_OFFSET		0x200000
#define	SFB_VRAM_SIZE		0x200000

/*  Register numbers, from the offsets in sfbreg.h:  */
#define	R(offset)		(((offset) - SFB_ASIC_OFFSET) / 4)
#define	N_SFB_REGS		32

#define	MODE_SIMPLE		0
#define	MODE_OPAQUESTIPPLE	1
#define	MODE_OPAQUELINE		2
#define	MODE_TRANSPARENTSTIPPLE	5
#define	MODE_TRANSPARENTLINE	6
#define	MODE_COPY		7

#define	ROP_COPY		3

struct sfb_data {
	struct vfb_data	*vfb_data;
	struct memory	*fb_mem;	/*  holds only the window's framebuffer  */
	unsigned char	*vram;
	uint32_t	screen_base;	/*  VRAM offset of the top left pixel  */

	uint32_t	reg[N_SFB_REGS];
	uint32_t	gp[2];

	/*  Where BCONT continues: after the last operation, or at the
	    address that was last written to the address register.  */
	uint32_t	next_address;

	/*  Copy mode:  */
	unsigned char	copybuf[32];
	unsigned char	residue[8];	/*  the quadword last shifted  */
	int		copy_to_vram;	/*  the next copy write is a destination  */

	/*  The VRAM offsets of the first and last pixels that changed:  */
	uint32_t	changed_low, changed_high;

	unsigned int	vidclk_count;	/*  video clocks in 256 TC clocks  */

	struct interrupt irq;
	int		irq_asserted;
	int		retrace_ticks, retrace_ticks_left;
};


/*
 *  sfb_reg_bits():
 *
 *  Returns the bits that a register holds; the copy buffer, foreground,
 *  background, plane mask, pixel mask, address, and Bresenham registers
 *  hold all 32 bits.
 */
static uint32_t sfb_reg_bits(int r)
{
	switch (r) {
	case R(SFB_MODE):		return 0x7;
	case R(SFB_BOOL_OP):		return 0xf;
	case R(SFB_PIXELSHIFT):		return 0xf;
	case R(SFB_DEEP):		return 0xf;
	case R(SFB_VREFRESH_COUNT):	return 0x3ff;
	case R(SFB_VHORIZONTAL):	return 0x0fffffff;
	case R(SFB_VVERTICAL):		return 0x0fffffff;
	case R(SFB_VBASE):		return 0x1ff;
	case R(SFB_INTERRUPT_ENABLE):	return 0x1;
	case R(SFB_TCCLK):		return 0xff;
	case R(SFB_VIDCLK):		return 0x3ff;
	}

	return 0xffffffff;
}


/*
 *  sfb_redraw():
 *
 *  Marks the part of the screen that shows the VRAM from offset low to
 *  offset high for redrawing.
 */
static void sfb_redraw(struct sfb_data *d, uint32_t low, uint32_t high)
{
	struct vfb_data *fb = d->vfb_data;
	uint32_t size = fb->bytes_per_line * fb->ysize;
	int x1, y1, x2, y2;

	if (low > high || high < d->screen_base ||
	    low >= d->screen_base + size)
		return;

	low = low < d->screen_base? 0 : low - d->screen_base;
	high -= d->screen_base;
	if (high >= size)
		high = size - 1;

	x1 = low % fb->bytes_per_line;   y1 = low / fb->bytes_per_line;
	x2 = high % fb->bytes_per_line;  y2 = high / fb->bytes_per_line;
	if (y1 != y2) {
		x1 = 0;
		x2 = fb->xsize - 1;
	}

	if (x1 < fb->update_x1)  fb->update_x1 = x1;
	if (x2 > fb->update_x2)  fb->update_x2 = x2;
	if (y1 < fb->update_y1)  fb->update_y1 = y1;
	if (y2 > fb->update_y2)  fb->update_y2 = y2;
}


/*
 *  sfb_update_screen():
 *
 *  Shows the VRAM from the video base address on, in the size that the
 *  video timing registers give (if resize is set) or else the current
 *  size. Sizes and base addresses that don't fit in the VRAM are ignored
 *  (e.g. while the option ROM's diagnostics test the registers).
 */
static void sfb_update_screen(struct sfb_data *d, int resize)
{
	struct vfb_data *fb = d->vfb_data;
	int xsize = (d->reg[R(SFB_VHORIZONTAL)] & 0x1ff) * 4;
	int ysize = d->reg[R(SFB_VVERTICAL)] & 0x7ff;
	uint32_t base = (d->reg[R(SFB_VBASE)] & 0x1ff) * 4096;

	if (!resize || xsize < 64 || ysize < 64 ||
	    (uint32_t) (xsize * ysize) > SFB_VRAM_SIZE) {
		xsize = fb->xsize;
		ysize = fb->ysize;
	}

	if (base + xsize * ysize > SFB_VRAM_SIZE)
		base = d->screen_base;
	if (base + xsize * ysize > SFB_VRAM_SIZE)
		base = 0;

	if (xsize != fb->xsize || ysize != fb->ysize) {
		/*  dev_fb_resize() allocates a framebuffer of its own, which
		    isn't needed, and mustn't free the VRAM:  */
		fb->framebuffer = NULL;
		dev_fb_resize(fb, xsize, ysize);
		free(fb->framebuffer);
		fb->framebuffer = NULL;
	}

	if (fb->framebuffer == d->vram + base)
		return;

	d->screen_base = base;
	fb->framebuffer = d->vram + base;
	memory_device_update_data(d->fb_mem, fb, fb->framebuffer);

	fb->update_x1 = fb->update_y1 = 0;
	fb->update_x2 = fb->xsize - 1;
	fb->update_y2 = fb->ysize - 1;
}


/*
 *  sfb_put():
 *
 *  Stores a pixel in the VRAM, through the raster operation and the plane
 *  mask. The raster operations are numbered as in X11 (GXcopy = 3, etc.),
 *  and the plane mask holds one byte for each of the four pixels in a
 *  word.
 */
static void sfb_put(struct sfb_data *d, uint32_t ofs, unsigned char src)
{
	int rop = d->reg[R(SFB_BOOL_OP)];
	unsigned char dst, pixel = 0, planes;

	ofs &= SFB_VRAM_SIZE - 1;
	dst = d->vram[ofs];
	planes = d->reg[R(SFB_PLANEMASK)] >> (8 * (ofs & 3));

	if (rop & 1)	pixel |= src & dst;
	if (rop & 2)	pixel |= src & ~dst;
	if (rop & 4)	pixel |= ~src & dst;
	if (rop & 8)	pixel |= ~src & ~dst;
	pixel = (pixel & planes) | (dst & ~planes);

	if (pixel == dst)
		return;

	d->vram[ofs] = pixel;
	if (ofs < d->changed_low)
		d->changed_low = ofs;
	if (ofs > d->changed_high)
		d->changed_high = ofs;
}


/*
 *  sfb_simple():
 *
 *  Simple mode: the bytes that the CPU writes are the pixels.
 */
static void sfb_simple(struct sfb_data *d, uint32_t ofs,
	const unsigned char *data, size_t len)
{
	size_t i;

	if (d->reg[R(SFB_BOOL_OP)] == ROP_COPY &&
	    d->reg[R(SFB_PLANEMASK)] == 0xffffffff) {
		if (memcmp(d->vram + ofs, data, len) == 0)
			return;
		memcpy(d->vram + ofs, data, len);
		if (ofs < d->changed_low)
			d->changed_low = ofs;
		if (ofs + len - 1 > d->changed_high)
			d->changed_high = ofs + len - 1;
		return;
	}

	for (i = 0; i < len; i++)
		sfb_put(d, ofs + i, data[i]);
}


/*
 *  sfb_stipple():
 *
 *  The stipple modes: bit i of the data is the pixel at ofs + i (with ofs
 *  rounded down to a word).  In transparent stipple mode, 1 bits store the
 *  foreground color and 0 bits leave the pixels as they are.  In opaque
 *  stipple mode, 1 bits store the foreground color and 0 bits the
 *  background color, in the pixels whose bits are set in the pixel mask.
 *  The foreground and background registers, like the plane mask, hold
 *  one byte for each of the four pixels in a word.
 */
static void sfb_stipple(struct sfb_data *d, uint32_t ofs, uint32_t stipple,
	int opaque)
{
	uint32_t pixels = opaque? d->reg[R(SFB_PIXELMASK)] : stipple;
	uint32_t color;
	int i;

	ofs &= ~3;
	for (i = 0; i < 32; i++) {
		if (!(pixels & ((uint32_t) 1 << i)))
			continue;

		color = stipple & ((uint32_t) 1 << i)?
		    d->reg[R(SFB_FOREGROUND)] : d->reg[R(SFB_BACKGROUND)];
		sfb_put(d, ofs + i, color >> (8 * ((ofs + i) & 3)));
	}

	d->next_address = SFB_VRAM_OFFSET + ofs + 32;
}


/*
 *  sfb_line():
 *
 *  The line modes draw up to 16 pixels of a line, the first of them at
 *  ofs.  Pixel i is the foreground color if bit i of the data is set, and
 *  otherwise the background color in opaque line mode, or left as it is
 *  in transparent line mode.  Between pixels, the address moves by one of
 *  two increments, chosen by the sign of the Bresenham error:
 *
 *	BRES1: address increment (high 16 bits) and error increment (low
 *	       16 bits), used while the error is negative
 *	BRES2: address increment and error decrement, used while the error
 *	       is zero or positive
 *	BRES3: the error (bits 31..14, signed, where the increments and
 *	       decrements count from bit 15) and the number of pixels to
 *	       draw (bits 3..0, where 0 means 16)
 *
 *  Afterwards, BRES3 holds the error for the pixel after the last one,
 *  and the number of pixels is 16, for BCONT to continue the line there.
 */
static void sfb_line(struct sfb_data *d, uint32_t ofs, uint32_t pattern,
	int opaque)
{
	uint32_t bres1 = d->reg[R(SFB_BRESENHAM1)];
	uint32_t bres2 = d->reg[R(SFB_BRESENHAM2)];
	int32_t error = (int32_t) d->reg[R(SFB_BRESENHAM3)] >> 14;
	int i, n = d->reg[R(SFB_BRESENHAM3)] & 15;

	if (n == 0)
		n = 16;

	for (i = 0; i < n; i++) {
		uint32_t color = d->reg[R(SFB_FOREGROUND)];

		if (!(pattern & (1 << i))) {
			if (!opaque)
				goto next;
			color = d->reg[R(SFB_BACKGROUND)];
		}
		sfb_put(d, ofs, color >> (8 * (ofs & 3)));

next:
		if (error < 0) {
			ofs += (int16_t) (bres1 >> 16);
			error += 2 * (bres1 & 0xffff);
		} else {
			ofs += (int16_t) (bres2 >> 16);
			error -= 2 * (bres2 & 0xffff);
		}
	}

	d->reg[R(SFB_BRESENHAM3)] = (uint32_t) error << 14;
	d->next_address = SFB_VRAM_OFFSET + (ofs & (SFB_VRAM_SIZE - 1));
}


/*
 *  sfb_copy_index():
 *
 *  Copies work on 32 pixels, as four quadwords (8 pixels each) that go
 *  through the copy logic one after the other, from left to right, or
 *  (with a negative pixel shift) from right to left.  The bits of the
 *  mask, and the pixels of the copy buffer, are in that order, so this
 *  returns the bit for pixel i of the 32, counted from the left.
 */
static int sfb_copy_index(int i, int shift)
{
	return shift < 0? 24 - (i & ~7) + (i & 7) : i;
}


/*
 *  sfb_copy():
 *
 *  In copy mode, writes take turns at being a source and a destination.
 *  A source write loads 32 pixels into the copy buffer, which the CPU can
 *  also write directly: the quadword at ofs (rounded down to 8 pixels)
 *  and the three to the right of it, or (with a negative pixel shift) the
 *  three to the left of it.  A destination write then stores the buffer's
 *  pixels whose bits are set in the data, in the 32 pixels that ofs gives
 *  in the same way, through a shifter that moves them by the pixel shift:
 *  pixel i is buffer pixel i - shift.  The pixels that this moves in at
 *  one end come from the quadword that went through it last, so that a
 *  copy can go on from one 32 pixels to the next: from left to right with
 *  a positive shift, and from right to left with a negative one.
 */
static void sfb_copy(struct sfb_data *d, uint32_t ofs, uint32_t mask)
{
	int i, shift = d->reg[R(SFB_PIXELSHIFT)];
	unsigned char pixels[40];	/*  the buffer and the residue  */
	unsigned char *buf;

	if (shift & 8)
		shift -= 16;

	/*  The leftmost of the 32 pixels:  */
	ofs = (ofs & ~7) - (shift < 0? 24 : 0);

	if (!d->copy_to_vram) {
		for (i = 0; i < 32; i++)
			d->copybuf[sfb_copy_index(i, shift)] =
			    d->vram[(ofs + i) & (SFB_VRAM_SIZE - 1)];

		d->copy_to_vram = 1;
		return;
	}

	/*  The buffer's pixels from left to right, and the residue at the
	    left end of them, or (with a negative shift) at the right end:  */
	buf = shift < 0? pixels : pixels + 8;
	for (i = 0; i < 32; i++)
		buf[i] = d->copybuf[sfb_copy_index(i, shift)];
	memcpy(shift < 0? pixels + 32 : pixels, d->residue, 8);
	memcpy(d->residue, buf + (shift < 0? 0 : 24), 8);

	for (i = 0; i < 32; i++)
		if (mask & ((uint32_t) 1 << sfb_copy_index(i, shift)))
			sfb_put(d, ofs + i, buf[i - shift]);

	d->copy_to_vram = 0;
	d->next_address = SFB_VRAM_OFFSET + ofs + (shift < 0? -8 : 32);
}


/*
 *  sfb_operation():
 *
 *  Does what a write of data (len bytes) to the VRAM at offset ofs does
 *  in the current mode, and marks the pixels that changed for redrawing.
 */
static void sfb_operation(struct sfb_data *d, uint32_t ofs,
	const unsigned char *data, size_t len, uint32_t idata)
{
	int mode = d->reg[R(SFB_MODE)];

	ofs &= SFB_VRAM_SIZE - 1;
	if (ofs + len > SFB_VRAM_SIZE)
		len = SFB_VRAM_SIZE - ofs;

	d->changed_low = (uint32_t) -1;
	d->changed_high = 0;

	switch (mode) {
	case MODE_OPAQUESTIPPLE:
	case MODE_TRANSPARENTSTIPPLE:
		sfb_stipple(d, ofs, idata, mode == MODE_OPAQUESTIPPLE);
		break;
	case MODE_OPAQUELINE:
	case MODE_TRANSPARENTLINE:
		sfb_line(d, ofs, idata, mode == MODE_OPAQUELINE);
		break;
	case MODE_COPY:
		sfb_copy(d, ofs, idata);
		break;
	default:
		sfb_simple(d, ofs, data, len);
	}

	/*  The pixel mask is for one operation only:  */
	d->reg[R(SFB_PIXELMASK)] = 0xffffffff;

	sfb_redraw(d, d->changed_low, d->changed_high);
}


DEVICE_TICK(sfb)
{
	struct sfb_data *d = (struct sfb_data *) extra;

	if (-- d->retrace_ticks_left > 0)
		return;

	d->retrace_ticks_left = d->retrace_ticks;

	/*  A vertical retrace, while the video runs:  */
	if (d->reg[R(SFB_VVALID)] & 1 &&
	    d->reg[R(SFB_INTERRUPT_ENABLE)] & 1 && !d->irq_asserted) {
		INTERRUPT_ASSERT(d->irq);
		d->irq_asserted = 1;
	}
}


DEVICE_ACCESS(sfb_vram)
{
	struct sfb_data *d = (struct sfb_data *) extra;
	uint64_t idata;

	if (relative_addr + len > SFB_VRAM_SIZE)
		return 0;

	if (writeflag == MEM_READ) {
		memcpy(data, d->vram + relative_addr, len);
		return 1;
	}

	idata = memory_readmax64(cpu, data, len);

	/*  In the line modes, bits 17..16 of the data are the low bits of
	    the first pixel's address, as the address of a write is a word's
	    (START and BCONT have a pixel's address):  */
	if ((d->reg[R(SFB_MODE)] & 3) == MODE_OPAQUELINE)
		relative_addr = (relative_addr & ~3) | ((idata >> 16) & 3);

	sfb_operation(d, relative_addr, data, len, idata);
	return 1;
}


DEVICE_ACCESS(sfb)
{
	struct sfb_data *d = (struct sfb_data *) extra;
	uint64_t idata = 0, odata = 0;
	unsigned char bytes[4];
	int r = (relative_addr >> 2) & (N_SFB_REGS - 1), i;

	if (writeflag == MEM_WRITE)
		idata = memory_readmax64(cpu, data, len);

	if (r < 8) {
		/*  The copy buffer, with pixel 0 in the low byte of word 0:  */
		unsigned char *p = d->copybuf + r * 4;

		if (writeflag == MEM_WRITE) {
			for (i = 0; i < 4; i++)
				p[i] = idata >> (8 * i);

			/*  The next copy write stores the buffer:  */
			d->copy_to_vram = 1;
		} else
			odata = p[0] | (p[1] << 8) | (p[2] << 16) |
			    ((uint32_t) p[3] << 24);
	} else if (writeflag == MEM_READ) {
		odata = d->reg[r];
	} else {
		uint32_t old = d->reg[r];

		d->reg[r] = idata & sfb_reg_bits(r);

		switch (r) {

		case R(SFB_PIXELSHIFT):
			/*  The next copy write is a source again:  */
			d->copy_to_vram = 0;
			break;

		case R(SFB_ADDRESS):
			d->next_address = idata;
			break;

		case R(SFB_START):
		case R(SFB_BCONT):
			/*  An operation at the address register's address,
			    or (BCONT) where the last one ended:  */
			for (i = 0; i < 4; i++)
				bytes[i] = idata >> (8 * i);
			sfb_operation(d, (r == R(SFB_START)?
			    d->reg[R(SFB_ADDRESS)] : d->next_address) -
			    SFB_VRAM_OFFSET, bytes, 4, idata);
			break;

		case R(SFB_CLEAR):
			if (d->irq_asserted) {
				INTERRUPT_DEASSERT(d->irq);
				d->irq_asserted = 0;
			}
			break;

		case R(SFB_INTERRUPT_ENABLE):
			if (!(idata & 1) && d->irq_asserted) {
				INTERRUPT_DEASSERT(d->irq);
				d->irq_asserted = 0;
			}

			/*
			 *  The next retrace may come at any time, and the
			 *  option ROM's int test waits for it with the PROM's
			 *  msdelay(), which counts on the slow uncached
			 *  execution of the real machine and is much faster
			 *  here.  So it comes at the next tick:
			 */
			if (idata & 1 && !(old & 1))
				d->retrace_ticks_left = 1;
			break;

		case R(SFB_VBASE):
			sfb_update_screen(d, 0);
			break;

		case R(SFB_VVALID):
			/*  The video starts with the new timing:  */
			if (idata & 1)
				sfb_update_screen(d, 1);
			break;

		case R(SFB_TCCLK):
			/*  Counting 256 TURBOchannel clocks, and the video
			    clocks meanwhile, is done at once:  */
			d->reg[R(SFB_TCCLK)] = 0xff;
			d->reg[R(SFB_VIDCLK)] = d->vidclk_count;
			break;
		}
	}

#ifdef SFB_DEBUG
	if (writeflag == MEM_WRITE)
		fatal("[ sfb: write to register 0x%02x: 0x%08x ]\n",
		    (int) (r * 4), (int) idata);
	else
		fatal("[ sfb: read from register 0x%02x: 0x%08x ]\n",
		    (int) (r * 4), (int) odata);
#endif

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, odata);

	return 1;
}


/*
 *  The general purpose outputs: GP0 selects one of two video oscillators
 *  (there is one, here). What GP1 drives is not known: NetBSD calls it
 *  the Bt459 reset, but the Bt459 has no reset input, and its registers
 *  have no reset values. The HX ROM writes GP1 0 and then 1 before it
 *  programs the Bt459, and Ultrix doesn't use it, so it is only stored.
 */
DEVICE_ACCESS(sfb_gp)
{
	struct sfb_data *d = (struct sfb_data *) extra;
	int gp = relative_addr >= SFB_GP_LENGTH / 2;

	if (writeflag == MEM_WRITE)
		d->gp[gp] = memory_readmax64(cpu, data, len) & 1;
	else
		memory_writemax64(cpu, data, len, d->gp[gp]);

	return 1;
}


/*
 *  dev_sfb_init():
 *
 *  A PMAGB-BA in the TURBOchannel slot at baseaddr. (The option ROM,
 *  at offset 0, is the TURBOchannel code's.)
 */
void dev_sfb_init(struct machine *machine, struct memory *mem,
	uint64_t baseaddr, const char *irq_path)
{
	struct sfb_data *d;
	uint64_t tc_hz;

	CHECK_ALLOCATION(d = (struct sfb_data *) malloc(sizeof(struct sfb_data)));
	memset(d, 0, sizeof(struct sfb_data));

	CHECK_ALLOCATION(d->vram = (unsigned char *) malloc(SFB_VRAM_SIZE));
	memset(d->vram, 0, SFB_VRAM_SIZE);

	/*
	 *  The window's framebuffer is in memory of its own, as the CPU
	 *  accesses the VRAM through the ASIC, and it is then pointed at
	 *  the part of the VRAM that is on the screen.
	 */
	d->fb_mem = memory_new(SFB_VRAM_SIZE);
	d->vfb_data = dev_fb_init(machine, d->fb_mem, 0, VFB_GENERIC,
	    SFB_XSIZE, SFB_YSIZE, SFB_XSIZE, SFB_YSIZE, 8, "PMAGB-BA");
	free(d->vfb_data->framebuffer);
	d->vfb_data->framebuffer = NULL;
	sfb_update_screen(d, 0);

	d->reg[R(SFB_PIXELMASK)] = 0xffffffff;

	/*  The TURBOchannel runs at 25 MHz in the 5000/200 and 5000/240,
	    and at 12.5 MHz in the other models:  */
	tc_hz = machine->machine_subtype == MACHINE_DEC_3MAX_5000 ||
	    machine->machine_subtype == MACHINE_DEC_3MAXPLUS_5000?
	    25000000 : 12500000;
	d->vidclk_count = ((uint64_t) SFB_PIXEL_CLOCK * 256 / 4 + tc_hz / 2)
	    / tc_hz;

	INTERRUPT_CONNECT(irq_path, d->irq);

	d->retrace_ticks = machine->emulated_hz /
	    (SFB_RETRACE_HZ << SFB_TICK_SHIFT);
	if (d->retrace_ticks < 1)
		d->retrace_ticks = 1;
	d->retrace_ticks_left = d->retrace_ticks;

	memory_device_register(mem, "sfb", baseaddr + SFB_ASIC_OFFSET,
	    SFB_REGS_LENGTH, dev_sfb_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "sfb gp", baseaddr + SFB_GP_OFFSET,
	    SFB_GP_LENGTH, dev_sfb_gp_access, d, DM_DEFAULT, NULL);

	/*  The ASIC owns the interrupt, not the Bt459:  */
	dev_bt459_init(machine, mem, baseaddr + SFB_OFFSET_BT459, 0,
	    d->vfb_data, 8, NULL, BT459_BBA);

	/*  Reads of the VRAM may go straight to it, but writes go through
	    the ASIC:  */
	memory_device_register(mem, "sfb vram", baseaddr + SFB_VRAM_OFFSET,
	    SFB_VRAM_SIZE, dev_sfb_vram_access, d,
	    DM_DYNTRANS_OK | DM_READS_HAVE_NO_SIDE_EFFECTS, d->vram);

	machine_add_tickfunction(machine, dev_sfb_tick, d, SFB_TICK_SHIFT);
}

