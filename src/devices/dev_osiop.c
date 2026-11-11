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
 *  COMMENT: NCR 53C710 SCSI I/O Processor (initiator mode, SCRIPTS)
 *
 *  The chip is in big-endian mode (as on the MVME187): the byte registers
 *  are swapped within each longword compared to the data manual, and
 *  longwords (DSA, DSP, ...) are big-endian. SCRIPTS register numbers are
 *  the manual's (little-endian lane) addresses.
 *
 *  The SCSI bus and its targets are modelled by phases: after selection
 *  the target asks for MSG OUT (with ATN) or COMMAND, then DATA IN/OUT,
 *  STATUS and MSG IN (command complete), and then disconnects once the
 *  initiator has released ACK. Targets never disconnect in between and do
 *  not negotiate synchronous transfers (they reject SDTR).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "device.h"
#include "diskimage.h"
#include "interrupt.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"


#define	OSIOP_LEN		0x40
#define	OSIOP_TICK_SHIFT	14
#define	MAX_SCRIPTS_PER_TICK	2000
#define	CHIP_REVISION		2

/*  Register offsets, as seen by the host in big-endian mode:  */
#define	R_SIEN		0x00
#define	R_SDID		0x01
#define	R_SCNTL1	0x02
#define	R_SCNTL0	0x03
#define	R_SOCL		0x04
#define	R_SODL		0x05
#define	R_SXFER		0x06
#define	R_SCID		0x07
#define	R_SBCL		0x08
#define	R_SBDL		0x09
#define	R_SIDL		0x0a
#define	R_SFBR		0x0b
#define	R_SSTAT2	0x0c
#define	R_SSTAT1	0x0d
#define	R_SSTAT0	0x0e
#define	R_DSTAT		0x0f
#define	R_DSA		0x10
#define	R_CTEST3	0x14
#define	R_CTEST2	0x15
#define	R_CTEST1	0x16
#define	R_CTEST0	0x17
#define	R_CTEST7	0x18
#define	R_CTEST6	0x19
#define	R_CTEST5	0x1a
#define	R_CTEST4	0x1b
#define	R_TEMP		0x1c
#define	R_LCRC		0x20
#define	R_CTEST8	0x21
#define	R_ISTAT		0x22
#define	R_DFIFO		0x23
#define	R_DCMD		0x24
#define	R_DBC		0x25	/*  24 bits  */
#define	R_DNAD		0x28
#define	R_DSP		0x2c
#define	R_DSPS		0x30
#define	R_SCRATCH	0x34
#define	R_DCNTL		0x38
#define	R_DWT		0x39
#define	R_DIEN		0x3a
#define	R_DMODE		0x3b
#define	R_ADDER		0x3c

/*  ISTAT  */
#define	ISTAT_ABRT	0x80
#define	ISTAT_RST	0x40
#define	ISTAT_SIGP	0x20
#define	ISTAT_CON	0x08
#define	ISTAT_SIP	0x02
#define	ISTAT_DIP	0x01

/*  DSTAT  */
#define	DSTAT_DFE	0x80
#define	DSTAT_BF	0x20
#define	DSTAT_ABRT	0x10
#define	DSTAT_SSI	0x08
#define	DSTAT_SIR	0x04
#define	DSTAT_WTD	0x02
#define	DSTAT_IID	0x01

/*  SSTAT0 / SIEN  */
#define	SSTAT0_MA	0x80
#define	SSTAT0_FCMP	0x40
#define	SSTAT0_STO	0x20
#define	SSTAT0_SEL	0x10
#define	SSTAT0_SGE	0x08
#define	SSTAT0_UDC	0x04
#define	SSTAT0_RST	0x02
#define	SSTAT0_PAR	0x01

/*  SBCL / SOCL  */
#define	SBCL_REQ	0x80
#define	SBCL_ACK	0x40
#define	SBCL_BSY	0x20
#define	SBCL_SEL	0x10
#define	SBCL_ATN	0x08
#define	SBCL_MSG	0x04
#define	SBCL_CD		0x02
#define	SBCL_IO		0x01

#define	SCNTL1_CON	0x10
#define	SCNTL1_RST	0x08
#define	DCNTL_SSM	0x10
#define	DCNTL_STD	0x04
#define	DMODE_MAN	0x01

/*  SCSI phases (MSG, C/D, I/O):  */
#define	PH_DATA_OUT	0
#define	PH_DATA_IN	1
#define	PH_COMMAND	2
#define	PH_STATUS	3
#define	PH_MSG_OUT	6
#define	PH_MSG_IN	7
#define	PH_BUS_FREE	(-1)

static const char *phase_names[8] = { "DATA OUT", "DATA IN", "COMMAND",
	"STATUS", "RESERVED OUT", "RESERVED IN", "MSG OUT", "MSG IN" };

struct osiop_data {
	struct machine		*machine;
	uint32_t		base;		/*  of the registers  */
	struct interrupt	irq;
	int			irq_asserted;

	uint8_t			reg[OSIOP_LEN];

	bool			running;	/*  SCRIPTS  */
	bool			waiting_resel;	/*  in WAIT RESELECT  */
	int			carry;

	/*  The SCSI bus:  */
	int			phase;		/*  or PH_BUS_FREE  */
	int			target, lun;
	bool			atn;
	bool			msg_in_ack;	/*  ACK held after MSG IN  */
	bool			disconnect_after_ack;

	/*  The target's state for the current command:  */
	uint8_t			msg_out[64];
	int			msg_out_len;
	uint8_t			cmd[16];
	int			cmd_len, cmd_want;
	uint8_t			*buf;		/*  data in or out  */
	size_t			buf_len, buf_pos;
	uint8_t			status;
	uint8_t			msg_in[8];
	int			msg_in_len, msg_in_pos;
	struct scsi_transfer	*xfer;
	bool			check_condition;	/*  bad lun  */
};


static uint32_t get32(struct osiop_data *d, int r)
{
	return ((uint32_t)d->reg[r] << 24) | (d->reg[r+1] << 16) |
	    (d->reg[r+2] << 8) | d->reg[r+3];
}

static void put32(struct osiop_data *d, int r, uint32_t v)
{
	d->reg[r] = v >> 24; d->reg[r+1] = v >> 16;
	d->reg[r+2] = v >> 8; d->reg[r+3] = v;
}

static void put_dbc(struct osiop_data *d, uint32_t v)
{
	d->reg[R_DBC] = v >> 16; d->reg[R_DBC+1] = v >> 8;
	d->reg[R_DBC+2] = v;
}


/*****************************************************************************
 *
 *  Memory (the chip is a bus master)
 */

static uint8_t reg_read(struct osiop_data *d, int r);
static void reg_write(struct osiop_data *d, int r, uint8_t v);

/*  The chip's own registers (SCRIPTS move to and from them):  */
static bool is_self(struct osiop_data *d, uint32_t addr, size_t len)
{
	return addr >= d->base && addr + len <= d->base + 0x1000;
}

static bool mem_read(struct osiop_data *d, uint32_t addr, uint8_t *buf,
	size_t len)
{
	struct cpu *cpu = d->machine->cpus[0];
	size_t i;

	if (is_self(d, addr, len)) {
		for (i = 0; i < len; i++)
			buf[i] = reg_read(d, (addr + i) & (OSIOP_LEN - 1));
		return true;
	}
	/*  (In pieces that do not cross pages.)  */
	while (len > 0) {
		size_t k = 0x1000 - (addr & 0xfff);
		if (k > len)
			k = len;
		if (cpu->memory_rw(cpu, cpu->mem, addr, buf, k, MEM_READ,
		    PHYSICAL | NO_EXCEPTIONS) != MEMORY_ACCESS_OK)
			return false;
		addr += k; buf += k; len -= k;
	}
	return true;
}

static bool mem_write(struct osiop_data *d, uint32_t addr, uint8_t *buf,
	size_t len)
{
	struct cpu *cpu = d->machine->cpus[0];
	size_t i;

	if (is_self(d, addr, len)) {
		for (i = 0; i < len; i++)
			reg_write(d, (addr + i) & (OSIOP_LEN - 1), buf[i]);
		return true;
	}
	while (len > 0) {
		size_t k = 0x1000 - (addr & 0xfff);
		if (k > len)
			k = len;
		if (cpu->memory_rw(cpu, cpu->mem, addr, buf, k, MEM_WRITE,
		    PHYSICAL | NO_EXCEPTIONS) != MEMORY_ACCESS_OK)
			return false;
		addr += k; buf += k; len -= k;
	}
	return true;
}

static bool mem_read32(struct osiop_data *d, uint32_t addr, uint32_t *v)
{
	uint8_t b[4];
	if (!mem_read(d, addr, b, 4))
		return false;
	*v = ((uint32_t)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
	return true;
}


/*****************************************************************************
 *
 *  Interrupts
 */

static void update_irq(struct osiop_data *d)
{
	int assert = 0;

	if ((d->reg[R_ISTAT] & ISTAT_DIP) &&
	    (d->reg[R_DSTAT] & d->reg[R_DIEN] & 0x7f))
		assert = 1;
	if ((d->reg[R_ISTAT] & ISTAT_SIP) &&
	    (d->reg[R_SSTAT0] & d->reg[R_SIEN]))
		assert = 1;

	if (assert && !d->irq_asserted)
		INTERRUPT_ASSERT(d->irq);
	else if (!assert && d->irq_asserted)
		INTERRUPT_DEASSERT(d->irq);
	d->irq_asserted = assert;
}

/*  A DMA (SCRIPTS) interrupt: halts the SCRIPTS processor.  */
static void dma_int(struct osiop_data *d, uint8_t bits)
{
	d->reg[R_DSTAT] |= bits | DSTAT_DFE;
	d->reg[R_ISTAT] |= ISTAT_DIP;
	d->running = false;
	update_irq(d);
}

/*  A SCSI interrupt: halts the SCRIPTS processor.  */
static void scsi_int(struct osiop_data *d, uint8_t bits)
{
	d->reg[R_SSTAT0] |= bits;
	d->reg[R_ISTAT] |= ISTAT_SIP;
	d->running = false;
	update_irq(d);
}


/*****************************************************************************
 *
 *  The SCSI bus and the targets
 */

static void update_bus_regs(struct osiop_data *d)
{
	uint8_t sbcl = 0;

	if (d->phase != PH_BUS_FREE) {
		sbcl = SBCL_BSY | (d->phase & 7);
		if (!d->msg_in_ack)
			sbcl |= SBCL_REQ;
		else
			sbcl |= SBCL_ACK;
		d->reg[R_ISTAT] |= ISTAT_CON;
		d->reg[R_SCNTL1] |= SCNTL1_CON;
		d->reg[R_SSTAT2] = (d->reg[R_SSTAT2] & ~7) | (d->phase & 7);
	} else {
		d->reg[R_ISTAT] &= ~ISTAT_CON;
		d->reg[R_SCNTL1] &= ~SCNTL1_CON;
	}
	if (d->atn)
		sbcl |= SBCL_ATN;
	d->reg[R_SBCL] = sbcl;
}

static void free_transfer(struct osiop_data *d)
{
	if (d->xfer != NULL)
		scsi_transfer_free(d->xfer);
	d->xfer = NULL;
	d->buf = NULL;
	d->buf_len = d->buf_pos = 0;
}

static void bus_free(struct osiop_data *d)
{
	free_transfer(d);
	d->phase = PH_BUS_FREE;
	d->atn = false;
	d->msg_in_ack = false;
	d->disconnect_after_ack = false;
	update_bus_regs(d);
}

static void go_status(struct osiop_data *d, uint8_t status)
{
	d->status = status;
	d->phase = PH_STATUS;
}

/*  Message in: command complete, then disconnect.  */
static void go_msg_in(struct osiop_data *d, const uint8_t *msg, int len,
	bool disconnect)
{
	memcpy(d->msg_in, msg, len);
	d->msg_in_len = len;
	d->msg_in_pos = 0;
	d->disconnect_after_ack = disconnect;
	d->phase = PH_MSG_IN;
}

/*  Length of a command, from its group code:  */
static int cdb_length(uint8_t op)
{
	switch (op >> 5) {
	case 0:	return 6;
	case 1: case 2:	return 10;
	case 4:	return 16;
	case 5:	return 12;
	}
	return 6;
}

/*  A command for a missing logical unit:  */
static void bad_lun_command(struct osiop_data *d)
{
	static uint8_t inq[36];
	static uint8_t sense[18];

	if (d->cmd[0] == 0x12) {		/*  INQUIRY  */
		memset(inq, 0, sizeof(inq));
		inq[0] = 0x7f;			/*  no device at this lun  */
		inq[4] = 31;
		d->buf = inq;
		d->buf_len = d->cmd[4] < sizeof(inq) ? d->cmd[4] : sizeof(inq);
		d->buf_pos = 0;
		d->status = 0;
		d->phase = d->buf_len > 0 ? PH_DATA_IN : PH_STATUS;
		return;
	}
	if (d->cmd[0] == 0x03) {		/*  REQUEST SENSE  */
		memset(sense, 0, sizeof(sense));
		sense[0] = 0x70;
		sense[2] = 0x05;		/*  illegal request  */
		sense[7] = 10;
		sense[12] = 0x25;		/*  lun not supported  */
		d->buf = sense;
		d->buf_len = d->cmd[4] < sizeof(sense) ? d->cmd[4] :
		    sizeof(sense);
		d->buf_pos = 0;
		d->status = 0;
		d->phase = d->buf_len > 0 ? PH_DATA_IN : PH_STATUS;
		return;
	}
	go_status(d, 0x02);			/*  check condition  */
}

/*  The command is complete: run it.  */
static void run_command(struct osiop_data *d)
{
	struct cpu *cpu = d->machine->cpus[0];
	int res;

	if (d->lun != 0) {
		bad_lun_command(d);
		return;
	}

	free_transfer(d);
	d->xfer = scsi_transfer_alloc();
	scsi_transfer_allocbuf(&d->xfer->cmd_len, &d->xfer->cmd,
	    d->cmd_len, 0);
	memcpy(d->xfer->cmd, d->cmd, d->cmd_len);

	if (getenv("OSIOP_CMDS") != NULL) {
		int i;
		fprintf(stderr, "[ osiop: id %i cmd", d->target);
		for (i = 0; i < d->cmd_len; i++)
			fprintf(stderr, " %02x", d->cmd[i]);
		fprintf(stderr, " ]\n");
	}

	res = diskimage_scsicommand(cpu, d->target, DISKIMAGE_SCSI, d->xfer);
	if (res == 2) {
		/*  The target wants data out.  */
		d->buf_len = d->xfer->data_out_len;
		d->buf_pos = 0;
		scsi_transfer_allocbuf(&d->xfer->data_out_len,
		    &d->xfer->data_out, d->buf_len, 1);
		d->buf = d->xfer->data_out;
		d->phase = PH_DATA_OUT;
		return;
	}

	if (d->xfer->data_in != NULL && d->xfer->data_in_len > 0) {
		d->buf = d->xfer->data_in;
		d->buf_len = d->xfer->data_in_len;
		d->buf_pos = 0;
		d->phase = PH_DATA_IN;
		return;
	}

	go_status(d, d->xfer->status != NULL ? d->xfer->status[0] : 0x02);
}

/*  The data out phase is done: run the command again, with the data.  */
static void data_out_done(struct osiop_data *d)
{
	struct cpu *cpu = d->machine->cpus[0];

	d->xfer->data_out_offset = d->buf_len;
	diskimage_scsicommand(cpu, d->target, DISKIMAGE_SCSI, d->xfer);
	go_status(d, d->xfer->status != NULL ? d->xfer->status[0] : 0x02);
}

/*  Messages from the initiator:  */
static void msg_out_done(struct osiop_data *d)
{
	int i = 0;
	bool reject = false;

	while (i < d->msg_out_len) {
		uint8_t m = d->msg_out[i];
		if (m & 0x80) {
			d->lun = m & 7;			/*  IDENTIFY  */
			i++;
		} else if (m == 0x01) {
			/*  Extended message (SDTR, WDTR): rejected.  */
			if (i + 1 < d->msg_out_len)
				i += 2 + d->msg_out[i+1];
			else
				i = d->msg_out_len;
			reject = true;
		} else if (m == 0x06 || m == 0x0c) {
			/*  ABORT, BUS DEVICE RESET  */
			bus_free(d);
			return;
		} else if (m >= 0x20 && m <= 0x2f) {
			i += 2;				/*  two-byte message  */
		} else {
			i++;				/*  NOP and the like  */
		}
	}
	d->msg_out_len = 0;

	if (reject) {
		static const uint8_t rej[1] = { 0x07 };
		go_msg_in(d, rej, 1, false);
		return;
	}

	/*  The command is still to come (or is in progress).  */
	if (d->cmd_len < d->cmd_want || d->cmd_want == 0)
		d->phase = PH_COMMAND;
}

/*  After a byte (or more) of message in has been taken, and ACK released: */
static void msg_in_ack_released(struct osiop_data *d)
{
	d->msg_in_ack = false;
	if (d->msg_in_pos < d->msg_in_len)
		return;

	if (d->disconnect_after_ack) {
		bus_free(d);
		return;
	}

	/*  After a message reject: on with the command.  */
	if (d->atn)
		d->phase = PH_MSG_OUT;
	else if (d->cmd_len < d->cmd_want || d->cmd_want == 0)
		d->phase = PH_COMMAND;
	else
		d->phase = PH_STATUS;
}

/*
 *  Transfers up to len bytes in the current phase between memory at addr
 *  and the target. Returns the number of bytes moved, or -1 on a bus fault.
 */
static int bus_transfer(struct osiop_data *d, uint32_t addr, uint32_t len)
{
	uint32_t n = 0;
	uint8_t b;

	while (n < len) {
		switch (d->phase) {

		case PH_DATA_IN: {
			size_t k = d->buf_len - d->buf_pos;
			if (k > len - n)
				k = len - n;
			if (!mem_write(d, addr + n, d->buf + d->buf_pos, k))
				return -1;
			if (n == 0 && k > 0)
				d->reg[R_SFBR] = d->buf[d->buf_pos];
			d->buf_pos += k;
			n += k;
			if (d->buf_pos >= d->buf_len)
				go_status(d, d->xfer != NULL && d->xfer->status
				    != NULL ? d->xfer->status[0] : d->status);
			return n;
		}

		case PH_DATA_OUT: {
			size_t k = d->buf_len - d->buf_pos;
			if (k > len - n)
				k = len - n;
			if (!mem_read(d, addr + n, d->buf + d->buf_pos, k))
				return -1;
			d->buf_pos += k;
			n += k;
			if (d->buf_pos >= d->buf_len)
				data_out_done(d);
			return n;
		}

		case PH_COMMAND:
			if (!mem_read(d, addr + n, &b, 1))
				return -1;
			if (d->cmd_len == 0)
				d->cmd_want = cdb_length(b);
			if (d->cmd_len < (int) sizeof(d->cmd))
				d->cmd[d->cmd_len++] = b;
			n++;
			if (d->cmd_len >= d->cmd_want) {
				run_command(d);
				return n;
			}
			break;

		case PH_STATUS:
			if (!mem_write(d, addr + n, &d->status, 1))
				return -1;
			d->reg[R_SFBR] = d->status;
			n++;
			{
				static const uint8_t cc[1] = { 0x00 };
				go_msg_in(d, cc, 1, true);
			}
			return n;

		case PH_MSG_IN:
			if (d->msg_in_ack)
				return n;	/*  waiting for ACK release  */
			b = d->msg_in[d->msg_in_pos++];
			if (!mem_write(d, addr + n, &b, 1))
				return -1;
			if (n == 0)
				d->reg[R_SFBR] = b;
			n++;
			/*  The last byte of a block move keeps ACK asserted,
			    as do message bytes the target is waiting on.  */
			if (n == len || d->msg_in_pos >= d->msg_in_len) {
				d->msg_in_ack = true;
				return n;
			}
			break;

		case PH_MSG_OUT:
			if (!mem_read(d, addr + n, &b, 1))
				return -1;
			if (d->msg_out_len < (int) sizeof(d->msg_out))
				d->msg_out[d->msg_out_len++] = b;
			n++;
			if (n == len) {
				/*  ATN is released before the last byte.  */
				d->atn = false;
				msg_out_done(d);
				return n;
			}
			break;

		default:
			return n;
		}
	}
	return n;
}


/*****************************************************************************
 *
 *  SCRIPTS
 */

/*  SCRIPTS register numbers are little-endian lane addresses:  */
static int script_reg(int a)
{
	return (a & ~3) | (3 - (a & 3));
}

static uint8_t read_reg_for_script(struct osiop_data *d, int a)
{
	int r = script_reg(a);

	/*  (Reading CTEST2 clears SIGP, also from SCRIPTS.)  */
	if (r == R_CTEST2)
		return reg_read(d, r);
	return d->reg[r];
}

static void write_reg_for_script(struct osiop_data *d, int a, uint8_t v)
{
	int r = script_reg(a);

	/*  Read-only status registers are not written.  */
	switch (r) {
	case R_DSTAT: case R_SSTAT0: case R_SSTAT1: case R_SSTAT2:
	case R_ISTAT:
		return;
	}
	d->reg[r] = v;
}

static bool phase_ok(struct osiop_data *d, int phase)
{
	return d->phase != PH_BUS_FREE && !d->msg_in_ack &&
	    d->phase == phase;
}

static void block_move(struct osiop_data *d, uint32_t w0, uint32_t w1)
{
	uint32_t count = w0 & 0xffffff, addr = w1;
	int phase = (w0 >> 24) & 7, n;

	if (w0 & 0x10000000) {
		/*  Table indirect: count and address at DSA + offset.  */
		uint32_t t = get32(d, R_DSA) + (((int32_t)(w1 << 8)) >> 8);
		if (!mem_read32(d, t, &count) || !mem_read32(d, t + 4, &addr)) {
			dma_int(d, DSTAT_BF);
			return;
		}
		count &= 0xffffff;
	} else if (w0 & 0x20000000) {
		/*  Indirect addressing:  */
		if (!mem_read32(d, w1, &addr)) {
			dma_int(d, DSTAT_BF);
			return;
		}
	}

	put_dbc(d, count);
	put32(d, R_DNAD, addr);

	if (d->phase == PH_BUS_FREE) {
		/*  Not connected.  */
		scsi_int(d, SSTAT0_UDC);
		return;
	}

	/*  The target releases its REQ only when ACK is released.  */
	if (d->msg_in_ack) {
		scsi_int(d, SSTAT0_MA);
		return;
	}

	if (d->phase != phase) {
		scsi_int(d, SSTAT0_MA);
		return;
	}

	while (count > 0) {
		n = bus_transfer(d, addr, count);
		if (n < 0) {
			dma_int(d, DSTAT_BF);
			return;
		}
		count -= n;
		addr += n;
		put_dbc(d, count);
		put32(d, R_DNAD, addr);
		update_bus_regs(d);

		if (count == 0)
			break;
		if (n == 0 || !phase_ok(d, phase)) {
			/*  The target changed phase before the end.  */
			scsi_int(d, SSTAT0_MA);
			return;
		}
	}
}

static void select_target(struct osiop_data *d, uint32_t w0, uint32_t w1)
{
	uint8_t ids = (w0 >> 16) & 0xff;
	int id;

	if (w0 & 0x02000000) {
		/*  Table indirect: the ID (and SXFER) at DSA + the offset in
		    the first word.  */
		uint32_t t = get32(d, R_DSA) + (((int32_t)(w0 << 8)) >> 8), v;
		if (!mem_read32(d, t, &v)) {
			dma_int(d, DSTAT_BF);
			return;
		}
		ids = (v >> 16) & 0xff;
		d->reg[R_SXFER] = v >> 8;
	}

	if (d->phase != PH_BUS_FREE) {
		/*  Already connected.  */
		dma_int(d, DSTAT_IID);
		return;
	}

	d->reg[R_SDID] = ids;
	for (id = 0; id < 8; id++)
		if (ids & (1 << id))
			break;

	if (ids == 0 || (ids & ~(1 << id)) != 0 || id >= 8 ||
	    (d->reg[R_SCID] & (1 << id)) ||
	    !diskimage_exist(d->machine, id, DISKIMAGE_SCSI)) {
		/*  Selection timeout.  */
		scsi_int(d, SSTAT0_STO);
		return;
	}

	d->target = id;
	d->lun = 0;
	d->atn = (w0 & 0x01000000) != 0;
	d->msg_out_len = 0;
	d->cmd_len = d->cmd_want = 0;
	d->msg_in_ack = false;
	d->disconnect_after_ack = false;
	d->check_condition = false;
	free_transfer(d);
	d->phase = d->atn ? PH_MSG_OUT : PH_COMMAND;
	update_bus_regs(d);
}

/*
 *  Transfer control instructions (53C710 layout): bit 23 relative
 *  addressing, bit 21 test carry, bit 19 jump if true, bit 18 compare
 *  data, bit 17 compare phase, bit 16 wait for valid phase, bits 15-8 the
 *  mask (1 = not compared), bits 7-0 the data.
 */
#define	TC_REL		0x00800000
#define	TC_CARRY	0x00200000
#define	TC_TRUE		0x00080000
#define	TC_DATA		0x00040000
#define	TC_PHASE	0x00020000
#define	TC_WAIT		0x00010000

static bool condition(struct osiop_data *d, uint32_t w0)
{
	bool result = true;

	if (!(w0 & (TC_CARRY | TC_DATA | TC_PHASE)))
		return (w0 & TC_TRUE) != 0;

	if (w0 & TC_PHASE) {
		int phase = (w0 >> 24) & 7;
		if (!phase_ok(d, phase))
			result = false;
	}
	if (w0 & TC_DATA) {
		uint8_t mask = (w0 >> 8) & 0xff, data = w0 & 0xff;
		if ((d->reg[R_SFBR] & ~mask) != (data & ~mask))
			result = false;
	}
	if (w0 & TC_CARRY) {
		if (!d->carry)
			result = false;
	}

	return (w0 & TC_TRUE) ? result : !result;
}

/*  Runs one SCRIPTS instruction. Returns false when SCRIPTS stopped.  */
static bool scripts_step(struct osiop_data *d)
{
	uint32_t dsp = get32(d, R_DSP), w0, w1, w2 = 0;
	int op;

	if (!mem_read32(d, dsp, &w0) || !mem_read32(d, dsp + 4, &w1)) {
		dma_int(d, DSTAT_BF);
		return false;
	}

	if (getenv("OSIOP_TRACE") != NULL)
		fprintf(stderr, "[ osiop: %08x: %08x %08x  phase=%i sfbr=%02x"
		    " istat=%02x ]\n", dsp, w0, w1, d->phase, d->reg[R_SFBR],
		    d->reg[R_ISTAT]);

	d->reg[R_DCMD] = w0 >> 24;
	put_dbc(d, w0 & 0xffffff);
	put32(d, R_DSPS, w1);
	dsp += 8;

	if ((w0 >> 30) == 3) {
		if (!mem_read32(d, dsp, &w2)) {
			dma_int(d, DSTAT_BF);
			return false;
		}
		dsp += 4;
	}
	put32(d, R_DSP, dsp);

	switch (w0 >> 30) {

	case 0:		/*  Block move  */
		block_move(d, w0, w1);
		break;

	case 1:		/*  I/O, read/write register  */
		op = (w0 >> 27) & 7;
		switch (op) {
		case 0:		/*  SELECT  */
			select_target(d, w0, w1);
			break;
		case 1:		/*  WAIT DISCONNECT  */
			if (d->phase != PH_BUS_FREE) {
				if (d->msg_in_ack && d->disconnect_after_ack &&
				    d->msg_in_pos >= d->msg_in_len)
					bus_free(d);
				else {
					/*  The target is not disconnecting.  */
					scsi_int(d, SSTAT0_UDC);
				}
			}
			break;
		case 2:		/*  WAIT RESELECT  */
			if (d->phase != PH_BUS_FREE) {
				dma_int(d, DSTAT_IID);
				break;
			}
			if (d->reg[R_ISTAT] & ISTAT_SIGP) {
				put32(d, R_DSP, w1);
				break;
			}
			/*  Nothing reselects: wait for SIGP.  */
			d->waiting_resel = true;
			put32(d, R_DSP, dsp - 8);
			return false;
		case 3:		/*  SET  */
		case 4:		/*  CLEAR  */
			if (w0 & 0x00000008) {
				d->atn = op == 3;
				/*  The target goes to MSG OUT.  */
				if (d->atn && d->phase != PH_BUS_FREE &&
				    !d->msg_in_ack && d->phase != PH_MSG_OUT)
					d->phase = PH_MSG_OUT;
			}
			if (w0 & 0x00000040) {
				if (op == 4 && d->msg_in_ack)
					msg_in_ack_released(d);
			}
			if (w0 & 0x00000400)
				d->carry = op == 3;
			if (op == 3 && d->atn && d->msg_in_ack == false &&
			    d->phase == PH_MSG_IN)
				d->phase = PH_MSG_OUT;
			update_bus_regs(d);
			break;
		case 5:		/*  MOVE SFBR (op) data TO register  */
		case 6:		/*  MOVE register (op) data TO SFBR  */
		case 7: {	/*  MOVE register (op) data TO register  */
			int a = (w0 >> 16) & 0x3f, oper = (w0 >> 24) & 7;
			uint8_t data = (w0 >> 8) & 0xff, src, res = 0;
			unsigned int sum;

			src = op == 5 ? d->reg[R_SFBR] :
			    read_reg_for_script(d, a);
			switch (oper) {
			case 0:	/*  MOVE data TO reg, or a plain move  */
				res = op == 7 ? data : src;
				break;
			case 1:	res = (src << 1) | d->carry;
				d->carry = src >> 7;
				break;
			case 2:	res = src | data; break;
			case 3:	res = src ^ data; break;
			case 4:	res = src & data; break;
			case 5:	res = (src >> 1) | (d->carry << 7);
				d->carry = src & 1;
				break;
			case 6:	sum = src + data;
				d->carry = sum >> 8;
				res = sum;
				break;
			case 7:	sum = src + data + d->carry;
				d->carry = sum >> 8;
				res = sum;
				break;
			}
			if (op == 6)
				d->reg[R_SFBR] = res;
			else
				write_reg_for_script(d, a, res);
			break;
		}
		}
		break;

	case 2:		/*  Transfer control  */
		op = (w0 >> 27) & 7;
		if (op > 3) {
			dma_int(d, DSTAT_IID);
			break;
		}
		if ((w0 & TC_WAIT) && (w0 & TC_PHASE) &&
		    d->phase == PH_BUS_FREE) {
			/*  Waiting for a phase with nobody connected.  */
			scsi_int(d, SSTAT0_UDC);
			break;
		}
		if (!condition(d, w0))
			break;
		if (w0 & TC_REL)
			w1 = dsp + (((int32_t)(w1 << 8)) >> 8);
		switch (op) {
		case 0:		/*  JUMP  */
			put32(d, R_DSP, w1);
			break;
		case 1:		/*  CALL  */
			put32(d, R_TEMP, dsp);
			put32(d, R_DSP, w1);
			break;
		case 2:		/*  RETURN  */
			put32(d, R_DSP, get32(d, R_TEMP));
			break;
		case 3:		/*  INT  */
			dma_int(d, DSTAT_SIR);
			break;
		}
		break;

	case 3: {	/*  Memory move  */
		uint32_t count = w0 & 0xffffff, i;
		uint8_t buf[256];
		for (i = 0; i < count; ) {
			uint32_t k = count - i > sizeof(buf) ? sizeof(buf) :
			    count - i;
			if (!mem_read(d, w1 + i, buf, k) ||
			    !mem_write(d, w2 + i, buf, k)) {
				dma_int(d, DSTAT_BF);
				break;
			}
			i += k;
		}
		put32(d, R_DNAD, w2 + count);
		put_dbc(d, 0);
		break;
	}
	}

	put32(d, R_ADDER, get32(d, R_DSP));

	if (d->running && (d->reg[R_DCNTL] & DCNTL_SSM))
		dma_int(d, DSTAT_SSI);

	return d->running;
}

static void scripts_run(struct osiop_data *d, int max)
{
	int i;

	for (i = 0; i < max && d->running; i++)
		if (!scripts_step(d))
			break;
}

static void scripts_start(struct osiop_data *d)
{
	if (d->reg[R_ISTAT] & (ISTAT_DIP | ISTAT_SIP))
		return;		/*  (the host must clear interrupts first)  */
	d->running = true;
	d->waiting_resel = false;
}


DEVICE_TICK(osiop)
{
	struct osiop_data *d = (struct osiop_data *) extra;

	if (d->waiting_resel && (d->reg[R_ISTAT] & ISTAT_SIGP)) {
		/*  WAIT RESELECT is left when SIGP is set.  */
		uint32_t w1;
		d->waiting_resel = false;
		if (mem_read32(d, get32(d, R_DSP) + 4, &w1))
			put32(d, R_DSP, w1);
		d->running = true;
	}

	if (d->running)
		scripts_run(d, MAX_SCRIPTS_PER_TICK);
}


/*****************************************************************************
 *
 *  Register access
 */

static void chip_reset(struct osiop_data *d)
{
	uint8_t scid = d->reg[R_SCID];

	memset(d->reg, 0, sizeof(d->reg));
	d->reg[R_SCID] = scid;
	d->reg[R_DSTAT] = DSTAT_DFE;
	d->reg[R_CTEST1] = 0xf0;	/*  DMA FIFO empty  */
	d->reg[R_CTEST8] = CHIP_REVISION << 4;
	d->running = false;
	d->waiting_resel = false;
	bus_free(d);
	update_irq(d);
}

static void scsi_bus_reset(struct osiop_data *d)
{
	bus_free(d);
	d->running = false;
	d->waiting_resel = false;
	scsi_int(d, SSTAT0_RST);
}

static uint8_t reg_read(struct osiop_data *d, int r)
{
	uint8_t v = d->reg[r];

	switch (r) {
	case R_DSTAT:
		/*  Reading DSTAT clears it (and DIP).  */
		d->reg[R_DSTAT] = DSTAT_DFE;
		d->reg[R_ISTAT] &= ~ISTAT_DIP;
		update_irq(d);
		return v | DSTAT_DFE;
	case R_SSTAT0:
		d->reg[R_SSTAT0] = 0;
		d->reg[R_ISTAT] &= ~ISTAT_SIP;
		update_irq(d);
		return v;
	case R_CTEST2:
		/*  Reading CTEST2 clears SIGP.  */
		v = (d->reg[R_ISTAT] & ISTAT_SIGP) ? 0x40 : 0;
		d->reg[R_ISTAT] &= ~ISTAT_SIGP;
		return v;
	case R_SSTAT1:
		return 0;
	case R_SSTAT2:
		return d->reg[R_SSTAT2] & 0x0f;	/*  SCSI FIFO empty  */
	case R_DFIFO:
		return 0;
	case R_CTEST1:
		return 0xf0;
	case R_CTEST8:
		return (CHIP_REVISION << 4) | (d->reg[R_CTEST8] & 0x0f);
	}
	return v;
}

static void reg_write(struct osiop_data *d, int r, uint8_t v)
{
	switch (r) {
	case R_DSTAT:
	case R_SSTAT0:
	case R_SSTAT1:
	case R_SSTAT2:
	case R_SFBR:
	case R_SIDL:
	case R_SBDL:
	case R_CTEST0:
	case R_CTEST1:
	case R_CTEST2:
	case R_CTEST3:
	case R_ADDER: case R_ADDER+1: case R_ADDER+2: case R_ADDER+3:
		return;

	case R_ISTAT:
		if (v & ISTAT_RST) {
			chip_reset(d);
			return;
		}
		if (v & ISTAT_ABRT) {
			if (d->running || d->waiting_resel) {
				d->waiting_resel = false;
				dma_int(d, DSTAT_ABRT);
			}
		}
		if (v & ISTAT_SIGP)
			d->reg[R_ISTAT] |= ISTAT_SIGP;
		return;

	case R_SCNTL1:
		if ((v & SCNTL1_RST) && !(d->reg[r] & SCNTL1_RST))
			scsi_bus_reset(d);
		d->reg[r] = (v & ~SCNTL1_CON) | (d->reg[r] & SCNTL1_CON);
		return;

	case R_DSP + 3:
		/*  Writing the low byte of DSP starts SCRIPTS.  */
		d->reg[r] = v;
		if (!(d->reg[R_DMODE] & DMODE_MAN))
			scripts_start(d);
		return;

	case R_DCNTL:
		d->reg[r] = v & ~DCNTL_STD;
		if (v & DCNTL_STD)
			scripts_start(d);
		return;
	}

	d->reg[r] = v;
}


DEVICE_ACCESS(osiop)
{
	struct osiop_data *d = (struct osiop_data *) extra;
	size_t i;

	for (i = 0; i < len; i++) {
		int r = (relative_addr + i) & (OSIOP_LEN - 1);
		if (writeflag == MEM_READ)
			data[i] = reg_read(d, r);
		else
			reg_write(d, r, data[i]);
	}

	/*  (SCRIPTS run from the tick function: drivers expect the CPU to
	    go on for a while after starting them.)  */
	return 1;
}


DEVINIT(osiop)
{
	struct osiop_data *d;

	CHECK_ALLOCATION(d = (struct osiop_data *) malloc(sizeof(struct osiop_data)));
	memset(d, 0, sizeof(struct osiop_data));
	d->machine = devinit->machine;
	d->base = devinit->addr;
	d->phase = PH_BUS_FREE;
	d->reg[R_SCID] = 0x80;		/*  ID 7  */

	INTERRUPT_CONNECT(devinit->interrupt_path, d->irq);

	chip_reset(d);

	memory_device_register(devinit->machine->memory, devinit->name,
	    devinit->addr, 0x1000, dev_osiop_access, (void *)d,
	    DM_DEFAULT, NULL);

	machine_add_tickfunction(devinit->machine,
	    dev_osiop_tick, d, OSIOP_TICK_SHIFT);

	(void) phase_names;
	return 1;
}

