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
 *  COMMENT: DEC 3000 (TURBOchannel Alpha) I/O space
 *
 *  The whole TURBOchannel I/O space of a DEC 3000/400 (physical 1.0000.0000
 *  to 1.FFFF.FFFF): CPU accesses through dense and sparse space are turned
 *  into longword TURBOchannel accesses (with a byte mask) to the slots:
 *
 *	slots 3..5	TURBOchannel options 0..2 (none yet: bus timeout)
 *	slot 6		dual SCSI ASIC (TCDS) with two 53C94s, TURBOchannel
 *			ASIC CSRs
 *	slot 7		IOCTL ASIC: system FEPROM, IOCTL registers, Ethernet
 *			address ROM, LANCE (TODO), 2 x SCC, DS1287 RTC,
 *			79C30A (TODO)
 *
 *  The SCCs and the RTC are the DECstation devices (dev_scc, dev_mc146818),
 *  registered in a memory object of their own at their IOCTL offsets; the
 *  53C94s are dev_asc, each in a memory object of its own. SCSI bus A
 *  has the disk images with IDs 0..7, bus B those with IDs 8..15.
 *
 *  Interrupts: the IOCTL's SIR (masked by SIMR) is TURBOchannel interrupt
 *  line 7, the TCDS's CIR (masked by IMER) line 6; the TURBOchannel lines (masked by IMR for the option slots) go
 *  to the CPU's irq_h[3]; uncorrectable errors in the IR to irq_h[4]. The
 *  RTC is connected to irq_h[1] by the machine.
 *
 *  Reference: DEC 3000 300/400/500/600/700/800/900 AXP Models System
 *  Programmer's Manual (EK-D3SYS-PM.B01), chapters 2, 3, 4, 7, 8, 9,
 *  appendix A.
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


#define	DEC3000_IO_BASE		0x100000000ULL
#define	DEC3000_IO_LENGTH	0x100000000ULL

#define	ROM_SIZE		(256 * 1024)

/*  TURBOchannel interrupt lines (IOSLOT/IR bit numbers):  */
#define	TC_LINE_SCSI		6
#define	TC_LINE_IOCTL		7

/*  IR error bits:  */
#define	IR_TIMEOUT		0x08000000
#define	IR_ERRORS		0xfff80000
#define	IR_CORRECTED		0x02000000

/*  Offsets in the IOCTL (slot 7):  */
#define	IOCTL_ROM		0x000000
#define	IOCTL_REGS		0x040000
#define	IOCTL_ESAR		0x080000
#define	IOCTL_LANCE		0x0c0000
#define	IOCTL_SCC0		0x100000
#define	IOCTL_SCC1		0x180000
#define	IOCTL_RTC		0x200000
#define	IOCTL_ISDN		0x240000

/*  IOCTL registers (offset >> 4):  */
#define	IOCTL_SSR		(0x100 >> 4)
#define	IOCTL_SIR		(0x110 >> 4)
#define	IOCTL_SIMR		(0x120 >> 4)
#define	IOCTL_SAR		(0x130 >> 4)
#define	N_IOCTL_REGS		32

#define	SSR_ROM_SINGLE		(1 << 26)
#define	SSR_ROM_SELECT		(1 << 9)	/*  TODO: and <7>?  */


struct dec3000_data {
	struct machine		*machine;

	/*  Devices behind the IOCTL, at their IOCTL offsets:  */
	struct memory		*ioctl_mem;

	unsigned char		sysrom[ROM_SIZE];
	unsigned char		iorom[ROM_SIZE];
	int			have_sysrom, have_iorom;
	int			n_banks;	/*  32 MB each  */

	/*  TURBOchannel ASIC:  */
	uint32_t		ioslot;
	uint32_t		tcconfig;
	uint32_t		fadr, tcereg;
	int			err_locked;
	uint32_t		imr;		/*  <5:0>, 1 = disabled  */
	uint32_t		ir_errors;	/*  <31:19>  */
	uint32_t		tc_lines;	/*  <8:0>  */
	int64_t			tcreset_end;	/*  in instructions  */
	uint32_t		mcr[8];
	uint32_t		sgmap[32768];
	struct interrupt	irq_io;		/*  cpu irq_h[3]  */
	struct interrupt	irq_err;	/*  cpu irq_h[4]  */
	struct interrupt	irq_corr;	/*  cpu irq_h[2]  */
	int			irq_io_on, irq_err_on, irq_corr_on;

	/*  IOCTL:  */
	uint32_t		ioctl_reg[N_IOCTL_REGS];
	uint32_t		sir_dev;	/*  SIR<15:0>, from devices  */
	unsigned char		esar[32];

	/*  TCDS:  */
	struct memory		*scsi_mem[2];
	void			*asc[2];
	int			asc_irq[2];
	uint32_t		tcds_cir;
	uint32_t		tcds_imer;
	uint32_t		tcds_sda[2], tcds_dic[2];
	uint32_t		tcds_dud0[2], tcds_dud1[2];
	uint32_t		tcds_buf[2][32];

	/*  AMD 79C30A (ISDN/audio), registers only:  */
	uint8_t			isdn_cr;		/*  indirect address  */
	int			isdn_idx;
	uint8_t			isdn_ind[256][16];

	/*  LANCE stub:  */
	uint16_t		lance_rap;
	uint16_t		lance_csr[4];
};


/*****************************************************************************
 *
 *  Interrupts
 */

static void update_cpu_irqs(struct dec3000_data *d)
{
	uint32_t lines = d->tc_lines & ~(d->imr & 0x3f) & 0x1ff;
	int io = lines != 0;
	int err = (d->ir_errors & IR_ERRORS & ~IR_CORRECTED) != 0;
	int corr = (d->ir_errors & IR_CORRECTED) != 0;

	if (io != d->irq_io_on) {
		if (io)
			INTERRUPT_ASSERT(d->irq_io);
		else
			INTERRUPT_DEASSERT(d->irq_io);
		d->irq_io_on = io;
	}
	if (err != d->irq_err_on) {
		if (err)
			INTERRUPT_ASSERT(d->irq_err);
		else
			INTERRUPT_DEASSERT(d->irq_err);
		d->irq_err_on = err;
	}
	if (corr != d->irq_corr_on) {
		if (corr)
			INTERRUPT_ASSERT(d->irq_corr);
		else
			INTERRUPT_DEASSERT(d->irq_corr);
		d->irq_corr_on = corr;
	}
}

static void set_tc_line(struct dec3000_data *d, int line, int on)
{
	if (on)
		d->tc_lines |= 1 << line;
	else
		d->tc_lines &= ~(1 << line);
	update_cpu_irqs(d);
}

/*  The IOCTL interrupts the TURBOchannel ASIC if SIR & SIMR:  */
static void update_ioctl_irq(struct dec3000_data *d)
{
	uint32_t sir = (d->ioctl_reg[IOCTL_SIR] & 0xffff0000) | d->sir_dev;

	set_tc_line(d, TC_LINE_IOCTL, (sir & d->ioctl_reg[IOCTL_SIMR]) != 0);
}

/*  Device interrupts into SIR<15:0>: "<machine>.dec3000.<bit>"  */
static void ioctl_irq_assert(struct interrupt *interrupt)
{
	struct dec3000_data *d = (struct dec3000_data *) interrupt->extra;
	d->sir_dev |= 1 << interrupt->line;
	update_ioctl_irq(d);
}
static void ioctl_irq_deassert(struct interrupt *interrupt)
{
	struct dec3000_data *d = (struct dec3000_data *) interrupt->extra;
	d->sir_dev &= ~(1 << interrupt->line);
	update_ioctl_irq(d);
}


/*  A TURBOchannel timeout (nothing in a slot, or a register that doesn't
    respond): latched in IR and the error registers.  */
static void tc_timeout(struct dec3000_data *d, uint32_t tc_addr, int slot,
	int writeflag)
{
	d->ir_errors |= IR_TIMEOUT;
	if (!d->err_locked) {
		d->fadr = tc_addr & 0x07ffffff;
		d->tcereg = (slot & 0xf) | 0x80 | (writeflag? 0x40000000 : 0);
		d->err_locked = 1;
	}
	update_cpu_irqs(d);
}


/*****************************************************************************
 *
 *  Devices behind the IOCTL (registered in d->ioctl_mem)
 */

static int sub_access(struct memory *mem, struct cpu *cpu,
	uint64_t addr, unsigned char *data, size_t len, int writeflag)
{
	int i;

	for (i = 0; i < mem->n_mmapped_devices; i++) {
		struct memory_device *dev = &mem->devices[i];
		if (addr >= dev->baseaddr && addr + len <= dev->endaddr)
			return dev->f(cpu, mem, addr - dev->baseaddr, data,
			    len, writeflag, dev->extra);
	}
	return 0;
}

/*  An 8-bit device on byte lane 'lane' of a bus:  */
static void byte_device(struct memory *mem, struct cpu *cpu,
	uint32_t off, int lane, uint32_t *val, int bytemask, int writeflag)
{
	unsigned char b;

	if (writeflag) {
		if (!(bytemask & (1 << lane)))
			return;
		b = *val >> (8 * lane);
		sub_access(mem, cpu, off + lane, &b, 1, MEM_WRITE);
	} else {
		b = 0;
		sub_access(mem, cpu, off + lane, &b, 1, MEM_READ);
		*val = (uint32_t)b << (8 * lane);
	}
}


static void rom_access(struct dec3000_data *d, uint32_t off, uint32_t *val,
	int writeflag)
{
	unsigned char *rom = (d->ioctl_reg[IOCTL_SSR] & SSR_ROM_SELECT)?
	    d->iorom : d->sysrom;
	int i;

	off &= ROM_SIZE - 1;

	if (writeflag) {
		/*  TODO: flash programming (28F020 commands).  */
		debugmsg(SUBSYS_DEVICE, "dec3000", VERBOSITY_INFO,
		    "FEPROM write 0x%08x to offset 0x%05x ignored",
		    *val, off);
		return;
	}

	if (d->ioctl_reg[IOCTL_SSR] & SSR_ROM_SINGLE) {
		*val = rom[off];
		return;
	}

	/*  Four byte reads, packed into a longword:  */
	*val = 0;
	for (i = 3; i >= 0; i--)
		*val = (*val << 8) | rom[(off & ~3) + i];
}


static void ioctl_regs_access(struct dec3000_data *d, uint32_t off,
	uint32_t *val, int writeflag)
{
	int r = (off >> 4) & (N_IOCTL_REGS - 1);

	if (!writeflag) {
		switch (r) {
		case IOCTL_SIR:
			*val = (d->ioctl_reg[IOCTL_SIR] & 0xffff0000) |
			    d->sir_dev;
			break;
		default:*val = d->ioctl_reg[r];
		}
		return;
	}

	switch (r) {
	case IOCTL_SIR:
		/*  <31:16> are cleared by writing 0; <15:0> are read-only.  */
		d->ioctl_reg[IOCTL_SIR] &= *val | 0xffff;
		break;
	case IOCTL_SAR:
		break;
	default:
		d->ioctl_reg[r] = *val;
	}

	if (r == IOCTL_SSR)
		debugmsg(SUBSYS_DEVICE, "dec3000", VERBOSITY_DEBUG,
		    "SSR = 0x%08x", *val);

	update_ioctl_irq(d);
}


/*  The LANCE, for now only so that its registers can be read and written.
    TODO: dev_le with IOCTL DMA.  */
static void lance_stub(struct dec3000_data *d, uint32_t off, uint32_t *val,
	int writeflag)
{
	if ((off & 4) == 4) {
		/*  RAP  */
		if (writeflag)
			d->lance_rap = *val & 3;
		else
			*val = d->lance_rap;
		return;
	}

	/*  RDP  */
	if (writeflag) {
		if (d->lance_rap == 0) {
			/*  CSR0: STOP resets; other bits aren't emulated.  */
			if (*val & 4)
				d->lance_csr[0] = 4;
		} else
			d->lance_csr[d->lance_rap] = *val;
	} else
		*val = d->lance_csr[d->lance_rap];
}


/*
 *  The AMD 79C30A's registers, so that they can be written and read back:
 *  direct registers 0..7 at 4-byte steps; CR (0) selects an indirect
 *  register, whose bytes are then read and written through DR (1). The
 *  data is on byte lane 1 (IOBUS HI), but reads give it on lanes 0 and 1,
 *  and a write with nothing on lane 1 takes lane 0. TODO: the codec, the
 *  D channel, interrupts.
 */
static void isdn_access(struct dec3000_data *d, uint32_t off, uint32_t *val,
	int writeflag)
{
	int reg = (off >> 2) & 7;
	uint8_t b = 0;

	if (writeflag)
		b = (*val & 0xff00)? (*val >> 8) : *val;

	switch (reg) {
	case 0:	/*  W: CR, R: IR (interrupt register)  */
		if (writeflag) {
			d->isdn_cr = b;
			d->isdn_idx = 0;
		} else
			b = 0;
		break;
	case 1:	/*  DR  */
		if (writeflag)
			d->isdn_ind[d->isdn_cr][d->isdn_idx] = b;
		else
			b = d->isdn_ind[d->isdn_cr][d->isdn_idx];
		d->isdn_idx = (d->isdn_idx + 1) & 15;
		break;
	default:
		/*  Status and buffer registers: nothing pending.  */
		b = 0;
	}

	if (!writeflag)
		*val = b | (b << 8);
}


static int ioctl_access(struct dec3000_data *d, struct cpu *cpu,
	uint32_t off, uint32_t *val, int bytemask, int writeflag)
{
	switch (off & ~0x3ffff) {
	case IOCTL_ROM:
		rom_access(d, off, val, writeflag);
		return 1;
	case IOCTL_REGS:
		ioctl_regs_access(d, off, val, writeflag);
		return 1;
	case IOCTL_ESAR:
		if (!writeflag)
			*val = d->esar[(off >> 2) & 31];
		return 1;
	case IOCTL_LANCE:
		lance_stub(d, off, val, writeflag);
		return 1;
	case IOCTL_SCC0:
	case IOCTL_SCC1:
		/*  RAP and data of channels B and A, at 0, 4, 8, c; the data
		    is on byte lane 1 (IOBUS HI).  */
		byte_device(d->ioctl_mem, cpu, (off & ~0x3ffff) | (off & 0xc),
		    1, val, bytemask, writeflag);
		return 1;
	case IOCTL_RTC:
		/*  Register n at n * 4, data on byte lane 0 (IOBUS LO).  */
		byte_device(d->ioctl_mem, cpu, IOCTL_RTC | (off & 0xffc), 0,
		    val, bytemask, writeflag);
		return 1;
	case IOCTL_ISDN:
		isdn_access(d, off, val, writeflag);
		return 1;
	}
	return 0;
}


/*****************************************************************************
 *
 *  TURBOchannel dual SCSI ASIC (TCDS, slot 6, offsets below 0x2000000)
 */

#define	TCDS_CIR_DMA_EN(ch)	(0x100 << (ch))
#define	TCDS_CIR_GPI		0xf0
/*
 *  The 53C94 interrupts are CIR<19:18> (enabled by IMER<3:2>), as in the
 *  SPM and in OSF/1's pdmaflam.h. <21:20> are "reserved" in the SPM, but
 *  the console's SCSI test enables them (IMER<5:4>) and waits for <20>
 *  after a selection, with DMA off; so they are set by a 53C94 interrupt
 *  too, when enabled (OSF/1 doesn't enable them), as well as by a DMA
 *  read prefetch.
 */
#define	TCDS_CIR_SCSI_INT(ch)	(0x40000 << (ch))
#define	TCDS_IMER_SCSI_INT(ch)	(0x4 << (ch))
#define	TCDS_DIC_WRITE		0x80	/*  DMA to memory  */
#define	TCDS_DIC_PREFETCH	0x40
#define	TCDS_CIR_PREFETCH(ch)	(0x100000 << (ch))

struct tcds_chan {
	struct dec3000_data	*d;
	int			ch;
};

static void update_tcds_irq(struct dec3000_data *d)
{
	int ch;

	/*  The 53C94 interrupts are levels; with their enable bits, they
	    (re)set their CIR bits.  */
	for (ch = 0; ch < 2; ch++) {
		if (!d->asc_irq[ch])
			continue;
		if (d->tcds_imer & TCDS_IMER_SCSI_INT(ch))
			d->tcds_cir |= TCDS_CIR_SCSI_INT(ch);
		if (d->tcds_imer & (0x10 << ch))
			d->tcds_cir |= TCDS_CIR_PREFETCH(ch);
	}

	set_tc_line(d, TC_LINE_SCSI,
	    (d->tcds_cir & d->tcds_imer & 0xffff0000) != 0);
}

static void scsi_irq_assert(struct interrupt *interrupt)
{
	struct dec3000_data *d = (struct dec3000_data *) interrupt->extra;
	d->asc_irq[interrupt->line] = 1;
	update_tcds_irq(d);
}
static void scsi_irq_deassert(struct interrupt *interrupt)
{
	struct dec3000_data *d = (struct dec3000_data *) interrupt->extra;
	d->asc_irq[interrupt->line] = 0;
	update_tcds_irq(d);
}


/*  A TURBOchannel DMA address to a physical address (through the
    scatter/gather map if it is enabled for the slot). Returns 0 if the
    map entry isn't valid.  */
static int tc_dma_addr(struct dec3000_data *d, int slot, uint64_t addr,
	uint64_t *paddr)
{
	/*  IOSLOT: 3 bits per slot (P, B, S); the SCSI's are <20:18>.  */
	int sbit = slot == 6? 18 : slot == 7? 21 : (slot - 3) * 3 + 9;

	if (d->ioslot & (1 << sbit)) {
		uint32_t e = d->sgmap[(addr >> 13) & 32767];
		if (!(e & 0x800000))
			return 0;
		*paddr = (((uint64_t)(e >> 4) & 0x1ffff) << 13) |
		    (addr & 0x1fff);
	} else
		*paddr = addr & 0x3fffffff;
	return 1;
}


/*
 *  tcds_dma():
 *
 *  The DMA engine of one SCSI channel, called by dev_asc to move data:
 *  writeflag is non-zero for data from the SCSI bus to memory. The address
 *  is SDA (longwords) with DIC<1:0>. At the end, SDA is where the
 *  remaining partial longword is, and DUD0/DUD1 have the partial first and
 *  last longwords of a transfer to memory (which is also written to
 *  memory directly, so that the driver merging them again does no harm).
 */
static size_t tcds_dma(void *extra, unsigned char *data, size_t len,
	int writeflag)
{
	struct tcds_chan *c = (struct tcds_chan *) extra;
	struct dec3000_data *d = c->d;
	struct cpu *cpu = d->machine->cpus[d->machine->bootstrap_cpu];
	int ch = c->ch;
	uint64_t addr, end;
	size_t done = 0;
	int i;

	if (!(d->tcds_cir & TCDS_CIR_DMA_EN(ch))) {
		debugmsg(SUBSYS_DEVICE, "dec3000", VERBOSITY_WARNING,
		    "SCSI[%i] DMA of %i bytes, but DMA is not enabled", ch,
		    (int) len);
		return 0;
	}
	if (((d->tcds_dic[ch] & TCDS_DIC_WRITE) != 0) != (writeflag != 0))
		debugmsg(SUBSYS_DEVICE, "dec3000", VERBOSITY_WARNING,
		    "SCSI[%i] DMA %s memory, but DIC says otherwise", ch,
		    writeflag? "to" : "from");

	addr = ((uint64_t) d->tcds_sda[ch] << 2) | (d->tcds_dic[ch] & 3);
	end = addr + len;

	while (done < len) {
		uint64_t paddr;
		size_t n = 0x2000 - ((addr + done) & 0x1fff);
		if (n > len - done)
			n = len - done;
		if (!tc_dma_addr(d, 6, addr + done, &paddr)) {
			/*  Scatter/gather entry not valid  */
			d->ir_errors |= 0x40000000;
			d->tcds_cir |= 0x400000 << ch;
			d->tcds_cir &= ~TCDS_CIR_DMA_EN(ch);
			update_cpu_irqs(d);
			update_tcds_irq(d);
			break;
		}
		cpu->memory_rw(cpu, cpu->mem, paddr, data + done, n,
		    writeflag? MEM_WRITE : MEM_READ, PHYSICAL | NO_EXCEPTIONS);
		done += n;
	}

	if (writeflag) {
		/*  Partial first longword:  */
		d->tcds_dud0[ch] = 0;
		if (addr & 3) {
			for (i = addr & 3; i < 4 && (size_t)(i - (addr & 3))
			    < len; i++)
				d->tcds_dud0[ch] |= ((uint32_t)
				    data[i - (addr & 3)] << (8 * i)) |
				    (1 << (i - 1));
		}
		/*  Partial last longword:  */
		d->tcds_dud1[ch] = 0;
		if ((end & 3) && (end & ~3ULL) > (addr & ~3ULL)) {
			int n = end & 3;
			for (i = 0; i < n; i++)
				d->tcds_dud1[ch] |= ((uint32_t)
				    data[len - n + i] << (8 * i)) |
				    (1 << (24 + i));
		}
	}

	d->tcds_sda[ch] = (end >> 2) & 0xffffffff;
	d->tcds_dic[ch] = (d->tcds_dic[ch] & ~3) | (end & 3);

	return done;
}


/*
 *  DMA read prefetch: with DMA enabled towards the SCSI bus and prefetch
 *  on, the ASIC fills its DMA buffer from memory and then says so in
 *  CIR<21:20> (see TCDS_CIR_SCSI_INT).
 */
static void tcds_prefetch(struct dec3000_data *d, int ch)
{
	struct cpu *cpu = d->machine->cpus[d->machine->bootstrap_cpu];
	uint64_t addr, paddr;
	unsigned char buf[128];
	int i;

	if (!(d->tcds_cir & TCDS_CIR_DMA_EN(ch)) ||
	    (d->tcds_dic[ch] & (TCDS_DIC_WRITE | TCDS_DIC_PREFETCH)) !=
	    TCDS_DIC_PREFETCH)
		return;

	addr = ((uint64_t) d->tcds_sda[ch] << 2) & ~(uint64_t)3;
	if (!tc_dma_addr(d, 6, addr, &paddr))
		return;
	cpu->memory_rw(cpu, cpu->mem, paddr, buf, sizeof(buf), MEM_READ,
	    PHYSICAL | NO_EXCEPTIONS);
	for (i = 0; i < 32; i++)
		d->tcds_buf[ch][i] = buf[4*i] | (buf[4*i+1] << 8) |
		    (buf[4*i+2] << 16) | ((uint32_t)buf[4*i+3] << 24);

	d->tcds_cir |= TCDS_CIR_PREFETCH(ch);
	update_tcds_irq(d);
}


static int tcds_access(struct dec3000_data *d, struct cpu *cpu,
	uint32_t off, uint32_t *val, int writeflag)
{
	int ch;

	switch (off & 0x3c0000) {
	case 0x000000:
		/*  FEPROM: none on the built-in TCDS.  */
		if (!writeflag)
			*val = 0xffffffff;
		return 1;

	case 0x040000:
		if ((off & 0xffff) < 0x1000) {
			if ((off & 0xffc) == 0) {
				/*  CIR  */
				if (writeflag) {
					d->tcds_cir = (d->tcds_cir &
					    (*val | 0xffff) & 0xffff0000) |
					    (*val & 0xff0f);
					update_tcds_irq(d);
					tcds_prefetch(d, 0);
					tcds_prefetch(d, 1);
				} else
					*val = d->tcds_cir | TCDS_CIR_GPI;
			} else if ((off & 0xffc) == 4) {
				/*  IMER  */
				if (writeflag) {
					d->tcds_imer = *val;
					update_tcds_irq(d);
				} else
					*val = d->tcds_imer;
			} else if (!writeflag)
				*val = 0;
			return 1;
		}
		ch = (off >> 8) & 1;
		switch (off & 0xfc) {
		case 0x00:
			if (writeflag)
				d->tcds_sda[ch] = *val;
			else
				*val = d->tcds_sda[ch];
			break;
		case 0x04:
			if (writeflag) {
				d->tcds_dic[ch] = *val & 0xc3;
				tcds_prefetch(d, ch);
			} else
				*val = d->tcds_dic[ch];
			break;
		case 0x08:
			if (!writeflag)
				*val = d->tcds_dud0[ch];
			break;
		case 0x0c:
			if (!writeflag)
				*val = d->tcds_dud1[ch];
			break;
		default:if (!writeflag)
				*val = 0;
		}
		return 1;

	case 0x080000:
		/*  53C94 registers, at 4-byte steps, on byte lane 0.  */
		ch = (off >> 8) & 1;
		byte_device(d->scsi_mem[ch], cpu, off & 0xfc, 0, val, 0xf,
		    writeflag);
		return 1;

	case 0x0c0000:
		ch = (off >> 8) & 1;
		if (writeflag)
			d->tcds_buf[ch][(off >> 2) & 31] = *val;
		else
			*val = d->tcds_buf[ch][(off >> 2) & 31];
		return 1;
	}

	return 0;
}


/*****************************************************************************
 *
 *  TURBOchannel ASIC CSRs (slot 6, offsets 0x2000000 and up)
 */

#define	IR_RESET		0x00400000

/*  Emulated time, in CPU cycles (as the cycle counter):  */
static int64_t now(struct dec3000_data *d)
{
	return ev4_cycles(d->machine->cpus[d->machine->bootstrap_cpu]);
}

static uint32_t read_ir(struct dec3000_data *d)
{
	uint32_t ir = (d->ir_errors & IR_ERRORS) | (d->tc_lines & 0x1ff);

	/*  A TURBOchannel reset takes 250 ms.  */
	if (now(d) < d->tcreset_end)
		ir |= IR_RESET;
	return ir;
}

static int tc_csr_access(struct dec3000_data *d, uint32_t off,
	uint32_t *val, int writeflag, int sparse)
{
	off &= 0x1ffffff;

	if (off < 0x200000) {
		switch (off & 0x38) {
		case 0x00:
		case 0x20:
			/*  The PBS bits <26:0> are written at 0x00, the byte
			    mask and valid bits <31:27> at 0x20 (the
			    "alternate address"); both read the whole.  */
			if (writeflag) {
				if ((off & 0x38) == 0)
					d->ioslot = (d->ioslot & 0xf8000000) |
					    (*val & 0x07ffffff);
				else
					d->ioslot = (d->ioslot & 0x07ffffff) |
					    (*val & 0xf8000000);
			} else
				*val = d->ioslot;
			return 1;
		case 0x08:
			if (writeflag)
				d->tcconfig = *val;	/*  (all bits read back)  */
			else
				*val = d->tcconfig;
			return 1;
		case 0x10:
		case 0x18:
			if (writeflag) {
				/*  A write to either one unlocks both (and,
				    as the console's test expects, they read
				    back what was written, without the lock
				    bit).  */
				if ((off & 0x38) == 0x10)
					d->fadr = *val;
				else
					d->tcereg = *val & ~0x80;
				d->tcereg &= ~0x80;
				d->err_locked = 0;
				return 1;
			}
			*val = (off & 0x38) == 0x10? d->fadr : d->tcereg;
			return 1;
		}
		if (!writeflag)
			*val = 0;
		return 1;
	}

	if (off < 0x400000) {
		/*  Memory configuration registers (the data is in the
		    address), and the victim address registers.  */
		int n = (off >> 16) & 7;
		if (off & 0x80000) {
			if (!writeflag)
				*val = 0;
			return 1;
		}
		if (writeflag)
			d->mcr[n] = off & 0xffe0;
		else
			*val = d->mcr[n];
		return 1;
	}

	if (off < 0x800000) {
		/*  Dense: IMR (and at 0x600000 clear IR). Sparse: IR (and at
		    0x600000 clear IR).  */
		if (writeflag)
			return 1;
		if (sparse)
			*val = read_ir(d);
		else
			*val = (d->ir_errors & IR_ERRORS) | (d->imr & 0x3f);
		if (off >= 0x600000) {
			d->ir_errors = 0;
			update_cpu_irqs(d);
		}
		return 1;
	}

	if (off < 0x820000) {
		/*  Scatter/gather map (sparse writes), IMR (dense writes)  */
		int i = (off >> 2) & 32767;
		if (writeflag) {
			if (sparse) {
				uint32_t x = *val & 0xdffff0;
				int bit, parity = 1;
				for (bit = 0; bit < 24; bit++)
					parity ^= (x >> bit) & 1;
				/*  Odd parity in bit 21:  */
				d->sgmap[i] = x | (parity << 21);
			} else {
				d->imr = *val & 0x3f;
				update_cpu_irqs(d);
			}
		} else
			*val = d->sgmap[i];
		return 1;
	}

	if (off >= 0xa00000 && off < 0xa20000) {
		/*  TCRESET: doesn't respond to reads.  */
		if (!writeflag)
			return 0;
		debugmsg(SUBSYS_DEVICE, "dec3000", VERBOSITY_INFO,
		    "TURBOchannel reset");
		d->tcreset_end = now(d) + d->machine->emulated_hz / 4;
		return 1;
	}

	if (!writeflag)
		*val = 0;
	return 1;
}


/*****************************************************************************
 *
 *  TURBOchannel slots
 */

/*
 *  A longword access on the TURBOchannel. bytemask: bit n set = byte n is
 *  written. Returns 0 if nothing responds (timeout).
 */
static int tc_access(struct dec3000_data *d, struct cpu *cpu, int slot,
	uint32_t off, uint32_t *val, int bytemask, int writeflag, int sparse)
{
	switch (slot) {
	case 6:
		if (off & 0x2000000)
			return tc_csr_access(d, off, val, writeflag, sparse);
		return tcds_access(d, cpu, off, val, writeflag);
	case 7:
		if (off & 0x2000000)
			return 0;		/*  CXTurbo: 500 models only  */
		return ioctl_access(d, cpu, off, val, bytemask, writeflag);
	}

	/*  Option slots 3..5 (TC0..TC2), and slots 0..2 (500 models only):
	    nothing there.  */
	return 0;
}


static uint32_t le32(unsigned char *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(unsigned char *p, uint32_t x)
{
	p[0] = x; p[1] = x >> 8; p[2] = x >> 16; p[3] = x >> 24;
}


DEVICE_ACCESS(dec3000_io)
{
	struct dec3000_data *d = (struct dec3000_data *) extra;
	uint64_t pa = DEC3000_IO_BASE + relative_addr;
	int slot = (pa >> 29) & 7, sparse = (pa >> 28) & 1;
	uint32_t off = pa & 0x0fffffff, tc, val;
	int ok = 1;

	if (sparse) {
		/*  Even longwords: data, at the dense offset * 2. Odd
		    longwords: byte masks (written with STQ).  */
		tc = (off >> 1) & ~3;
		if (writeflag == MEM_READ) {
			if (off & 4)
				val = 0;
			else
				ok = tc_access(d, cpu, slot, tc, &val, 0xf,
				    MEM_READ, 1);
			put_le32(data, val);
			if (len == 8)
				put_le32(data + 4, val);
		} else {
			int mask = 0xf;
			if (off & 4)
				return 1;
			if (len == 8) {
				mask = data[4] & 0xf;
				if (mask == 0)
					mask = 0xf;
			}
			val = le32(data);
			ok = tc_access(d, cpu, slot, tc, &val, mask,
			    MEM_WRITE, 1);
		}
	} else {
		/*  Dense: longwords, or two of them for a quadword.  */
		int i;
		tc = off & ~3;
		for (i = 0; i < (len == 8? 2 : 1) && ok; i++) {
			if (writeflag == MEM_WRITE) {
				val = le32(data + 4 * i);
				ok = tc_access(d, cpu, slot, tc + 4 * i, &val,
				    0xf, MEM_WRITE, 0);
			} else {
				val = 0;
				ok = tc_access(d, cpu, slot, tc + 4 * i, &val,
				    0xf, MEM_READ, 0);
				put_le32(data + 4 * i, val);
			}
		}
	}

	if (!ok) {
		tc_timeout(d, tc, slot, writeflag);
		if (writeflag == MEM_READ)
			memset(data, 0xff, len);
		/*  The CPU gets a machine check (see memory_rw.c).  */
		return 0;
	}
	return 1;
}


/*****************************************************************************
 *
 *  Main memory beyond the installed RAM, and the non-cacheable alias
 */

DEVICE_ACCESS(dec3000_nomem)
{
	/*  Nonexistent memory reads UNPREDICTABLE data (no error).  */
	if (writeflag == MEM_READ)
		memset(data, 0, len);
	return 1;
}


/*****************************************************************************
 *
 *  Setup
 */

/*  The Ethernet address ROM, with its checksum and test patterns.  */
static void make_esar(struct dec3000_data *d, const unsigned char *a)
{
	uint32_t sum = 0;
	int i;

	for (i = 0; i < 3; i++) {
		sum <<= 1;
		if (sum > 0xffff)
			sum -= 0xffff;
		sum += (a[2*i] << 8) | a[2*i + 1];
		if (sum > 0xffff)
			sum -= 0xffff;
	}
	if (sum == 0xffff)
		sum = 0;

	for (i = 0; i < 6; i++) {
		d->esar[i] = a[i];
		d->esar[15 - i] = a[i];
		d->esar[16 + i] = a[i];
	}
	d->esar[6] = d->esar[9] = d->esar[22] = sum >> 8;
	d->esar[7] = d->esar[8] = d->esar[23] = sum & 0xff;
	for (i = 0; i < 8; i++)
		d->esar[24 + i] = "\xff\x00\x55\xaa"[i & 3];
}


static void add_ioctl_irq_line(struct dec3000_data *d, int bit)
{
	struct interrupt templ;
	char name[300];

	snprintf(name, sizeof(name), "%s.dec3000.%i", d->machine->path, bit);
	memset(&templ, 0, sizeof(templ));
	templ.line = bit;
	templ.name = name;
	templ.extra = d;
	templ.interrupt_assert = ioctl_irq_assert;
	templ.interrupt_deassert = ioctl_irq_deassert;
	interrupt_handler_register(&templ);
}


/*
 *  dev_dec3000_init():
 *
 *  ram_size is the installed memory, in 32 MB banks (MCRs are set as the
 *  serial ROM would leave them). cpu_irq is the path of the CPU's
 *  interrupt pins ("machine[0].cpu[0]").
 */
struct dec3000_data *dev_dec3000_init(struct machine *machine,
	struct memory *mem, uint64_t ram_size, const char *cpu_irq)
{
	struct dec3000_data *d;
	char tmp[300];
	unsigned char ether[6] = { 0x08, 0x00, 0x2b, 0x30, 0x00, 0x01 };
	int i, banks;

	CHECK_ALLOCATION(d = (struct dec3000_data *) calloc(1, sizeof(*d)));
	d->machine = machine;
	d->ioctl_mem = memory_new(0);
	d->tcconfig = 0x14;		/*  as left by the serial ROM  */
	d->imr = 0x3f;

	snprintf(tmp, sizeof(tmp), "%s.2", cpu_irq);
	INTERRUPT_CONNECT(tmp, d->irq_corr);
	snprintf(tmp, sizeof(tmp), "%s.3", cpu_irq);
	INTERRUPT_CONNECT(tmp, d->irq_io);
	snprintf(tmp, sizeof(tmp), "%s.4", cpu_irq);
	INTERRUPT_CONNECT(tmp, d->irq_err);

	/*  Memory: 32 MB banks from 0; the others mapped out.  */
	banks = (ram_size + (32 << 20) - 1) / (32 << 20);
	d->n_banks = banks;
	for (i = 0; i < 8; i++) {
		if (i < banks && i < 4)
			d->mcr[i] = ((i * 4) << 9) | (0x3 << 5);
		else
			d->mcr[i] = 0x70 << 9;
	}
	if (ram_size < 0x40000000ULL)
		memory_device_register(mem, "dec3000_nomem", ram_size,
		    0x100000000ULL - ram_size, dev_dec3000_nomem_access, d,
		    DM_DEFAULT, NULL);

	memory_device_register(mem, "dec3000_io", DEC3000_IO_BASE,
	    DEC3000_IO_LENGTH, dev_dec3000_io_access, d, DM_DEFAULT, NULL);

	/*  IOCTL devices:  */
	for (i = 0; i < 16; i++)
		add_ioctl_irq_line(d, i);

	snprintf(tmp, sizeof(tmp), "%s.dec3000.6", machine->path);
	dev_scc_init(machine, d->ioctl_mem, IOCTL_SCC0, tmp,
	    machine->display_md.in_use, 0, 1);
	snprintf(tmp, sizeof(tmp), "%s.dec3000.7", machine->path);
	dev_scc_init(machine, d->ioctl_mem, IOCTL_SCC1, tmp,
	    machine->display_md.in_use, 1, 1);

	snprintf(tmp, sizeof(tmp), "%s.1", cpu_irq);
	dev_mc146818_init(machine, d->ioctl_mem, IOCTL_RTC, tmp,
	    MC146818_DEC, 1);

	/*  The dual SCSI ASIC's 53C94s:  */
	for (i = 0; i < 2; i++) {
		struct interrupt templ;
		struct tcds_chan *c;
		char name[300];

		snprintf(name, sizeof(name), "%s.dec3000.scsi.%i",
		    machine->path, i);
		memset(&templ, 0, sizeof(templ));
		templ.line = i;
		templ.name = name;
		templ.extra = d;
		templ.interrupt_assert = scsi_irq_assert;
		templ.interrupt_deassert = scsi_irq_deassert;
		interrupt_handler_register(&templ);

		CHECK_ALLOCATION(c = (struct tcds_chan *) malloc(sizeof(*c)));
		c->d = d;
		c->ch = i;
		d->scsi_mem[i] = memory_new(0);
		d->asc[i] = dev_asc_init(machine, d->scsi_mem[i], 0, name,
		    NULL, DEV_ASC_DEC, tcds_dma, c);
		dev_asc_set_target_offset(d->asc[i], 8 * i);
	}

	make_esar(d, ether);
	d->lance_csr[0] = 4;			/*  STOP  */

	return d;
}


/*
 *  dev_dec3000_set_rom():
 *
 *  Load the system (sys != 0) or I/O ROM image.
 */
void dev_dec3000_set_rom(struct dec3000_data *d, int sys,
	const unsigned char *image, size_t len)
{
	unsigned char *rom = sys? d->sysrom : d->iorom;

	memset(rom, 0xff, ROM_SIZE);
	memcpy(rom, image, len > ROM_SIZE? ROM_SIZE : len);
	if (sys)
		d->have_sysrom = 1;
	else
		d->have_iorom = 1;
}


/*
 *  dev_dec3000_srom():
 *
 *  What the serial ROM does after memory sizing and tests: copy the
 *  contents of the system ROM (after its 0x40-byte header, the
 *  "manufacturing data") to 0x4000, and those of the I/O ROM to 0x44000;
 *  the decompressor at the start of the system ROM expects them there.
 *  Returns the entry point.
 */
#define	ROM_HEADER_SIZE		0x40

uint64_t dev_dec3000_srom(struct dec3000_data *d, struct cpu *cpu)
{
	uint64_t banks = 0;
	int i;

	/*
	 *  The serial ROM's results, in registers (the console saves them
	 *  in its PALcode's data, and reads them from there):
	 *
	 *	at	memory banks: byte n for bank n, size code in bits
	 *		<7:6> (1 = 8 MB, 2 = 32 MB, 3 = 128 MB, 0 = none)
	 *	a4	memory size in MB
	 *
	 *  (Found from the console code; the other registers, ra, t10,
	 *  a1..a3, s1, s2, t6, t7, are not known yet and are left 0.)
	 */
	for (i = 0; i < d->n_banks && i < 8; i++)
		banks |= (uint64_t)0x80 << (8 * i);
	cpu->cd.alpha.r[ALPHA_AT] = banks;
	cpu->cd.alpha.r[ALPHA_A4] = d->n_banks * 32;

	/*  (Called after each ROM file is loaded.)  */
	if (d->have_sysrom)
		cpu->memory_rw(cpu, cpu->mem, 0x4000,
		    d->sysrom + ROM_HEADER_SIZE, ROM_SIZE - ROM_HEADER_SIZE,
		    MEM_WRITE, PHYSICAL | NO_EXCEPTIONS);
	if (d->have_iorom)
		cpu->memory_rw(cpu, cpu->mem, 0x44000,
		    d->iorom + ROM_HEADER_SIZE, ROM_SIZE - ROM_HEADER_SIZE,
		    MEM_WRITE, PHYSICAL | NO_EXCEPTIONS);

	return 0x4000;
}
