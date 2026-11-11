/*
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
 *  COMMENT: Intel 82596CA LAN coprocessor (as on the MVME167/187)
 *
 *  The chip is driven through two registers: the MPU port (a 32-bit
 *  command written as two 16-bit halves, the low half first) and channel
 *  attention. Everything else is in memory, which the chip reads and
 *  writes by DMA: the SCP, ISCP and SCB, the command list and the receive
 *  frame area.
 *
 *  Only the linear addressing mode is implemented. The chip is wired for
 *  big-endian operation: 16-bit fields are big-endian, a 32-bit field is
 *  two 16-bit halves with the low half first, and byte strings (addresses,
 *  configuration bytes, frame data) are in address order.
 *
 *  Implemented: port reset/self-test/alternate SCP/dump, the SCB commands,
 *  NOP, IA setup, configure, MC setup, transmit (simplified and flexible),
 *  TDR, dump and diagnose commands, receive into RFDs (simplified and
 *  flexible, with RBDs), internal and external loopback, address filtering.
 *
 *  Not implemented: the 82586 and segmented modes, throttle timers, bus
 *  throttling, error counters, collision and other error statistics.
 *
 *  Debug messages: subsystem "i82596" (info: commands and frames, debug:
 *  register accesses and SCB commands).
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
#include "net.h"


#define	I596_TICK_SHIFT		14
#define	I596_NULL		0xffffffffU
#define	MAX_FRAME		1600
#define	MAX_CMDS_PER_RUN	64
#define	MAX_MC			64

/*  Port commands:  */
#define	PORT_RESET		0
#define	PORT_SELFTEST		1
#define	PORT_ALTSCP		2
#define	PORT_DUMP		3

/*  SCB status and command words:  */
#define	STAT_CX			0x8000
#define	STAT_FR			0x4000
#define	STAT_CNA		0x2000
#define	STAT_RNR		0x1000
#define	STAT_ACK_MASK		0xf000
#define	CUS_SHIFT		8
#define	RUS_SHIFT		4
#define	CMD_CUC(x)		(((x) >> 8) & 7)
#define	CMD_RESET		0x0080
#define	CMD_RUC(x)		(((x) >> 4) & 7)

#define	CUS_IDLE		0
#define	CUS_SUSPENDED		1
#define	CUS_ACTIVE		2

#define	RUS_IDLE		0
#define	RUS_SUSPENDED		1
#define	RUS_NORESOURCES		2
#define	RUS_READY		4

/*  Command and frame descriptor status/command bits:  */
#define	DS_C			0x8000
#define	DS_B			0x4000
#define	DS_OK			0x2000
#define	DS_A			0x1000
#define	DC_EL			0x8000
#define	DC_S			0x4000
#define	DC_I			0x2000
#define	DC_NC			0x0010
#define	DC_SF			0x0008
#define	DC_CMD(x)		((x) & 7)

#define	CNT_EOF			0x8000
#define	CNT_F			0x4000
#define	CNT_MASK		0x3fff

/*  RFD error status bits:  */
#define	RFD_NORES		0x0200	/*  out of buffer space  */
#define	RFD_TRUNC		0x0020	/*  frame truncated  */

/*  Configuration bytes:  */
#define	CF_ALLOC(d)		((d)->config[3] & 0x08)
#define	CF_LOOPBACK(d)		(((d)->config[3] >> 6) & 3)
#define	CF_PROMISC(d)		((d)->config[8] & 0x01)
#define	CF_BCDIS(d)		((d)->config[8] & 0x02)
#define	CF_MCALL(d)		((d)->config[11] & 0x20)


struct i596_data {
	struct machine	*machine;
	struct interrupt irq;
	bool		irq_asserted;
	int		subsys;		/*  for debugmsg  */

	struct nic_data	nic;

	/*  MPU port:  */
	uint16_t	port_low;

	/*  Initialization:  */
	uint32_t	scp;
	bool		initialized;
	uint8_t		sysbus;
	uint32_t	scb;

	/*  Units:  */
	uint16_t	status;
	int		cus, rus;
	uint32_t	cu_addr;	/*  current command block  */
	uint32_t	rfd;		/*  next RFD to fill  */
	uint32_t	rbd;		/*  next free RBD  */

	uint8_t		config[16];
	int		n_mc;
	uint8_t		mc[MAX_MC][6];

	/*  A frame from the network that is waiting for an RFD:  */
	unsigned char	*rx_packet;
	int		rx_len;
};


/*****************************************************************************
 *
 *  DMA.
 */

static void mem_rw(struct i596_data *d, uint32_t addr, uint8_t *buf,
	size_t len, int writeflag)
{
	struct cpu *cpu = d->machine->cpus[d->machine->bootstrap_cpu];

	/*  (In pieces that do not cross pages.)  */
	while (len > 0) {
		size_t k = 0x1000 - (addr & 0xfff);
		if (k > len)
			k = len;
		cpu->memory_rw(cpu, cpu->mem, addr, buf, k, writeflag,
		    PHYSICAL | NO_EXCEPTIONS);
		addr += k; buf += k; len -= k;
	}
}

static uint16_t rd16(struct i596_data *d, uint32_t addr)
{
	uint8_t b[2];
	mem_rw(d, addr, b, 2, MEM_READ);
	return (b[0] << 8) | b[1];
}

static uint32_t rd32(struct i596_data *d, uint32_t addr)
{
	return rd16(d, addr) | ((uint32_t) rd16(d, addr + 2) << 16);
}

static void wr16(struct i596_data *d, uint32_t addr, uint16_t v)
{
	uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t) v };
	mem_rw(d, addr, b, 2, MEM_WRITE);
}

static void wr32(struct i596_data *d, uint32_t addr, uint32_t v)
{
	wr16(d, addr, v);
	wr16(d, addr + 2, v >> 16);
}


/*****************************************************************************/


/*
 *  The INT pin: asserted while the SCB has unacknowledged status bits.
 *  Its polarity is set by the SYSBUS byte (bit 5 clear: active low, as
 *  SYSTEM V/88 uses it). The interrupt line to the board is "the pin is
 *  low".
 */
static void set_int(struct i596_data *d, bool asserted)
{
	bool active_low = !d->initialized || !(d->sysbus & 0x20);
	bool low = active_low ? asserted : !asserted;

	if (low && !d->irq_asserted)
		INTERRUPT_ASSERT(d->irq);
	else if (!low && d->irq_asserted)
		INTERRUPT_DEASSERT(d->irq);
	d->irq_asserted = low;
}

static void update_irq(struct i596_data *d)
{
	set_int(d, d->initialized && (d->status & STAT_ACK_MASK));
}

static void write_status(struct i596_data *d)
{
	d->status = (d->status & STAT_ACK_MASK) | (d->cus << CUS_SHIFT) |
	    (d->rus << RUS_SHIFT);
	wr16(d, d->scb, d->status);
	update_irq(d);
}

static void chip_reset(struct i596_data *d)
{
	d->initialized = false;
	d->scp = 0x00fffff4;
	d->status = 0;
	d->cus = CUS_IDLE;
	d->rus = RUS_IDLE;
	d->n_mc = 0;
	memset(d->config, 0, sizeof(d->config));
	/*  Defaults: 6-byte addresses in the command blocks, no loopback.  */
	d->config[3] = 0x26;
	free(d->rx_packet);
	d->rx_packet = NULL;
	update_irq(d);
}


/*****************************************************************************
 *
 *  Receive unit.
 */

static bool address_match(struct i596_data *d, const unsigned char *dst)
{
	int i;

	if (CF_PROMISC(d))
		return true;
	if (dst[0] & 1) {
		if (net_ether_broadcast(dst))
			return !CF_BCDIS(d);
		if (CF_MCALL(d))
			return true;
		for (i = 0; i < d->n_mc; i++)
			if (memcmp(dst, d->mc[i], 6) == 0)
				return true;
		return false;
	}
	return memcmp(dst, d->nic.mac_address, 6) == 0;
}

/*
 *  Write a frame into the receive frame area. Returns false if the
 *  receive unit is not ready (the frame then has to wait or be dropped).
 */
static bool receive(struct i596_data *d, const unsigned char *pkt, int len)
{
	uint32_t rfd = d->rfd, rbd, first_rbd = I596_NULL, data;
	uint16_t cmd, size, st = DS_C | DS_OK;
	int pos = 0, n, hdr = 0;

	if (d->rus != RUS_READY)
		return false;

	if (len < 14 || !address_match(d, pkt))
		return true;

	debugmsg(d->subsys, "rx", VERBOSITY_INFO,
	    "%i bytes into RFD 0x%08x", len, rfd);

	cmd = rd16(d, rfd + 2);
	size = rd16(d, rfd + 14) & CNT_MASK;
	data = rfd + 16;

	/*  Without AL-LOC, the header goes in the RFD's address fields.  */
	if (!CF_ALLOC(d)) {
		mem_rw(d, rfd + 16, (uint8_t *) pkt, 14, MEM_WRITE);
		pos = hdr = 14;
		data = rfd + 30;
	}

	/*  The RFD's own data area (all of it in the simplified mode):  */
	n = len - pos;
	if (n > size)
		n = size;
	if (n > 0)
		mem_rw(d, data, (uint8_t *) pkt + pos, n, MEM_WRITE);
	pos += n;
	wr16(d, rfd + 12, n | CNT_F | (pos == len ? CNT_EOF : 0));

	if (pos < len && !(cmd & DC_SF))
		st |= RFD_TRUNC;

	/*  The rest goes in receive buffers (flexible mode). The RBD list
	    is followed independently of the RFDs (only the first RFD's RBD
	    pointer is read, at RU start), and the first RBD used is written
	    to the RFD.  */
	if (cmd & DC_SF) {
		rbd = d->rbd;
		while (pos < len) {
			uint16_t bsize;
			if (rbd == I596_NULL) {
				st |= RFD_NORES;
				break;
			}
			if (first_rbd == I596_NULL)
				first_rbd = rbd;
			bsize = rd16(d, rbd + 12);
			n = len - pos;
			if (n > (bsize & CNT_MASK))
				n = bsize & CNT_MASK;
			mem_rw(d, rd32(d, rbd + 8), (uint8_t *) pkt + pos, n,
			    MEM_WRITE);
			pos += n;
			wr16(d, rbd, n | CNT_F | (pos == len ? CNT_EOF : 0));
			if (bsize & DC_EL) {
				rbd = I596_NULL;
				if (pos < len)
					st |= RFD_NORES;
			} else
				rbd = rd32(d, rbd + 4);
		}
		d->rbd = rbd;
		if (first_rbd != I596_NULL)
			wr32(d, rfd + 8, first_rbd);
	}

	if (st & (RFD_NORES | RFD_TRUNC))
		st &= ~DS_OK;
	wr16(d, rfd, st);
	(void) hdr;

	d->status |= STAT_FR;
	if ((cmd & DC_SF) && d->rbd == I596_NULL) {
		/*  Out of receive buffers.  */
		d->rus = RUS_NORESOURCES;
		d->status |= STAT_RNR;
		d->rfd = rd32(d, rfd + 4);
	} else if (cmd & DC_EL) {
		d->rus = RUS_NORESOURCES;
		d->status |= STAT_RNR;
	} else if (cmd & DC_S) {
		d->rus = RUS_SUSPENDED;
		d->status |= STAT_RNR;
	} else {
		d->rfd = rd32(d, rfd + 4);
	}
	write_status(d);
	return true;
}


/*****************************************************************************
 *
 *  Command unit.
 */

static void transmit(struct i596_data *d, uint32_t cb, uint16_t cmd)
{
	unsigned char pkt[MAX_FRAME + 64];
	uint32_t tbd = rd32(d, cb + 8);
	uint16_t tcount = rd16(d, cb + 12) & CNT_MASK;
	int len = 0, n;

	if (!CF_ALLOC(d)) {
		/*  Destination and length field from the TCB, our address
		    inserted as the source:  */
		mem_rw(d, cb + 16, pkt, 6, MEM_READ);
		memcpy(pkt + 6, d->nic.mac_address, 6);
		mem_rw(d, cb + 22, pkt + 12, 2, MEM_READ);
		len = 14;
		n = tcount;
		if (n > MAX_FRAME)
			n = MAX_FRAME;
		mem_rw(d, cb + 24, pkt + len, n, MEM_READ);
		len += n;
	} else {
		n = tcount;
		if (n > MAX_FRAME)
			n = MAX_FRAME;
		mem_rw(d, cb + 16, pkt, n, MEM_READ);
		len = n;
	}

	if (cmd & DC_SF) {
		int guard = 0;
		while (tbd != I596_NULL && guard++ < 64) {
			uint16_t size = rd16(d, tbd);
			n = size & CNT_MASK;
			if (len + n > MAX_FRAME)
				n = MAX_FRAME - len;
			if (n > 0)
				mem_rw(d, rd32(d, tbd + 8), pkt + len, n,
				    MEM_READ);
			len += n;
			if (size & CNT_EOF)
				break;
			tbd = rd32(d, tbd + 4);
		}
	}

	if (DEBUGMSG_ENABLED(d->subsys, VERBOSITY_INFO)) {
		char buf[3 * 48 + 1];
		int i;
		for (i = 0; i < len && i < 48; i++)
			snprintf(buf + 3 * i, 4, " %02x", pkt[i]);
		buf[3 * i] = '\0';
		debugmsg(d->subsys, "tx", VERBOSITY_INFO, "%i bytes:%s",
		    len, buf);
	}

	if (len < 14)
		return;

	if (CF_LOOPBACK(d) != 0) {
		/*  Internal or external loopback: the frame comes back
		    (if the receive unit takes it).  */
		receive(d, pkt, len);
		return;
	}

	/*  Short frames are padded to the minimum length:  */
	if (len < 60) {
		memset(pkt + len, 0, 60 - len);
		len = 60;
	}

	if (d->machine->emul->net != NULL)
		net_ethernet_tx(d->machine->emul->net, &d->nic, pkt, len);

	/*  Our own broadcasts and multicasts are not received.  */
}

/*
 *  The dump area: 304 bytes of internal state (here: the configuration
 *  bytes and the individual address, the rest zero), followed by the
 *  completion word 0xA006.
 */
static void dump(struct i596_data *d, uint32_t addr)
{
	uint8_t z[304];

	debugmsg(d->subsys, "dump", VERBOSITY_INFO, "to 0x%08x", addr);
	memset(z, 0, sizeof(z));
	memcpy(z, d->config, sizeof(d->config));
	memcpy(z + 0x10, d->nic.mac_address, 6);
	mem_rw(d, addr, z, sizeof(z), MEM_WRITE);
	wr16(d, addr + sizeof(z), 0xa006);
}

static void run_cu(struct i596_data *d)
{
	int count = 0;

	while (d->cus == CUS_ACTIVE && count++ < MAX_CMDS_PER_RUN) {
		uint32_t cb = d->cu_addr;
		uint16_t cmd = rd16(d, cb + 2), st = DS_C | DS_OK;
		int i;

		debugmsg(d->subsys, "command", VERBOSITY_INFO,
		    "0x%04x at 0x%08x", cmd, cb);

		switch (DC_CMD(cmd)) {
		case 0:		/*  NOP  */
			break;
		case 1:		/*  individual address setup  */
			mem_rw(d, cb + 8, d->nic.mac_address, 6, MEM_READ);
			debugmsg(d->subsys, "IA setup", VERBOSITY_INFO,
			    "%02x:%02x:%02x:%02x:%02x:%02x",
				    d->nic.mac_address[0], d->nic.mac_address[1],
				    d->nic.mac_address[2], d->nic.mac_address[3],
				    d->nic.mac_address[4], d->nic.mac_address[5]);
			break;
		case 2:		/*  configure  */
			{
				uint8_t b[16];
				int n;
				mem_rw(d, cb + 8, b, 16, MEM_READ);
				n = b[0] & 0x0f;
				if (n < 4)
					n = 4;
				for (i = 1; i < n && i < 16; i++)
					d->config[i] = b[i];
				d->config[0] = b[0];
				d->nic.promiscuous_mode = CF_PROMISC(d) ? 1 : 0;
				if (DEBUGMSG_ENABLED(d->subsys,
				    VERBOSITY_INFO)) {
					char buf[3 * 16 + 1];
					for (i = 0; i < n && i < 16; i++)
						snprintf(buf + 3 * i, 4, " %02x",
						    b[i]);
					buf[3 * i] = '\0';
					debugmsg(d->subsys, "configure",
					    VERBOSITY_INFO, "%s", buf + 1);
				}
			}
			break;
		case 3:		/*  multicast setup  */
			{
				int n = (rd16(d, cb + 8) & CNT_MASK) / 6;
				if (n > MAX_MC)
					n = MAX_MC;
				for (i = 0; i < n; i++)
					mem_rw(d, cb + 10 + 6 * i, d->mc[i], 6,
					    MEM_READ);
				d->n_mc = n;
			}
			break;
		case 4:		/*  transmit  */
			transmit(d, cb, cmd);
			break;
		case 5:		/*  TDR: link OK  */
			wr16(d, cb + 8, 0x8000);
			break;
		case 6:		/*  dump  */
			dump(d, rd32(d, cb + 8));
			break;
		case 7:		/*  diagnose: passed  */
			break;
		}

		wr16(d, cb, st);

		if (cmd & DC_I)
			d->status |= STAT_CX;
		if (cmd & DC_EL) {
			d->cus = CUS_IDLE;
			d->status |= STAT_CNA;
		} else if (cmd & DC_S) {
			d->cus = CUS_SUSPENDED;
			d->status |= STAT_CNA;
		} else {
			d->cu_addr = rd32(d, cb + 4);
		}
	}

	write_status(d);
}


/*****************************************************************************
 *
 *  Channel attention.
 */

static void channel_attention(struct i596_data *d)
{
	uint16_t cmd;

	if (!d->initialized) {
		uint8_t scp[4];
		uint32_t iscp;

		mem_rw(d, d->scp, scp, 4, MEM_READ);
		d->sysbus = rd32(d, d->scp) >> 16;
		iscp = rd32(d, d->scp + 8);
		d->scb = rd32(d, iscp + 4);
		debugmsg(d->subsys, "init", VERBOSITY_INFO,
		    "SCP 0x%08x (%02x %02x %02x %02x): sysbus 0x%02x, "
		    "ISCP 0x%08x, SCB 0x%08x",
			    d->scp, scp[0], scp[1], scp[2], scp[3], d->sysbus,
			    iscp, d->scb);
		if ((d->sysbus & 0x06) != 0x04)
			fatal("[ i82596: SYSBUS 0x%02x: only the linear mode "
			    "is implemented ]\n", d->sysbus);

		d->initialized = true;
		wr32(d, iscp, 0);	/*  BUSY cleared  */
		d->status = STAT_CX | STAT_CNA;
		d->cus = CUS_IDLE;
		d->rus = RUS_IDLE;
		write_status(d);
		return;
	}

	cmd = rd16(d, d->scb + 2);
	debugmsg(d->subsys, "CA", VERBOSITY_DEBUG, "SCB command 0x%04x", cmd);

	/*  Acknowledged status bits. INT goes inactive on a CA, and active
	    again if some bits are still not acknowledged (so that an edge
	    triggered interrupt sees them).  */
	d->status &= ~(cmd & STAT_ACK_MASK);
	set_int(d, false);
	if (d->status & STAT_ACK_MASK)
		debugmsg(d->subsys, "CA", VERBOSITY_DEBUG,
		    "still not acknowledged: 0x%04x",
		    d->status & STAT_ACK_MASK);

	if (cmd & CMD_RESET) {
		wr16(d, d->scb + 2, 0);
		chip_reset(d);
		return;
	}

	switch (CMD_CUC(cmd)) {
	case 1:		/*  start  */
		d->cu_addr = rd32(d, d->scb + 4);
		d->cus = CUS_ACTIVE;
		break;
	case 2:		/*  resume  */
		if (d->cus == CUS_SUSPENDED) {
			d->cus = CUS_ACTIVE;
			d->cu_addr = rd32(d, d->cu_addr + 4);
		}
		break;
	case 3:		/*  suspend  */
		if (d->cus == CUS_ACTIVE) {
			d->cus = CUS_SUSPENDED;
			d->status |= STAT_CNA;
		}
		break;
	case 4:		/*  abort  */
		if (d->cus != CUS_IDLE) {
			d->cus = CUS_IDLE;
			d->status |= STAT_CNA;
		}
		break;
	}

	switch (CMD_RUC(cmd)) {
	case 1:		/*  start  */
		d->rfd = rd32(d, d->scb + 8);
		d->rbd = rd32(d, d->rfd + 8);
		d->rus = RUS_READY;
		break;
	case 2:		/*  resume  */
		if (d->rus == RUS_SUSPENDED) {
			d->rus = RUS_READY;
			d->rfd = rd32(d, d->rfd + 4);
		}
		break;
	case 3:		/*  suspend  */
		if (d->rus == RUS_READY) {
			d->rus = RUS_SUSPENDED;
			d->status |= STAT_RNR;
		}
		break;
	case 4:		/*  abort  */
		if (d->rus != RUS_IDLE) {
			d->rus = RUS_IDLE;
			d->status |= STAT_RNR;
		}
		break;
	}

	/*  The command is accepted. (The command unit runs from the tick
	    function: drivers expect the CPU to go on for a while after
	    starting it.)  */
	wr16(d, d->scb + 2, 0);
	write_status(d);
}


/*****************************************************************************/


DEVICE_TICK(i82596)
{
	struct i596_data *d = (struct i596_data *) extra;
	struct net *net = d->machine->emul->net;

	if (d->cus == CUS_ACTIVE)
		run_cu(d);

	while (d->rus == RUS_READY) {
		if (d->rx_packet == NULL) {
			if (net == NULL || !net_ethernet_rx_avail(net, &d->nic))
				break;
			net_ethernet_rx(net, &d->nic, &d->rx_packet,
			    &d->rx_len);
			if (d->rx_packet == NULL)
				break;
		}
		if (!receive(d, d->rx_packet, d->rx_len))
			break;
		free(d->rx_packet);
		d->rx_packet = NULL;
	}

	/*  Frames arriving while the receive unit cannot take them are
	    lost (only the one already taken from the network waits).  */
}


DEVICE_ACCESS(i82596)
{
	struct i596_data *d = (struct i596_data *) extra;
	uint32_t a = relative_addr & 0xfff, cmd, addr;

	debugmsg(d->subsys, "access", VERBOSITY_DEBUG,
	    "%s offset 0x%x len %i: 0x%llx",
		    writeflag == MEM_READ ? "read" : "write", (int) a, (int) len,
		    writeflag == MEM_READ ? 0ULL :
		    (unsigned long long) memory_readmax64(cpu, data, len));

	/*  (Reads are acknowledged, with undefined data.)  */
	if (writeflag == MEM_READ) {
		memset(data, 0, len);
		return 1;
	}

	/*  Channel attention: any access to the second longword.  */
	if (a >= 4 && a < 8) {
		channel_attention(d);
		return 1;
	}

	/*  The port only decodes writes, of two 16-bit halves.  */
	if (a >= 4)
		return 1;

	if (len == 4) {
		uint32_t v = memory_readmax64(cpu, data, len);
		cmd = (v << 16) | (v >> 16);
	} else if (len == 2 && a < 2) {
		d->port_low = memory_readmax64(cpu, data, len);
		return 1;
	} else if (len == 2) {
		cmd = ((uint32_t) memory_readmax64(cpu, data, len) << 16) |
		    d->port_low;
	} else
		return 1;

	addr = cmd & ~0xf;
	debugmsg(d->subsys, "port", VERBOSITY_INFO, "command 0x%08x", cmd);

	switch (cmd & 0xf) {
	case PORT_RESET:
		chip_reset(d);
		break;
	case PORT_SELFTEST:
		/*  ROM signature, then the result (0 = passed):  */
		chip_reset(d);
		wr16(d, addr, 0); wr16(d, addr + 2, 0);
		wr16(d, addr + 4, 0); wr16(d, addr + 6, 0);
		break;
	case PORT_ALTSCP:
		d->scp = addr;
		break;
	case PORT_DUMP:
		dump(d, addr);
		break;
	}

	return 1;
}


DEVINIT(i82596)
{
	struct i596_data *d;

	CHECK_ALLOCATION(d = (struct i596_data *) malloc(sizeof(*d)));
	memset(d, 0, sizeof(*d));
	d->machine = devinit->machine;
	d->subsys = debugmsg_register_subsystem("i82596");

	INTERRUPT_CONNECT(devinit->interrupt_path, d->irq);

	/*  (The address is set by the guest with an IA setup command.)  */
	if (devinit->machine->emul->net != NULL)
		net_add_nic(devinit->machine->emul->net, &d->nic);

	chip_reset(d);

	memory_device_register(devinit->machine->memory, devinit->name,
	    devinit->addr, 0x1000, dev_i82596_access, (void *)d,
	    DM_DEFAULT, NULL);

	machine_add_tickfunction(devinit->machine,
	    dev_i82596_tick, d, I596_TICK_SHIFT);

	return 1;
}

