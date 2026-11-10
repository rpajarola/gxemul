/*
 *  Copyright (C) 2005-2020  Anders Gavare.  All rights reserved.
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
 *  COMMENT: Intel 8253/8254 Programmable Interval Timer
 *
 *  Timer 0 causes clock interrupts at its frequency.  The counters don't
 *  count down: reading one gives the value that was loaded.  Each counter
 *  has its own access mode (LSB, MSB or both) and byte order, and the
 *  counter latch and 8254 read-back commands latch counts.  Nothing is
 *  done with counters 1 and 2 (DMA refresh and the speaker tone).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "device.h"
#include "emul.h"
#include "interrupt.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"
#include "timer.h"

#include "thirdparty/i8253reg.h"


/*  #define debug fatal  */

#define	DEV_8253_LENGTH		4
#define	TICK_SHIFT		14


struct pit8253_data {
	int		in_use;

	int		mode[3];
	int		counter[3];		/*  reload value  */
	int		hz[3];

	/*  Per counter: access mode (I8253_TIMER_LSB, _MSB or _16BIT), which
	    byte comes next when written or read as 16 bits, and a latched
	    count:  */
	int		rw_mode[3];
	int		write_msb_next[3];
	int		read_msb_next[3];
	int		latched[3];
	int		latch_value[3];

	struct timer	*timer0;
	struct interrupt irq;
	int		pending_interrupts_timer0;
};


static void timer0_tick(struct timer *t, void *extra)
{
	struct pit8253_data *d = (struct pit8253_data *) extra;
	d->pending_interrupts_timer0 ++;

	/*  printf("%i ", d->pending_interrupts_timer0); fflush(stdout);  */
}


DEVICE_TICK(8253)
{
	struct pit8253_data *d = (struct pit8253_data *) extra;

	if (!d->in_use)
		return;

	// Generate interrupts regardless of (d->mode[0] & 0x0e)?
	// (It seems like Linux/MALTA kernels like this.)
	if (d->pending_interrupts_timer0 > 0)
		INTERRUPT_ASSERT(d->irq);
}


/*
 *  The current count of a counter.  The counters don't count down: this is
 *  the value that was loaded.  (Firmware tests such as the Jensen ARC
 *  console's "Timer 1" check expect the count read back soon after loading
 *  to be close to the loaded value, which host-time counting at the
 *  emulator's speed does not give.)
 */
static int pit_count(struct pit8253_data *d, int c)
{
	return d->counter[c] & 0xffff;
}


/*  A new reload value has been written to counter c:  */
static void pit_load(struct pit8253_data *d, int c)
{
	if (d->counter[c] != 0)
		d->hz[c] = (int) (I8253_TIMER_FREQ / (float) d->counter[c] + 0.5);
	else
		d->hz[c] = 0;
	debug("[ 8253: counter %i set to %i (%i Hz) ]\n", c, d->counter[c],
	    d->hz[c]);

	switch (c) {
	case 0:	if (d->timer0 == NULL)
			d->timer0 = timer_add(d->hz[0], timer0_tick, d);
		else
			timer_update_frequency(d->timer0, d->hz[0]);
		break;
	case 1:	debug("[ 8253: counter 1 (DMA refresh) not emulated ]\n");
		break;
	case 2:	debug("[ 8253: counter 2 (tone generation) not emulated ]\n");
		break;
	}
}


static void pit_latch(struct pit8253_data *d, int c)
{
	if (d->latched[c])
		return;
	d->latched[c] = 1;
	d->latch_value[c] = pit_count(d, c);
	d->read_msb_next[c] = 0;
}


DEVICE_ACCESS(8253)
{
	struct pit8253_data *d = (struct pit8253_data *) extra;
	uint64_t idata = 0, odata = 0;
	int c = relative_addr, value;

	if (writeflag == MEM_WRITE)
		idata = memory_readmax64(cpu, data, len);

	d->in_use = 1;

	switch (relative_addr) {

	case I8253_TIMER_CNTR0:
	case I8253_TIMER_CNTR1:
	case I8253_TIMER_CNTR2:
		if (writeflag == MEM_WRITE) {
			switch (d->rw_mode[c]) {
			case I8253_TIMER_LSB:
				d->counter[c] = idata & 0xff;
				pit_load(d, c);
				break;
			case I8253_TIMER_MSB:
				d->counter[c] = (idata & 0xff) << 8;
				pit_load(d, c);
				break;
			default:
				if (!d->write_msb_next[c]) {
					d->counter[c] = (d->counter[c] & 0xff00)
					    | (idata & 0xff);
					d->write_msb_next[c] = 1;
				} else {
					d->counter[c] = (d->counter[c] & 0x00ff)
					    | ((idata & 0xff) << 8);
					d->write_msb_next[c] = 0;
					pit_load(d, c);
				}
			}
		} else {
			value = d->latched[c]? d->latch_value[c] :
			    pit_count(d, c);
			switch (d->rw_mode[c]) {
			case I8253_TIMER_LSB:
				odata = value & 0xff;
				d->latched[c] = 0;
				break;
			case I8253_TIMER_MSB:
				odata = (value >> 8) & 0xff;
				d->latched[c] = 0;
				break;
			default:
				if (!d->read_msb_next[c]) {
					odata = value & 0xff;
					d->read_msb_next[c] = 1;
				} else {
					odata = (value >> 8) & 0xff;
					d->read_msb_next[c] = 0;
					d->latched[c] = 0;
				}
			}
		}
		break;

	case I8253_TIMER_MODE:
		if (writeflag == MEM_READ) {
			debug("[ 8253: read; can this actually happen? ]\n");
			break;
		}

		c = idata >> 6;
		if (c == 3) {
			/*  8254 read-back command: latch the counts of the
			    selected counters (status latching is not
			    emulated).  */
			if (!(idata & 0x20))
				for (c = 0; c < 3; c++)
					if (idata & (2 << c))
						pit_latch(d, c);
			break;
		}

		if ((idata & 0x30) == 0) {
			/*  Counter latch command:  */
			pit_latch(d, c);
			break;
		}

		d->rw_mode[c] = idata & 0x30;
		d->mode[c] = idata & 0x0e;
		d->write_msb_next[c] = d->read_msb_next[c] = 0;
		d->latched[c] = 0;
		debug("[ 8253: counter %i mode=0x%x access=0x%x ]\n", c,
		    d->mode[c], d->rw_mode[c]);

		if (idata & I8253_TIMER_BCD)
			debug("[ 8253: BCD counting not emulated ]\n");
		break;

	default:if (writeflag == MEM_WRITE) {
			fatal("[ 8253: unimplemented write to address 0x%x"
			    " data=0x%02x ]\n", (int)relative_addr, (int)idata);
		} else {
			fatal("[ 8253: unimplemented read from address 0x%x "
			    "]\n", (int)relative_addr);
		}
		exit(1);
	}

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, odata);

	return 1;
}


DEVINIT(8253)
{
	struct pit8253_data *d;

	CHECK_ALLOCATION(d = (struct pit8253_data *) malloc(sizeof(struct pit8253_data)));
	memset(d, 0, sizeof(struct pit8253_data));

	d->in_use = devinit->in_use;

	INTERRUPT_CONNECT(devinit->interrupt_path, d->irq);

	/*  Don't cause interrupt, by default.  */
	d->mode[0] = I8253_TIMER_RATEGEN;
	d->mode[1] = I8253_TIMER_RATEGEN;
	d->mode[2] = I8253_TIMER_RATEGEN;
	d->rw_mode[0] = d->rw_mode[1] = d->rw_mode[2] = I8253_TIMER_16BIT;

	devinit->machine->isa_pic_data.pending_timer_interrupts =
	    &d->pending_interrupts_timer0;

	memory_device_register(devinit->machine->memory, devinit->name,
	    devinit->addr, DEV_8253_LENGTH, dev_8253_access, (void *)d,
	    DM_DEFAULT, NULL);

	machine_add_tickfunction(devinit->machine, dev_8253_tick,
	    d, TICK_SHIFT);

	return 1;
}

