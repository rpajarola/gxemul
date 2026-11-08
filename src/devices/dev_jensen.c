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
 *  COMMENT: DECpc AXP 150 / DEC 2000 Model 300 ("Jensen") chipset & bus router
 *
 *  Address mapping (DEC EK-A0638-TD.001):
 *	0x0.0000.0000	System RAM (up to 128 MB)
 *	0x1.0000.0000	Interrupt Acknowledge (INTA) cycle register (82357 ISP)
 *	0x1.8000.0000	Flash ROM window 0 (sparse: addr >> 9)
 *	0x1.A000.0000	Flash ROM window 1 (sparse: addr >> 9)
 *	0x1.C000.0000	Local Combo Super I/O (VL82C106: KBD, RTC, COM1, COM2, LPT)
 *	0x1.D000.0000	Hardware Address Extension (HAE) register
 *	0x1.E000.0000	System Control (SYSCTL) register (RAM size/watchdog)
 *	0x1.F000.0000	Spare register
 *	0x2.0000.0000	Sparse EISA Memory Space (windowed via HAE)
 *	0x3.0000.0000	Sparse EISA I/O Space (stride 128, width in bits 5..6)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus_isa.h"
#include "cpu.h"
#include "cpu_alpha.h"
#include "device.h"
#include "devices.h"
#include "interrupt.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#define	JENSEN_ROM_SIZE		(1024 * 1024)

#define	JENSEN_INTA_BASE	0x100000000ULL
#define	JENSEN_INTA_SIZE	0x10000000ULL

#define	JENSEN_FLASH0_BASE	0x180000000ULL
#define	JENSEN_FLASH0_SIZE	0x20000000ULL

#define	JENSEN_FLASH1_BASE	0x1a0000000ULL
#define	JENSEN_FLASH1_SIZE	0x20000000ULL

#define	JENSEN_COMBO_BASE	0x1c0000000ULL
#define	JENSEN_COMBO_SIZE	0x10000000ULL

#define	JENSEN_HAE_BASE		0x1d0000000ULL
#define	JENSEN_HAE_SIZE		0x10000000ULL

#define	JENSEN_SYSCTL_BASE	0x1e0000000ULL
#define	JENSEN_SYSCTL_SIZE	0x10000000ULL

#define	JENSEN_SPARE_BASE	0x1f0000000ULL
#define	JENSEN_SPARE_SIZE	0x10000000ULL

#define	JENSEN_EISA_MEM_BASE	0x200000000ULL
#define	JENSEN_EISA_MEM_SIZE	0x100000000ULL

#define	JENSEN_EISA_IO_BASE	0x300000000ULL
#define	JENSEN_EISA_IO_SIZE	0x100000000ULL

#define	JENSEN_ISA_PORTBASE	0x10000000000ULL
#define	JENSEN_ISA_MEMBASE	0x10001000000ULL


struct jensen_data {
	struct machine		*machine;
	struct memory		*mem;
	uint64_t		ram_size;

	uint8_t			hae;
	uint8_t			sysctl;
	uint8_t			spare;
	uint8_t			nmi_control;
	uint8_t			refresh_toggle;
	uint8_t			inta_vector;
	int			inta_second;

	struct interrupt	irq_rtc;	/* EV4 irq_h[0] */
	struct interrupt	irq_pic;	/* EV4 irq_h[1] */
	struct interrupt	irq_nmi;	/* EV4 irq_h[2] */
	struct interrupt	irq_kbd;	/* EV4 irq_h[3] */
	struct interrupt	irq_serial;	/* EV4 irq_h[5] */
	uint8_t			serial_irq_mask;

	int			have_rom;
	size_t			rom_size;
	unsigned char		rom[JENSEN_ROM_SIZE];
	uint8_t			flash_cmd;
	uint8_t			flash_status;
};


/*
 *  jensen_pic_interrupt_assert():
 *  jensen_pic_interrupt_deassert():
 *
 *  Interrupt handlers receiving assertions from the primary 8259 PIC
 *  and forwarding them to EV4 irq_h[1].
 */
static void jensen_pic_interrupt_assert(struct interrupt *interrupt)
{
	struct jensen_data *d = (struct jensen_data *) interrupt->extra;
	INTERRUPT_ASSERT(d->irq_pic);
}

static void jensen_pic_interrupt_deassert(struct interrupt *interrupt)
{
	struct jensen_data *d = (struct jensen_data *) interrupt->extra;
	INTERRUPT_DEASSERT(d->irq_pic);
}

static void jensen_rtc_interrupt_assert(struct interrupt *interrupt)
{
	struct jensen_data *d = (struct jensen_data *) interrupt->extra;
	INTERRUPT_ASSERT(d->irq_rtc);
}

static void jensen_rtc_interrupt_deassert(struct interrupt *interrupt)
{
	struct jensen_data *d = (struct jensen_data *) interrupt->extra;
	INTERRUPT_DEASSERT(d->irq_rtc);
}

static void jensen_serial_interrupt_assert(struct interrupt *interrupt)
{
	struct jensen_data *d = (struct jensen_data *) interrupt->extra;
	d->serial_irq_mask |= 1 << interrupt->line;
	INTERRUPT_ASSERT(d->irq_serial);
}

static void jensen_serial_interrupt_deassert(struct interrupt *interrupt)
{
	struct jensen_data *d = (struct jensen_data *) interrupt->extra;
	d->serial_irq_mask &= ~(1 << interrupt->line);
	if (d->serial_irq_mask == 0)
		INTERRUPT_DEASSERT(d->irq_serial);
}

static void jensen_kbd_interrupt_assert(struct interrupt *interrupt)
{
	struct jensen_data *d = (struct jensen_data *) interrupt->extra;
	INTERRUPT_ASSERT(d->irq_kbd);
}

static void jensen_kbd_interrupt_deassert(struct interrupt *interrupt)
{
	struct jensen_data *d = (struct jensen_data *) interrupt->extra;
	INTERRUPT_DEASSERT(d->irq_kbd);
}


/*
 *  jensen_flash_access():
 *
 *  Sparse Flash ROM windows at 0x1.8000.0000 and 0x1.A000.0000.
 *  Byte offset is (relative_addr >> 9) & 0xfffff.
 *
 *  Emulates an Intel 28F008SA 1 MB Flash memory:
 *  - Command 0x90: Read Identifier (Offset 0 = 0x89 Intel, Offset 1 = 0xa2 28F008SA)
 *  - Command 0x70: Read Status Register (0x80 = Ready)
 *  - Command 0x50: Clear Status Register
 *  - Command 0x40 / 0x10: Byte Write Setup
 *  - Command 0x20: Block Erase Setup (followed by 0xd0 Confirm)
 *  - Command 0xff / 0x00: Read Array
 */
DEVICE_ACCESS(jensen_flash)
{
	struct jensen_data *d = (struct jensen_data *) extra;
	uint32_t offset = (relative_addr >> 9) & 0xfffff;
	uint8_t byte = 0xff;

	if (writeflag == MEM_WRITE) {
		uint8_t val = data[0];
		if (d->flash_cmd == 0x40 || d->flash_cmd == 0x10) {
			/* Byte write: protect Block 6 (0x60000) and Block C (0xc0000) */
			if ((offset < 0x60000 || (offset >= 0x70000 && offset < 0xc0000) || offset >= 0xd0000)
			    && offset < d->rom_size)
				d->rom[offset] = val;
			d->flash_cmd = 0x70;
			d->flash_status = 0x80;
			return 1;
		}
		if (d->flash_cmd == 0x20 && val == 0xd0) {
			/* Block Erase Confirm: erase 64KB block (protect Block 6 and Block C) */
			uint32_t block_start = offset & ~0xffff;
			uint32_t block_end = block_start + 0x10000;
			if (block_start != 0x60000 && block_start != 0xc0000) {
				if (block_end > d->rom_size)
					block_end = d->rom_size;
				if (block_start < block_end)
					memset(d->rom + block_start, 0xff, block_end - block_start);
			}
			d->flash_cmd = 0x70;
			d->flash_status = 0x80;
			return 1;
		}

		switch (val) {
		case 0x90:	/* Read Identifier */
			d->flash_cmd = 0x90;
			break;
		case 0x70:	/* Read Status */
			d->flash_cmd = 0x70;
			break;
		case 0x50:	/* Clear Status */
			d->flash_status = 0x80;
			d->flash_cmd = 0x00;
			break;
		case 0x20:	/* Block Erase Setup */
			d->flash_cmd = 0x20;
			break;
		case 0x40:	/* Byte Write Setup */
		case 0x10:
			d->flash_cmd = val;
			break;
		case 0xff:	/* Read Array */
		case 0x00:
		default:
			d->flash_cmd = 0x00;
			break;
		}
		return 1;
	}

	if (d->flash_cmd == 0x90) {
		/* Intel 28F008SA: offset 0 = 0x89 (Intel), offset 1 = 0xa2 (28F008SA) */
		if ((offset & 1) == 0)
			byte = 0x89;
		else
			byte = 0xa2;
	} else if (d->flash_cmd == 0x70) {
		byte = d->flash_status ? d->flash_status : 0x80;
	} else {
		if (offset < d->rom_size)
			byte = d->rom[offset];
	}

	memory_writemax64(cpu, data, len, byte);
	return 1;
}


/*
 *  jensen_hae_access():
 *
 *  Hardware Address Extension (HAE) register at 0x1.D000.0000.
 */
DEVICE_ACCESS(jensen_hae)
{
	struct jensen_data *d = (struct jensen_data *) extra;

	if (writeflag == MEM_WRITE) {
		d->hae = memory_readmax64(cpu, data, len);
	} else {
		memory_writemax64(cpu, data, len, d->hae);
	}

	return 1;
}


/*
 *  jensen_sysctl_access():
 *
 *  System Control (SYSCTL) register at 0x1.E000.0000.
 */
DEVICE_ACCESS(jensen_sysctl)
{
	struct jensen_data *d = (struct jensen_data *) extra;

	if (writeflag == MEM_WRITE) {
		d->sysctl = memory_readmax64(cpu, data, len);
	} else {
		memory_writemax64(cpu, data, len, d->sysctl);
	}

	return 1;
}


/*
 *  jensen_spare_access():
 *
 *  Spare register at 0x1.F000.0000.
 */
DEVICE_ACCESS(jensen_spare)
{
	struct jensen_data *d = (struct jensen_data *) extra;

	if (writeflag == MEM_WRITE) {
		d->spare = memory_readmax64(cpu, data, len);
	} else {
		memory_writemax64(cpu, data, len, d->spare);
	}

	return 1;
}


/*
 *  jensen_inta_access():
 *
 *  Interrupt Acknowledge (INTA) cycle register at 0x1.0000.0000.
 *  Reading initiates an INTA cycle to the dual cascaded 8259 PICs.
 *  82357 ISP uses two cycles:
 *    Cycle 1: captures vector, returns 0xff on data bus
 *    Cycle 2: drives the captured vector
 */
DEVICE_ACCESS(jensen_inta)
{
	struct jensen_data *d = (struct jensen_data *) extra;
	uint8_t vector = 7;
	int irq;

	if (writeflag == MEM_WRITE)
		return 1;

	if (!d->inta_second) {
		if (d->machine->isa_pic_data.pic1 != NULL) {
			struct pic8259_data *pic1 = d->machine->isa_pic_data.pic1;
			struct pic8259_data *pic2 = d->machine->isa_pic_data.pic2;
			int active1 = pic1->irr & ~pic1->ier;

			if (active1) {
				for (irq = 0; irq < 8; irq++) {
					if (active1 & (1 << irq))
						break;
				}
				if (irq == 2 && pic2 != NULL) {
					int active2 = pic2->irr & ~pic2->ier;
					int irq2 = 7;
					if (active2) {
						for (irq2 = 0; irq2 < 8; irq2++) {
							if (active2 & (1 << irq2))
								break;
						}
					}
					pic2->isr |= (1 << irq2);
					pic1->isr |= (1 << 2);
					vector = pic2->irq_base + irq2;
				} else if (irq < 8) {
					pic1->isr |= (1 << irq);
					vector = pic1->irq_base + irq;
				}
			}
		}
		d->inta_vector = vector;
		vector = 0xff;
	} else {
		vector = d->inta_vector;
	}
	d->inta_second = !d->inta_second;

	debug("[ jensen: INTA cycle returning 0x%02x ]\n", vector);

	memory_writemax64(cpu, data, len, vector);
	return 1;
}


/*
 *  jensen_bus_access():
 *
 *  Directly dispatch a bus access (I/O or memory) to the registered ISA/EISA
 *  devices on the memory object. If no device handles the address, reads return
 *  0xff (floating bus) and writes are ignored, without triggering spurious EV4
 *  CPU machine checks.
 */
static int jensen_bus_access(struct cpu *cpu, struct memory *mem,
	uint64_t paddr, unsigned char *data, size_t len, int writeflag)
{
	int start = 0, end = mem->n_mmapped_devices - 1;

	while (start <= end) {
		int mid = (start + end) >> 1;
		if (paddr >= mem->devices[mid].baseaddr &&
		    paddr < mem->devices[mid].endaddr) {
			mem->last_accessed_device = mid;
			return mem->devices[mid].f(cpu, mem,
			    paddr - mem->devices[mid].baseaddr,
			    data, len, writeflag, mem->devices[mid].extra);
		}
		if (paddr < mem->devices[mid].baseaddr)
			end = mid - 1;
		else
			start = mid + 1;
	}

	if (writeflag == MEM_READ)
		memset(data, 0xff, len);

	return 1;
}


/*
 *  jensen_combo_access():
 *
 *  Local Motherboard Combo Super I/O (VL82C106) at 0x1.C000.0000.
 *  Port address is (relative_addr >> 9) & 0xffff.
 *  All accesses are 8-bit.
 */
DEVICE_ACCESS(jensen_combo)
{
	uint32_t port = (relative_addr >> 9) & 0xffff;
	uint8_t byte = 0xff;

	/*  RTC aliases 0x170 / 0x171 -> 0x70 / 0x71  */
	if (port == 0x170)
		port = 0x70;
	else if (port == 0x171)
		port = 0x71;

	if (writeflag == MEM_WRITE) {
		byte = data[0];
		jensen_bus_access(cpu, cpu->mem, JENSEN_ISA_PORTBASE + port,
		    &byte, 1, MEM_WRITE);
		return 1;
	}

	jensen_bus_access(cpu, cpu->mem, JENSEN_ISA_PORTBASE + port,
	    &byte, 1, MEM_READ);

	memory_writemax64(cpu, data, len, byte);
	return 1;
}


/*
 *  jensen_board_id_access():
 *
 *  System-board EISA ID DEC2400 at port 0xc80..0xc83.
 */
DEVICE_ACCESS(jensen_board_id)
{
	uint8_t b = 0xff;

	if (writeflag == MEM_WRITE)
		return 1;

	memory_writemax64(cpu, data, len, b);
	return 1;
}


/*
 *  jensen_isp_access():
 *
 *  Port 0x61: NMI control and refresh toggle.
 *  Port 0x461: NMI extended status.
 */
DEVICE_ACCESS(jensen_isp)
{
	struct jensen_data *d = (struct jensen_data *) extra;
	uint8_t byte = 0;

	if (writeflag == MEM_WRITE) {
		if (relative_addr == 0)
			d->nmi_control = data[0] & 0x0f;
		return 1;
	}

	if (relative_addr == 0) {
		d->refresh_toggle ^= 0x10;
		byte = d->nmi_control | d->refresh_toggle;
	} else {
		byte = 0;
	}

	memory_writemax64(cpu, data, len, byte);
	return 1;
}


/*
 *  jensen_eisa_io_access():
 *
 *  Sparse EISA I/O space at 0x3.0000.0000.
 *  Bus address is (relative_addr >> 7) & 0xffff.
 *  Access length encoded in bits 6:5:
 *	0: 1 byte
 *	1: 2 bytes
 *	2: 3 bytes
 *	3: 4 bytes
 *  Data placed on byte lane (busaddr & 3).
 */
DEVICE_ACCESS(jensen_eisa_io)
{
	uint64_t busaddr = (relative_addr >> 7) & 0xffff;
	unsigned access_len = ((relative_addr >> 5) & 3) + 1;
	unsigned i;

	if (writeflag == MEM_WRITE) {
		uint64_t val = memory_readmax64(cpu, data, len);
		val >>= (busaddr & 3) * 8;
		for (i = 0; i < access_len; i++) {
			uint8_t b = val >> (i * 8);
			jensen_bus_access(cpu, cpu->mem,
			    JENSEN_ISA_PORTBASE + busaddr + i,
			    &b, 1, MEM_WRITE);
		}
		return 1;
	}

	/*  Read:  */
	{
		uint64_t val = 0;
		for (i = 0; i < access_len; i++) {
			uint8_t b = 0xff;
			jensen_bus_access(cpu, cpu->mem,
			    JENSEN_ISA_PORTBASE + busaddr + i,
			    &b, 1, MEM_READ);
			val |= (uint64_t)b << (i * 8);
		}
		val <<= (busaddr & 3) * 8;
		memory_writemax64(cpu, data, len, val);
	}

	return 1;
}


/*
 *  jensen_eisa_mem_access():
 *
 *  Sparse EISA Memory space at 0x2.0000.0000 (windowed via HAE).
 *  Bus address is ((relative_addr >> 7) & 0x1ffffff) | (hae << 25).
 *  Access length encoded in bits 6:5:
 *	0: 1 byte
 *	1: 2 bytes
 *	2: 3 bytes
 *	3: 4 bytes
 *  Data placed on byte lane (busaddr & 3).
 */
DEVICE_ACCESS(jensen_eisa_mem)
{
	struct jensen_data *d = (struct jensen_data *) extra;
	uint64_t busaddr = ((relative_addr >> 7) & 0x1ffffff) |
	    ((uint64_t)d->hae << 25);
	unsigned access_len = ((relative_addr >> 5) & 3) + 1;
	unsigned i;

	if (writeflag == MEM_WRITE) {
		uint64_t val = memory_readmax64(cpu, data, len);
		val >>= (busaddr & 3) * 8;
		for (i = 0; i < access_len; i++) {
			uint8_t b = val >> (i * 8);
			jensen_bus_access(cpu, cpu->mem,
			    JENSEN_ISA_MEMBASE + busaddr + i,
			    &b, 1, MEM_WRITE);
		}
		return 1;
	}

	/*  Read:  */
	{
		uint64_t val = 0;
		for (i = 0; i < access_len; i++) {
			uint8_t b = 0xff;
			jensen_bus_access(cpu, cpu->mem,
			    JENSEN_ISA_MEMBASE + busaddr + i,
			    &b, 1, MEM_READ);
			val |= (uint64_t)b << (i * 8);
		}
		val <<= (busaddr & 3) * 8;
		memory_writemax64(cpu, data, len, val);
	}

	return 1;
}


/*
 *  dev_jensen_init():
 *
 *  Initialize the Jensen machine devices, memory maps, and standard PC
 *  peripherals.
 */
struct jensen_data *dev_jensen_init(struct machine *machine,
	struct memory *mem, uint64_t ram_size, const char *cpu_irq)
{
	struct jensen_data *d;
	struct interrupt templ;
	char jensen_irq_path[300];
	char tmp[300];
	uint32_t bus_isa_flags;

	CHECK_ALLOCATION(d = (struct jensen_data *) malloc(sizeof(struct jensen_data)));
	memset(d, 0, sizeof(struct jensen_data));

	d->machine = machine;
	d->mem = mem;
	d->ram_size = ram_size;

	d->sysctl = (ram_size == 128 * 1024 * 1024) ? 0xa0 : 0x20;
	d->flash_status = 0x80;
	d->flash_cmd = 0x00;

	/*
	 *  Connect to EV4 hardware interrupt lines irq_h[5:0]:
	 *    0: RTC (MC146818)
	 *    1: EISA/ISA PIC (primary 8259)
	 *    2: System NMI
	 *    3: Motherboard Keyboard / Mouse
	 *    5: Motherboard Serial (COM1 / COM2)
	 */
	snprintf(tmp, sizeof(tmp), "%s.0", cpu_irq);
	INTERRUPT_CONNECT(tmp, d->irq_rtc);

	snprintf(tmp, sizeof(tmp), "%s.1", cpu_irq);
	INTERRUPT_CONNECT(tmp, d->irq_pic);

	snprintf(tmp, sizeof(tmp), "%s.2", cpu_irq);
	INTERRUPT_CONNECT(tmp, d->irq_nmi);

	snprintf(tmp, sizeof(tmp), "%s.3", cpu_irq);
	INTERRUPT_CONNECT(tmp, d->irq_kbd);

	snprintf(tmp, sizeof(tmp), "%s.5", cpu_irq);
	INTERRUPT_CONNECT(tmp, d->irq_serial);

	/*  Register INTA cycle register at 0x1.0000.0000 (256 MB):  */
	memory_device_register(mem, "jensen_inta", JENSEN_INTA_BASE,
	    JENSEN_INTA_SIZE, dev_jensen_inta_access, d, DM_DEFAULT, NULL);

	/*  Register Flash ROM window 0 at 0x1.8000.0000 (512 MB):  */
	memory_device_register(mem, "jensen_flash0", JENSEN_FLASH0_BASE,
	    JENSEN_FLASH0_SIZE, dev_jensen_flash_access, d, DM_DEFAULT, NULL);

	/*  Register Flash ROM window 1 at 0x1.A000.0000 (512 MB):  */
	memory_device_register(mem, "jensen_flash1", JENSEN_FLASH1_BASE,
	    JENSEN_FLASH1_SIZE, dev_jensen_flash_access, d, DM_DEFAULT, NULL);

	/*  Register Local Combo Super I/O at 0x1.C000.0000 (256 MB):  */
	memory_device_register(mem, "jensen_combo", JENSEN_COMBO_BASE,
	    JENSEN_COMBO_SIZE, dev_jensen_combo_access, d, DM_DEFAULT, NULL);

	/*  Register HAE register at 0x1.D000.0000 (256 MB):  */
	memory_device_register(mem, "jensen_hae", JENSEN_HAE_BASE,
	    JENSEN_HAE_SIZE, dev_jensen_hae_access, d, DM_DEFAULT, NULL);

	/*  Register SYSCTL register at 0x1.E000.0000 (256 MB):  */
	memory_device_register(mem, "jensen_sysctl", JENSEN_SYSCTL_BASE,
	    JENSEN_SYSCTL_SIZE, dev_jensen_sysctl_access, d, DM_DEFAULT, NULL);

	/*  Register SPARE register at 0x1.F000.0000 (256 MB):  */
	memory_device_register(mem, "jensen_spare", JENSEN_SPARE_BASE,
	    JENSEN_SPARE_SIZE, dev_jensen_spare_access, d, DM_DEFAULT, NULL);

	/*  Register Sparse EISA Memory at 0x2.0000.0000 (4 GB):  */
	memory_device_register(mem, "jensen_eisa_mem", JENSEN_EISA_MEM_BASE,
	    JENSEN_EISA_MEM_SIZE, dev_jensen_eisa_mem_access, d, DM_DEFAULT, NULL);

	/*  Register Sparse EISA I/O at 0x3.0000.0000 (4 GB):  */
	memory_device_register(mem, "jensen_eisa_io", JENSEN_EISA_IO_BASE,
	    JENSEN_EISA_IO_SIZE, dev_jensen_eisa_io_access, d, DM_DEFAULT, NULL);

	/*  Register System-board EISA ID DEC2400 at port 0xc80 (4 bytes):  */
	memory_device_register(mem, "jensen_board_id", JENSEN_ISA_PORTBASE + 0xc80,
	    4, dev_jensen_board_id_access, d, DM_DEFAULT, NULL);

	/*  Register ISP extended port 0x461:  */
	memory_device_register(mem, "jensen_port461", JENSEN_ISA_PORTBASE + 0x461,
	    1, dev_jensen_isp_access, d, DM_DEFAULT, NULL);

	/*
	 *  Set up Jensen primary PIC interrupt handler so bus_isa_init's
	 *  pic1 connects to machine.jensen:
	 */
	snprintf(jensen_irq_path, sizeof(jensen_irq_path), "%s.jensen",
	    machine->path);

	memset(&templ, 0, sizeof(templ));
	templ.line = 0;
	templ.name = jensen_irq_path;
	templ.extra = d;
	templ.interrupt_assert = jensen_pic_interrupt_assert;
	templ.interrupt_deassert = jensen_pic_interrupt_deassert;
	interrupt_handler_register(&templ);

	/*  Jensen dedicated RTC interrupt (EV4 irq_h[0]):  */
	snprintf(tmp, sizeof(tmp), "%s.jensen.rtc", machine->path);
	memset(&templ, 0, sizeof(templ));
	templ.line = 0;
	templ.name = tmp;
	templ.extra = d;
	templ.interrupt_assert = jensen_rtc_interrupt_assert;
	templ.interrupt_deassert = jensen_rtc_interrupt_deassert;
	interrupt_handler_register(&templ);

	/*  Jensen dedicated Serial COM1/COM2 interrupt (EV4 irq_h[5]):  */
	snprintf(tmp, sizeof(tmp), "%s.jensen.serial.0", machine->path);
	memset(&templ, 0, sizeof(templ));
	templ.line = 0;
	templ.name = tmp;
	templ.extra = d;
	templ.interrupt_assert = jensen_serial_interrupt_assert;
	templ.interrupt_deassert = jensen_serial_interrupt_deassert;
	interrupt_handler_register(&templ);

	snprintf(tmp, sizeof(tmp), "%s.jensen.serial.1", machine->path);
	templ.line = 1;
	templ.name = tmp;
	interrupt_handler_register(&templ);

	/*  Jensen dedicated Keyboard interrupt (EV4 irq_h[3]):  */
	snprintf(tmp, sizeof(tmp), "%s.jensen.kbd", machine->path);
	memset(&templ, 0, sizeof(templ));
	templ.line = 0;
	templ.name = tmp;
	templ.extra = d;
	templ.interrupt_assert = jensen_kbd_interrupt_assert;
	templ.interrupt_deassert = jensen_kbd_interrupt_deassert;
	interrupt_handler_register(&templ);

	/*
	 *  Initialize standard PC peripherals on ISA/EISA bus:
	 *  8259 PICs, 8254 PIT, DS1287 RTC, 8042 KBC, COM1, COM2, LPT, VGA.
	 */
	bus_isa_flags = BUS_ISA_VGA | BUS_ISA_LPTBASE_3BC | BUS_ISA_JENSEN_IRQS;
	bus_isa_init(machine, jensen_irq_path, bus_isa_flags,
	    JENSEN_ISA_PORTBASE, JENSEN_ISA_MEMBASE);

	/*  Adaptec AHA-1742A SCSI host adapter in EISA slot 1:  */
	dev_aha1742_init(machine, mem, JENSEN_ISA_PORTBASE + 0x1c80, jensen_irq_path);

	return d;
}


/*
 *  dev_jensen_init_ecu():
 *
 *  Populate standard factory ECU configuration for Jensen:
 *  Block 6 (0x60000): Flash NVRAM Table for EISA slots 1..6.
 *  For slot s in [1..6], entry is at 12 * (s - 1) bytes.
 *    0..7: 8-byte ASCII device ID ("ADP0001 ")
 *    8..11: 32-bit sbb_offset.
 *  Slots 2..6 are zero (sbb_offset = 0 => slot empty).
 */
static void dev_jensen_init_ecu(struct jensen_data *d)
{
	/*  Clear Block 6 (headers and NVRAM):  */
	memset(d->rom + 0x60000, 0, 0x10000);

	/*
	 *  OpenVMS AXP autoconfiguration (SYS$CPU_ROUTINES_0702.EXE):
	 *  Slot 1 entry at 0x60000:
	 *  0x60000..0x60007: "ADP0001 "
	 *  0x60008..0x6000b: sbb_offset = 0x00ff.
	 *
	 *  Physical address = 0x1ac000000 + (sbb_offset << 9) + 512.
	 *  With sbb_offset = 0x00ff:
	 *  Physical address = 0x1ac020000 (dense ROM offset 0x60100).
	 */
	memcpy(d->rom + 0x60000, "ADP0001 ", 8);
	d->rom[0x60008] = 0xff;
	d->rom[0x60009] = 0x00;
	d->rom[0x6000a] = 0x00;
	d->rom[0x6000b] = 0x00;

	/*
	 *  Function data at 0x60100:
	 *  Offset 65 (0x41): Function info (0x04 = Interrupt config present)
	 *  Offset 0xd1 (0xb2 + 0x1f): IRQ 11, level-triggered (0x2b, 0x00)
	 *  Offset 31 (0x1f): Also set for compatibility
	 */
	d->rom[0x60100 + 0x1f] = 0x2b;
	d->rom[0x60100 + 0x20] = 0x00;
	d->rom[0x60100 + 0x21] = 0x00;
	d->rom[0x60100 + 0x22] = 0x00;
	d->rom[0x60100 + 0x41] = 0x04;
	d->rom[0x60100 + 0xd1] = 0x2b;
	d->rom[0x60100 + 0xd2] = 0x00;
}


/*
 *  dev_jensen_set_rom():
 *
 *  Load the Flash ROM image into memory.
 */
void dev_jensen_set_rom(struct jensen_data *d, const unsigned char *image,
	size_t len)
{
	memset(d->rom, 0xff, JENSEN_ROM_SIZE);
	if (len > JENSEN_ROM_SIZE)
		len = JENSEN_ROM_SIZE;
	memcpy(d->rom, image, len);
	d->rom_size = len;
	d->have_rom = 1;

	dev_jensen_init_ecu(d);
}


/*
 *  dev_jensen_srom():
 *
 *  Emulate the Serial ROM (SROM) boot sequence:
 *  1. Initialize 8259 PICs and CMOS registers.
 *  2. Copy bootstrap code from ROM to RAM at 0x4000 (first 512 KiB).
 *  3. Configure initial CPU registers:
 *     - a4 (R20): RAM size in MB
 *     - t10 (R24): 6600 (CPU timing constant for 150 MHz)
 *     - at (R28): Bank configuration bitmap
 *     - ra (R26): SROM version code
 *     - pal_base: 0x4000
 *     - palmode: 1
 *  4. Return entry point (ALPHA_PALMODE_TAG | 0x4040).
 */
uint64_t dev_jensen_srom(struct jensen_data *d, struct cpu *cpu)
{
	static const struct {
		uint16_t port;
		uint8_t value;
	} pic_init[] = {
		/* Slave PIC first: */
		{ 0xa0, 0x11 }, { 0xa1, 0x10 }, { 0xa1, 0x02 }, { 0xa1, 0x01 },
		{ 0xa1, 0xff },
		/* Master PIC: */
		{ 0x20, 0x11 }, { 0x21, 0x08 }, { 0x21, 0x04 }, { 0x21, 0x01 },
		{ 0x21, 0xff },
	};
	uint64_t banks = 0;
	uint8_t cmos_idx, cmos_val;
	int i, n_banks;

	n_banks = d->ram_size / (32 * 1024 * 1024);
	if (n_banks == 0)
		n_banks = 1;

	for (i = 0; i < n_banks && i < 4; i++)
		banks |= (uint64_t)0x80 << (8 * i);

	/* Initialize PICs: */
	for (i = 0; i < (int)(sizeof(pic_init)/sizeof(pic_init[0])); i++) {
		uint8_t val = pic_init[i].value;
		jensen_bus_access(cpu, cpu->mem, JENSEN_ISA_PORTBASE + pic_init[i].port,
		    &val, 1, MEM_WRITE);
	}

	/* Initialize CMOS registers 0x69 = 0x9d, 0x6a = 0x07: */
	cmos_idx = 0x69;
	jensen_bus_access(cpu, cpu->mem, JENSEN_ISA_PORTBASE + 0x70, &cmos_idx, 1,
	    MEM_WRITE);
	cmos_val = 0x9d;
	jensen_bus_access(cpu, cpu->mem, JENSEN_ISA_PORTBASE + 0x71, &cmos_val, 1,
	    MEM_WRITE);

	cmos_idx = 0x6a;
	jensen_bus_access(cpu, cpu->mem, JENSEN_ISA_PORTBASE + 0x70, &cmos_idx, 1,
	    MEM_WRITE);
	cmos_val = 0x07;
	jensen_bus_access(cpu, cpu->mem, JENSEN_ISA_PORTBASE + 0x71, &cmos_val, 1,
	    MEM_WRITE);

	/* CPU registers on SROM handoff: */
	cpu->cd.alpha.r[ALPHA_A4] = d->ram_size >> 20;
	cpu->cd.alpha.r[ALPHA_A5] = 0;
	cpu->cd.alpha.r[ALPHA_T10] = 6600;
	cpu->cd.alpha.r[ALPHA_T11] = 0;
	cpu->cd.alpha.r[ALPHA_AT] = banks;
	cpu->cd.alpha.r[ALPHA_RA] = 0x30363030;

	cpu->cd.alpha.pal_base = 0x4000;
	cpu->cd.alpha.palmode = 1;

	/* Load first 512 KiB from Flash ROM to physical RAM at 0x4000: */
	if (d->have_rom) {
		size_t boot_size = d->rom_size > (512 * 1024) ? (512 * 1024) : d->rom_size;
		size_t pos = 0;
		while (pos < boot_size) {
			size_t chunk = boot_size - pos;
			if (chunk > 4096)
				chunk = 4096;
			cpu->memory_rw(cpu, cpu->mem, 0x4000 + pos,
			    d->rom + pos, chunk,
			    MEM_WRITE, PHYSICAL | NO_EXCEPTIONS);
			pos += chunk;
		}
	}

	return ALPHA_PALMODE_TAG | 0x4040;
}
