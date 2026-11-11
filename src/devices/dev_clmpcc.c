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
 *  COMMENT: Cirrus Logic CD2400/CD2401 four channel serial controller
 *
 *  Asynchronous mode, with interrupts or polling (no DMA yet). Channel 0 is
 *  the console; the others are further serial ports.
 *
 *  The CD2401 has three interrupt requests (modem, transmit, receive). An
 *  interrupt acknowledge cycle (or a PCCchip2 pseudo-IACK) for a request
 *  makes the CD2401 enter that interrupt context for the requesting
 *  channel: the vector comes from LIVR, CAR selects the channel, and the
 *  data registers (RDR/TDR, RFOC, TFTC, RISR, ...) belong to it until the
 *  end-of-interrupt register is written.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "cpu.h"
#include "device.h"
#include "devices.h"
#include "emul.h"
#include "interrupt.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "thirdparty/mvme_pcctworeg.h"
#include "thirdparty/clmpccreg.h"


#define	CLMPCC_LEN		0x1000
#define	DEV_CLMPCC_TICK_SHIFT	14
#define	N_CHANNELS		4
#define	FIFO_LEN		16
#define	GFRCR_VALUE		0x13

/*  Interrupt types (as in LIVR):  */
#define	T_MODEM			1
#define	T_TX			2
#define	T_RX			3

struct channel {
	uint8_t		reg[0x100];	/*  per-channel registers  */
	uint8_t		rxfifo[FIFO_LEN];
	int		rx_n;
	int		console_handle;
};

struct clmpcc_data {
	uint8_t		greg[0x100];	/*  global registers  */
	struct channel	ch[N_CHANNELS];

	/*  Interrupt contexts (channel, or -1), and the saved CAR:  */
	int		ctx[4];
	int		saved_car[4];

	struct interrupt irq_m;
	struct interrupt irq_tx;
	struct interrupt irq_rx;
	int		irq_state[4];
};


/*  Registers that belong to the channel selected by CAR:  */
static bool per_channel(int r)
{
	if (r < 0x80)
		return r != CLMPCC_REG_LIVR;
	switch (r) {
	case CLMPCC_REG_TCOR: case CLMPCC_REG_TBPR:
	case CLMPCC_REG_RCOR: case CLMPCC_REG_RBPR:
	case CLMPCC_REG_CPSR: case CLMPCC_REG_MSVR_RTS:
	case CLMPCC_REG_MSVR_DTR:
		return true;
	}
	return false;
}

/*  The current channel: that of the active context, or CAR.  */
static int cur_channel(struct clmpcc_data *d, int type)
{
	if (type > 0 && d->ctx[type] >= 0)
		return d->ctx[type];
	return d->greg[CLMPCC_REG_CAR] & CLMPCC_CAR_MASK;
}


static void rx_poll(struct clmpcc_data *d)
{
	int i;

	for (i = 0; i < N_CHANNELS; i++) {
		struct channel *c = &d->ch[i];
		if (c->console_handle < 0)
			continue;
		while (c->rx_n < FIFO_LEN &&
		    console_charavail(c->console_handle)) {
			int ch = console_readchar(c->console_handle);
			if (ch < 0)
				break;
			if (!(c->reg[CLMPCC_REG_CSR] & CLMPCC_CSR_RX_ENABLED))
				continue;
			c->rxfifo[c->rx_n++] = ch;
		}
	}
}

/*  Does channel i request an interrupt of a type?  */
static bool chan_wants(struct clmpcc_data *d, int i, int type)
{
	struct channel *c = &d->ch[i];
	uint8_t ier = c->reg[CLMPCC_REG_IER];

	switch (type) {
	case T_RX:
		if (!(c->reg[CLMPCC_REG_CSR] & CLMPCC_CSR_RX_ENABLED) ||
		    c->rx_n == 0)
			return false;
		if (ier & CLMPCC_IER_RET)
			return true;
		if (ier & CLMPCC_IER_RX_FIFO) {
			int thr = c->reg[CLMPCC_REG_COR4] & CLMPCC_COR4_FIFO_MASK;
			return c->rx_n >= (thr == 0 ? 1 : thr);
		}
		return false;
	case T_TX:
		/*  Characters go out at once, so the FIFO is empty.  */
		return (c->reg[CLMPCC_REG_CSR] & CLMPCC_CSR_TX_ENABLED) &&
		    (ier & (CLMPCC_IER_TX_EMPTY | CLMPCC_IER_TX_FIFO));
	}
	return false;
}

static int requesting_channel(struct clmpcc_data *d, int type)
{
	int i;

	for (i = 0; i < N_CHANNELS; i++)
		if (chan_wants(d, i, type))
			return i;
	return -1;
}

static void update_irqs(struct clmpcc_data *d)
{
	int type;

	rx_poll(d);

	for (type = T_MODEM; type <= T_RX; type++) {
		/*  No new request of a type while in its context.  */
		int want = d->ctx[type] < 0 && requesting_channel(d, type) >= 0;
		struct interrupt *irq = type == T_MODEM ? &d->irq_m :
		    type == T_TX ? &d->irq_tx : &d->irq_rx;

		if (want == d->irq_state[type])
			continue;
		d->irq_state[type] = want;
		if (want)
			INTERRUPT_ASSERT(*irq);
		else
			INTERRUPT_DEASSERT(*irq);
	}
}


/*
 *  The stack register holds the current, previous and second previous
 *  interrupt level (1 = modem, 2 = transmit, 3 = receive), with their high
 *  bits in bits 7-5 and their low bits in bits 0-2: CL1 PL1 SL1 0 0 SL0
 *  PL0 CL0.
 */
static uint8_t stk_push(uint8_t stk, int level)
{
	uint8_t hi = stk & 0xe0, lo = stk & 0x07;

	hi = ((hi >> 1) & 0x60) | ((level & 2) << 6);
	lo = ((lo << 1) & 0x06) | (level & 1);
	return hi | lo;
}

static uint8_t stk_pop(uint8_t stk)
{
	return ((stk << 1) & 0xe0) | ((stk >> 1) & 0x03);
}


/*
 *  clmpcc_iack():
 *
 *  An interrupt acknowledge (type 1 = modem, 2 = transmit, 3 = receive)
 *  from the PCCchip2. Enters the interrupt context and returns the vector.
 */
static int clmpcc_iack(void *extra, int type)
{
	struct clmpcc_data *d = (struct clmpcc_data *) extra;
	uint8_t livr = d->greg[CLMPCC_REG_LIVR] & 0xf0;
	int ch, ctxreg;
	uint8_t en;

	update_irqs(d);

	/*  The vector is LIVR with the channel in bits 3-2 and the type
	    in bits 1-0.  */
	if (d->ctx[type] >= 0 || (ch = requesting_channel(d, type)) < 0)
		return livr;

	d->ctx[type] = ch;
	d->saved_car[type] = d->greg[CLMPCC_REG_CAR];
	d->greg[CLMPCC_REG_CAR] = ch;
	d->greg[CLMPCC_REG_LICR] = ch << 2;
	d->greg[CLMPCC_REG_STK] = stk_push(d->greg[CLMPCC_REG_STK], type);

	ctxreg = type == T_MODEM ? CLMPCC_REG_MIR : type == T_TX ?
	    CLMPCC_REG_TIR : CLMPCC_REG_RIR;
	en = 0x80 | 0x40;	/*  enabled, active  */
	d->greg[ctxreg] = en | ch;

	if (type == T_RX) {
		d->greg[CLMPCC_REG_RISRl] = 0;
		d->greg[CLMPCC_REG_RISRh] = 0;
	}
	if (type == T_TX)
		d->greg[CLMPCC_REG_TISR] = CLMPCC_TISR_TX_EMPTY |
		    CLMPCC_TISR_TX_FIFO;

	update_irqs(d);
	return livr | (ch << 2) | type;
}

static void end_of_interrupt(struct clmpcc_data *d, int type)
{
	int ctxreg = type == T_MODEM ? CLMPCC_REG_MIR : type == T_TX ?
	    CLMPCC_REG_TIR : CLMPCC_REG_RIR;

	if (d->ctx[type] < 0)
		return;
	d->greg[CLMPCC_REG_CAR] = d->saved_car[type];
	d->greg[ctxreg] = 0;
	d->greg[CLMPCC_REG_STK] = stk_pop(d->greg[CLMPCC_REG_STK]);
	d->ctx[type] = -1;
}


static void transmit(struct clmpcc_data *d, int ch, uint8_t c)
{
	struct channel *chan = &d->ch[ch];

	/*  Local loopback: the receiver gets what is sent.  */
	if (chan->reg[CLMPCC_REG_TCOR] & CLMPCC_TCOR_LOCAL_LOOP) {
		if (chan->rx_n < FIFO_LEN &&
		    (chan->reg[CLMPCC_REG_CSR] & CLMPCC_CSR_RX_ENABLED))
			chan->rxfifo[chan->rx_n++] = c;
		return;
	}

	if (chan->console_handle >= 0)
		console_putchar(chan->console_handle, c);
}

static void reset_all(struct clmpcc_data *d)
{
	int i;

	for (i = 0; i < N_CHANNELS; i++) {
		int h = d->ch[i].console_handle;
		memset(&d->ch[i], 0, sizeof(d->ch[i]));
		d->ch[i].console_handle = h;
	}
	for (i = 0; i < 4; i++)
		d->ctx[i] = -1;
	memset(d->greg, 0, sizeof(d->greg));
	d->greg[CLMPCC_REG_GFRCR] = GFRCR_VALUE;
}

static void clmpcc_reset(void *extra)
{
	struct clmpcc_data *d = (struct clmpcc_data *) extra;
	reset_all(d);
	update_irqs(d);
}

static void channel_command(struct clmpcc_data *d, int ch, uint8_t cmd)
{
	struct channel *c = &d->ch[ch];

	if (cmd & 0x80) {
		/*  Type 1: clear timers, clear receiver.  */
		if (cmd & 0x10)
			c->rx_n = 0;
		return;
	}

	if (cmd & CLMPCC_CCR_T0_RESET_ALL) {
		reset_all(d);
		return;
	}

	if (cmd & CLMPCC_CCR_T0_CLEAR) {
		c->rx_n = 0;
		c->reg[CLMPCC_REG_CSR] = 0;
	}

	if (cmd & CLMPCC_CCR_T0_TX_EN)
		c->reg[CLMPCC_REG_CSR] |= CLMPCC_CSR_TX_ENABLED;
	if (cmd & CLMPCC_CCR_T0_TX_DIS)
		c->reg[CLMPCC_REG_CSR] &= ~CLMPCC_CSR_TX_ENABLED;
	if (cmd & CLMPCC_CCR_T0_RX_EN)
		c->reg[CLMPCC_REG_CSR] |= CLMPCC_CSR_RX_ENABLED;
	if (cmd & CLMPCC_CCR_T0_RX_DIS) {
		c->reg[CLMPCC_REG_CSR] &= ~CLMPCC_CSR_RX_ENABLED;
		c->rx_n = 0;
	}
}


static uint8_t reg_read(struct clmpcc_data *d, int r)
{
	struct channel *c;
	int ch;
	uint8_t v;

	switch (r) {
	case CLMPCC_REG_RDR:
		ch = cur_channel(d, T_RX);
		c = &d->ch[ch];
		if (c->rx_n == 0)
			return 0;
		v = c->rxfifo[0];
		memmove(c->rxfifo, c->rxfifo + 1, --c->rx_n);
		return v;
	case CLMPCC_REG_RFOC:
		return d->ch[cur_channel(d, T_RX)].rx_n;
	case CLMPCC_REG_TFTC:
		return FIFO_LEN;
	case CLMPCC_REG_CCR:
		return 0;	/*  commands complete at once  */
	case CLMPCC_REG_STCR:
		return 0;
	case CLMPCC_REG_MSVR_RTS:
	case CLMPCC_REG_MSVR_DTR:
		/*  DSR, CD and CTS on; DTR and RTS as written.  */
		c = &d->ch[cur_channel(d, 0)];
		return CLMPCC_MSVR_DSR | CLMPCC_MSVR_CD | CLMPCC_MSVR_CTS |
		    CLMPCC_MSVR_PORT_ID |
		    (c->reg[CLMPCC_REG_MSVR_DTR] & CLMPCC_MSVR_DTR) |
		    (c->reg[CLMPCC_REG_MSVR_RTS] & CLMPCC_MSVR_RTS);
	case CLMPCC_REG_GFRCR:
	case CLMPCC_REG_CAR:
	case CLMPCC_REG_LIVR:
	case CLMPCC_REG_LICR:
	case CLMPCC_REG_STK:
	case CLMPCC_REG_RIR:
	case CLMPCC_REG_TIR:
	case CLMPCC_REG_MIR:
	case CLMPCC_REG_RISRl:
	case CLMPCC_REG_RISRh:
	case CLMPCC_REG_TISR:
	case CLMPCC_REG_MISR:
		return d->greg[r];
	}

	if (per_channel(r))
		return d->ch[cur_channel(d, 0)].reg[r];
	return d->greg[r];
}

static void reg_write(struct clmpcc_data *d, int r, uint8_t v)
{
	struct channel *c;
	int ch;

	switch (r) {
	case CLMPCC_REG_TDR:
		transmit(d, cur_channel(d, T_TX), v);
		return;
	case CLMPCC_REG_CCR:
		channel_command(d, cur_channel(d, 0), v);
		return;
	case CLMPCC_REG_STCR:
		ch = cur_channel(d, 0);
		c = &d->ch[ch];
		if ((v & CLMPCC_STCR_SND_SPC) && (v & 7) >= 1 && (v & 7) <= 4) {
			/*  SCHR1..4 are at 0x1f down to 0x1c.  */
			transmit(d, ch, c->reg[CLMPCC_REG_SCHR1 + 1 - (v & 7)]);
		}
		return;
	case CLMPCC_REG_REOIR:
		end_of_interrupt(d, T_RX);
		return;
	case CLMPCC_REG_TEOIR:
		end_of_interrupt(d, T_TX);
		return;
	case CLMPCC_REG_MEOIR:
		end_of_interrupt(d, T_MODEM);
		return;
	case CLMPCC_REG_GFRCR:
	case CLMPCC_REG_LICR:
	case CLMPCC_REG_RIR:
	case CLMPCC_REG_TIR:
	case CLMPCC_REG_MIR:
	case CLMPCC_REG_RISRl:
	case CLMPCC_REG_RISRh:
	case CLMPCC_REG_TISR:
	case CLMPCC_REG_MISR:
	case CLMPCC_REG_RFOC:
	case CLMPCC_REG_TFTC:
		/*  Read-only (or context) registers.  */
		return;
	case CLMPCC_REG_CAR:
	case CLMPCC_REG_LIVR:
	case CLMPCC_REG_STK:
		d->greg[r] = v;
		return;
	}

	if (per_channel(r))
		d->ch[cur_channel(d, 0)].reg[r] = v;
	else
		d->greg[r] = v;
}


DEVICE_TICK(clmpcc)
{
	struct clmpcc_data *d = (struct clmpcc_data *) extra;
	update_irqs(d);
}


DEVICE_ACCESS(clmpcc)
{
	struct clmpcc_data *d = (struct clmpcc_data *) extra;
	int r = relative_addr & 0xff;
	size_t i;

	/*  8- and 16-bit registers only; 32-bit cycles are not answered.  */
	if (len > 2)
		return 0;

	for (i = 0; i < len; i++) {
		if (writeflag == MEM_READ)
			data[i] = reg_read(d, (r + i) & 0xff);
		else
			reg_write(d, (r + i) & 0xff, data[i]);
	}

	update_irqs(d);
	return 1;
}


DEVINIT(clmpcc)
{
	struct clmpcc_data *d;
	char tmpstr[300];
	int i;

	CHECK_ALLOCATION(d = (struct clmpcc_data *) malloc(sizeof(struct clmpcc_data)));
	memset(d, 0, sizeof(struct clmpcc_data));

	for (i = 0; i < 4; i++)
		d->ctx[i] = -1;
	d->greg[CLMPCC_REG_GFRCR] = GFRCR_VALUE;

	d->ch[0].console_handle =
	    console_start_slave(devinit->machine, devinit->name2 != NULL?
	    devinit->name2 : devinit->name, devinit->in_use);
	for (i = 1; i < N_CHANNELS; i++) {
		snprintf(tmpstr, sizeof(tmpstr), "cl%i", i);
		d->ch[i].console_handle = console_start_slave(devinit->machine,
		    tmpstr, CONSOLE_OUTPUT_ONLY);
	}

	/*  Interrupt request lines to the PCCchip2 ("...pcc2.n"):  */
	snprintf(tmpstr, sizeof(tmpstr), "%s.%i",
	    devinit->interrupt_path, PCC2V_SCC_M);
	INTERRUPT_CONNECT(tmpstr, d->irq_m);
	snprintf(tmpstr, sizeof(tmpstr), "%s.%i",
	    devinit->interrupt_path, PCC2V_SCC_TX);
	INTERRUPT_CONNECT(tmpstr, d->irq_tx);
	snprintf(tmpstr, sizeof(tmpstr), "%s.%i",
	    devinit->interrupt_path, PCC2V_SCC_RX);
	INTERRUPT_CONNECT(tmpstr, d->irq_rx);

	memory_device_register(devinit->machine->memory, devinit->name,
	    devinit->addr, CLMPCC_LEN, dev_clmpcc_access, (void *)d,
	    DM_DEFAULT, NULL);

	machine_add_tickfunction(devinit->machine,
	    dev_clmpcc_tick, d, DEV_CLMPCC_TICK_SHIFT);

	if (devinit->machine->md.mvme187 != NULL)
		dev_mvme187_set_scc(devinit->machine->md.mvme187,
		    clmpcc_iack, clmpcc_reset, d);

	/*
	 *  NOTE:  Ugly cast into a pointer, because this is a convenient way
	 *         to return the console handle to code in src/machines/.
	 */
	devinit->return_ptr = (void *)(size_t)d->ch[0].console_handle;

	return 1;
}
