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
 *  COMMENT: ISA/EISA DMA controller (dual 8237, Intel 82357 ISP extensions)
 *
 *  Two cascaded 8237A controllers: channels 0..3 (8-bit) at ports
 *  0x00..0x0f, channels 4..7 (16-bit) at ports 0xc0..0xdf, with page
 *  registers at 0x80..0x8f.  The 82357 EISA extensions are also
 *  implemented: high page registers (0x481..0x48b), high byte count
 *  registers (0x401..0x407, 0x4c6..0x4ce) and the extended mode registers
 *  (0x40b, 0x4d6).
 *
 *  Bus master devices (e.g. the floppy controller) move data by calling
 *  isadma_transfer().  The DMA bus address is used directly as a physical
 *  memory address, which is how EISA systems such as the Jensen map DMA.
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


/*  Mode register bits:  */
#define	MODE_XFER_MASK		0x0c
#define	MODE_XFER_VERIFY	0x00
#define	MODE_XFER_WRITE		0x04	/*  device -> memory  */
#define	MODE_XFER_READ		0x08	/*  memory -> device  */
#define	MODE_AUTOINIT		0x10
#define	MODE_DECREMENT		0x20

/*  Extended mode register, transfer size (bits 3:2):  */
#define	EXT_SIZE_8		0	/*  8-bit, byte count  */
#define	EXT_SIZE_16W		1	/*  16-bit, word count (ISA)  */
#define	EXT_SIZE_32		2	/*  32-bit, byte count  */
#define	EXT_SIZE_16B		3	/*  16-bit, byte count  */

struct isadma_data {
	struct machine	*machine;

	uint16_t	base_addr[8];
	uint16_t	base_count[8];
	uint16_t	cur_addr[8];
	uint16_t	cur_count[8];
	uint8_t		base_hcount[8];
	uint8_t		cur_hcount[8];
	uint8_t		page[8];
	uint8_t		hpage[8];
	uint8_t		mode[8];
	uint8_t		ext_mode[8];

	uint8_t		mask;		/*  bit n = channel n masked  */
	uint8_t		request;
	uint8_t		status[2];	/*  TC bits 0..3, cleared on read  */
	uint8_t		command[2];
	int		flipflop[2];

	uint8_t		chain[2];	/*  chaining registers at 0x40a / 0x4d4  */
	uint8_t		scratch[16];	/*  unused page register ports  */
	uint8_t		elcr[2];	/*  ELCR1/ELCR2 at 0x4d0/0x4d1  */
	uint8_t		ext[8][3];	/*  high address and stop count (0x4e0..0x4fe)  */
};

/*  Port 0x80 + n -> channel (or -1 for scratch registers):  */
static const int page_port_to_channel[16] = {
	-1, 2, 3, 1, -1, -1, -1, 0, -1, 6, 7, 5, -1, -1, -1, 4
};


/*
 *  isadma_ext_size():
 *
 *  Returns the channel's transfer size.  Channels 0..3 default to 8-bit,
 *  channels 4..7 default to 16-bit word count (ISA compatible).
 */
static int isadma_ext_size(struct isadma_data *d, int ch)
{
	return (d->ext_mode[ch] >> 2) & 3;
}


/*
 *  isadma_reset():
 *
 *  Master clear of one controller (0 = channels 0..3, 1 = channels 4..7).
 */
static void isadma_reset(struct isadma_data *d, int ctrl)
{
	d->command[ctrl] = 0;
	d->status[ctrl] = 0;
	d->flipflop[ctrl] = 0;
	d->chain[ctrl] = 0;
	d->mask |= (0x0f << (ctrl * 4));
	d->request &= ~(0x0f << (ctrl * 4));
}


/*
 *  isadma_transfer():
 *
 *  Called by a DMA slave device to move up to len bytes over the given
 *  channel.  to_memory is nonzero when data flows from the device into
 *  memory (DMA "write" transfer).  Returns the number of bytes actually
 *  transferred; *tc is set to 1 if terminal count was reached.
 *
 *  A masked channel transfers nothing.
 */
int isadma_transfer(struct isadma_data *d, int ch, unsigned char *buf,
	int len, int to_memory, int *tc)
{
	struct cpu *cpu = d->machine->cpus[d->machine->bootstrap_cpu];
	int ctrl = ch >> 2, size, unit, done = 0;
	uint8_t xfer;

	*tc = 0;

	if (ch < 0 || ch > 7 || (d->mask & (1 << ch)))
		return 0;

	xfer = d->mode[ch] & MODE_XFER_MASK;
	size = isadma_ext_size(d, ch);
	unit = (size == EXT_SIZE_16W) ? 2 : 1;

	while (done < len) {
		uint32_t count = d->cur_count[ch] |
		    ((uint32_t)d->cur_hcount[ch] << 16);
		uint64_t paddr;
		int n;

		if (size == EXT_SIZE_16W)
			paddr = ((uint64_t)d->hpage[ch] << 24) |
			    ((uint64_t)(d->page[ch] & 0xfe) << 16) |
			    ((uint64_t)d->cur_addr[ch] << 1);
		else
			paddr = ((uint64_t)d->hpage[ch] << 24) |
			    ((uint64_t)d->page[ch] << 16) | d->cur_addr[ch];

		n = unit;
		if (done + n > len)
			n = len - done;

		if (xfer == MODE_XFER_WRITE && to_memory)
			cpu->memory_rw(cpu, cpu->mem, paddr, buf + done, n,
			    MEM_WRITE, PHYSICAL | NO_EXCEPTIONS);
		else if (xfer == MODE_XFER_READ && !to_memory)
			cpu->memory_rw(cpu, cpu->mem, paddr, buf + done, n,
			    MEM_READ, PHYSICAL | NO_EXCEPTIONS);
		/*  Verify or mismatched direction: no memory access.  */

		done += n;

		/*  Advance the address (carrying into the page registers
		    for the 32-bit EISA modes):  */
		if (size == EXT_SIZE_8 || size == EXT_SIZE_16W) {
			if (d->mode[ch] & MODE_DECREMENT)
				d->cur_addr[ch] --;
			else
				d->cur_addr[ch] ++;
		} else {
			uint32_t a = ((uint32_t)d->hpage[ch] << 24) |
			    ((uint32_t)d->page[ch] << 16) | d->cur_addr[ch];
			if (d->mode[ch] & MODE_DECREMENT)
				a --;
			else
				a ++;
			d->cur_addr[ch] = a;
			d->page[ch] = a >> 16;
			d->hpage[ch] = a >> 24;
		}

		/*  The transfer ends when the count wraps from 0 to -1:  */
		if (count == 0) {
			*tc = 1;
			d->status[ctrl] |= 1 << (ch & 3);
			d->request &= ~(1 << ch);
			if (d->mode[ch] & MODE_AUTOINIT) {
				d->cur_addr[ch] = d->base_addr[ch];
				d->cur_count[ch] = d->base_count[ch];
				d->cur_hcount[ch] = d->base_hcount[ch];
			} else {
				d->cur_count[ch] = 0xffff;
				d->cur_hcount[ch] = 0xff;
				d->mask |= 1 << ch;
			}
			break;
		}

		count --;
		d->cur_count[ch] = count;
		d->cur_hcount[ch] = count >> 16;
	}

	return done;
}


/*
 *  isadma_channel_reg():
 *
 *  Address/count register access for one controller.  reg is 0..7
 *  (even = address, odd = count of channel reg/2).
 */
static uint8_t isadma_channel_reg(struct isadma_data *d, int ctrl, int reg,
	int writeflag, uint8_t val)
{
	int ch = ctrl * 4 + reg / 2;
	uint16_t *base = (reg & 1) ? &d->base_count[ch] : &d->base_addr[ch];
	uint16_t *cur = (reg & 1) ? &d->cur_count[ch] : &d->cur_addr[ch];
	int hi = d->flipflop[ctrl];
	uint8_t r = 0;

	d->flipflop[ctrl] ^= 1;

	if (writeflag == MEM_WRITE) {
		if (hi)
			*base = (*base & 0x00ff) | (val << 8);
		else
			*base = (*base & 0xff00) | val;
		*cur = *base;
		if (reg & 1) {
			d->base_hcount[ch] = 0;
			d->cur_hcount[ch] = 0;
		} else {
			d->hpage[ch] = 0;
		}
	} else {
		r = hi ? (*cur >> 8) : (*cur & 0xff);
	}

	return r;
}


/*
 *  isadma_control_reg():
 *
 *  Control register access for one controller.  reg is 8..15.
 */
static uint8_t isadma_control_reg(struct isadma_data *d, int ctrl, int reg,
	int writeflag, uint8_t val)
{
	int shift = ctrl * 4, ch;
	uint8_t r = 0;

	if (writeflag == MEM_READ) {
		switch (reg) {
		case 8:	/*  Status: TC bits 0..3, request bits 4..7  */
			r = d->status[ctrl] |
			    (((d->request >> shift) & 0x0f) << 4);
			d->status[ctrl] = 0;
			break;
		case 15:	/*  Mask (82357 allows reading it)  */
			r = (d->mask >> shift) & 0x0f;
			break;
		default:
			break;
		}
		return r;
	}

	switch (reg) {
	case 8:		/*  Command  */
		d->command[ctrl] = val;
		break;
	case 9:		/*  Request  */
		ch = shift + (val & 3);
		if (val & 4)
			d->request |= 1 << ch;
		else
			d->request &= ~(1 << ch);
		break;
	case 10:	/*  Single mask  */
		ch = shift + (val & 3);
		if (val & 4)
			d->mask |= 1 << ch;
		else
			d->mask &= ~(1 << ch);
		break;
	case 11:	/*  Mode  */
		d->mode[shift + (val & 3)] = val;
		break;
	case 12:	/*  Clear byte pointer flip-flop  */
		d->flipflop[ctrl] = 0;
		break;
	case 13:	/*  Master clear  */
		isadma_reset(d, ctrl);
		break;
	case 14:	/*  Clear mask register  */
		d->mask &= ~(0x0f << shift);
		break;
	case 15:	/*  Write all mask bits  */
		d->mask = (d->mask & ~(0x0f << shift)) |
		    ((val & 0x0f) << shift);
		break;
	}

	return 0;
}


/*  Controller 1: ports 0x00..0x0f  */
DEVICE_ACCESS(isadma1)
{
	struct isadma_data *d = (struct isadma_data *) extra;
	uint8_t val = 0, r;
	int reg = relative_addr & 0x0f;

	if (writeflag == MEM_WRITE)
		val = data[0];

	if (reg < 8)
		r = isadma_channel_reg(d, 0, reg, writeflag, val);
	else
		r = isadma_control_reg(d, 0, reg, writeflag, val);

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, r);

	return 1;
}


/*  Controller 2: ports 0xc0..0xdf (registers on even addresses)  */
DEVICE_ACCESS(isadma2)
{
	struct isadma_data *d = (struct isadma_data *) extra;
	uint8_t val = 0, r = 0;
	int reg = (relative_addr >> 1) & 0x0f;

	if (writeflag == MEM_WRITE)
		val = data[0];

	if (relative_addr & 1) {
		/*  Odd addresses are not decoded.  */
	} else if (reg < 8)
		r = isadma_channel_reg(d, 1, reg, writeflag, val);
	else
		r = isadma_control_reg(d, 1, reg, writeflag, val);

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, r);

	return 1;
}


/*  Page registers: ports 0x80..0x8f  */
DEVICE_ACCESS(isadma_page)
{
	struct isadma_data *d = (struct isadma_data *) extra;
	int reg = relative_addr & 0x0f;
	int ch = page_port_to_channel[reg];

	if (writeflag == MEM_WRITE) {
		if (ch >= 0) {
			d->page[ch] = data[0];
			/*  82357: writing the low page clears the high page  */
			d->hpage[ch] = 0;
		} else
			d->scratch[reg] = data[0];
	} else {
		memory_writemax64(cpu, data, len,
		    ch >= 0 ? d->page[ch] : d->scratch[reg]);
	}

	return 1;
}


/*  EISA high page registers: ports 0x480..0x48f  */
DEVICE_ACCESS(isadma_hpage)
{
	struct isadma_data *d = (struct isadma_data *) extra;
	int ch = page_port_to_channel[relative_addr & 0x0f];
	uint8_t r = 0;

	if (writeflag == MEM_WRITE) {
		if (ch >= 0)
			d->hpage[ch] = data[0];
	} else {
		if (ch >= 0)
			r = d->hpage[ch];
		memory_writemax64(cpu, data, len, r);
	}

	return 1;
}


/*
 *  EISA high count and extended mode registers:
 *	0x401, 0x403, 0x405, 0x407	High count, channels 0..3
 *	0x40a				Chaining status (read/write)
 *	0x40b				Extended mode, channels 0..3
 *	0x4c6, 0x4ca, 0x4ce		High count, channels 5..7
 *	0x4d0, 0x4d1			PIC ELCR1 / ELCR2
 *	0x4d4				Chaining status (read/write)
 *	0x4d6				Extended mode, channels 4..7
 */
static int isadma_hcount_channel(uint32_t port)
{
	switch (port) {
	case 0x401: return 0;
	case 0x403: return 1;
	case 0x405: return 2;
	case 0x407: return 3;
	case 0x4c6: return 5;
	case 0x4ca: return 6;
	case 0x4ce: return 7;
	}
	return -1;
}

static void isadma_eisa_port(struct isadma_data *d, struct cpu *cpu,
	uint32_t port, unsigned char *data, size_t len, int writeflag)
{
	int ch = isadma_hcount_channel(port);
	uint8_t r = 0;

	if (writeflag == MEM_WRITE) {
		uint8_t val = data[0];
		if (ch >= 0) {
			d->base_hcount[ch] = val;
			d->cur_hcount[ch] = val;
		} else if (port == 0x40a)
			d->chain[0] = val;
		else if (port == 0x40b)
			d->ext_mode[val & 3] = val;
		else if (port == 0x4d4)
			d->chain[1] = val;
		else if (port == 0x4d6)
			d->ext_mode[4 + (val & 3)] = val;
		else if (port == 0x4d0 || port == 0x4d1) {
			if (port == 0x4d0)
				d->elcr[0] = val & 0xf8;
			else
				d->elcr[1] = val & 0xde;

			/*
			 *  Tell the PICs which inputs are level-triggered.
			 *  The cascade input (IRQ 2) follows the slave PIC's
			 *  output, which is a level.
			 */
			if (d->machine->isa_pic_data.pic1 != NULL)
				d->machine->isa_pic_data.pic1->elcr =
				    d->elcr[0] | 0x04;
			if (d->machine->isa_pic_data.pic2 != NULL)
				d->machine->isa_pic_data.pic2->elcr =
				    d->elcr[1];
		}
		return;
	}

	if (ch >= 0)
		r = d->cur_hcount[ch];
	else if (port == 0x40a)
		r = d->chain[0];
	else if (port == 0x4d4)
		r = d->chain[1];
	else if (port == 0x4d0)
		r = d->elcr[0] & 0xf8;
	else if (port == 0x4d1)
		r = d->elcr[1] & 0xde;

	memory_writemax64(cpu, data, len, r);
}

DEVICE_ACCESS(isadma_eisa1)
{
	isadma_eisa_port((struct isadma_data *) extra, cpu,
	    0x400 + relative_addr, data, len, writeflag);
	return 1;
}

DEVICE_ACCESS(isadma_eisa2)
{
	isadma_eisa_port((struct isadma_data *) extra, cpu,
	    0x4c0 + relative_addr, data, len, writeflag);
	return 1;
}


/*  EISA high address and stop count extension registers: 0x4e0..0x4fe  */
static int isadma_ext_port_to_ch(uint32_t port, int *subreg)
{
	if (port >= 0x4e0 && port <= 0x4ee) {
		*subreg = (port - 0x4e0) & 3;
		return (port - 0x4e0) >> 2; /* ch 0..3 */
	}
	if (port >= 0x4f4 && port <= 0x4fe) {
		*subreg = (port - 0x4f4) & 3;
		return 5 + ((port - 0x4f4) >> 2); /* ch 5..7 */
	}
	return -1;
}

DEVICE_ACCESS(isadma_ext)
{
	struct isadma_data *d = (struct isadma_data *) extra;
	uint32_t port = 0x4e0 + relative_addr;
	int subreg = 0;
	int ch = isadma_ext_port_to_ch(port, &subreg);
	uint8_t byte = 0;

	if (writeflag == MEM_WRITE) {
		byte = data[0];
		if (ch >= 0 && subreg < 3) {
			if (subreg == 0)
				d->ext[ch][subreg] = byte & 0xfc;
			else
				d->ext[ch][subreg] = byte;
		}
		return 1;
	}

	if (ch >= 0 && subreg < 3) {
		byte = d->ext[ch][subreg];
		if (subreg == 0)
			byte &= 0xfc;
	}

	memory_writemax64(cpu, data, len, byte);
	return 1;
}


/*
 *  dev_isadma_init():
 *
 *  Registers the DMA controllers at the given ISA I/O port base.
 */
struct isadma_data *dev_isadma_init(struct machine *machine,
	struct memory *mem, uint64_t isa_portbase)
{
	struct isadma_data *d;
	int ch;

	CHECK_ALLOCATION(d = (struct isadma_data *) malloc(sizeof(struct isadma_data)));
	memset(d, 0, sizeof(struct isadma_data));

	d->machine = machine;
	d->mask = 0xff;

	/*  Channels 4..7 are 16-bit word-count channels by default:  */
	for (ch = 4; ch < 8; ch++)
		d->ext_mode[ch] = (EXT_SIZE_16W << 2) | (ch & 3);
	for (ch = 0; ch < 4; ch++)
		d->ext_mode[ch] = ch;

	/*  Channel 4 is the cascade between the two controllers:  */
	d->mode[4] = 0xc0;

	memory_device_register(mem, "isadma1", isa_portbase + 0x00, 0x10,
	    dev_isadma1_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "isadma_page", isa_portbase + 0x80, 0x10,
	    dev_isadma_page_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "isadma2", isa_portbase + 0xc0, 0x20,
	    dev_isadma2_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "isadma_eisa1", isa_portbase + 0x400, 0x10,
	    dev_isadma_eisa1_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "isadma_hpage", isa_portbase + 0x480, 0x10,
	    dev_isadma_hpage_access, d, DM_DEFAULT, NULL);
	/*  0x4c0..0x4d7: high count 5..7 and extended mode 4..7:  */
	memory_device_register(mem, "isadma_eisa2", isa_portbase + 0x4c0, 0x18,
	    dev_isadma_eisa2_access, d, DM_DEFAULT, NULL);
	/*  0x4e0..0x4ff: high address and stop count extensions:  */
	memory_device_register(mem, "isadma_ext", isa_portbase + 0x4e0, 0x20,
	    dev_isadma_ext_access, d, DM_DEFAULT, NULL);

	return d;
}
