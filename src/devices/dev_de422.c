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
 *  COMMENT: DEC DE422 EtherWORKS Turbo EISA Ethernet controller
 *
 *  A DEPCA-family card: an AMD LANCE (Am7990) with 64 KB of buffer RAM on
 *  the card, which the host sees through a 32 or 64 KB window in ISA
 *  memory and the LANCE at its own addresses 0..FFFFh (the low 16 bits of
 *  the host's bus address). From the ECU's board file (!DEC4220.CFG) and
 *  the Linux depca driver:
 *
 *	zC00h	NICSR, the network interface control and status register:
 *		IEN (bit 1) enables and IM (bit 2) masks the interrupt
 *		(SHE, bit 7, which enables the memory of other DEPCA cards,
 *		does not matter on the DE422: NT's driver never sets it)
 *	zC02h	RBI, RAM buffer index (2 KB buffer mode; stored)
 *	zC04h	the LANCE's register data port (RDP)
 *	zC06h	the LANCE's register address port (RAP)
 *	zC08h	HBASE, high memory base (stored)
 *	zC0Ch	reads: the Ethernet address ROM, a byte at a time (32 bytes,
 *		repeating); writes: the configuration register, which the
 *		ECU programs: bits 6..3 the IRQ (5, 9, 10, 11), bits 2..0
 *		the window (C0000, E0000, D0000 64 KB; C8000, E8000, D8000
 *		32 KB)
 *	zC0Eh	RBSA, RAM buffer start address (stored)
 *	zC80h	the EISA board ID, DEC4220; zC84h bit 0 enables the board
 *		(and the window)
 *
 *  The window at C0000 is not decoded (the QVision's option ROM is there).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "device.h"
#include "devices.h"
#include "interrupt.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"


#define	DE422_RAM_MASK		0xffff

/*  NICSR bits:  */
#define	NICSR_IEN		0x0002
#define	NICSR_IM		0x0004

/*  "DEC4220", compressed:  */
static const uint8_t de422_eisa_id[4] = { 0x10, 0xa3, 0x42, 0x20 };


struct de422_data {
	int		subsys;

	struct le_data	*le;
	unsigned char	*ram;
	const uint8_t	*rom;
	int		rom_ptr;

	uint16_t	nicsr;
	uint16_t	rbi, hbase, rbsa;
	uint16_t	cnfg;
	uint16_t	latch[2];		/*  RDP, RAP, a byte at a time  */
	uint8_t		board_ctrl;		/*  zC84  */

	/*  The LANCE's interrupt, and the card's EISA interrupt:  */
	int		lance_irq;
	struct interrupt irq[4];		/*  IRQ 5, 9, 10, 11  */
	int		irq_asserted;		/*  index, or -1  */
};

static const int de422_irqs[4] = { 5, 9, 10, 11 };


/*  The card's EISA interrupt follows the LANCE's, as NICSR and the
    configuration register allow:  */
static void de422_update_irq(struct de422_data *d)
{
	int sel = -1, i;

	if (d->lance_irq && (d->nicsr & NICSR_IEN) && !(d->nicsr & NICSR_IM))
		for (i = 0; i < 4; i++)
			if (d->cnfg & (0x08 << i)) {
				sel = i;
				break;
			}

	if (sel == d->irq_asserted)
		return;
	if (d->irq_asserted >= 0)
		INTERRUPT_DEASSERT(d->irq[d->irq_asserted]);
	if (sel >= 0)
		INTERRUPT_ASSERT(d->irq[sel]);
	d->irq_asserted = sel;
}

static void de422_lance_interrupt_assert(struct interrupt *interrupt)
{
	struct de422_data *d = (struct de422_data *) interrupt->extra;
	d->lance_irq = 1;
	de422_update_irq(d);
}

static void de422_lance_interrupt_deassert(struct interrupt *interrupt)
{
	struct de422_data *d = (struct de422_data *) interrupt->extra;
	d->lance_irq = 0;
	de422_update_irq(d);
}


/*  The window in ISA memory, from the configuration register:  */
static void de422_window(struct de422_data *d, uint32_t *start,
	uint32_t *len)
{
	static const uint32_t base[8] = { 0xc0000, 0xe0000, 0xd0000, 0xd0000,
	    0xc8000, 0xe8000, 0xd8000, 0xd8000 };

	*start = base[d->cnfg & 7];
	*len = (d->cnfg & 4)? 0x8000 : 0x10000;
}


/*  A 16-bit register, written a byte or a word at a time:  */
static void de422_reg16(uint16_t *reg, uint64_t offset, unsigned char *data,
	size_t len, int writeflag)
{
	if (writeflag == MEM_WRITE) {
		if (len >= 2)
			*reg = data[0] | (data[1] << 8);
		else if (offset & 1)
			*reg = (*reg & 0x00ff) | (data[0] << 8);
		else
			*reg = (*reg & 0xff00) | data[0];
	} else {
		data[0] = (offset & 1)? (*reg >> 8) : *reg;
		if (len >= 2)
			data[1] = *reg >> 8;
	}
}


DEVICE_ACCESS(de422)
{
	struct de422_data *d = (struct de422_data *) extra;
	uint16_t v;
	size_t i;

	switch (relative_addr & ~1) {
	case 0x00:
		de422_reg16(&d->nicsr, relative_addr, data, len, writeflag);
		if (writeflag == MEM_WRITE) {
			debugmsg(d->subsys, "de422", VERBOSITY_DEBUG,
			    "NICSR = 0x%04x\n", d->nicsr);
			de422_update_irq(d);
		}
		break;
	case 0x02:
		de422_reg16(&d->rbi, relative_addr, data, len, writeflag);
		break;
	case 0x04:
	case 0x06: {
		/*
		 *  The LANCE's registers are 16 bits, and the Jensen's EISA
		 *  bridge accesses them a byte at a time, low byte first: a
		 *  write takes place with the high byte, and a read of the
		 *  low byte reads the register (the high byte comes from
		 *  that read).
		 */
		int rap = (relative_addr & ~1) == 0x06;
		uint16_t *latch = &d->latch[rap];

		if (writeflag == MEM_WRITE) {
			if (len >= 2 || (relative_addr & 1)) {
				v = (len >= 2)? (data[0] | (data[1] << 8)) :
				    ((*latch & 0xff) | (data[0] << 8));
				dev_le_reg_access(cpu, d->le, rap, MEM_WRITE, v);
			} else
				*latch = data[0];
		} else {
			if (!(relative_addr & 1))
				*latch = dev_le_reg_access(cpu, d->le, rap,
				    MEM_READ, 0);
			data[0] = (relative_addr & 1)? *latch >> 8 : *latch;
			if (len >= 2)
				data[1] = *latch >> 8;
		}
		break;
	}
	case 0x08:
		de422_reg16(&d->hbase, relative_addr, data, len, writeflag);
		break;
	case 0x0c:
		if (writeflag == MEM_WRITE) {
			de422_reg16(&d->cnfg, relative_addr, data, len,
			    writeflag);
			debugmsg(d->subsys, "de422", VERBOSITY_DEBUG,
			    "configuration = 0x%04x\n", d->cnfg);
			de422_update_irq(d);
		} else {
			/*  The Ethernet address ROM, a byte per read:  */
			for (i = 0; i < len; i++) {
				data[i] = d->rom[d->rom_ptr];
				d->rom_ptr = (d->rom_ptr + 1) & 31;
			}
		}
		break;
	case 0x0e:
		de422_reg16(&d->rbsa, relative_addr, data, len, writeflag);
		break;
	default:
		if (writeflag == MEM_READ)
			memset(data, 0xff, len);
	}

	return 1;
}


/*  The shared memory window(s):  */
DEVICE_ACCESS(de422_ram)
{
	struct de422_data *d = (struct de422_data *) extra;
	uint32_t start, wlen, a = 0xc8000 + relative_addr;
	size_t i;

	de422_window(d, &start, &wlen);

	for (i = 0; i < len; i++, a++) {
		if (a < start || a >= start + wlen ||
		    !(d->board_ctrl & 1)) {
			if (writeflag == MEM_READ)
				data[i] = 0xff;
			continue;
		}
		if (writeflag == MEM_WRITE)
			d->ram[a & DE422_RAM_MASK] = data[i];
		else
			data[i] = d->ram[a & DE422_RAM_MASK];
	}

	return 1;
}


/*  EISA slot registers zC80..zC84:  */
DEVICE_ACCESS(de422_slot)
{
	struct de422_data *d = (struct de422_data *) extra;
	size_t i;

	for (i = 0; i < len; i++) {
		uint64_t r = relative_addr + i;
		if (writeflag == MEM_WRITE) {
			if (r == 4)
				d->board_ctrl = data[i];
		} else
			data[i] = r < 4? de422_eisa_id[r] : r == 4?
			    d->board_ctrl : 0xff;
	}
	return 1;
}


/*
 *  dev_de422_init():
 *
 *  A DE422 in EISA slot "slot", with the ISA bus's I/O ports at
 *  isa_portbase and memory at isa_membase, and its interrupts on
 *  isa_irq_path.isa.N.
 */
void dev_de422_init(struct machine *machine, struct memory *mem,
	uint64_t isa_portbase, uint64_t isa_membase, int slot,
	const char *isa_irq_path)
{
	struct de422_data *d;
	struct interrupt templ;
	char path[300], name[64];
	int i;

	CHECK_ALLOCATION(d = (struct de422_data *) malloc(sizeof(*d)));
	memset(d, 0, sizeof(*d));
	d->subsys = debugmsg_register_subsystem("de422");
	d->irq_asserted = -1;

	/*  The configuration the ECU's first choices give (IRQ 5, a 64 KB
	    window at D0000), until the firmware writes it:  */
	d->cnfg = 0x0a;
	d->nicsr = NICSR_IM;

	for (i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "%s.isa.%i", isa_irq_path,
		    de422_irqs[i]);
		INTERRUPT_CONNECT(path, d->irq[i]);
	}

	/*  The LANCE's interrupt comes here first:  */
	snprintf(path, sizeof(path), "%s.de422.%i", machine->path, slot);
	memset(&templ, 0, sizeof(templ));
	templ.line = 0;
	templ.name = path;
	templ.extra = d;
	templ.interrupt_assert = de422_lance_interrupt_assert;
	templ.interrupt_deassert = de422_lance_interrupt_deassert;
	interrupt_handler_register(&templ);

	d->le = dev_le_ext_init(machine, path, &d->ram, &d->rom);

	snprintf(name, sizeof(name), "de422 [%02x:%02x:%02x:%02x:%02x:%02x]",
	    d->rom[0], d->rom[1], d->rom[2], d->rom[3], d->rom[4], d->rom[5]);
	memory_device_register(mem, strdup(name),
	    isa_portbase + slot * 0x1000 + 0xc00, 0x10,
	    dev_de422_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "de422_slot",
	    isa_portbase + slot * 0x1000 + 0xc80, 5,
	    dev_de422_slot_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "de422_ram", isa_membase + 0xc8000,
	    0xf0000 - 0xc8000, dev_de422_ram_access, d, DM_DEFAULT, NULL);
}
