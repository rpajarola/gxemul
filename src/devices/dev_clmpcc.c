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
 *  Asynchronous mode, with interrupts, polling or DMA. Channel 0 is the
 *  console; the others are further serial ports.
 *
 *  The CD2401 has three interrupt requests (modem, transmit, receive). An
 *  interrupt acknowledge cycle (or a PCCchip2 pseudo-IACK) for a request
 *  makes the CD2401 enter that interrupt context for the requesting
 *  channel: the vector comes from LIVR, CAR selects the channel, and the
 *  data registers (RDR/TDR, RFOC, TFTC, RISR, ...) belong to it until the
 *  end-of-interrupt register is written.
 *
 *  DMA: each direction of a channel has two buffers (A and B) which the
 *  host hands to the chip by setting their ownership bit, and which the
 *  chip uses in turn. Transmission is paced at the programmed baud rate
 *  (in emulated time; only on the MVME187, the others send at once), so
 *  that 187Bug's loopback and baud rate self-tests see real line timing.
 *  Received characters are checked for the special characters, and a
 *  receive timeout ends a partly filled buffer.
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
#define	FCLK_HZ			20000000	/*  the chip's clock  */

/*  Interrupt types (as in LIVR):  */
#define	T_MODEM			1
#define	T_TX			2
#define	T_RX			3

/*  DMA buffer registers, by buffer (A, B): address upper and lower, byte
    count, status.  */
static const int rxbreg[2][4] = { { 0x40, 0x42, 0x4a, 0x4f },
				  { 0x44, 0x46, 0x48, 0x4e } };
static const int txbreg[2][4] = { { 0x50, 0x52, 0x5a, 0x5f },
				  { 0x54, 0x56, 0x58, 0x5e } };
#define	REG_DMABSTS		0x19
#define	REG_RCBADRU		0x3c
#define	REG_RCBADRL		0x3e
#define	REG_TCBADRU		0x38
#define	REG_TCBADRL		0x3a
#define	DMABSTS_NTBUF		0x08
#define	DMABSTS_TBUSY		0x04
#define	DMABSTS_NRBUF		0x02
#define	DMABSTS_RBUSY		0x01
#define	BSTS_OWN		0x01	/*  the chip owns the buffer  */
#define	BSTS_INTR		0x02	/*  (transmit) interrupt when sent  */
#define	BSTS_EOB		0x20
#define	BSTS_EOF		0x40
#define	ISR_BUFB		0x08	/*  RISRh/TISR: buffer B  */
#define	REOIR_TERMBUFF		0x80
#define	REOIR_DISCEXC		0x40
#define	RISRL_TIMEOUT		0x80

struct channel {
	uint8_t		reg[0x100];	/*  per-channel registers  */
	uint8_t		rxfifo[FIFO_LEN];
	int		rx_n;
	int		console_handle;

	/*  DMA:  */
	int		tx_buf;		/*  buffer being sent (0 = A, 1 = B)  */
	uint32_t	tx_addr;
	int		tx_left;
	uint64_t	tx_free;	/*  end of the last character (ns)  */
	bool		tx_sending;	/*  ... which is still being sent  */
	bool		tx_idle;	/*  nothing was sent after it  */
	int		tx_ev;		/*  TISR of a pending interrupt  */
	int		rx_buf;		/*  buffer being filled  */
	uint32_t	rx_addr;
	int		rx_cap, rx_cnt;
	int		rx_ev;		/*  RISR of a pending interrupt  */
	bool		rx_ev_char;	/*  ... an exception on rxfifo[0]  */
	uint64_t	rx_last;	/*  when data last arrived (ns)  */
	bool		rx_tmo;		/*  a receive timeout is due  */
};

struct clmpcc_data {
	int		subsys;		/*  for debugmsg  */
	struct machine	*machine;
	struct mvme187_data *board;	/*  emulated time, or NULL  */
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
		return true;	/*  (also LIVR, the vector, per channel)  */
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


static void transmit(struct clmpcc_data *d, int ch, uint8_t c);


/*****************************************************************************
 *
 *  DMA.
 */

static uint64_t now_ns(struct clmpcc_data *d)
{
	return d->board != NULL ? dev_mvme187_time_ns(d->board) : 0;
}

/*  The time to send one character, in ns (0 without a clock):  */
static uint64_t char_ns(struct clmpcc_data *d, struct channel *c)
{
	static const int clkdiv[8] = { 8, 32, 128, 512, 2048, 0, 0, 0 };
	int div = clkdiv[c->reg[CLMPCC_REG_TCOR] >> 5];
	int halfbits, stop;

	if (d->board == NULL || div == 0)
		return 0;

	/*  Start bit, data bits, parity, stop bits:  */
	halfbits = 2 * (1 + (c->reg[CLMPCC_REG_COR1] & 7) + 1);
	if (c->reg[CLMPCC_REG_COR1] & 0x60)
		halfbits += 2;
	stop = c->reg[CLMPCC_REG_COR3] & 7;
	halfbits += stop >= 2 && stop <= 4 ? stop : 2;

	return (uint64_t) halfbits * div * (c->reg[CLMPCC_REG_TBPR] + 1) *
	    (1000000000 / FCLK_HZ) / 2;
}

/*  The receive timeout, in ns (RTPR ticks of the timer prescaler):  */
static uint64_t rx_timeout_ns(struct clmpcc_data *d, struct channel *c)
{
	uint64_t rtpr = (c->reg[CLMPCC_REG_RTPRh] << 8) |
	    c->reg[CLMPCC_REG_RTPRl];

	return rtpr * d->greg[CLMPCC_REG_TPR] * 2048 *
	    (1000000000 / FCLK_HZ);
}

static uint32_t reg16(struct channel *c, int r)
{
	return (c->reg[r] << 8) | c->reg[r + 1];
}

static void set_reg16(struct channel *c, int r, uint32_t v)
{
	c->reg[r] = v >> 8;
	c->reg[r + 1] = v;
}

static uint32_t buf_addr(struct channel *c, const int *r)
{
	return (reg16(c, r[0]) << 16) | reg16(c, r[1]);
}

static void mem_rw(struct clmpcc_data *d, uint32_t addr, uint8_t *b,
	int writeflag)
{
	struct cpu *cpu = d->machine->cpus[d->machine->bootstrap_cpu];
	cpu->memory_rw(cpu, cpu->mem, addr, b, 1, writeflag,
	    PHYSICAL | NO_EXCEPTIONS);
}

static void dma_reset(struct channel *c)
{
	int b;

	c->tx_buf = c->rx_buf = -1;
	c->tx_sending = false;
	c->tx_idle = true;
	c->tx_ev = c->rx_ev = -1;
	c->rx_tmo = false;
	c->reg[REG_DMABSTS] = 0;
	for (b = 0; b < 2; b++)
		c->reg[rxbreg[b][3]] = c->reg[txbreg[b][3]] = 0;
}

/*  Is the transmitter idle (no buffer to send, and the line quiet)?  */
static bool tx_idle(struct clmpcc_data *d, struct channel *c)
{
	int b = c->reg[REG_DMABSTS] & DMABSTS_NTBUF ? 1 : 0;

	return c->tx_buf < 0 && !(c->reg[txbreg[b][3]] & BSTS_OWN) &&
	    c->tx_free <= now_ns(d);
}

/*  Transmit DMA: send the characters that are due.  */
static void dma_tx(struct clmpcc_data *d, int i, uint64_t now)
{
	struct channel *c = &d->ch[i];
	uint64_t t = char_ns(d, c);
	const int *r;
	uint8_t ch;

	if (!(c->reg[CLMPCC_REG_CMR] & CLMPCC_CMR_TX_DMA) ||
	    !(c->reg[CLMPCC_REG_CSR] & CLMPCC_CSR_TX_ENABLED)) {
		c->tx_idle = true;
		return;
	}

	for (;;) {
		if (c->tx_buf < 0) {
			int b = c->reg[REG_DMABSTS] & DMABSTS_NTBUF ? 1 : 0;
			r = txbreg[b];
			if (!(c->reg[r[3]] & BSTS_OWN)) {
				c->tx_idle = true;
				return;
			}
			c->tx_buf = b;
			c->tx_addr = buf_addr(c, r);
			c->tx_left = reg16(c, r[2]);
			c->reg[REG_DMABSTS] = (c->reg[REG_DMABSTS] ^
			    DMABSTS_NTBUF) | DMABSTS_TBUSY;
			debugmsg(d->subsys, "dma", VERBOSITY_DEBUG,
			    "ch%i tx buffer %c: 0x%08x, %i bytes", i, 'A' + b,
			    c->tx_addr, c->tx_left);
		}
		r = txbreg[c->tx_buf];

		if (c->tx_left > 0) {
			/*  The next character: back to back, or starting now
			    after a pause. It is received when it is sent.  */
			if (!c->tx_sending) {
				c->tx_free = (c->tx_idle && c->tx_free < now ?
				    now : c->tx_free) + t;
				c->tx_sending = true;
				c->tx_idle = false;
			}
			if (c->tx_free > now)
				return;
			c->tx_sending = false;
			mem_rw(d, c->tx_addr, &ch, MEM_READ);
			c->tx_addr ++;
			c->tx_left --;
			set_reg16(c, REG_TCBADRU, c->tx_addr >> 16);
			set_reg16(c, REG_TCBADRL, c->tx_addr);
			transmit(d, i, ch);
			continue;
		}

		/*  The buffer is done. One interrupt at a time:  */
		if ((c->reg[r[3]] & BSTS_INTR) && c->tx_ev >= 0) {
			c->tx_idle = true;
			return;
		}
		if (c->reg[r[3]] & BSTS_INTR)
			c->tx_ev = BSTS_EOB | (c->reg[r[3]] & BSTS_EOF) |
			    (c->tx_buf ? ISR_BUFB : 0);
		c->reg[r[3]] = (c->reg[r[3]] & ~BSTS_OWN) | BSTS_EOB;
		c->reg[REG_DMABSTS] &= ~DMABSTS_TBUSY;
		c->tx_buf = -1;
	}
}

/*  Which special character (SCdet value) is ch, if any?  */
static int special_char(struct channel *c, uint8_t ch)
{
	uint8_t cor3 = c->reg[CLMPCC_REG_COR3];

	if (cor3 & CLMPCC_COR3_SCDE) {
		if (ch == c->reg[CLMPCC_REG_SCHR1])
			return 1;
		if (ch == c->reg[CLMPCC_REG_SCHR2])
			return 2;
	}
	if (cor3 & CLMPCC_COR3_ESCDE) {
		if (ch == c->reg[CLMPCC_REG_SCHR3])
			return 3;
		if (ch == c->reg[CLMPCC_REG_SCHR4])
			return 4;
	}
	if ((cor3 & CLMPCC_COR3_RngDE) && ch >= c->reg[CLMPCC_REG_SCRl] &&
	    ch <= c->reg[CLMPCC_REG_SCRh])
		return 7;
	return 0;
}

/*  The buffer that receive status is about (RISRh bit 3):  */
static int rx_bufbit(struct channel *c)
{
	int b = c->rx_buf >= 0 ? c->rx_buf :
	    (c->reg[REG_DMABSTS] & DMABSTS_NRBUF ? 1 : 0);
	return b ? ISR_BUFB << 8 : 0;
}

/*  Put a received character in the current receive buffer. Returns false
    if there is no buffer.  */
static bool rx_put(struct clmpcc_data *d, int i, uint8_t ch, uint64_t now)
{
	struct channel *c = &d->ch[i];
	const int *r;

	if (c->rx_buf < 0) {
		int b = c->reg[REG_DMABSTS] & DMABSTS_NRBUF ? 1 : 0;
		r = rxbreg[b];
		if (!(c->reg[r[3]] & BSTS_OWN))
			return false;
		c->rx_buf = b;
		c->rx_addr = buf_addr(c, r);
		c->rx_cap = reg16(c, r[2]);
		c->rx_cnt = 0;
		c->reg[REG_DMABSTS] = (c->reg[REG_DMABSTS] ^ DMABSTS_NRBUF) |
		    DMABSTS_RBUSY;
		debugmsg(d->subsys, "dma", VERBOSITY_DEBUG,
		    "ch%i rx buffer %c: 0x%08x, %i bytes", i, 'A' + b,
		    c->rx_addr, c->rx_cap);
	}
	r = rxbreg[c->rx_buf];

	mem_rw(d, c->rx_addr, &ch, MEM_WRITE);
	c->rx_addr ++;
	c->rx_cnt ++;
	set_reg16(c, REG_RCBADRU, c->rx_addr >> 16);
	set_reg16(c, REG_RCBADRL, c->rx_addr);
	c->rx_last = now;
	c->rx_tmo = true;

	if (c->rx_cnt >= c->rx_cap) {
		/*  The buffer is full:  */
		set_reg16(c, r[2], c->rx_cnt);
		c->reg[r[3]] = (c->reg[r[3]] & ~BSTS_OWN) | BSTS_EOB;
		c->reg[REG_DMABSTS] &= ~DMABSTS_RBUSY;
		c->rx_ev = (BSTS_EOB << 8) | (c->rx_buf ? ISR_BUFB << 8 : 0);
		c->rx_ev_char = false;
		c->rx_buf = -1;
	}
	return true;
}

static void rx_pop(struct channel *c)
{
	memmove(c->rxfifo, c->rxfifo + 1, --c->rx_n);
}

/*  Receive DMA: move characters from the FIFO to the buffers.  */
static void dma_rx(struct clmpcc_data *d, int i, uint64_t now)
{
	struct channel *c = &d->ch[i];
	uint64_t tmo;
	int sc;

	if (!(c->reg[CLMPCC_REG_CMR] & CLMPCC_CMR_RX_DMA) ||
	    !(c->reg[CLMPCC_REG_CSR] & CLMPCC_CSR_RX_ENABLED))
		return;

	while (c->rx_ev < 0 && c->rx_n > 0) {
		sc = special_char(c, c->rxfifo[0]);
		if (sc != 0) {
			/*  An exception; the host says what to do with it.  */
			c->rx_ev = rx_bufbit(c) | (sc << 4);
			c->rx_ev_char = true;
			debugmsg(d->subsys, "dma", VERBOSITY_DEBUG,
			    "ch%i special character %i (0x%02x) at %llu ns", i,
			    sc, c->rxfifo[0], (unsigned long long) now);
			return;
		}
		if (!rx_put(d, i, c->rxfifo[0], now))
			return;
		rx_pop(c);
	}

	/*  No data for a while: tell the host about a partly full buffer.  */
	tmo = rx_timeout_ns(d, c);
	if (c->rx_ev < 0 && c->rx_n == 0 && c->rx_tmo && tmo != 0 &&
	    (c->reg[CLMPCC_REG_IER] & CLMPCC_IER_RET) &&
	    now >= c->rx_last + tmo) {
		c->rx_ev = rx_bufbit(c) | RISRL_TIMEOUT;
		c->rx_ev_char = false;
		c->rx_tmo = false;
	}
}

/*  The end of a receive interrupt, in DMA mode.  */
static void dma_reoir(struct clmpcc_data *d, int i, uint8_t v)
{
	struct channel *c = &d->ch[i];
	bool exc_char = c->rx_ev >= 0 && c->rx_ev_char;

	if (c->rx_ev < 0)
		return;
	c->rx_ev = -1;

	if (v & REOIR_TERMBUFF) {
		/*  Go on with the other buffer.  */
		c->rx_buf = -1;
		c->reg[REG_DMABSTS] &= ~DMABSTS_RBUSY;
	}

	if (exc_char && c->rx_n > 0) {
		/*  The exception character goes to the buffer, unless it
		    is to be discarded (or there is no buffer). This may
		    fill the buffer, which is a new interrupt.  */
		if (!(v & REOIR_DISCEXC))
			rx_put(d, i, c->rxfifo[0], now_ns(d));
		rx_pop(c);
	}
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
			debugmsg(d->subsys, "rx", VERBOSITY_DEBUG,
			    "'%c' ch%i csr=%02x ier=%02x cor4=%02x ctx=%i/%i/%i"
			    " stk=%02x car=%02x irq=%i/%i/%i cmr=%02x dmabsts="
			    "%02x rx_n=%i rx_ev=%i tx_ev=%i rxbsts=%02x/%02x "
			    "txbsts=%02x/%02x", ch, i,
			    c->reg[CLMPCC_REG_CSR], c->reg[CLMPCC_REG_IER],
			    c->reg[CLMPCC_REG_COR4], d->ctx[1], d->ctx[2],
			    d->ctx[3], d->greg[CLMPCC_REG_STK],
			    d->greg[CLMPCC_REG_CAR], d->irq_state[1],
			    d->irq_state[2], d->irq_state[3],
			    c->reg[CLMPCC_REG_CMR], c->reg[REG_DMABSTS],
			    c->rx_n, c->rx_ev, c->tx_ev, c->reg[rxbreg[0][3]],
			    c->reg[rxbreg[1][3]], c->reg[txbreg[0][3]],
			    c->reg[txbreg[1][3]]);
			if (!(c->reg[CLMPCC_REG_CSR] & CLMPCC_CSR_RX_ENABLED))
				continue;
			c->rxfifo[c->rx_n++] = ch;
			c->rx_last = now_ns(d);
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
		if (c->reg[CLMPCC_REG_CMR] & CLMPCC_CMR_RX_DMA)
			return c->rx_ev >= 0;
		if (!(c->reg[CLMPCC_REG_CSR] & CLMPCC_CSR_RX_ENABLED) ||
		    c->rx_n == 0)
			return false;
		if (ier & CLMPCC_IER_RET)
			return true;
		if (ier & CLMPCC_IER_RX_FIFO) {
			int thr = c->reg[CLMPCC_REG_COR4] & CLMPCC_COR4_FIFO_MASK;
			/*  Below the threshold, the data is passed on after
			    the receive timeout:  */
			return c->rx_n >= (thr == 0 ? 1 : thr) ||
			    d->board == NULL ||
			    now_ns(d) >= c->rx_last + rx_timeout_ns(d, c);
		}
		return false;
	case T_TX:
		if (c->reg[CLMPCC_REG_CMR] & CLMPCC_CMR_TX_DMA)
			return c->tx_ev >= 0 || ((c->reg[CLMPCC_REG_CSR] &
			    CLMPCC_CSR_TX_ENABLED) && (ier & CLMPCC_IER_TX_EMPTY)
			    && tx_idle(d, c));
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

void dev_clmpcc_tick(struct cpu *, void *);

/*  Have the next thing that is due (a character sent, a receive timeout)
    happen on time, not at the next tick.  */
static void schedule(struct clmpcc_data *d, uint64_t now)
{
	uint64_t next = UINT64_MAX, tmo;
	int i;

	if (d->board == NULL)
		return;

	for (i = 0; i < N_CHANNELS; i++) {
		struct channel *c = &d->ch[i];
		if ((c->reg[CLMPCC_REG_CMR] & CLMPCC_CMR_TX_DMA) &&
		    c->tx_free > now && c->tx_free < next)
			next = c->tx_free;
		tmo = rx_timeout_ns(d, c);
		if (!(c->reg[CLMPCC_REG_CMR] & CLMPCC_CMR_RX_DMA) &&
		    c->rx_n > 0 && c->rx_last + tmo > now &&
		    c->rx_last + tmo < next)
			next = c->rx_last + tmo;
		if ((c->reg[CLMPCC_REG_CMR] & CLMPCC_CMR_RX_DMA) &&
		    c->rx_tmo && tmo != 0 &&
		    (c->reg[CLMPCC_REG_IER] & CLMPCC_IER_RET) &&
		    c->rx_last + tmo > now && c->rx_last + tmo < next)
			next = c->rx_last + tmo;
	}

	if (next != UINT64_MAX)
		dev_mvme187_at_ns(d->board, next, dev_clmpcc_tick, d);
}

static void update_irqs(struct clmpcc_data *d)
{
	uint64_t now = now_ns(d);
	int type, i;

	rx_poll(d);

	for (i = 0; i < N_CHANNELS; i++) {
		dma_tx(d, i, now);
		dma_rx(d, i, now);
	}

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

	schedule(d, now);
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
	int ch, ctxreg;
	uint8_t en;

	update_irqs(d);

	/*  The vector is the channel's LIVR with the type in bits 1-0.  */
	if (d->ctx[type] >= 0 || (ch = requesting_channel(d, type)) < 0)
		return d->ch[cur_channel(d, 0)].reg[CLMPCC_REG_LIVR] & 0xfc;

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
		int ev = d->ch[ch].rx_ev;
		if (!(d->ch[ch].reg[CLMPCC_REG_CMR] & CLMPCC_CMR_RX_DMA) ||
		    ev < 0)
			ev = 0;
		d->greg[CLMPCC_REG_RISRl] = ev;
		d->greg[CLMPCC_REG_RISRh] = ev >> 8;
	}
	if (type == T_TX) {
		if (d->ch[ch].reg[CLMPCC_REG_CMR] & CLMPCC_CMR_TX_DMA)
			d->greg[CLMPCC_REG_TISR] = d->ch[ch].tx_ev >= 0 ?
			    d->ch[ch].tx_ev : CLMPCC_TISR_TX_EMPTY;
		else
			d->greg[CLMPCC_REG_TISR] = CLMPCC_TISR_TX_EMPTY |
			    CLMPCC_TISR_TX_FIFO;
	}
	debugmsg(d->subsys, "iack", VERBOSITY_DEBUG, "type %i ch%i risr %02x%02x"
	    " tisr %02x at %llu ns", type, ch, d->greg[CLMPCC_REG_RISRh],
	    d->greg[CLMPCC_REG_RISRl], d->greg[CLMPCC_REG_TISR],
	    (unsigned long long) now_ns(d));

	update_irqs(d);
	return (d->ch[ch].reg[CLMPCC_REG_LIVR] & 0xfc) | type;
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
		    (chan->reg[CLMPCC_REG_CSR] & CLMPCC_CSR_RX_ENABLED)) {
			chan->rxfifo[chan->rx_n++] = c;
			chan->rx_last = now_ns(d);
		}
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
		dma_reset(&d->ch[i]);
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
		dma_reset(c);
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
		if (d->ctx[T_RX] >= 0 && (d->ch[d->ctx[T_RX]].reg[CLMPCC_REG_CMR]
		    & CLMPCC_CMR_RX_DMA))
			dma_reoir(d, d->ctx[T_RX], v);
		end_of_interrupt(d, T_RX);
		return;
	case CLMPCC_REG_TEOIR:
		if (d->ctx[T_TX] >= 0)
			d->ch[d->ctx[T_TX]].tx_ev = -1;
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
	case REG_DMABSTS:
		/*  Read-only (or context) registers.  */
		return;
	case CLMPCC_REG_CAR:
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
	d->subsys = debugmsg_register_subsystem("clmpcc");
	d->machine = devinit->machine;
	d->board = devinit->machine->md.mvme187;

	for (i = 0; i < 4; i++)
		d->ctx[i] = -1;
	for (i = 0; i < N_CHANNELS; i++)
		dma_reset(&d->ch[i]);
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
