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
 *  COMMENT: MC146818 real-time clock
 *
 *  (DS1687 as used in some other machines is also similar to the MC146818.)
 *
 *  This device contains Date/time, the machine's ethernet address (on
 *  DECstation 3100), and can cause periodic (hardware) interrupts.
 *
 *  NOTE: Many register offsets are multiplied by 4 in this code; this is
 *  because I originally wrote it for DECstation 3100 emulation, where the
 *  registered are spaced that way.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "devices.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"
#include "timer.h"

#include "thirdparty/mc146818reg.h"


#define	to_bcd(x)	( ((x)/10) * 16 + ((x)%10) )
#define	from_bcd(x)	( ((x)>>4) * 10 + ((x)&15) )

/*  #define MC146818_DEBUG  */

#define	MC146818_TICK_SHIFT	14


/*  256 on DECstation, SGI uses reg at 72*4 as the Century  */
#define	N_REGISTERS	1024
struct mc_data {
	int		access_style;
	int		last_addr;

	int		register_choice;
	int		reg[N_REGISTERS];
	int		addrdiv;

	int		use_bcd;

	int		timebase_hz;
	int		interrupt_hz;
	int		old_interrupt_hz;
	struct interrupt irq;
	struct timer	*timer;
	volatile int	pending_timer_interrupts;

	int		previous_second;
	int		n_seconds_elapsed;
	int		uip_threshold;

	int		ugly_netbsd_prep_hack_done;
	int		ugly_netbsd_prep_hack_sec;

	int		dec_year_done;	/*  register 63 has been set  */

	/*  The NVRAM (registers 14..63) is kept in this file, if any:  */
	char		*nvram_file;
	bool		nvram_dirty;
	bool		nvram_have_saved;
	unsigned char	nvram_saved[MC_NVRAM_SIZE];
	time_t		nvram_changed;

	/*
	 *  A time set by the guest (by clearing the SET bit), which then
	 *  runs on from there with the host's clock.  The day of the week
	 *  counts on from what was set, like on the real chip.
	 */
	int		guest_time_set;
	time_t		guest_base;
	time_t		host_base;
	int		guest_dow;

	time_t		last_update;	/*  for the Update-ended flag  */
};


/*
 *  Ugly hack to fool NetBSD/prep to accept the clock.  (See mcclock_isa_match
 *  in NetBSD's arch/prep/isa/mcclock_isa.c for details.)
 */
#define	NETBSD_HACK_INIT		0
#define	NETBSD_HACK_FIRST_1		1
#define	NETBSD_HACK_FIRST_2		2
#define	NETBSD_HACK_SECOND_1		3
#define	NETBSD_HACK_SECOND_2		4
#define	NETBSD_HACK_DONE		5


/*
 *  timer_tick():
 *
 *  Called d->interrupt_hz times per (real-world) second.
 */
static void timer_tick(struct timer *timer, void *extra)
{
	struct mc_data *d = (struct mc_data *) extra;
	d->pending_timer_interrupts ++;
}


/*
 *  mc146818_set_guest_time():
 *
 *  Called when the guest clears the SET bit on a DECstation, to make the
 *  clock run on from the time in the registers.  (The 5000/1xx PROM's
 *  self-test sets 1999-12-31 23:59:59 and checks that it becomes
 *  2000-01-01.)
 */
static void mc146818_set_guest_time(struct mc_data *d)
{
	struct tm tm;
	int r[10], i, year;

	for (i = MC_SEC; i <= MC_YEAR; i++)
		r[i] = d->use_bcd? from_bcd(d->reg[4 * i]) : d->reg[4 * i];

	year = r[MC_YEAR] % 100;
	memset(&tm, 0, sizeof(tm));
	tm.tm_sec  = r[MC_SEC];
	tm.tm_min  = r[MC_MIN];
	tm.tm_hour = r[MC_HOUR];
	tm.tm_mday = r[MC_DOM];
	tm.tm_mon  = r[MC_MONTH] - 1;
	tm.tm_year = year < 70? year + 100 : year;

	d->guest_base = timegm(&tm);
	d->host_base = time(NULL);
	d->guest_dow = r[MC_DOW] >= 1 && r[MC_DOW] <= 7? r[MC_DOW] : 1;
	d->guest_time_set = 1;
	d->last_update = d->host_base;
}


/*
 *  NVRAM file:  the 50 bytes of registers 14..63, which the machine's
 *  firmware keeps its settings in (e.g. the DECstation PROM's boot and
 *  console variables).  It is read when the machine starts (if it
 *  exists), and written after changes, at most once a second, and when
 *  gxemul exits or resets the machine.
 */

#define	MAX_NVRAMS	4
static struct mc_data *nvrams[MAX_NVRAMS];
static int n_nvrams;

static bool nvram_has_changed(struct mc_data *d)
{
	int i;

	if (!d->nvram_have_saved)
		return d->nvram_dirty;
	for (i = 0; i < MC_NVRAM_SIZE; i++)
		if ((unsigned char) d->reg[4 * (MC_NVRAM_START + i)] !=
		    d->nvram_saved[i])
			return true;
	return false;
}

static void nvram_write(struct mc_data *d)
{
	unsigned char buf[MC_NVRAM_SIZE];
	FILE *f;
	int i;

	for (i = 0; i < MC_NVRAM_SIZE; i++)
		buf[i] = d->reg[4 * (MC_NVRAM_START + i)];
	f = fopen(d->nvram_file, "wb");
	if (f == NULL || fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) {
		fprintf(stderr, "[ mc146818: could not write the NVRAM to "
		    "%s ]\n", d->nvram_file);
		d->nvram_file = NULL;
	} else {
		memcpy(d->nvram_saved, buf, sizeof(buf));
		d->nvram_have_saved = true;
	}
	if (f != NULL)
		fclose(f);
	d->nvram_dirty = false;
}

static void nvram_read(struct mc_data *d)
{
	unsigned char buf[MC_NVRAM_SIZE];
	FILE *f = fopen(d->nvram_file, "rb");
	int i;

	if (f == NULL)
		return;		/*  (A new NVRAM: it is written when set.)  */
	if (fread(buf, 1, sizeof(buf), f) == sizeof(buf)) {
		for (i = 0; i < MC_NVRAM_SIZE; i++)
			d->reg[4 * (MC_NVRAM_START + i)] = buf[i];
		memcpy(d->nvram_saved, buf, sizeof(buf));
		d->nvram_have_saved = true;
		d->dec_year_done = 1;
	} else
		fprintf(stderr, "[ mc146818: %s is too short to be an NVRAM; "
		    "ignored ]\n", d->nvram_file);
	fclose(f);
}


/*
 *  dev_mc146818_save_nvram():
 *
 *  Writes the NVRAMs that have changed to their files.
 */
void dev_mc146818_save_nvram(void)
{
	int i;

	for (i = 0; i < n_nvrams; i++)
		if (nvrams[i]->nvram_file != NULL &&
		    nvram_has_changed(nvrams[i]))
			nvram_write(nvrams[i]);
}


DEVICE_TICK(mc146818)
{
	struct mc_data *d = (struct mc_data *) extra;
	int pti = d->pending_timer_interrupts;
	time_t now = time(NULL);

	/*
	 *  Once a second (unless SET stops the updates), the time is
	 *  updated, and the Update-ended flag set, with an interrupt if
	 *  UIE is enabled.  Also check whether the NVRAM has changed and
	 *  settled (no writes in the current second), and persist it.
	 */
	if (now != d->last_update) {
		d->last_update = now;
		if (d->nvram_file != NULL && now != d->nvram_changed) {
			if (nvram_has_changed(d))
				nvram_write(d);
			else
				d->nvram_dirty = false;
		}
		if (!(d->reg[MC_REGB * 4] & MC_REGB_SET)) {
			d->reg[MC_REGC * 4] |= MC_REGC_UF;
			if (d->reg[MC_REGB * 4] & MC_REGB_UIE)
				INTERRUPT_ASSERT(d->irq);
		}
	}

	if ((d->reg[MC_REGB * 4] & MC_REGB_PIE) && pti > 0) {
#if 0
		/*  For debugging, to see how much the interrupts are
		    lagging behind the real clock:  */
		{
			static int x = 0;
			if (++x == 1) {
				x = 0;
				printf("%i ", pti);
				fflush(stdout);
			}
		}
#endif

		INTERRUPT_ASSERT(d->irq);

		d->reg[MC_REGC * 4] |= MC_REGC_PF;
	}

	if (d->reg[MC_REGC * 4] & MC_REGC_UF ||
	    d->reg[MC_REGC * 4] & MC_REGC_AF ||
	    d->reg[MC_REGC * 4] & MC_REGC_PF)
		d->reg[MC_REGC * 4] |= MC_REGC_IRQF;
}


/*
 *  dev_mc146818_jazz_access():
 *
 *  It seems like JAZZ machines accesses the mc146818 by writing one byte to
 *  0x90000070 and then reading or writing another byte at 0x......0004000.
 */
DEVICE_ACCESS(mc146818_jazz)
{
	struct mc_data *d = (struct mc_data *) extra;

#ifdef MC146818_DEBUG
	if (writeflag == MEM_WRITE) {
		int i;
		fatal("[ mc146818_jazz: write to addr=0x%04x: ",
		    (int)relative_addr);
		for (i=0; i<len; i++)
			fatal("%02x ", data[i]);
		fatal("]\n");
	} else
		fatal("[ mc146818_jazz: read from addr=0x%04x ]\n",
		    (int)relative_addr);
#endif

	if (writeflag == MEM_WRITE) {
		d->last_addr = data[0];
		return 1;
	} else {
		data[0] = d->last_addr;
		return 1;
	}
}


/*
 *  mc146818_update_time():
 *
 *  This function updates the MC146818 registers by reading
 *  the host's clock.
 */
static void mc146818_update_time(struct mc_data *d)
{
	struct tm *tmp;
	time_t timet;

	if (d->guest_time_set) {
		int days;

		timet = d->guest_base + (time(NULL) - d->host_base);
		tmp = gmtime(&timet);
		days = timet / 86400 - d->guest_base / 86400;

		d->reg[4 * MC_SEC]   = tmp->tm_sec;
		d->reg[4 * MC_MIN]   = tmp->tm_min;
		d->reg[4 * MC_HOUR]  = tmp->tm_hour;
		d->reg[4 * MC_DOW]   = (d->guest_dow - 1 + days) % 7 + 1;
		d->reg[4 * MC_DOM]   = tmp->tm_mday;
		d->reg[4 * MC_MONTH] = tmp->tm_mon + 1;
		d->reg[4 * MC_YEAR]  = tmp->tm_year % 100;

		if (d->use_bcd) {
			int r;
			for (r = MC_SEC; r <= MC_YEAR; r++)
				d->reg[4 * r] = to_bcd(d->reg[4 * r]);
		}
		return;
	}

	timet = time(NULL);
	tmp = gmtime(&timet);

	d->reg[4 * MC_SEC]   = tmp->tm_sec;
	d->reg[4 * MC_MIN]   = tmp->tm_min;
	d->reg[4 * MC_HOUR]  = tmp->tm_hour;
	d->reg[4 * MC_DOW]   = tmp->tm_wday + 1;
	d->reg[4 * MC_DOM]   = tmp->tm_mday;
	d->reg[4 * MC_MONTH] = tmp->tm_mon + 1;
	d->reg[4 * MC_YEAR]  = tmp->tm_year;

	/*
	 *  Special hacks for emulating the behaviour of various machines:
	 */
	switch (d->access_style) {
	case MC146818_ALGOR:
		/*
		 *  NetBSD/evbmips sources indicate that the Algor year base
		 *  is 1920. This makes the time work with NetBSD in Malta
		 *  emulation. However, for Linux, commenting out this line
		 *  works better.  (TODO: Find a way to make both work?)
		 */
		d->reg[4 * MC_YEAR] += 80;
		break;
	case MC146818_ARC_NEC:
		d->reg[4 * MC_YEAR] += (0x18 - 104);
		break;
	case MC146818_CATS:
		d->reg[4 * MC_YEAR] %= 100;
		break;
	case MC146818_SGI:
		/*
		 *  NetBSD/sgimips assumes data in BCD format.
		 *  Also, IRIX stores the year value in a weird
		 *  format, according to ../arch/sgimips/sgimips/clockvar.h
		 *  in NetBSD:
		 *
		 *  "If year < 1985, store (year - 1970), else
		 *   (year - 1940). This matches IRIX semantics."
		 *
		 *  Another rule: It seems that a real SGI IP32 box
		 *  uses the value 5 for the year 2005.
		 */
		d->reg[4 * MC_YEAR] =
		    d->reg[4 * MC_YEAR] >= 100 ?
			(d->reg[4 * MC_YEAR] - 100) :
		      (
			d->reg[4 * MC_YEAR] < 85 ?
			  (d->reg[4 * MC_YEAR] - 30 + 40)
			: (d->reg[4 * MC_YEAR] - 40)
		      );
		/*  Century:  */
		d->reg[72 * 4] = 19 + (tmp->tm_year / 100);
		break;
	case MC146818_DEC:
		/*
		 *  DECstations must have 72 or 73 in the Year field, or
		 *  Ultrix screems. This is also mentioned in NetBSD's
		 *  mcclock.c.  Unfortunately, this means that NetBSD cannot
		 *  know what year it actually is, so it has to deduce it
		 *  from the filesystem.
		 */
		d->reg[4 * MC_YEAR] = 72;

		/*
		 *  Linux on DECstation stores the year in register 63,
		 *  but no other DECstation OS does? (Hm.)  It is NVRAM,
		 *  though, which the 5000/1xx PROM's self-test writes
		 *  and reads back, so it's only set here at first.
		 */
		if (!d->dec_year_done) {
			d->reg[4 * 63] = tmp->tm_year - 100;
			if (d->use_bcd)
				d->reg[4 * 63] = to_bcd(d->reg[4 * 63]);
			d->dec_year_done = 1;
		}
		break;
	}

	if (d->use_bcd) {
		d->reg[4 * MC_SEC]   = to_bcd(d->reg[4 * MC_SEC]);
		d->reg[4 * MC_MIN]   = to_bcd(d->reg[4 * MC_MIN]);
		d->reg[4 * MC_HOUR]  = to_bcd(d->reg[4 * MC_HOUR]);
		d->reg[4 * MC_DOW]   = to_bcd(d->reg[4 * MC_DOW]);
		d->reg[4 * MC_DOM]   = to_bcd(d->reg[4 * MC_DOM]);
		d->reg[4 * MC_MONTH] = to_bcd(d->reg[4 * MC_MONTH]);
		d->reg[4 * MC_YEAR]  = to_bcd(d->reg[4 * MC_YEAR]);

		/*  Used on SGI:  */
		d->reg[4 * 72]       = to_bcd(d->reg[4 * 72]);
	}
}


DEVICE_ACCESS(mc146818)
{
	struct mc_data *d = (struct mc_data *) extra;
	struct tm *tmp;
	time_t timet;
	size_t i;

	/*  NOTE/TODO: This access function only handles 8-bit accesses!  */

	relative_addr /= d->addrdiv;

	/*  Different ways of accessing the registers:  */
	switch (d->access_style) {
	case MC146818_ALGOR:
	case MC146818_CATS:
	case MC146818_PC_CMOS:
		if ((relative_addr & 1) == 0x00) {
			if (writeflag == MEM_WRITE) {
				d->last_addr = data[0];
				return 1;
			} else {
				data[0] = d->last_addr;
				return 1;
			}
		} else
			relative_addr = d->last_addr * 4;
		break;
	case MC146818_ARC_NEC:
		if (relative_addr == 0x01) {
			if (writeflag == MEM_WRITE) {
				d->last_addr = data[0];
				return 1;
			} else {
				data[0] = d->last_addr;
				return 1;
			}
		} else if (relative_addr == 0x00)
			relative_addr = d->last_addr * 4;
		else {
			fatal("[ mc146818: not accessed as an "
			    "MC146818_ARC_NEC device! ]\n");
		}
		break;
	case MC146818_ARC_JAZZ:
		/*  See comment for dev_mc146818_jazz_access().  */
		relative_addr = d->last_addr * 4;
		break;
	case MC146818_DEC:
		/*
		 *  The registers repeat through the RTC's address range, as
		 *  on the DECstations' boards (whose PROMs copy their NVRAM
		 *  with loops that go two registers past the last one).
		 */
		relative_addr &= DEV_MC146818_LENGTH - 1;
		break;
	case MC146818_SGI:
		/*
		 *  This device was originally written for DECstation
		 *  emulation, so no changes are necessary for that access
		 *  style.
		 *
		 *  SGI access bytes 0x0..0xd at offsets 0x0yz..0xdyz, where yz
		 *  should be ignored. It works _almost_ as DEC, if offsets are
		 *  divided by 0x40.
		 */
		break;
	case MC146818_PMPPC:
		relative_addr *= 4;
		break;
	default:
		;
	}

#ifdef MC146818_DEBUG
	if (writeflag == MEM_WRITE) {
		fatal("[ mc146818: write to addr=0x%04x (len %i): ",
		    (int)relative_addr, (int)len);
		for (i=0; i<len; i++)
			fatal("0x%02x ", data[i]);
		fatal("]\n");
	}
#endif

	/*
	 *  Sprite seems to wants UF interrupt status, once every second, or
	 *  it hangs forever during bootup.  (These do not cause interrupts,
	 *  but it is good enough... Sprite polls this, iirc.)
	 *
	 *  Linux on at least sgimips and evbmips (Malta) wants the UIP bit
	 *  in REGA to be updated once a second.
	 */
	if (relative_addr == MC_REGA*4 || relative_addr == MC_REGC*4) {
		timet = time(NULL);
		tmp = gmtime(&timet);
		d->reg[MC_REGC * 4] &= ~MC_REGC_UF;
		if (tmp->tm_sec != d->previous_second) {
			d->n_seconds_elapsed ++;
			d->previous_second = tmp->tm_sec;
		}
		if (d->n_seconds_elapsed > d->uip_threshold) {
			d->n_seconds_elapsed = 0;

			d->reg[MC_REGA * 4] |= MC_REGA_UIP;

			d->reg[MC_REGC * 4] |= MC_REGC_UF;
			d->reg[MC_REGC * 4] |= MC_REGC_IRQF;

			/*  For some reason, some Linux/DECstation KN04
			    kernels want the PF (periodic flag) bit set,
			    even though interrupts are not enabled?  */
			d->reg[MC_REGC * 4] |= MC_REGC_PF;
		} else
			d->reg[MC_REGA * 4] &= ~MC_REGA_UIP;
	}

	/*  RTC data is in either BCD format or binary:  */
	if (d->use_bcd)
		d->reg[MC_REGB * 4] &= ~(1 << 2);
	else
		d->reg[MC_REGB * 4] |= (1 << 2);

	/*  RTC date/time is always Valid:  */
	d->reg[MC_REGD * 4] |= MC_REGD_VRT;

	if (writeflag == MEM_WRITE) {
		/*  WRITE:  */
		if (relative_addr % 4 == 0 &&
		    relative_addr >= MC_NVRAM_START * 4 && relative_addr <
		    (MC_NVRAM_START + MC_NVRAM_SIZE) * 4 &&
		    d->reg[relative_addr] != data[0]) {
			d->nvram_dirty = true;
			d->nvram_changed = time(NULL);
		}

		switch (relative_addr) {
		case MC_REGA*4:
			if ((data[0] & MC_REGA_DVMASK) == MC_BASE_32_KHz)
				d->timebase_hz = 32000;
			if ((data[0] & MC_REGA_DVMASK) == MC_BASE_1_MHz)
				d->timebase_hz = 1000000;
			if ((data[0] & MC_REGA_DVMASK) == MC_BASE_4_MHz)
				d->timebase_hz = 4000000;
			switch (data[0] & MC_REGA_RSMASK) {
			case MC_RATE_NONE:
				d->interrupt_hz = 0;
				break;
			case MC_RATE_1:
				if (d->timebase_hz == 32000)
					d->interrupt_hz = 256;
				else
					d->interrupt_hz = 32768;
				break;
			case MC_RATE_2:
				if (d->timebase_hz == 32000)
					d->interrupt_hz = 128;
				else
					d->interrupt_hz = 16384;
				break;
			case MC_RATE_8192_Hz:	d->interrupt_hz = 8192;	break;
			case MC_RATE_4096_Hz:	d->interrupt_hz = 4096;	break;
			case MC_RATE_2048_Hz:	d->interrupt_hz = 2048;	break;
			case MC_RATE_1024_Hz:	d->interrupt_hz = 1024;	break;
			case MC_RATE_512_Hz:	d->interrupt_hz = 512;	break;
			case MC_RATE_256_Hz:	d->interrupt_hz = 256;	break;
			case MC_RATE_128_Hz:	d->interrupt_hz = 128;	break;
			case MC_RATE_64_Hz:	d->interrupt_hz = 64;	break;
			case MC_RATE_32_Hz:	d->interrupt_hz = 32;	break;
			case MC_RATE_16_Hz:	d->interrupt_hz = 16;	break;
			case MC_RATE_8_Hz:	d->interrupt_hz = 8;	break;
			case MC_RATE_4_Hz:	d->interrupt_hz = 4;	break;
			case MC_RATE_2_Hz:	d->interrupt_hz = 2;	break;
			default:/*  debug("[ mc146818: unimplemented "
				    "MC_REGA RS: %i ]\n",
				    data[0] & MC_REGA_RSMASK);  */
				;
			}

			if (d->interrupt_hz != d->old_interrupt_hz) {
				debug("[ rtc changed to interrupt at %i Hz ]\n",
				    d->interrupt_hz);

				d->old_interrupt_hz = d->interrupt_hz;

				if (d->timer == NULL)
					d->timer = timer_add(d->interrupt_hz,
					    timer_tick, d);
				else
					timer_update_frequency(d->timer,
					    d->interrupt_hz);
			}

			d->reg[MC_REGA * 4] =
			    data[0] & (MC_REGA_RSMASK | MC_REGA_DVMASK);
			break;
		case MC_REGB*4:
			/*  (Other machines have their own year formats,
			    which mc146818_set_guest_time() doesn't know.)  */
			if (d->access_style == MC146818_DEC &&
			    (d->reg[MC_REGB*4] & MC_REGB_SET) &&
			    !(data[0] & MC_REGB_SET))
				mc146818_set_guest_time(d);
			d->reg[MC_REGB*4] = data[0];
			if (!(data[0] & MC_REGB_PIE)) {
				INTERRUPT_DEASSERT(d->irq);
			}

			/*  debug("[ mc146818: write to MC_REGB, data[0] "
			    "= 0x%02x ]\n", data[0]);  */
			break;
		case MC_REGC*4:
			d->reg[MC_REGC * 4] = data[0];
			debug("[ mc146818: write to MC_REGC, data[0] = "
			    "0x%02x ]\n", data[0]);
			break;
		case 0x128:
			d->reg[relative_addr] = data[0];
			if (data[0] & 8) {
				int j;

				/*  Used on SGI to power off the machine.  */
				fatal("[ md146818: power off ]\n");
				for (j=0; j<cpu->machine->ncpus; j++)
					cpu->machine->cpus[j]->running = false;
			}
			break;
		default:
			d->reg[relative_addr] = data[0];

			debug("[ mc146818: unimplemented write to "
			    "relative_addr = %08lx: ", (long)relative_addr);
			for (i=0; i<len; i++)
				debug("%02x ", data[i]);
			debug("]\n");
		}
	} else {
		/*  READ:  */
		switch (relative_addr) {
		case 0x01:	/*  Station's ethernet address (6 bytes)  */
		case 0x05:	/*  (on DECstation 3100)  */
		case 0x09:
		case 0x0d:
		case 0x11:
		case 0x15:
			break;
		case 4 * MC_SEC:
			if (d->ugly_netbsd_prep_hack_done < NETBSD_HACK_DONE) {
				d->ugly_netbsd_prep_hack_done ++;
				switch (d->ugly_netbsd_prep_hack_done) {
				case NETBSD_HACK_FIRST_1:
					d->ugly_netbsd_prep_hack_sec =
					    from_bcd(d->reg[relative_addr]);
					break;
				case NETBSD_HACK_FIRST_2:
					d->reg[relative_addr] = to_bcd(
					    d->ugly_netbsd_prep_hack_sec);
					break;
				case NETBSD_HACK_SECOND_1:
				case NETBSD_HACK_SECOND_2:
					d->reg[relative_addr] = to_bcd((1 +
					    d->ugly_netbsd_prep_hack_sec) % 60);
					break;
				}
			}
			// fall through
		case 4 * MC_MIN:
		case 4 * MC_HOUR:
		case 4 * MC_DOW:
		case 4 * MC_DOM:
		case 4 * MC_MONTH:
		case 4 * MC_YEAR:
		case 4 * 63:	/*  63 is used by Linux on DECstation  */
		case 4 * 72:	/*  72 is Century, on SGI (DS1687)  */
			/*
			 *  If the SET bit is set, then we don't automatically
			 *  update the values.  Otherwise, we update them by
			 *  reading from the host's clock:
			 */
			if (d->reg[MC_REGB * 4] & MC_REGB_SET)
				break;

			if (d->ugly_netbsd_prep_hack_done >= NETBSD_HACK_DONE)
				mc146818_update_time(d);
			break;
		case 4 * MC_REGA:
			break;
		case 4 * MC_REGC:	/*  Interrupt ack.  */
			/*  NOTE: Acking is done below, _after_ the
			    register has been read.  */
			break;
		default:debug("[ mc146818: read from relative_addr = "
			    "%04x ]\n", (int)relative_addr);
		}

		data[0] = d->reg[relative_addr];

		if (relative_addr == MC_REGC*4) {
			INTERRUPT_DEASSERT(d->irq);

			/*
			 *  Acknowledging an interrupt decreases the
			 *  number of pending "real world" timer ticks.
			 */
			if (d->reg[MC_REGC * 4] & MC_REGC_PF &&
			    d->pending_timer_interrupts > 0)
				d->pending_timer_interrupts --;

			d->reg[MC_REGC * 4] = 0x00;
		}
	}

#ifdef MC146818_DEBUG
	if (writeflag == MEM_READ) {
		fatal("[ mc146818: read from addr=0x%04x (len %i): ",
		    (int)relative_addr, (int)len);
		for (i=0; i<len; i++)
			fatal("0x%02x ", data[i]);
		fatal("]\n");
	}
#endif

	return 1;
}


/*
 *  dev_mc146818_init():
 *
 *  This needs to work for both DECstation emulation and other machine types,
 *  so it contains both rtc related stuff and the station's Ethernet address.
 */
void dev_mc146818_init(struct machine *machine, struct memory *mem,
	uint64_t baseaddr, char *irq_path, int access_style, int addrdiv)
{
	unsigned char ether_address[6];
	int i, dev_len;
	struct mc_data *d;

	CHECK_ALLOCATION(d = (struct mc_data *) malloc(sizeof(struct mc_data)));
	memset(d, 0, sizeof(struct mc_data));

	d->access_style  = access_style;
	d->addrdiv       = addrdiv;

	INTERRUPT_CONNECT(irq_path, d->irq);

	d->use_bcd = 0;
	switch (access_style) {
	case MC146818_SGI:
	case MC146818_PC_CMOS:
	case MC146818_PMPPC:
		d->use_bcd = 1;
	}

	if (machine->machine_type != MACHINE_PREP) {
		/*  NetBSD/prep has a really ugly clock detection code;
		    no other machines/OSes don't need this.  */
		d->ugly_netbsd_prep_hack_done = NETBSD_HACK_DONE;
	}

	if (access_style == MC146818_DEC) {
		/*  Station Ethernet Address, on DECstation 3100:  */
		for (i=0; i<6; i++)
			ether_address[i] = 0x10 * (i+1);

		d->reg[0x01] = ether_address[0];
		d->reg[0x05] = ether_address[1];
		d->reg[0x09] = ether_address[2];
		d->reg[0x0d] = ether_address[3];
		d->reg[0x11] = ether_address[4];
		d->reg[0x15] = ether_address[5];
		/*  TODO:  19, 1d, 21, 25 = checksum bytes 1,2,2,1 resp. */
		d->reg[0x29] = ether_address[5];
		d->reg[0x2d] = ether_address[4];
		d->reg[0x31] = ether_address[3];
		d->reg[0x35] = ether_address[2];
		d->reg[0x39] = ether_address[1];
		d->reg[0x3d] = ether_address[1];
		d->reg[0x41] = ether_address[0];
		d->reg[0x45] = ether_address[1];
		d->reg[0x49] = ether_address[2];
		d->reg[0x4d] = ether_address[3];
		d->reg[0x51] = ether_address[4];
		d->reg[0x55] = ether_address[5];
		/*  TODO:  59, 5d = checksum bytes 1,2 resp. */
		d->reg[0x61] = 0xff;
		d->reg[0x65] = 0x00;
		d->reg[0x69] = 0x55;
		d->reg[0x6d] = 0xaa;
		d->reg[0x71] = 0xff;
		d->reg[0x75] = 0x00;
		d->reg[0x79] = 0x55;
		d->reg[0x7d] = 0xaa;

		/*  Battery valid, for DECstations  */
		d->reg[0xf8] = 1;
	}

	/*
	 *  uip_threshold should ideally be 1, but when Linux polls the UIP bit
	 *  it looses speed. This hack gives Linux the impression that the cpu
	 *  is uip_threshold times faster than the slow clock it would
	 *  otherwise detect.
	 *
	 *  TODO:  Find out if this messes up Sprite emulation; if so, then
	 *         this hack has to be removed.
	 */
	d->uip_threshold = 8;

	if (access_style == MC146818_ARC_JAZZ)
		memory_device_register(mem, "mc146818_jazz", 0x90000070ULL,
		    1, dev_mc146818_jazz_access, d, DM_DEFAULT, NULL);

	dev_len = DEV_MC146818_LENGTH;
	switch (access_style) {
	case MC146818_CATS:
	case MC146818_PC_CMOS:
		dev_len = 2;
		break;
	case MC146818_SGI:
		dev_len = 0x400;
		break;
	case MC146818_DEC:
		/*
		 *  A whole page, where the registers repeat:  otherwise, an
		 *  access beyond them would make the rest of the page RAM, for
		 *  dyntrans, and later accesses to the registers on that page
		 *  would reach that RAM instead of the device.
		 */
		dev_len = 0x1000;
		break;
	}

	memory_device_register(mem, "mc146818", baseaddr,
	    dev_len * addrdiv, dev_mc146818_access,
	    d, DM_DEFAULT, NULL);

	if (machine->nvram_filename != NULL && n_nvrams < MAX_NVRAMS) {
		d->nvram_file = machine->nvram_filename;
		nvram_read(d);
		if (n_nvrams == 0)
			atexit(dev_mc146818_save_nvram);
		nvrams[n_nvrams++] = d;
	}

	mc146818_update_time(d);

	machine_add_tickfunction(machine, dev_mc146818_tick, d,
	    MC146818_TICK_SHIFT);
}

