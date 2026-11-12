/*
 *  Copyright (C) 2007-2021  Anders Gavare.  All rights reserved.
 *  Copyright (C) 2026  Rico Pajarola.
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
 *  COMMENT: MVME187 board logic (ROM, SRAM, VMEchip2, PCCchip2, MEMC040)
 *
 *  References: "Single Board Computers Programmer's Reference Guide"
 *  (VMESBCA1/PG1 and PG2, June 1996), "MVME187 Installation Guide".
 *
 *	$FF800000-$FFBFFFFF	EPROM (187Bug), also at 0 while ROM0 is set
 *	$FFE00000-$FFEFFFFF	128 KB SRAM (repeated)
 *	$FFF40000		VMEchip2 LCSR, $FFF40100 GCSR
 *	$FFF42000		PCCchip2
 *	$FFF43000		MEMC040
 *	$FFFE0000		interrupt acknowledge (MVME187 only)
 *
 *  The 88100 has a single interrupt input. The PCCchip2 combines its own
 *  interrupt sources with those of the VMEchip2 (on its EIPL inputs) and
 *  asserts INT when the highest level is above its Interrupt Mask Level
 *  Register. Software reads the level from the Interrupt Priority Level
 *  Register and the vector from the IACK space; the PCCchip2 answers first,
 *  then the VMEchip2 (daisy chain). The CD2401 supplies its own vectors.
 *
 *  Unacknowledged local bus cycles (reserved areas, the VMEbus without
 *  boards) end with TEA, which the CMMU reports as a bus error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "cpu.h"
#include "device.h"
#include "devices.h"
#include "emul.h"
#include "interrupt.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "thirdparty/mvme187.h"
#include "thirdparty/mvme88k_vme.h"
#include "thirdparty/mvme_pcctworeg.h"
#include "thirdparty/m8820x.h"


#define	ROM_BASE		0xff800000
#define	ROM_WINDOW		0x400000
#define	SRAM_BASE		0xffe00000
#define	SRAM_SIZE		0x20000
#define	SRAM_WINDOW		0x100000
#define	LCSR_BASE		0xfff40000
#define	GCSR_BASE		0xfff40100
#define	PCC_BASE		0xfff42000
#define	MEMC_BASE		0xfff43000
#define	IACK_BASE		0xfffe0000

#define	DEV_MVME187_TICK_SHIFT	14

/*  VMEchip2 register $30, bit 20:  */
#define	VME2_ROM0		0x00100000

/*  PCCchip2 interrupt sources (bottom four bits of the vector):  */
#define	N_PCC_SOURCES		16


struct tick_timer {
	uint32_t	count;
	int		overflows;
};

/*  A prescaler: makes the tick timers' clock from the bus clock.  */
struct prescaler {
	uint64_t	last_bclk;
	uint64_t	frac;
};

struct mvme187_data {
	int		subsys;		/*  for debugmsg  */
	struct machine	*machine;
	struct cpu	*cpu;
	struct interrupt cpu_irq;
	int		irq_asserted;
	uint64_t	stall;		/*  wait states (bus clocks)  */

	uint8_t		*rom;		/*  ROM_WINDOW bytes  */
	uint8_t		*rom_at_0;	/*  a copy (dyntrans needs its own)  */
	bool		rom0;
	uint8_t		*sram;

	/*  VMEchip2:  */
	uint32_t	lcsr[0x100 / 4];
	uint8_t		gcsr[0x100];
	uint32_t	vme_irqstat;
	struct tick_timer vtimer[2];
	struct prescaler vpre;

	/*  PCCchip2:  */
	uint8_t		pcc[0x40];
	bool		pcc_vbr_written;
	struct tick_timer ptimer[2];
	struct prescaler ppre;
	bool		ext_irq[N_PCC_SOURCES];	/*  device request lines  */
	bool		pin_active[0x40];	/*  edge detectors, by ICR  */

	/*  CD2401 interrupt acknowledge (type 1 = modem, 2 = tx, 3 = rx),
	    and reset:  */
	int		(*scc_iack)(void *, int);
	void		(*scc_reset)(void *);
	void		*scc;

	/*  A local reset (GCSR RST, or SYSRESET) is done at the next tick:  */
	bool		reset_pending;

	/*  MEMC040:  */
	uint8_t		memc[2][0x20];
	int		n_memc;

	/*  DRAM parity: bytes written with wrong parity (MEMC040 WWP), one
	    bitmap per 4 KB page (allocated when needed), and the overlay
	    device on the RAM which watches the accesses meanwhile.  */
	uint64_t	ram_size;
	uint8_t		**par_bad;
	uint32_t	*par_nbad;	/*  bad bytes, per page  */
	uint64_t	par_total;
	bool		par_overlay;

};


/*
 *  Time: the bus clock (BCLK) runs at 33 MHz. Unless the machine has an
 *  emulated_hz (then the host's clock is used), emulated time is the number
 *  of instructions run, at one per BCLK cycle; the firmware calibrates its
 *  delays from the tick timers and the RTC, so these must agree. Slow
 *  devices add wait states (the BBRAM, whose speed 187Bug tests).
 */
#define	BCLK_HZ		33000000

/*  MEMC040 RAM control and status bits, VMEchip2 MPU status:  */
#define	MEMC_EXTPEN		0x10	/*  status: parity checking possible  */
#define	MEMC_PAREN		0x02
#define	MEMC_PARINT		0x04
#define	MEMC_WWP		0x08
#define	DMASTAT_MLPE		0x00000200
#define	DMASTAT_MCLR		0x00000800
#define	DMASTAT_MPU		0x00000780	/*  MLBE, MLPE, MLOB, MLTO  */
#define	PAR_PAGE_SHIFT		12

static void parity_overlay(struct mvme187_data *d);

static uint64_t bclk(struct mvme187_data *d)
{
	struct timeval tv;
	static struct timeval start;

	if (d->machine->emulated_hz == 0)
		return d->cpu->ninstrs + d->cpu->n_translated_instrs +
		    d->stall;

	gettimeofday(&tv, NULL);
	if (start.tv_sec == 0 && start.tv_usec == 0)
		start = tv;
	return (uint64_t)(tv.tv_sec - start.tv_sec) * BCLK_HZ +
	    (uint64_t)((tv.tv_usec - start.tv_usec) * (BCLK_HZ / 1000000));
}

/*
 *  The board configuration in the BBRAM (at $FFFC1EF8, shown and set by
 *  187Bug's CNFG command), as programmed at the factory. 187Bug only
 *  enables the LAN controller with an Ethernet address there, and it uses
 *  the board identifier to tell which devices are on the board. A blank
 *  area is filled in.
 */
void dev_mvme187_brdid_defaults(struct machine *machine, uint8_t *nvram)
{
	uint8_t *p = nvram + 0x1ef8, mac[6];
	int i, sum;

	for (i = 0; i < 0x66; i++)
		if (p[i] != 0x00 && p[i] != 0xff)
			return;

	if (machine->nic_mac_set) {
		memcpy(mac, machine->nic_mac, 6);
	} else {
		/*  Motorola's, 08003E2xxxxx:  */
		mac[0] = 0x08; mac[1] = 0x00; mac[2] = 0x3e;
		mac[3] = 0x20 | (random() & 0x0f);
		mac[4] = random(); mac[5] = random();
	}

	memset(p, ' ', 0x66);
	memcpy(p + 0x00, "0100", 4);			/*  version  */
	memcpy(p + 0x04, "000000000001", 12);		/*  serial number  */
	memcpy(p + 0x10, "MVME187-03", 10);		/*  board id  */
	memcpy(p + 0x20, "01-W3827B03A", 12);		/*  artwork  */
	snprintf((char *) p + 0x30, 5, "%02i00", BCLK_HZ / 1000000);
	memcpy(p + 0x34, mac, 6);			/*  Ethernet address  */
	p[0x3a] = p[0x3b] = 0;
	memcpy(p + 0x3c, "07", 2);			/*  SCSI id  */
	memset(p + 0x3e, 0, 8);				/*  system id  */

	/*  The checksum: the 8-bit sum with end-around carry of the
	    bytes up to it, at $FFFC1FF7.  */
	for (i = 0, sum = 0; i < 0xff; i++) {
		sum += p[i];
		if (sum > 0xff)
			sum = (sum + 1) & 0xff;
	}
	p[0xff] = sum;
}


/*  Emulated time since power-up, in microseconds (also for the RTC):  */
uint64_t dev_mvme187_time_us(struct mvme187_data *d)
{
	return bclk(d) / (BCLK_HZ / 1000000);
}

static uint64_t mvme187_realtime_us(struct machine *machine)
{
	return dev_mvme187_time_us((struct mvme187_data *)
	    machine->realtime_extra);
}

/*  The same, in nanoseconds (for serial line timing):  */
uint64_t dev_mvme187_time_ns(struct mvme187_data *d)
{
	return bclk(d) * 1000 / (BCLK_HZ / 1000000);
}

/*  Calls f(cpu, extra) at a bus clock, when time is counted in
    instructions (otherwise the device has to make do with its ticks).  */
static void at_bclk(struct mvme187_data *d, uint64_t when,
	void (*f)(struct cpu *, void *), void *extra)
{
	if (d->machine->emulated_hz != 0)
		return;
	machine_add_event(d->machine, when > d->stall ? when - d->stall : 0,
	    f, extra);
}

/*  The same, at an emulated time in ns:  */
void dev_mvme187_at_ns(struct mvme187_data *d, uint64_t ns,
	void (*f)(struct cpu *, void *), void *extra)
{
	at_bclk(d, (ns * (BCLK_HZ / 1000000) + 999) / 1000, f, extra);
}

/*  A BBRAM access takes 5 bus clocks with the PCCchip2's FAST bit, or 9
    without (one is the instruction):  */
void dev_mvme187_bbram_access(struct mvme187_data *d)
{
	if (d->machine->emulated_hz == 0)
		d->stall += d->pcc[PCCTWO_GENCTL] & PCC2_GENCTL_FAST ? 4 : 8;
}

/*  A PCCchip2 or VMEchip2 register access: taken to be as slow as a fast
    BBRAM cycle (5 bus clocks). (187Bug's tick timer interrupt test needs
    its handler to take about as long as on the real board.)  */
#define	REG_WAIT_STATES		4
static void reg_access(struct mvme187_data *d)
{
	if (d->machine->emulated_hz == 0)
		d->stall += REG_WAIT_STATES;
}

/*  Number of tick timer clocks since the last call (the prescaler divides
    BCLK by 256 - adjust):  */
static uint64_t prescale(struct prescaler *p, uint64_t now, uint8_t adjust)
{
	uint64_t div = 256 - adjust, n;

	if (now < p->last_bclk)
		now = p->last_bclk;
	n = now - p->last_bclk + p->frac;
	p->last_bclk = now;
	p->frac = n % div;
	return n / div;
}


/*
 *  Tick timers (both chips have two; 1 MHz, 32-bit counter, compare
 *  register, clear on compare, 4-bit overflow counter). Returns the number
 *  of compare matches since the last update.
 */
static int timer_advance(struct tick_timer *t, uint32_t cmp, int ctl,
	uint64_t delta)
{
	uint64_t c = t->count, end;
	int hits = 0;

	if (!(ctl & PCC2_TCTL_CEN) || delta == 0)
		return 0;

	if ((ctl & PCC2_TCTL_COC) && cmp == 0) {
		/*  Every count matches, and clears the counter.  */
		hits = delta > 15 ? 15 : delta;
		c = 0;
	} else if (ctl & PCC2_TCTL_COC) {
		/*  Above the compare value: up to the wrap first.  */
		if (c > cmp) {
			uint64_t to_wrap = 0x100000000ULL - c;
			if (delta < to_wrap) {
				c += delta;
				delta = 0;
			} else {
				delta -= to_wrap;
				c = 0;
			}
		}
		end = c + delta;
		if (end >= cmp) {
			hits = 1 + (end - cmp) / cmp;
			c = (end - cmp) % cmp;
		} else
			c = end;
	} else {
		end = c + delta;
		if (c < cmp && end >= cmp)
			hits = 1;
		if (end > 0xffffffffULL) {
			/*  Wrapped; the next match is above 2^32.  */
			uint64_t next = (uint64_t) cmp + 0x100000000ULL;
			if (end >= next)
				hits += 1 + (end - next) / 0x100000000ULL;
		}
		c = end & 0xffffffffULL;
	}

	t->count = c;
	t->overflows = (t->overflows + hits) & 15;
	return hits;
}


/*****************************************************************************
 *
 *  Interrupts
 */

/*  PCCchip2 interrupt control register for each source:  */
static int pcc_icr_offset(int src)
{
	switch (src) {
	case PCC2V_PPBUSY:	return PCCTWO_PRTBICR;
	case PCC2V_PPPE:	return PCCTWO_PTRPICR;
	case PCC2V_PPSELECT:	return PCCTWO_PTRSICR;
	case PCC2V_PPFAULT:	return PCCTWO_PTRFICR;
	case PCC2V_PPACK:	return PCCTWO_PRTICR;
	case PCC2V_SCSI:	return PCCTWO_SCSIICR;
	case PCC2V_IEFAIL:	return PCCTWO_IEBERR;
	case PCC2V_IE:		return PCCTWO_IEICR;
	case PCC2V_TIMER2:	return PCCTWO_T2ICR;
	case PCC2V_TIMER1:	return PCCTWO_T1ICR;
	case PCC2V_GPIO:	return PCCTWO_GPIO_ICR;
	case 0xb:		return PCCTWO_SCCICR;	/*  modem  */
	case 0xc:		return PCCTWO_SCCRX;
	case 0xd:		return PCCTWO_SCCTX;
	}
	return -1;
}

/*  The level a PCCchip2 source requests, or 0:  */
static int pcc_source_level(struct mvme187_data *d, int src)
{
	int off = pcc_icr_offset(src);
	uint8_t icr;

	if (off < 0)
		return 0;
	icr = d->pcc[off];
	if (!(icr & PCC2_IRQ_IEN) || !(icr & PCC2_IRQ_INT))
		return 0;
	return icr & PCC2_IRQ_IPL;
}

static int pcc_level(struct mvme187_data *d)
{
	int src, level = 0;

	for (src = 0; src < N_PCC_SOURCES; src++) {
		int l = pcc_source_level(d, src);
		if (l > level)
			level = l;
	}
	return level;
}

/*  Level of a VMEchip2 local bus interrupter source (status bit nr):  */
static int vme_source_level(struct mvme187_data *d, int bit)
{
	int reg = VME2_IRQL1 / 4 + 3 - (bit / 8);
	return (d->lcsr[reg] >> ((bit & 7) * 4)) & 7;
}

static int vme_level(struct mvme187_data *d, int *srcp)
{
	uint32_t pending = d->vme_irqstat & d->lcsr[VME2_IRQEN / 4];
	int bit, level = 0;

	if (!(d->lcsr[VME2_VBR / 4] & VME2_IOCTL1_MIEN))
		return 0;

	for (bit = 31; bit >= 0; bit--) {
		if (pending & (1U << bit)) {
			int l = vme_source_level(d, bit);
			if (l > level) {
				level = l;
				if (srcp != NULL)
					*srcp = bit;
			}
		}
	}
	return level;
}

static int combined_level(struct mvme187_data *d)
{
	int lp = pcc_level(d), lv = vme_level(d, NULL);
	return lp > lv ? lp : lv;
}

static void update_irq(struct mvme187_data *d)
{
	int assert = 0;

	if ((d->pcc[PCCTWO_GENCTL] & PCC2_MIEN) &&
	    !(d->pcc[PCCTWO_GENCTL] & PCC2_C040)) {
		int level = combined_level(d);
		if (level > (d->pcc[PCCTWO_MASK] & 7))
			assert = 1;
	}

	if (assert != d->irq_asserted)
		debugmsg(d->subsys, "irq", VERBOSITY_DEBUG, "%s at %llu, pc "
		    "0x%08llx psr 0x%08x", assert ? "asserted" : "deasserted",
		    (unsigned long long) bclk(d), (unsigned long long)
		    d->cpu->pc, (unsigned) d->cpu->cd.m88k.cr[M88K_CR_PSR]);
	if (assert && !d->irq_asserted)
		INTERRUPT_ASSERT(d->cpu_irq);
	else if (!assert && d->irq_asserted)
		INTERRUPT_DEASSERT(d->cpu_irq);
	d->irq_asserted = assert;
}


/*
 *  The GPIO, LAN and printer interrupt inputs are pins with a polarity
 *  (PLTY) and edge or level (E/L*) sensitivity: the request is active
 *  when the pin is high with PLTY set, or low with PLTY clear; an edge
 *  sensitive one sets INT when the request becomes active (until ICLR),
 *  a level sensitive one follows it. Unconnected pins are pulled high.
 */
static int pin_level(struct mvme187_data *d, int icr)
{
	switch (icr) {
	case PCCTWO_GPIO_ICR:
		/*  The GPIO pin is an output if GPOE is set.  */
		if (d->pcc[PCCTWO_GPIO_PCR] & PCC2_GPIOCR_OE)
			return d->pcc[PCCTWO_GPIO_PCR] & PCC2_GPIOCR_O;
		return 1;
	case PCCTWO_IEICR:
		/*  The 82596CA's interrupt output (active low).  */
		return !d->ext_irq[PCC2V_IE];
	}
	return 1;	/*  printer status inputs: nothing connected  */
}

static void pin_update(struct mvme187_data *d, int icr)
{
	uint8_t v = d->pcc[icr];
	bool active = (v & 0x80) ? pin_level(d, icr) : !pin_level(d, icr);

	if (v & 0x40) {
		/*  Edge sensitive:  */
		if (active && !d->pin_active[icr])
			d->pcc[icr] |= PCC2_IRQ_INT;
	} else {
		if (active)
			d->pcc[icr] |= PCC2_IRQ_INT;
		else
			d->pcc[icr] &= ~PCC2_IRQ_INT;
	}
	d->pin_active[icr] = active;
}

static void pins_update(struct mvme187_data *d)
{
	pin_update(d, PCCTWO_GPIO_ICR);
	pin_update(d, PCCTWO_IEICR);
	pin_update(d, PCCTWO_PRTICR);
	pin_update(d, PCCTWO_PTRFICR);
	pin_update(d, PCCTWO_PTRSICR);
	pin_update(d, PCCTWO_PTRPICR);
	pin_update(d, PCCTWO_PRTBICR);
}


/*
 *  Interrupt request lines from the CD2401, 82596CA and 53C710. The
 *  names are "<cpu>.pcc2.<n>", n = PCC2V_SCSI, PCC2V_IE, PCC2V_SCC_M,
 *  PCC2V_SCC_TX or PCC2V_SCC_RX.
 */
static void pcc_line(struct interrupt *interrupt, int value)
{
	struct mvme187_data *d = (struct mvme187_data *) interrupt->extra;
	int line = interrupt->line, src, off;

	switch (line) {
	case PCC2V_SCSI:	src = PCC2V_SCSI; break;
	case PCC2V_IE:		src = PCC2V_IE; break;
	case PCC2V_SCC_M:	src = 0xb; break;
	case PCC2V_SCC_RX:	src = 0xc; break;
	case PCC2V_SCC_TX:	src = 0xd; break;
	default:		return;
	}

	d->ext_irq[src] = value;
	off = pcc_icr_offset(src);

	if (src == PCC2V_IE) {
		pin_update(d, off);
	} else {
		if (value)
			d->pcc[off] |= PCC2_IRQ_INT;
		else
			d->pcc[off] &= ~PCC2_IRQ_INT;
	}

	update_irq(d);
}

static void pcc_line_assert(struct interrupt *interrupt)
{
	pcc_line(interrupt, 1);
}

static void pcc_line_deassert(struct interrupt *interrupt)
{
	pcc_line(interrupt, 0);
}


/*
 *  Interrupt acknowledge at a level: returns the vector, or -1 if no
 *  device responds (the cycle then ends with a bus error).
 */
static int iack(struct mvme187_data *d, int level)
{
	static const int pcc_priority[] = { 0xd, 0xc, 0xb, PCC2V_GPIO,
	    PCC2V_TIMER1, PCC2V_TIMER2, PCC2V_IE, PCC2V_IEFAIL, PCC2V_SCSI,
	    PCC2V_PPACK, PCC2V_PPFAULT, PCC2V_PPSELECT, PCC2V_PPPE,
	    PCC2V_PPBUSY };
	size_t i;
	int src, vec;

	if (d->pcc[PCCTWO_GENCTL] & PCC2_MIEN) {
		for (i = 0; i < sizeof(pcc_priority) / sizeof(int); i++) {
			src = pcc_priority[i];
			if (pcc_source_level(d, src) != level)
				continue;

			if (src >= 0xb && src <= 0xd &&
			    !(d->pcc[pcc_icr_offset(src)] & PCC2_SCC_AVEC)) {
				/*  Direct mode: the CD2401 gives the vector.  */
				int type = src == 0xb ? 1 : src == 0xd ? 2 : 3;
				if (d->scc_iack == NULL)
					return -1;
				vec = d->scc_iack(d->scc, type);
				update_irq(d);
				return vec;
			}

			if (!d->pcc_vbr_written)
				return 0x0f;
			return (d->pcc[PCCTWO_VECBASE] & 0xf0) | src;
		}
	}

	/*  The VMEchip2:  */
	{
		uint32_t pending = d->vme_irqstat & d->lcsr[VME2_IRQEN / 4];
		int bit;

		if (!(d->lcsr[VME2_VBR / 4] & VME2_IOCTL1_MIEN))
			return -1;

		for (bit = 31; bit >= 0; bit--) {
			uint32_t vbr = d->lcsr[VME2_VBR / 4];
			if (!(pending & (1U << bit)) ||
			    vme_source_level(d, bit) != level)
				continue;
			if (bit < 7)
				return -1;	/*  VMEbus IRQs: no boards  */
			/*  Spare and software interrupts: base 1,
			    the others: base 0 (Table 4-4).  */
			if (bit < 16)
				return ((vbr >> 20) & 0xf0) | bit;
			return ((vbr >> 24) & 0xf0) | (bit - 16);
		}
	}

	return -1;
}


/*****************************************************************************
 *
 *  Timers
 */

/*  Timer clocks until the next compare match (0 = none):  */
static uint64_t ticks_to_match(struct tick_timer *t, uint32_t cmp, int ctl)
{
	if (!(ctl & PCC2_TCTL_CEN))
		return 0;
	if ((ctl & PCC2_TCTL_COC) && cmp == 0)
		return 1;
	if (t->count < cmp)
		return cmp - t->count;
	return 0x100000000ULL - t->count + cmp;
}

/*  The bus clock at which a prescaler has given n more clocks:  */
static uint64_t prescaler_at(struct prescaler *p, uint64_t n, uint8_t adjust)
{
	return p->last_bclk + n * (256 - adjust) - p->frac;
}

void dev_mvme187_tick(struct cpu *, void *);

/*
 *  Interrupts from the tick timers come on time (not at the next tick),
 *  when emulated time is counted in instructions: an event at the next
 *  compare match of each timer which may interrupt.
 */
static void timers_schedule(struct mvme187_data *d)
{
	uint64_t next = UINT64_MAX, n, when;
	int i;

	if (d->machine->emulated_hz != 0)
		return;

	for (i = 0; i < 2; i++) {
		int ctlreg = i == 0 ? PCCTWO_T1CTL : PCCTWO_T2CTL;
		int icr = i == 0 ? PCCTWO_T1ICR : PCCTWO_T2ICR;
		int cmpreg = i == 0 ? PCCTWO_T1CMP : PCCTWO_T2CMP;
		uint32_t cmp = (d->pcc[cmpreg] << 24) | (d->pcc[cmpreg+1] << 16)
		    | (d->pcc[cmpreg+2] << 8) | d->pcc[cmpreg+3];

		if (!(d->pcc[icr] & PCC2_TTIRQ_IEN) ||
		    (d->pcc[icr] & PCC2_TTIRQ_INT))
			continue;
		n = ticks_to_match(&d->ptimer[i], cmp, d->pcc[ctlreg]);
		if (n == 0)
			continue;
		when = prescaler_at(&d->ppre, n, d->pcc[PCCTWO_PSCALEADJ]);
		if (when < next)
			next = when;
	}

	for (i = 0; i < 2; i++) {
		uint32_t bit = i == 0 ? VME2_IRQ_TIC1 : VME2_IRQ_TIC2;
		uint32_t cmp = d->lcsr[(i == 0 ? VME2_T1CMP : VME2_T2CMP) / 4];
		int ctl = (d->lcsr[VME2_TCTL / 4] >> (8 * i)) & 0xff;

		if (!(d->lcsr[VME2_IRQEN / 4] & bit) || (d->vme_irqstat & bit))
			continue;
		n = ticks_to_match(&d->vtimer[i], cmp, ctl);
		if (n == 0)
			continue;
		when = prescaler_at(&d->vpre, n, d->lcsr[VME2_TCR / 4]);
		if (when < next)
			next = when;
	}

	if (next != UINT64_MAX) {
		debugmsg(d->subsys, "timer", VERBOSITY_DEBUG, "next match at "
		    "%llu (now %llu)", (unsigned long long) next,
		    (unsigned long long) bclk(d));
		at_bclk(d, next, dev_mvme187_tick, d);
	}
}

static void timers_update(struct mvme187_data *d)
{
	uint64_t now = bclk(d);
	uint64_t pticks = prescale(&d->ppre, now, d->pcc[PCCTWO_PSCALEADJ]);
	uint64_t vticks = prescale(&d->vpre, now, d->lcsr[VME2_TCR / 4]);
	int i, hits;

	for (i = 0; i < 2; i++) {
		/*  PCCchip2 tick timer i+1:  */
		int ctlreg = i == 0 ? PCCTWO_T1CTL : PCCTWO_T2CTL;
		int icr = i == 0 ? PCCTWO_T1ICR : PCCTWO_T2ICR;
		int cmpreg = i == 0 ? PCCTWO_T1CMP : PCCTWO_T2CMP;
		uint32_t cmp = (d->pcc[cmpreg] << 24) | (d->pcc[cmpreg+1] << 16)
		    | (d->pcc[cmpreg+2] << 8) | d->pcc[cmpreg+3];

		hits = timer_advance(&d->ptimer[i], cmp, d->pcc[ctlreg], pticks);
		if (hits > 0) {
			debugmsg(d->subsys, "timer", VERBOSITY_DEBUG, "PCCchip2"
			    " timer %i: %i match(es) at %llu", i + 1, hits,
			    (unsigned long long) now);
			d->pcc[icr] |= PCC2_TTIRQ_INT;
		}
	}

	for (i = 0; i < 2; i++) {
		/*  VMEchip2 tick timer i+1:  */
		uint32_t cmp = d->lcsr[(i == 0 ? VME2_T1CMP : VME2_T2CMP) / 4];
		int ctl = (d->lcsr[VME2_TCTL / 4] >> (8 * i)) & 0xff;

		hits = timer_advance(&d->vtimer[i], cmp, ctl, vticks);
		if (hits > 0)
			d->vme_irqstat |= i == 0 ? VME2_IRQ_TIC1 : VME2_IRQ_TIC2;
	}

	update_irq(d);
}


static void local_reset(struct mvme187_data *d);

DEVICE_TICK(mvme187)
{
	struct mvme187_data *d = (struct mvme187_data *) extra;

	if (d->reset_pending) {
		d->reset_pending = false;
		local_reset(d);
		return;
	}

	timers_update(d);
	timers_schedule(d);

	if (d->par_overlay)
		parity_overlay(d);
}


/*****************************************************************************
 *
 *  ROM and SRAM
 */

DEVICE_ACCESS(mvme187_rom);

static void set_rom0(struct mvme187_data *d, bool on)
{
	struct memory *mem = d->machine->memory;
	int i;

	if (on == d->rom0)
		return;
	d->rom0 = on;

	if (on) {
		memory_device_register(mem, "mvme187_rom0", 0, ROM_WINDOW,
		    dev_mvme187_rom_access, d, DM_DYNTRANS_OK |
		    DM_READS_HAVE_NO_SIDE_EFFECTS, d->rom_at_0);
	} else {
		for (i = 0; i < mem->n_mmapped_devices; i++)
			if (strcmp(mem->devices[i].name, "mvme187_rom0") == 0) {
				memory_device_remove(mem, i);
				break;
			}
	}

	/*  Code translated from the pages that now change meaning (RAM or
	    EPROM) must be translated again:  */
	for (i = 0; i < d->machine->ncpus; i++) {
		struct cpu *c = d->machine->cpus[i];
		uint32_t a;
		for (a = 0; a < ROM_WINDOW; a += 0x1000)
			c->invalidate_code_translation(c, a, INVALIDATE_PADDR);
		c->invalidate_translation_caches(c, 0, INVALIDATE_ALL);
	}
}


DEVICE_ACCESS(mvme187_rom)
{
	struct mvme187_data *d = (struct mvme187_data *) extra;

	/*  Writes to the EPROM are ignored.  */
	if (writeflag == MEM_READ)
		memcpy(data, d->rom + (relative_addr & (ROM_WINDOW - 1)), len);
	return 1;
}


DEVICE_ACCESS(mvme187_sram)
{
	struct mvme187_data *d = (struct mvme187_data *) extra;
	uint32_t a = relative_addr & (SRAM_SIZE - 1);

	if (a + len > SRAM_SIZE)
		return 0;
	if (writeflag == MEM_WRITE)
		memcpy(d->sram + a, data, len);
	else
		memcpy(data, d->sram + a, len);
	return 1;
}


/*****************************************************************************
 *
 *  VMEchip2
 */

DEVICE_ACCESS(mvme187_lcsr)
{
	struct mvme187_data *d = (struct mvme187_data *) extra;
	uint32_t reg = relative_addr & ~3, shift, mask, idata = 0, odata, old;

	shift = (4 - len - (relative_addr & 3)) * 8;
	mask = len == 4 ? 0xffffffff : ((1U << (len * 8)) - 1) << shift;

	if (writeflag == MEM_WRITE)
		idata = memory_readmax64(cpu, data, len) << shift;

	reg_access(d);
	timers_update(d);

	switch (reg) {
	case VME2_T1COUNT:
		odata = d->vtimer[0].count;
		break;
	case VME2_T2COUNT:
		odata = d->vtimer[1].count;
		break;
	case VME2_TCTL:
		odata = (d->lcsr[reg / 4] & ~0xf0f0) |
		    (d->vtimer[0].overflows << 4) |
		    (d->vtimer[1].overflows << 12);
		break;
	case VME2_PRESCALE:
		/*  The low 8 bits count from the adjust value to 0xff at the
		    bus clock rate; the upper 24 bits count their reloads
		    (microseconds, with the adjust value set right).  */
		{
			uint8_t adj = d->lcsr[VME2_TCR / 4] & 0xff;
			uint64_t b = bclk(d);
			odata = (uint32_t) ((b / (256 - adj)) << 8) |
			    (adj + (b % (256 - adj)));
		}
		break;
	case VME2_IRQSTAT:
		odata = d->vme_irqstat;
		break;
	case VME2_SETSOFTIRQ:
	case VME2_IRQCLR:
		odata = 0;
		break;
	default:
		odata = d->lcsr[reg / 4];
	}

	if (writeflag == MEM_READ) {
		memory_writemax64(cpu, data, len, (odata & mask) >> shift);
		return 1;
	}

	old = d->lcsr[reg / 4];
	d->lcsr[reg / 4] = (old & ~mask) | (idata & mask);

	switch (reg) {
	case 0x30:
		set_rom0(d, d->lcsr[reg / 4] & VME2_ROM0);
		break;
	case VME2_T1COUNT:
		d->vtimer[0].count = d->lcsr[reg / 4];
		break;
	case VME2_T2COUNT:
		d->vtimer[1].count = d->lcsr[reg / 4];
		break;
	case VME2_TCTL:
		if (idata & VME2_TCTL1_COVF)
			d->vtimer[0].overflows = 0;
		if (idata & VME2_TCTL2_COVF)
			d->vtimer[1].overflows = 0;
		/*  Clear power-up reset status:  */
		if (idata & VME2_TCTL_CPURS)
			d->lcsr[reg / 4] &= ~VME2_TCTL_PURS;
		else
			d->lcsr[reg / 4] = (d->lcsr[reg / 4] &
			    ~VME2_TCTL_PURS) | (old & VME2_TCTL_PURS);
		/*  Read-only status bits keep their value:  */
		d->lcsr[reg / 4] = (d->lcsr[reg / 4] & ~(VME2_TCTL_SCON |
		    VME2_TCTL_BDFLI | VME2_TCTL_WDTO)) | (old &
		    (VME2_TCTL_SCON | VME2_TCTL_BDFLI | VME2_TCTL_WDTO));
		d->lcsr[reg / 4] &= ~(VME2_TCTL1_COVF | VME2_TCTL2_COVF |
		    VME2_TCTL_WDCC | VME2_TCTL_WDCS | VME2_TCTL_CPURS);
		if (idata & VME2_TCTL_SRST) {
			d->lcsr[reg / 4] &= ~VME2_TCTL_SRST;
			d->reset_pending = true;
		}
		break;
	case VME2_IRQSTAT:
		d->lcsr[reg / 4] = old;		/*  read-only  */
		break;
	case VME2_DMASTAT:
		/*  The MPU status bits are read-only; MCLR clears them.  */
		d->lcsr[reg / 4] = (d->lcsr[reg / 4] & ~(DMASTAT_MPU |
		    DMASTAT_MCLR)) | (old & DMASTAT_MPU);
		if (idata & mask & DMASTAT_MCLR)
			d->lcsr[reg / 4] &= ~DMASTAT_MPU;
		break;
	case VME2_SETSOFTIRQ:
		d->vme_irqstat |= idata & 0xff00;
		d->lcsr[reg / 4] = 0;
		break;
	case VME2_IRQCLR:
		d->vme_irqstat &= ~(idata & 0xffffff00);
		d->lcsr[reg / 4] = 0;
		break;
	}

	timers_schedule(d);
	update_irq(d);
	return 1;
}


DEVICE_ACCESS(mvme187_gcsr)
{
	struct mvme187_data *d = (struct mvme187_data *) extra;
	uint64_t idata;
	size_t i;

	if (writeflag == MEM_READ) {
		for (i = 0; i < len; i++)
			data[i] = d->gcsr[(relative_addr + i) & 0xff];
		return 1;
	}

	idata = memory_readmax64(cpu, data, len);
	for (i = 0; i < len; i++) {
		uint32_t a = (relative_addr + i) & 0xff;
		uint8_t v = idata >> ((len - 1 - i) * 8);

		switch (a) {
		case 0: case 1: case 2: case 3:
			/*  Chip ID and revision are read-only.  */
			break;
		case 6:
			/*  LM3-0 are read-only; SIG3-0 are set by writing 1.  */
			d->gcsr[a] |= v & 0x0f;
			break;
		case 7:
			/*  RST resets the board (local reset).  */
			if (v & 0x80)
				d->reset_pending = true;
			d->gcsr[a] = v & 0x40;
			break;
		default:
			d->gcsr[a] = v;
		}
	}
	return 1;
}


/*****************************************************************************
 *
 *  PCCchip2
 */

static uint8_t pcc_read(struct mvme187_data *d, uint32_t a)
{
	int t;

	switch (a) {
	case PCCTWO_T1COUNT: case PCCTWO_T1COUNT+1:
	case PCCTWO_T1COUNT+2: case PCCTWO_T1COUNT+3:
		return d->ptimer[0].count >> ((3 - (a & 3)) * 8);
	case PCCTWO_T2COUNT: case PCCTWO_T2COUNT+1:
	case PCCTWO_T2COUNT+2: case PCCTWO_T2COUNT+3:
		return d->ptimer[1].count >> ((3 - (a & 3)) * 8);
	case PCCTWO_PSCALECNT:
		/*  Counts from the adjust value to 0xff at BCLK:  */
		{
			uint8_t adj = d->pcc[PCCTWO_PSCALEADJ];
			return adj + bclk(d) % (256 - adj);
		}
	case PCCTWO_T1CTL:
		return (d->pcc[a] & 0x0b) | (d->ptimer[0].overflows << 4);
	case PCCTWO_T2CTL:
		return (d->pcc[a] & 0x0b) | (d->ptimer[1].overflows << 4);
	case PCCTWO_VECBASE:
		return d->pcc_vbr_written ? d->pcc[a] & 0xf0 : 0x0f;
	case PCCTWO_SCCMOIACK:
	case PCCTWO_SCCTXIACK:
	case PCCTWO_SCCRXIACK:
		/*  Pseudo interrupt acknowledge to the CD2401:  */
		t = a == PCCTWO_SCCMOIACK ? 1 : a == PCCTWO_SCCTXIACK ? 2 : 3;
		if (d->scc_iack == NULL)
			return 0;
		return d->scc_iack(d->scc, t);
	case PCCTWO_IPL:
		return combined_level(d);
	case PCCTWO_PRTSTATUS:
		/*  No printer: all the status inputs are high.  */
		return 0x1f;
	case PCCTWO_GPIO_PCR:
		/*  GPI: the pin's level.  */
		return (d->pcc[a] & 3) | (pin_level(d, PCCTWO_GPIO_ICR) << 2);
	}
	return d->pcc[a];
}

static void pcc_write(struct mvme187_data *d, uint32_t a, uint8_t v)
{
	switch (a) {
	case PCCTWO_CHIPID:
	case PCCTWO_CHIPREV:
	case PCCTWO_IPL:
	case PCCTWO_PRTSTATUS:
		return;
	case PCCTWO_VECBASE:
		d->pcc[a] = v & 0xf0;
		d->pcc_vbr_written = true;
		return;
	case PCCTWO_T1COUNT: case PCCTWO_T1COUNT+1:
	case PCCTWO_T1COUNT+2: case PCCTWO_T1COUNT+3:
	case PCCTWO_T2COUNT: case PCCTWO_T2COUNT+1:
	case PCCTWO_T2COUNT+2: case PCCTWO_T2COUNT+3: {
		struct tick_timer *t = &d->ptimer[a >= PCCTWO_T2COUNT];
		int sh = (3 - (a & 3)) * 8;
		t->count = (t->count & ~(0xffU << sh)) | ((uint32_t) v << sh);
		return;
	}
	case PCCTWO_T1CTL:
	case PCCTWO_T2CTL:
		if (v & PCC2_TCTL_COVF)
			d->ptimer[a == PCCTWO_T2CTL].overflows = 0;
		d->pcc[a] = v & 0x03;
		return;
	case PCCTWO_T1ICR:
	case PCCTWO_T2ICR:
	case PCCTWO_IEBERR:
		/*  INT is status; ICLR clears it.  */
		d->pcc[a] = (d->pcc[a] & PCC2_IRQ_INT) | (v & ~(PCC2_IRQ_INT |
		    PCC2_IRQ_ICLR));
		if (v & PCC2_IRQ_ICLR)
			d->pcc[a] &= ~PCC2_IRQ_INT;
		return;
	case PCCTWO_GPIO_ICR:
	case PCCTWO_IEICR:
	case PCCTWO_PRTICR:
	case PCCTWO_PTRFICR:
	case PCCTWO_PTRSICR:
	case PCCTWO_PTRPICR:
	case PCCTWO_PRTBICR:
		/*  Pin inputs (see pin_update()); ICLR clears an edge.  */
		d->pcc[a] = (d->pcc[a] & PCC2_IRQ_INT) | (v & ~(PCC2_IRQ_INT |
		    PCC2_IRQ_ICLR));
		if (v & PCC2_IRQ_ICLR)
			d->pcc[a] &= ~PCC2_IRQ_INT;
		pin_update(d, a);
		return;
	case PCCTWO_GPIO_PCR:
		d->pcc[a] = v & 3;
		pin_update(d, PCCTWO_GPIO_ICR);
		return;
	case PCCTWO_SCCICR:
	case PCCTWO_SCCTX:
	case PCCTWO_SCCRX:
	case PCCTWO_SCSIICR:
		/*  The IRQ bit follows the device's request line.  */
		d->pcc[a] = (d->pcc[a] & PCC2_IRQ_INT) | (v & ~PCC2_IRQ_INT);
		return;
	case PCCTWO_SCCERR:
	case PCCTWO_IEERR:
	case PCCTWO_SCSIERR:
		/*  Error status; SCLR (bit 0) clears it.  */
		if (v & 1)
			d->pcc[a] = 0;
		return;
	}
	d->pcc[a] = v;
}

DEVICE_ACCESS(mvme187_pcc)
{
	struct mvme187_data *d = (struct mvme187_data *) extra;
	uint32_t a = relative_addr & 0x3f;
	uint64_t idata;
	size_t i;

	reg_access(d);
	timers_update(d);

	if (writeflag == MEM_READ) {
		for (i = 0; i < len; i++)
			data[i] = pcc_read(d, (a + i) & 0x3f);
	} else {
		idata = memory_readmax64(cpu, data, len);
		for (i = 0; i < len; i++)
			pcc_write(d, (a + i) & 0x3f,
			    idata >> ((len - 1 - i) * 8));
		timers_schedule(d);
	}

	update_irq(d);
	return 1;
}


/*****************************************************************************
 *
 *  MEMC040
 */


static bool parity_bad(struct mvme187_data *d, uint64_t a)
{
	uint8_t *b = d->par_bad[a >> PAR_PAGE_SHIFT];
	uint32_t o = a & ((1 << PAR_PAGE_SHIFT) - 1);
	return b != NULL && (b[o >> 3] & (1 << (o & 7)));
}

static void parity_set(struct mvme187_data *d, uint64_t a, bool bad)
{
	uint64_t page = a >> PAR_PAGE_SHIFT;
	uint32_t o = a & ((1 << PAR_PAGE_SHIFT) - 1);
	uint8_t bit = 1 << (o & 7);

	if (bad == parity_bad(d, a))
		return;
	if (d->par_bad[page] == NULL)
		CHECK_ALLOCATION(d->par_bad[page] = (uint8_t *)
		    calloc(1, (1 << PAR_PAGE_SHIFT) / 8));
	if (bad) {
		d->par_bad[page][o >> 3] |= bit;
		d->par_nbad[page] ++;
		d->par_total ++;
	} else {
		d->par_bad[page][o >> 3] &= ~bit;
		d->par_nbad[page] --;
		d->par_total --;
	}
}

/*
 *  The RAM normally goes straight to host memory. While parity matters
 *  (WWP is set, or some bytes have wrong parity), a device on top of the
 *  RAM sees every access: writes set or clear the bad parity of their
 *  bytes, and reads of bad bytes are parity errors.
 */
DEVICE_ACCESS(mvme187_parity)
{
	struct mvme187_data *d = (struct mvme187_data *) extra;
	uint8_t ctl = d->memc[0][0x18];
	bool error = false;
	size_t i;

	for (i = 0; i < len; i++) {
		uint64_t a = relative_addr + i;
		unsigned char *h = memory_paddr_to_hostaddr(mem, a, writeflag);

		if (writeflag == MEM_WRITE) {
			if (h != NULL)
				*h = data[i];
			parity_set(d, a, ctl & MEMC_WWP);
		} else {
			data[i] = h != NULL ? *h : 0;
			if (parity_bad(d, a))
				error = true;
		}
	}

	if (!error || !(d->memc[0][0x08] & MEMC_EXTPEN))
		return 1;

	/*  PARINT: an interrupt (the cycle ends normally); PAREN alone: a
	    bus error (TEA).  */
	if (ctl & MEMC_PARINT) {
		debugmsg(d->subsys, "parity", VERBOSITY_INFO, "parity error at "
		    "0x%08llx: interrupt", (unsigned long long) relative_addr);
		d->vme_irqstat |= VME2_IRQ_PE;
		update_irq(d);
		return 1;
	}
	if (ctl & MEMC_PAREN) {
		debugmsg(d->subsys, "parity", VERBOSITY_INFO, "parity error at "
		    "0x%08llx: bus error", (unsigned long long) relative_addr);
		d->lcsr[VME2_DMASTAT / 4] |= DMASTAT_MLPE;
		return 0;
	}
	return 1;
}

/*  Put the overlay on the RAM, or take it away (from outside its own
    access function, i.e. from the MEMC040 registers or a tick).  */
static void parity_overlay(struct mvme187_data *d)
{
	struct memory *mem = d->machine->memory;
	bool want = (d->memc[0][0x18] & MEMC_WWP) || d->par_total > 0;
	int i;

	if (want == d->par_overlay)
		return;
	d->par_overlay = want;

	if (want) {
		memory_device_register(mem, "parity", 0, d->ram_size,
		    dev_mvme187_parity_access, d, DM_DEFAULT |
		    DM_READS_HAVE_NO_SIDE_EFFECTS, NULL);
	} else {
		for (i = 0; i < mem->n_mmapped_devices; i++)
			if (strcmp(mem->devices[i].name, "parity") == 0) {
				memory_device_remove(mem, i);
				break;
			}
	}

	debugmsg(d->subsys, "parity", VERBOSITY_DEBUG, "RAM overlay %s",
	    want ? "on" : "off");

	/*  RAM pages may be mapped (or translated) directly.  */
	d->cpu->invalidate_translation_caches(d->cpu, 0, INVALIDATE_ALL);
	d->cpu->invalidate_code_translation(d->cpu, 0, INVALIDATE_ALL);
}

DEVICE_ACCESS(mvme187_memc)
{
	struct mvme187_data *d = (struct mvme187_data *) extra;
	int n = (relative_addr >> 8) & 1;
	uint32_t a = relative_addr & 0xff;
	uint8_t *r = d->memc[n];

	if (n >= d->n_memc)
		return 0;

	/*  8-bit registers at every fourth byte; the rest reads 0.  */
	if (writeflag == MEM_READ) {
		size_t i;
		for (i = 0; i < len; i++) {
			uint32_t b = a + i;
			data[i] = (b & 3) == 0 && b < 0x20 ? r[b] : 0;
		}
		return 1;
	}

	for (size_t i = 0; i < len; i++) {
		switch (a + i) {
		case 0x00:
			if (data[i] == 0x0f) {
				/*  Software reset.  */
				r[0x10] = r[0x14] = r[0x18] = 0;
				if (n == 0)
					parity_overlay(d);
			}
			break;
		case 0x10:
		case 0x14:
		case 0x18:
			r[a + i] = data[i];
			if (n == 0 && a + i == 0x18)
				parity_overlay(d);
			break;
		case 0x1c:
			/*  Bus clock (not changed by a software reset).  */
			r[a + i] = data[i];
			break;
		}
	}
	return 1;
}


/*****************************************************************************
 *
 *  Interrupt acknowledge space, and areas that do not respond.
 */

DEVICE_ACCESS(mvme187_iack)
{
	struct mvme187_data *d = (struct mvme187_data *) extra;
	int level = (relative_addr >> 2) & 7, vec;

	if (writeflag == MEM_WRITE || level == 0)
		return 0;

	vec = iack(d, level);
	debugmsg(d->subsys, "iack", VERBOSITY_DEBUG,
	    "level %i: vector %i (0x%x); pcc %02x%02x%02x%02x %02x%02x%02x%02x;"
	    " vme stat %08x en %08x vbr %08x", level, vec, vec, d->pcc[0x18], d->pcc[0x1a],
		    d->pcc[0x1b], d->pcc[0x1d], d->pcc[0x1e], d->pcc[0x1f],
		    d->pcc[0x2a], d->pcc[0x2f], d->vme_irqstat,
		    d->lcsr[VME2_IRQEN / 4], d->lcsr[VME2_VBR / 4]);
	if (vec < 0)
		return 0;

	memset(data, 0, len);
	data[len - 1] = vec;
	return 1;
}


/*
 *  The VMEbus A16 (short I/O) window. There are no other boards; only the
 *  board's own GCSR answers, at $XXY0-$XXYF (XX = GCSR group, Y = board),
 *  when the short I/O decoder is enabled.
 */
DEVICE_ACCESS(mvme187_a16)
{
	struct mvme187_data *d = (struct mvme187_data *) extra;
	uint32_t ctl = d->lcsr[VME2_GCSRCTL / 4];
	uint32_t group = (ctl >> 24) & 0xff, board = (ctl >> 20) & 0xf;
	uint32_t a = relative_addr & 0xffff;
	size_t i;

	if (!(ctl & VME2_GCSRCTL_I1EN) || (a >> 8) != group ||
	    ((a >> 4) & 0xf) != board || (a & 0xf) + len > 0x10)
		return 0;

	/*  VMEbus offset o is local offset (o & ~1) * 2 + 2 + (o & 1):  */
	for (i = 0; i < len; i++) {
		uint32_t o = (a + i) & 0xf;
		uint64_t local = ((o & ~1) * 2) + 2 + (o & 1);
		if (dev_mvme187_gcsr_access(cpu, mem, local, data + i, 1,
		    writeflag, extra) == 0)
			return 0;
	}
	return 1;
}


DEVICE_ACCESS(mvme187_nodev)
{
	/*  No acknowledge: the cycle ends with TEA (a bus error).  */
	return 0;
}


DEVICE_ACCESS(mvme187_ack)
{
	/*  Acknowledged, but nothing there.  */
	if (writeflag == MEM_READ)
		memset(data, 0xff, len);
	return 1;
}


/*****************************************************************************
 *
 *  Reset
 */

/*  The reset state of the board's chips (power-up, or a local reset):  */
static void board_reset(struct mvme187_data *d, bool power_up)
{
	int i;

	memset(d->lcsr, 0, sizeof(d->lcsr));
	d->lcsr[0x30 / 4] = VME2_ROM0;
	d->lcsr[VME2_TCTL / 4] = VME2_TCTL_SCON |
	    (power_up ? VME2_TCTL_PURS : 0);
	d->lcsr[VME2_TCR / 4] = 256 - 33;	/*  prescaler adjust  */
	d->vme_irqstat = 0;
	memset(d->vtimer, 0, sizeof(d->vtimer));

	/*  (Each 16-bit GCSR register is in the low half of a longword.)  */
	memset(d->gcsr, 0, sizeof(d->gcsr));
	d->gcsr[2] = 0x01;	/*  revision  */
	d->gcsr[3] = 0x10;	/*  chip id  */
	d->gcsr[6] = 0xf0;	/*  LM3-LM0  */

	memset(d->pcc, 0, sizeof(d->pcc));
	d->pcc[PCCTWO_CHIPID] = PCC2_ID;
	d->pcc[PCCTWO_CHIPREV] = 0x00;
	d->pcc_vbr_written = false;
	memset(d->ptimer, 0, sizeof(d->ptimer));
	for (i = 0; i < N_PCC_SOURCES; i++)
		if (d->ext_irq[i] && pcc_icr_offset(i) >= 0)
			d->pcc[pcc_icr_offset(i)] |= PCC2_IRQ_INT;

	for (i = 0; i < 2; i++)
		d->memc[i][0x10] = d->memc[i][0x14] = d->memc[i][0x18] = 0;

	memset(d->pin_active, 0, sizeof(d->pin_active));
	pins_update(d);
}

static void local_reset(struct mvme187_data *d)
{
	int i;

	board_reset(d, false);
	if (d->scc_reset != NULL)
		d->scc_reset(d->scc);

	set_rom0(d, true);

	/*  (No instruction is running at a tick: all translated code can
	    be thrown away.)  */
	for (i = 0; i < d->machine->ncpus; i++) {
		cpu_create_or_reset_tc(d->machine->cpus[i]);
		m88k_cpu_reset(d->machine->cpus[i]);
	}

	update_irq(d);
}


/*****************************************************************************/


/*
 *  dev_mvme187_set_rom():
 *
 *  Loads the EPROM contents (repeated over the 4 MB EPROM window).
 */
void dev_mvme187_set_rom(struct mvme187_data *d, const unsigned char *image,
	size_t len)
{
	size_t i;

	if (len == 0)
		return;
	if (len > ROM_WINDOW)
		len = ROM_WINDOW;
	for (i = 0; i < ROM_WINDOW; i += len)
		memcpy(d->rom + i, image, ROM_WINDOW - i < len ?
		    ROM_WINDOW - i : len);
	memcpy(d->rom_at_0, d->rom, ROM_WINDOW);

	set_rom0(d, true);
}


void dev_mvme187_set_scc(struct mvme187_data *d, int (*f)(void *, int),
	void (*reset)(void *), void *extra)
{
	d->scc_iack = f;
	d->scc_reset = reset;
	d->scc = extra;
}


struct mvme187_data *dev_mvme187_init(struct machine *machine)
{
	struct mvme187_data *d;
	struct memory *mem = machine->memory;
	struct m8820x_cmmu *cmmu;
	char tmpstr[300];
	int i, msiz, ram_mb = machine->physical_ram_in_mb;

	CHECK_ALLOCATION(d = (struct mvme187_data *) malloc(sizeof(*d)));
	memset(d, 0, sizeof(*d));
	d->machine = machine;

	/*  Time is counted in instructions, one per bus clock: while idle,
	    keep it in step with the host's clock.  */
	if (machine->emulated_hz == 0) {
		machine->realtime_hz = BCLK_HZ;
		machine->realtime_us = mvme187_realtime_us;
		machine->realtime_extra = d;
	}
	d->cpu = machine->cpus[machine->bootstrap_cpu];
	d->subsys = debugmsg_register_subsystem("mvme187");

	CHECK_ALLOCATION(d->rom = (uint8_t *) malloc(ROM_WINDOW));
	memset(d->rom, 0xff, ROM_WINDOW);
	CHECK_ALLOCATION(d->rom_at_0 = (uint8_t *) malloc(ROM_WINDOW));
	CHECK_ALLOCATION(d->sram = (uint8_t *) malloc(SRAM_SIZE));
	memset(d->sram, 0, SRAM_SIZE);

	/*  The 88100's INT pin:  */
	snprintf(tmpstr, sizeof(tmpstr), "%s", d->cpu->path);
	INTERRUPT_CONNECT(tmpstr, d->cpu_irq);

	/*  Device interrupt request lines:  */
	for (i = 0; i < N_PCC_SOURCES + 4; i++) {
		struct interrupt templ;
		char name[300];
		snprintf(name, sizeof(name), "%s.pcc2.%i", d->cpu->path, i);
		memset(&templ, 0, sizeof(templ));
		templ.line = i;
		templ.name = name;
		templ.extra = d;
		templ.interrupt_assert = pcc_line_assert;
		templ.interrupt_deassert = pcc_line_deassert;
		interrupt_handler_register(&templ);
	}

	board_reset(d, true);

	/*  MEMC040: one controller, sized for the whole memory.  */
	for (msiz = 0; msiz < 5 && (4 << msiz) < ram_mb; msiz++)
		;
	if ((4 << msiz) != ram_mb)
		fatal("WARNING: MVME187 memory should be 4, 8, 16, 32, 64 "
		    "or 128 MB (not %i MB)\n", ram_mb);
	d->n_memc = 1;
	d->ram_size = (uint64_t) ram_mb << 20;
	CHECK_ALLOCATION(d->par_bad = (uint8_t **) calloc(
	    (d->ram_size >> PAR_PAGE_SHIFT) + 1, sizeof(uint8_t *)));
	CHECK_ALLOCATION(d->par_nbad = (uint32_t *) calloc(
	    (d->ram_size >> PAR_PAGE_SHIFT) + 1, sizeof(uint32_t)));
	d->memc[0][0x00] = 0x80;	/*  chip id  */
	d->memc[0][0x04] = 0x01;	/*  revision  */
	d->memc[0][0x08] = 0x20 | 0x10 | msiz;	/*  FSTRD, EXTPEN  */
	d->memc[0][0x1c] = 0x21;	/*  33 MHz (set at power-up only)  */

	/*  ROM (not at 0 until there is an image) and SRAM:  */
	memory_device_register(mem, "mvme187_rom", ROM_BASE, ROM_WINDOW,
	    dev_mvme187_rom_access, d, DM_DYNTRANS_OK |
	    DM_READS_HAVE_NO_SIDE_EFFECTS, d->rom);
	memory_device_register(mem, "mvme187_sram", SRAM_BASE, SRAM_SIZE,
	    dev_mvme187_sram_access, d, DM_DYNTRANS_OK | DM_DYNTRANS_WRITE_OK
	    | DM_READS_HAVE_NO_SIDE_EFFECTS, d->sram);
	memory_device_register(mem, "mvme187_sram_rep", SRAM_BASE + SRAM_SIZE,
	    SRAM_WINDOW - SRAM_SIZE, dev_mvme187_sram_access, d,
	    DM_READS_HAVE_NO_SIDE_EFFECTS, NULL);

	/*  Local I/O devices:  */
	memory_device_register(mem, "vme2_lcsr", LCSR_BASE, 0x100,
	    dev_mvme187_lcsr_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "vme2_gcsr", GCSR_BASE, 0x100,
	    dev_mvme187_gcsr_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "pcc2", PCC_BASE, 0x1000,
	    dev_mvme187_pcc_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "memc040", MEMC_BASE, 0x1000,
	    dev_mvme187_memc_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "iack", IACK_BASE, 0x10000,
	    dev_mvme187_iack_access, d, DM_DEFAULT, NULL);

	/*  Areas without devices:  */
	memory_device_register(mem, "reserved0", 0xffc00000, 0x200000,
	    dev_mvme187_nodev_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "reserved1", 0xfff00000, 0x40000,
	    dev_mvme187_nodev_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "reserved2", 0xfff40200, 0xe00,
	    dev_mvme187_nodev_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "vsbchip2", 0xfff41000, 0x1000,
	    dev_mvme187_nodev_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "reserved3", 0xfff44000, 0x1000,
	    dev_mvme187_nodev_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "reserved4", 0xfff48000, 0x28000,
	    dev_mvme187_nodev_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "reserved5", 0xfff70000, 0x7000,
	    dev_mvme187_ack_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "reserved6", 0xfff78000, 0x7000,
	    dev_mvme187_ack_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "reserved7", 0xfff80000, 0x20000,
	    dev_mvme187_ack_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "reserved8", 0xfffa0000, 0x20000,
	    dev_mvme187_nodev_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "reserved9", 0xfffd0000, 0x10000,
	    dev_mvme187_nodev_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "vme_a16", 0xffff0000, 0x10000,
	    dev_mvme187_a16_access, d, DM_DEFAULT, NULL);

	/*  The CMMUs (MC88200, revision 9): code and data.  */
	for (i = 0; i < 2; i++) {
		CHECK_ALLOCATION(cmmu = (struct m8820x_cmmu *)
		    malloc(sizeof(struct m8820x_cmmu)));
		memset(cmmu, 0, sizeof(struct m8820x_cmmu));
		d->cpu->cd.m88k.cmmu[i] = cmmu;
		cmmu->reg[CMMU_IDR] = ((7 - i) << 24) | (M88200_ID << 21) |
		    (9 << 16);
		cmmu->batc[8] = BATC8;
		cmmu->batc[9] = BATC9;
		snprintf(tmpstr, sizeof(tmpstr), "m8820x addr=0x%x addr2=%i",
		    i == 0 ? MVME187_SBC_CMMU_I : MVME187_SBC_CMMU_D, i);
		device_add(machine, tmpstr);
	}

	machine_add_tickfunction(machine, dev_mvme187_tick, d,
	    DEV_MVME187_TICK_SHIFT);

	return d;
}

