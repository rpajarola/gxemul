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
 *  COMMENT: Adaptec AHA-1740A / AHA-1742A EISA SCSI host adapter
 *
 *  Standard SCSI controller on DECpc AXP 150 / DEC 2000 Model 300 ("Jensen")
 *  installed in EISA slot 1 (I/O base 0x1c80).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "device.h"
#include "devices.h"
#include "diskimage.h"
#include "interrupt.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#define	DEV_AHA1742_LENGTH	0x80

/*
 * EISA Board configuration registers (relative to base 0x1c80):
 */
#define	AHA_HID0		0x00
#define	AHA_HID1		0x01
#define	AHA_HID2		0x02
#define	AHA_HID3		0x03
#define	AHA_EBCNTRL		0x04

#define	AHA_PORTADDR		0x40
#define	 PORTADDR_ENHANCED	0x80
#define	AHA_BIOSADDR		0x41
#define	AHA_INTDEF		0x42
#define	AHA_SCSIDEF		0x43
#define	AHA_BUSDEF		0x44

/*
 * Enhanced mode mailbox & control registers (relative to base 0x1c80):
 */
#define	AHA_MBOXOUT0		0x50
#define	AHA_MBOXOUT1		0x51
#define	AHA_MBOXOUT2		0x52
#define	AHA_MBOXOUT3		0x53

#define	AHA_ATTN		0x54
#define	 ATTN_TARGET		0x0F
#define	 ATTN_OPCODE		0xF0
#define	  OP_IMMED		0x10
#define	  OP_START_ECB		0x40
#define	  OP_ABORT_ECB		0x50

#define	AHA_G2CNTRL		0x55
#define	 G2CNTRL_SET_HOST_READY	0x20
#define	 G2CNTRL_CLEAR_EISA_INT	0x40
#define	 G2CNTRL_HARD_RESET	0x80

#define	AHA_G2INTST		0x56
#define	 G2INTST_TARGET		0x0F
#define	 G2INTST_INT_STAT	0xF0
#define	  AHB_ECB_OK		0x10
#define	  AHB_ECB_RECOVERED	0x50
#define	  AHB_HW_ERR		0x70
#define	  AHB_IMMED_OK		0xA0
#define	  AHB_ECB_ERR		0xC0
#define	  AHB_IMMED_ERR		0xE0

#define	AHA_G2STAT		0x57
#define	 G2STAT_BUSY		0x01
#define	 G2STAT_INT_PEND	0x02
#define	 G2STAT_MBOX_EMPTY	0x04

#define	AHA_MBOXIN0		0x58
#define	AHA_MBOXIN1		0x59
#define	AHA_MBOXIN2		0x5A
#define	AHA_MBOXIN3		0x5B

#define	AHA_G2STAT2		0x5C
#define	 G2STAT2_HOST_READY	0x01

/*
 * Enhanced Control Block (ECB) layout:
 */
#define	ECB_CNE			0x0001
#define	ECB_DI			0x0080
#define	ECB_SES			0x0400
#define	ECB_S_G			0x1000
#define	ECB_DSB			0x4000
#define	ECB_ARS			0x8000

#define	ECB_LUN			0x0007
#define	ECB_DAT			0x0100

#define	ST_DON			0x0001
#define	AHA_SW_ME		0x1000
#define	HS_OK			0x00
#define	HS_TIMED_OUT		0x11
#define	HS_HARDWARE_ERR		0x20


#define	AHA_COMPLETION_QUEUE_LEN	64

struct aha1742_data {
	struct machine		*machine;
	struct interrupt	irq[16];
	int			irq_asserted;

	/* EISA Config: */
	uint8_t			ebcntrl;
	uint8_t			portaddr;
	uint8_t			biosaddr;
	uint8_t			intdef;
	uint8_t			scsidef;
	uint8_t			busdef;

	/* Enhanced registers: */
	uint32_t		mboxout;
	uint32_t		mboxin;
	uint8_t			attn;
	uint8_t			g2cntrl;
	uint8_t			g2intst;
	uint8_t			g2stat;
	uint8_t			g2stat2;

	/*
	 *  Completions waiting to be presented to the host. Only one
	 *  completion (mailbox in + interrupt status) is visible at a time;
	 *  the next one is presented when the host clears the interrupt.
	 */
	int			cq_head, cq_len;
	struct {
		uint32_t	mboxin;
		uint8_t		intst;
		uint8_t		set_mboxin;
	}			cq[AHA_COMPLETION_QUEUE_LEN];
};


static int aha1742_get_irq(struct aha1742_data *d)
{
	switch (d->intdef & 0x07) {
	case 0: return 9;
	case 1: return 10;
	case 2: return 11;
	case 3: return 12;
	case 5: return 14;
	case 6: return 15;
	default: return 11;
	}
}

static void aha1742_assert_irq(struct aha1742_data *d)
{
	int irq = aha1742_get_irq(d);

	if (!(d->intdef & 0x10))	/* Interrupt enable bit */
		return;

	if (d->irq_asserted) {
		INTERRUPT_DEASSERT(d->irq[d->irq_asserted]);
	}

	d->irq_asserted = irq;
	debug("[ aha1742: ASSERT IRQ %d intdef=0x%02x ]\n", irq, d->intdef);
	INTERRUPT_ASSERT(d->irq[irq]);
}

static void aha1742_deassert_irq(struct aha1742_data *d)
{
	if (!d->irq_asserted)
		return;

	debug("[ aha1742: DEASSERT IRQ %d ]\n", d->irq_asserted);
	INTERRUPT_DEASSERT(d->irq[d->irq_asserted]);
	d->irq_asserted = 0;
}

/*
 *  aha1742_dma():
 *
 *  Bus master DMA between host memory and a buffer. memory_rw() cannot
 *  cross a memory block boundary in one call, so the transfer is split
 *  at page boundaries.
 */
static int aha1742_dma(struct cpu *cpu, struct memory *mem, uint64_t addr,
	unsigned char *buf, size_t len, int writeflag, int flags)
{
	while (len > 0) {
		size_t chunk = 4096 - (addr & 4095);

		if (chunk > len)
			chunk = len;
		if (!cpu->memory_rw(cpu, mem, addr, buf, chunk, writeflag,
		    flags))
			return 0;
		addr += chunk;
		buf += chunk;
		len -= chunk;
	}

	return MEMORY_ACCESS_OK;
}

static void aha1742_present(struct aha1742_data *d, uint32_t mboxin,
	uint8_t intst, int set_mboxin)
{
	if (set_mboxin)
		d->mboxin = mboxin;
	d->g2intst = intst;
	d->g2stat |= G2STAT_INT_PEND;
	aha1742_assert_irq(d);
}

/*
 *  aha1742_complete():
 *
 *  Report a completed ECB or immediate command. If the host has not yet
 *  cleared the previous completion interrupt, the completion is queued.
 */
static void aha1742_complete(struct aha1742_data *d, uint32_t mboxin,
	uint8_t intst, int set_mboxin)
{
	d->g2stat |= G2STAT_MBOX_EMPTY;

	if (!(d->g2stat & G2STAT_INT_PEND)) {
		aha1742_present(d, mboxin, intst, set_mboxin);
		return;
	}

	if (d->cq_len >= AHA_COMPLETION_QUEUE_LEN) {
		fatal("[ aha1742: completion queue overflow ]\n");
		return;
	}

	{
		int i = (d->cq_head + d->cq_len) % AHA_COMPLETION_QUEUE_LEN;
		d->cq[i].mboxin = mboxin;
		d->cq[i].intst = intst;
		d->cq[i].set_mboxin = set_mboxin;
		d->cq_len ++;
	}
}

static void aha1742_execute_ecb(struct cpu *cpu, struct aha1742_data *d)
{
	uint8_t ecb_buf[0x40];
	uint8_t opcode, scsi_cmd_len;
	uint16_t opt1, opt2;
	uint32_t data_addr, data_len, status_addr;
	int target, lun, res;
	struct scsi_transfer *xferp;
	uint8_t status_block[32];

	target = d->attn & ATTN_TARGET;

	/* Read ECB structure from host physical memory: */
	if (!aha1742_dma(cpu, cpu->mem, d->mboxout, ecb_buf,
	    sizeof(ecb_buf), MEM_READ, PHYSICAL | NO_EXCEPTIONS)) {
		fatal("[ aha1742: memory_rw() failed reading ECB at 0x%x ]\n",
		    d->mboxout);
		return;
	}

	opcode = ecb_buf[0];
	opt1 = ecb_buf[2] | (ecb_buf[3] << 8);
	opt2 = ecb_buf[4] | (ecb_buf[5] << 8);
	data_addr = ecb_buf[8] | (ecb_buf[9] << 8) | (ecb_buf[10] << 16) |
	    (ecb_buf[11] << 24);
	data_len = ecb_buf[12] | (ecb_buf[13] << 8) | (ecb_buf[14] << 16) |
	    (ecb_buf[15] << 24);
	status_addr = ecb_buf[16] | (ecb_buf[17] << 8) | (ecb_buf[18] << 16) |
	    (ecb_buf[19] << 24);
	scsi_cmd_len = ecb_buf[33];

	lun = opt2 & ECB_LUN;

	debug("[ aha1742: ECB cmd op=0x%02x target=%d lun=%d cdb_len=%d data_addr=0x%x data_len=%d status=0x%x ]\n",
	    opcode, target, lun, scsi_cmd_len, data_addr, data_len, status_addr);
	memset(status_block, 0, sizeof(status_block));

	if (opcode == 0x0a) {
		/*
		 *  Read Host Adapter Inquiry Data (AHA_OP_RHAID).
		 *  Target 7 is the host adapter itself.
		 */
		uint8_t inq[256];
		uint32_t to_copy = data_len;
		memset(inq, 0, sizeof(inq));
		inq[0] = 0x00;  /* dev_type */
		inq[1] = 0x00;
		inq[2] = 0x01;  /* support_level */
		inq[3] = 0x00;
		inq[4] = 0x00;  /* residual_length */
		inq[5] = 0x08;  /* num_lun */
		inq[6] = 0x40;  /* num_cb = 64 */
		inq[7] = 0x00;  /* flags */
		memcpy(inq + 8,  "Adaptec ", 8);
		memcpy(inq + 16, "AHA-1740", 8);
		memcpy(inq + 24, "enhanced", 8);
		memcpy(inq + 32, "H   ", 4);    /* firmware_rev: rev H */
		memcpy(inq + 36, "010193  ", 8);
		memcpy(inq + 44, "120000  ", 8);

		if (to_copy > sizeof(inq))
			to_copy = sizeof(inq);

		if (data_addr != 0 && to_copy > 0) {
			if (opt1 & ECB_S_G) {
				uint32_t cur = 0;
				uint32_t seg_idx = 0;
				while (cur < to_copy && seg_idx * 8 < data_len) {
					uint8_t sbuf[8];
					uint32_t saddr, slen, chunk;
					aha1742_dma(cpu, cpu->mem,
					    data_addr + seg_idx * 8, sbuf, 8,
					    MEM_READ, PHYSICAL | NO_EXCEPTIONS);
					saddr = sbuf[0] | (sbuf[1]<<8) | (sbuf[2]<<16) | (sbuf[3]<<24);
					slen  = sbuf[4] | (sbuf[5]<<8) | (sbuf[6]<<16) | (sbuf[7]<<24);
					chunk = to_copy - cur;
					if (chunk > slen)
						chunk = slen;
					aha1742_dma(cpu, cpu->mem, saddr,
					    inq + cur, chunk,
					    MEM_WRITE, PHYSICAL | NO_EXCEPTIONS);
					cur += chunk;
					seg_idx++;
				}
			} else {
				aha1742_dma(cpu, cpu->mem, data_addr,
				    inq, to_copy,
				    MEM_WRITE, PHYSICAL | NO_EXCEPTIONS);
			}
		}

		status_block[0] = ST_DON & 0xff;
		status_block[1] = (ST_DON >> 8) & 0xff;
		status_block[2] = HS_OK;
		status_block[3] = 0x00;

		if (status_addr != 0) {
			aha1742_dma(cpu, cpu->mem, status_addr, status_block,
			    sizeof(status_block), MEM_WRITE, PHYSICAL | NO_EXCEPTIONS);
		}

		aha1742_complete(d, d->mboxout,
		    (target & ATTN_TARGET) | AHB_ECB_OK, 1);
		return;
	}

	if (opcode == 0x00) {
		/* AHA_OP_NOP */
		status_block[0] = ST_DON & 0xff;
		status_block[1] = (ST_DON >> 8) & 0xff;
		status_block[2] = HS_OK;
		status_block[3] = 0x00;

		if (status_addr != 0) {
			aha1742_dma(cpu, cpu->mem, status_addr, status_block,
			    sizeof(status_block), MEM_WRITE, PHYSICAL | NO_EXCEPTIONS);
		}

		aha1742_complete(d, d->mboxout,
		    (target & ATTN_TARGET) | AHB_ECB_OK, 1);
		return;
	}

	if (scsi_cmd_len > 12)
		scsi_cmd_len = 12;

	/* Check if target device exists on SCSI bus: */
	if (!diskimage_exist(d->machine, target, DISKIMAGE_SCSI) || lun != 0) {
		status_block[0] = 0x00;
		status_block[1] = (AHA_SW_ME >> 8) & 0xff;
		status_block[2] = HS_TIMED_OUT;
		status_block[3] = 0x00;

		if (status_addr != 0) {
			aha1742_dma(cpu, cpu->mem, status_addr, status_block,
			    sizeof(status_block), MEM_WRITE, PHYSICAL | NO_EXCEPTIONS);
		}

		aha1742_complete(d, d->mboxout,
		    (target & ATTN_TARGET) | AHB_ECB_ERR, 1);
		return;
	}

	xferp = scsi_transfer_alloc();
	scsi_transfer_allocbuf(&xferp->cmd_len, &xferp->cmd, scsi_cmd_len, 0);
	memcpy(xferp->cmd, ecb_buf + 36, scsi_cmd_len);

	res = diskimage_scsicommand(cpu, target, DISKIMAGE_SCSI, xferp);

	if (res == 2) {
		/* DATA_OUT: write data to device */
		if (xferp->data_out_len > 0) {
			if (xferp->data_out == NULL)
				scsi_transfer_allocbuf(&xferp->data_out_len,
				    &xferp->data_out, xferp->data_out_len, 0);

			if (opt1 & ECB_S_G) {
				/* Scatter / gather */
				uint32_t cur = 0;
				uint32_t seg_idx = 0;
				while (cur < xferp->data_out_len &&
				    seg_idx * 8 < data_len) {
					uint8_t sbuf[8];
					uint32_t saddr, slen, chunk;
					aha1742_dma(cpu, cpu->mem,
					    data_addr + seg_idx * 8, sbuf, 8,
					    MEM_READ, PHYSICAL | NO_EXCEPTIONS);
					saddr = sbuf[0] | (sbuf[1]<<8) | (sbuf[2]<<16) | (sbuf[3]<<24);
					slen  = sbuf[4] | (sbuf[5]<<8) | (sbuf[6]<<16) | (sbuf[7]<<24);
					chunk = xferp->data_out_len - cur;
					if (chunk > slen)
						chunk = slen;
					aha1742_dma(cpu, cpu->mem, saddr,
					    xferp->data_out + cur, chunk,
					    MEM_READ, PHYSICAL | NO_EXCEPTIONS);
					cur += chunk;
					seg_idx++;
				}
				xferp->data_out_offset = cur;
			} else {
				uint32_t chunk = xferp->data_out_len;
				if (chunk > data_len)
					chunk = data_len;
				aha1742_dma(cpu, cpu->mem, data_addr,
				    xferp->data_out, chunk,
				    MEM_READ, PHYSICAL | NO_EXCEPTIONS);
				xferp->data_out_offset = chunk;
			}

			/*
			 *  Re-run to commit the data. The disk image only
			 *  writes once data_out_offset says that all of the
			 *  data has arrived.
			 */
			res = diskimage_scsicommand(cpu, target, DISKIMAGE_SCSI, xferp);
		}
	} else if (res == 1) {
		/* DATA_IN: read data from device */
		if (xferp->data_in_len > 0) {
			if (opt1 & ECB_S_G) {
				uint32_t cur = 0;
				uint32_t seg_idx = 0;
				while (cur < xferp->data_in_len &&
				    seg_idx * 8 < data_len) {
					uint8_t sbuf[8];
					uint32_t saddr, slen, chunk;
					aha1742_dma(cpu, cpu->mem,
					    data_addr + seg_idx * 8, sbuf, 8,
					    MEM_READ, PHYSICAL | NO_EXCEPTIONS);
					saddr = sbuf[0] | (sbuf[1]<<8) | (sbuf[2]<<16) | (sbuf[3]<<24);
					slen  = sbuf[4] | (sbuf[5]<<8) | (sbuf[6]<<16) | (sbuf[7]<<24);
					chunk = xferp->data_in_len - cur;
					if (chunk > slen)
						chunk = slen;
					aha1742_dma(cpu, cpu->mem, saddr,
					    xferp->data_in + cur, chunk,
					    MEM_WRITE, PHYSICAL | NO_EXCEPTIONS);
					cur += chunk;
					seg_idx++;
				}
			} else {
				uint32_t chunk = xferp->data_in_len;
				if (chunk > data_len)
					chunk = data_len;
				aha1742_dma(cpu, cpu->mem, data_addr,
				    xferp->data_in, chunk,
				    MEM_WRITE, PHYSICAL | NO_EXCEPTIONS);
			}
		}
	}

	/* Fill status block: */
	status_block[0] = ST_DON & 0xff;
	status_block[1] = (ST_DON >> 8) & 0xff;
	status_block[2] = HS_OK;
	status_block[3] = (xferp->status != NULL && xferp->status_len > 0) ?
	    xferp->status[0] : 0;

	/*  A command still waiting for data out never completed:  */
	if (res == 2) {
		debugmsg(SUBSYS_DEVICE, "aha1742", VERBOSITY_WARNING,
		    "target %d: command 0x%02x got %d of %d data out bytes\n",
		    target, xferp->cmd[0], (int)xferp->data_out_offset,
		    (int)xferp->data_out_len);
		status_block[2] = HS_HARDWARE_ERR;
	}

	if (status_addr != 0) {
		aha1742_dma(cpu, cpu->mem, status_addr, status_block,
		    sizeof(status_block), MEM_WRITE, PHYSICAL | NO_EXCEPTIONS);
	}

	aha1742_complete(d, d->mboxout, (target & ATTN_TARGET) |
	    (status_block[2] != HS_OK || status_block[3] ?
	    AHB_ECB_ERR : AHB_ECB_OK), 1);
	scsi_transfer_free(xferp);
}


DEVICE_ACCESS(aha1742)
{
	struct aha1742_data *d = (struct aha1742_data *) extra;
	static const uint8_t board_id[4] = { 0x04, 0x90, 0x00, 0x01 }; /* "ADP0001" */
	uint64_t idata = 0, odata = 0;

	if (writeflag == MEM_WRITE)
		idata = memory_readmax64(cpu, data, len);


	switch (relative_addr) {
	case AHA_HID0:
	case AHA_HID1:
	case AHA_HID2:
	case AHA_HID3:
		if (writeflag == MEM_WRITE)
			break;
		odata = board_id[relative_addr - AHA_HID0];
		break;

	case AHA_EBCNTRL:
		if (writeflag == MEM_WRITE)
			d->ebcntrl = idata;
		else
			odata = d->ebcntrl;
		break;

	case AHA_PORTADDR:
		if (writeflag == MEM_WRITE)
			d->portaddr = idata;
		else
			odata = d->portaddr;
		break;

	case AHA_BIOSADDR:
		if (writeflag == MEM_WRITE)
			d->biosaddr = idata;
		else
			odata = d->biosaddr;
		break;

	case AHA_INTDEF:
		if (writeflag == MEM_WRITE) {
			d->intdef = idata;
			debug("[ aha1742: write INTDEF = 0x%02x (IRQ %d) ]\n",
			    d->intdef, aha1742_get_irq(d));
		} else {
			odata = d->intdef;
		}
		break;

	case AHA_SCSIDEF:
		if (writeflag == MEM_WRITE)
			d->scsidef = idata;
		else
			odata = d->scsidef;
		break;

	case AHA_BUSDEF:
		if (writeflag == MEM_WRITE)
			d->busdef = idata;
		else
			odata = d->busdef;
		break;

	case AHA_MBOXOUT0:
		if (writeflag == MEM_WRITE)
			d->mboxout = (d->mboxout & 0xffffff00) | (idata & 0xff);
		else
			odata = d->mboxout & 0xff;
		break;

	case AHA_MBOXOUT1:
		if (writeflag == MEM_WRITE)
			d->mboxout = (d->mboxout & 0xffff00ff) | ((idata & 0xff) << 8);
		else
			odata = (d->mboxout >> 8) & 0xff;
		break;

	case AHA_MBOXOUT2:
		if (writeflag == MEM_WRITE)
			d->mboxout = (d->mboxout & 0xff00ffff) | ((idata & 0xff) << 16);
		else
			odata = (d->mboxout >> 16) & 0xff;
		break;

	case AHA_MBOXOUT3:
		if (writeflag == MEM_WRITE)
			d->mboxout = (d->mboxout & 0x00ffffff) | ((idata & 0xff) << 24);
		else
			odata = (d->mboxout >> 24) & 0xff;
		break;

	case AHA_ATTN:
		if (writeflag == MEM_WRITE) {
			d->attn = idata;
			debug("[ aha1742: write ATTN = 0x%02x (op 0x%02x targ %d) mboxout=0x%x ]\n",
			    d->attn, d->attn & ATTN_OPCODE, d->attn & ATTN_TARGET, d->mboxout);
			if ((d->attn & ATTN_OPCODE) == OP_START_ECB) {
				aha1742_execute_ecb(cpu, d);
			} else if ((d->attn & ATTN_OPCODE) == OP_IMMED) {
				/* Immediate command (target reset) */
				debug("[ aha1742: OP_IMMED target=%d ]\n", d->attn & ATTN_TARGET);
				aha1742_complete(d, 0,
				    (d->attn & ATTN_TARGET) | AHB_IMMED_OK, 0);
			} else if ((d->attn & ATTN_OPCODE) == OP_ABORT_ECB) {
				/* Abort ECB */
				debug("[ aha1742: OP_ABORT_ECB target=%d ]\n", d->attn & ATTN_TARGET);
				aha1742_complete(d, d->mboxout,
				    (d->attn & ATTN_TARGET) | AHB_ECB_OK, 1);
			} else {
				debug("[ aha1742: unknown ATTN opcode 0x%02x ]\n", d->attn & ATTN_OPCODE);
			}
		} else {
			odata = d->attn;
		}
		break;

	case AHA_G2CNTRL:
		if (writeflag == MEM_WRITE) {
			d->g2cntrl = idata;
			debug("[ aha1742: write G2CNTRL = 0x%02x ]\n", (int)idata);
			if (idata & G2CNTRL_CLEAR_EISA_INT) {
				d->g2stat &= ~G2STAT_INT_PEND;
				aha1742_deassert_irq(d);

				/*  Present the next queued completion:  */
				if (d->cq_len > 0) {
					int i = d->cq_head;
					d->cq_head = (d->cq_head + 1) %
					    AHA_COMPLETION_QUEUE_LEN;
					d->cq_len --;
					aha1742_present(d, d->cq[i].mboxin,
					    d->cq[i].intst,
					    d->cq[i].set_mboxin);
				}
			}
			if (idata & G2CNTRL_HARD_RESET) {
				aha1742_deassert_irq(d);
				d->g2stat = G2STAT_MBOX_EMPTY;
				d->g2intst = 0;
				d->mboxin = 0;
				d->cq_head = d->cq_len = 0;
				d->mboxout = 0;
				d->attn = 0;
			}
		} else {
			odata = d->g2cntrl;
		}
		break;

	case AHA_G2INTST:
		if (writeflag == MEM_WRITE)
			break;
		debug("[ aha1742: read G2INTST = 0x%02x ]\n", (int)d->g2intst);
		odata = d->g2intst;
		break;

	case AHA_G2STAT:
		if (writeflag == MEM_WRITE)
			break;
		odata = d->g2stat;
		break;

	case AHA_MBOXIN0:
		if (writeflag == MEM_WRITE)
			break;
		odata = d->mboxin & 0xff;
		break;

	case AHA_MBOXIN1:
		if (writeflag == MEM_WRITE)
			break;
		odata = (d->mboxin >> 8) & 0xff;
		break;

	case AHA_MBOXIN2:
		if (writeflag == MEM_WRITE)
			break;
		odata = (d->mboxin >> 16) & 0xff;
		break;

	case AHA_MBOXIN3:
		if (writeflag == MEM_WRITE)
			break;
		debug("[ aha1742: read MBOXIN = 0x%08x ]\n", (unsigned int)d->mboxin);
		odata = (d->mboxin >> 24) & 0xff;
		break;

	case AHA_G2STAT2:
		if (writeflag == MEM_WRITE)
			d->g2stat2 = idata;
		else
			odata = d->g2stat2;
		break;

	default:
		if (writeflag == MEM_WRITE)
			debug("[ aha1742: write to unimpl reg 0x%02x: 0x%02x ]\n",
			    (int)relative_addr, (int)idata);
		else
			debug("[ aha1742: read from unimpl reg 0x%02x ]\n",
			    (int)relative_addr);
		break;
	}

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, odata);

	return 1;
}


/*
 *  dev_aha1742_init():
 *
 *  Initialize an Adaptec AHA-1740A/1742A SCSI controller in EISA slot 1.
 */
struct aha1742_data *dev_aha1742_init(struct machine *machine,
	struct memory *mem, uint64_t baseaddr, const char *interrupt_base_path)
{
	struct aha1742_data *d;
	char tmp[300];
	int i;

	CHECK_ALLOCATION(d = (struct aha1742_data *) malloc(sizeof(struct aha1742_data)));
	memset(d, 0, sizeof(struct aha1742_data));

	d->machine = machine;
	d->ebcntrl = 0x01;		/* Board enabled */
	d->portaddr = PORTADDR_ENHANCED;/* Enhanced mode */
	d->intdef = 0x12;		/* INT11 | INTEN (IRQ 11 enabled) */
	d->scsidef = 0x17;		/* SCSI ID 7, RSTPWR */
	d->g2stat = G2STAT_MBOX_EMPTY;	/* Ready for command */

	/* Connect all possible IRQs (9, 10, 11, 12, 14, 15): */
	for (i = 0; i < 16; i++) {
		snprintf(tmp, sizeof(tmp), "%s.isa.%i", interrupt_base_path, i);
		INTERRUPT_CONNECT(tmp, d->irq[i]);
	}

	memory_device_register(mem, "aha1742", baseaddr,
	    DEV_AHA1742_LENGTH, dev_aha1742_access, d, DM_DEFAULT, NULL);

	return d;
}
