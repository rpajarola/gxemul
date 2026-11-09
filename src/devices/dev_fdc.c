/*
 *  Copyright (C) 2003-2009  Anders Gavare.  All rights reserved.
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
 *  COMMENT: PC-style floppy controller (Intel 82077AA)
 *
 *  Two variants live in this file:
 *
 *  o)  dev_fdc_init(): a working 82077AA, used by machines with an ISA DMA
 *      controller (dev_isadma.c), e.g. the DECpc AXP 150 (Jensen).  Drive n
 *      is floppy disk image id n (-d f:image.img).  Supports DMA and
 *      non-DMA (PIO) data transfers, SEEK/RECALIBRATE, READ/WRITE DATA,
 *      READ ID, FORMAT TRACK, VERIFY, SENSE INTERRUPT/DRIVE STATUS,
 *      SPECIFY, CONFIGURE, LOCK, VERSION, DUMPREG and PERPENDICULAR MODE.
 *
 *  o)  The "fdc" device_add() entry: the old register-only dummy, still
 *      used by the ARC and Algor machines.
 *
 *  See http://members.tripod.com/~oldboard/assembly/765.html for a
 *  quick overview, and the Intel 82077AA data sheet for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "devices.h"
#include "diskimage.h"
#include "interrupt.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"


#define	DEV_FDC_LENGTH		6	/*  TODO 8, but collision with wdc  */


struct fdc_data {
	uint8_t			reg[DEV_FDC_LENGTH];
	struct interrupt	irq;
};


DEVICE_ACCESS(fdc)
{
	struct fdc_data *d = (struct fdc_data *) extra;
	uint64_t idata = 0, odata = 0;
	size_t i;

	if (writeflag == MEM_WRITE)
		idata = memory_readmax64(cpu, data, len);

	switch (relative_addr) {
	case 0x04:
		break;
	default:if (writeflag==MEM_READ) {
			fatal("[ fdc: read from reg %i ]\n",
			    (int)relative_addr);
			odata = d->reg[relative_addr];
		} else {
			fatal("[ fdc: write to reg %i:", (int)relative_addr);
			for (i=0; i<len; i++)
				fatal(" %02x", data[i]);
			fatal(" ]\n");
			d->reg[relative_addr] = idata;
		}
	}

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, odata);

	return 1;
}


DEVINIT(fdc)
{
	struct fdc_data *d;

	CHECK_ALLOCATION(d = (struct fdc_data *) malloc(sizeof(struct fdc_data)));
	memset(d, 0, sizeof(struct fdc_data));

	INTERRUPT_CONNECT(devinit->interrupt_path, d->irq);

	memory_device_register(devinit->machine->memory, devinit->name,
	    devinit->addr, DEV_FDC_LENGTH, dev_fdc_access, d,
	    DM_DEFAULT, NULL);

	return 1;
}



/*****************************************************************************
 *
 *  Intel 82077AA floppy disk controller
 */

#define	FDC_NDRIVES		4
#define	FDC_SECTOR_SIZE		512

/*  Main status register:  */
#define	MSR_RQM			0x80
#define	MSR_DIO			0x40	/*  1 = controller -> CPU  */
#define	MSR_NDMA		0x20
#define	MSR_CB			0x10

/*  Digital output register:  */
#define	DOR_DRIVE_MASK		0x03
#define	DOR_NRESET		0x04
#define	DOR_DMAEN		0x08

/*  Status register 0:  */
#define	ST0_ABNORMAL		0x40
#define	ST0_INVALID		0x80
#define	ST0_SEEK_END		0x20
#define	ST0_EQUIP_CHECK		0x10

/*  Status register 1:  */
#define	ST1_EN			0x80	/*  end of cylinder  */
#define	ST1_OR			0x10	/*  overrun / underrun  */
#define	ST1_ND			0x04	/*  no data  */
#define	ST1_NW			0x02	/*  not writable  */
#define	ST1_MA			0x01	/*  missing address mark  */

/*  Status register 2:  */
#define	ST2_WC			0x10	/*  wrong cylinder  */

/*  Status register 3:  */
#define	ST3_WP			0x40
#define	ST3_RDY			0x20
#define	ST3_T0			0x10
#define	ST3_TS			0x08

enum fdc_phase {
	PHASE_COMMAND = 0,
	PHASE_EXEC_PIO_OUT,	/*  non-DMA read: data to the CPU  */
	PHASE_EXEC_PIO_IN,	/*  non-DMA write/format: data from the CPU  */
	PHASE_RESULT
};

struct fdc82077_data {
	struct machine		*machine;
	struct isadma_data	*dma;
	int			dma_channel;

	struct interrupt	irq;
	int			irq_pending;

	uint8_t			dor;
	uint8_t			tdr;
	uint8_t			dsr;
	uint8_t			ccr;
	int			in_reset;

	enum fdc_phase		phase;
	uint8_t			cmd[16];
	int			cmd_len, cmd_pos;
	uint8_t			res[16];
	int			res_len, res_pos;

	/*  Non-DMA execution phase buffer:  */
	unsigned char		*pio_buf;
	int			pio_len, pio_pos;

	/*  Drive state:  */
	uint8_t			pcn[FDC_NDRIVES];
	int			dskchg[FDC_NDRIVES];

	/*  Pending SENSE INTERRUPT STATUS results:  */
	int			n_sense;
	uint8_t			sense_st0[FDC_NDRIVES];
	uint8_t			sense_pcn[FDC_NDRIVES];

	/*  SPECIFY, CONFIGURE, PERPENDICULAR MODE, LOCK:  */
	uint8_t			specify[2];
	uint8_t			config[2];
	uint8_t			perp;
	int			lock;
};


/*
 *  fdc_drive_present():
 *
 *  Drive 0 always exists.  Other drives exist if a floppy image with that
 *  id was given.
 */
static int fdc_drive_present(struct fdc82077_data *d, int drive)
{
	return drive == 0 || diskimage_exist(d->machine, drive,
	    DISKIMAGE_FLOPPY);
}


/*
 *  fdc_medium():
 *
 *  Returns the floppy disk image in the drive, or NULL if there is none.
 */
static struct diskimage *fdc_medium(struct fdc82077_data *d, int drive)
{
	struct diskimage *di = d->machine->first_diskimage;

	while (di != NULL) {
		if (di->type == DISKIMAGE_FLOPPY && di->id == drive)
			break;
		di = di->next;
	}

	if (di == NULL || di->f == NULL || di->total_size <= 0 ||
	    di->heads <= 0 || di->sectors_per_track <= 0)
		return NULL;

	return di;
}


/*
 *  fdc_rate_ok():
 *
 *  The data rate (CCR/DSR bits 1:0: 0 = 500, 1 = 300, 2 = 250, 3 = 1000
 *  kbps) must match the medium, or the controller finds no ID fields.
 *  Drivers rely on this to detect the type of a disk.
 */
static int fdc_rate_ok(struct fdc82077_data *d, struct diskimage *di)
{
	int rate = d->ccr & 3;

	switch (di->sectors_per_track) {
	case 8:
	case 9:	 return rate == 2 || rate == 1;	/*  360/720 KB  */
	case 15:
	case 18: return rate == 0;		/*  1.2/1.44 MB  */
	case 36: return rate == 3;		/*  2.88 MB  */
	}
	return 1;
}


static void fdc_update_irq(struct fdc82077_data *d)
{
	if (d->irq_pending && (d->dor & DOR_DMAEN))
		INTERRUPT_ASSERT(d->irq);
	else
		INTERRUPT_DEASSERT(d->irq);
}


static void fdc_raise_irq(struct fdc82077_data *d)
{
	d->irq_pending = 1;
	fdc_update_irq(d);
}


static void fdc_lower_irq(struct fdc82077_data *d)
{
	d->irq_pending = 0;
	fdc_update_irq(d);
}


static void fdc_free_pio(struct fdc82077_data *d)
{
	free(d->pio_buf);
	d->pio_buf = NULL;
	d->pio_len = d->pio_pos = 0;
}


/*
 *  fdc_reset():
 *
 *  Controller reset (DOR bit 2 low, or DSR bit 7).  On leaving reset, the
 *  controller interrupts and has four "ready changed" status words for
 *  SENSE INTERRUPT STATUS.
 */
static void fdc_reset(struct fdc82077_data *d)
{
	int i;

	fdc_free_pio(d);
	d->phase = PHASE_COMMAND;
	d->cmd_pos = d->cmd_len = 0;
	d->res_pos = d->res_len = 0;
	d->n_sense = 0;
	d->ccr = d->dsr & 3;

	if (!d->lock) {
		d->config[0] = 0x20;	/*  FIFO disabled, threshold 0  */
		d->config[1] = 0x00;
		d->perp = 0;
	}

	fdc_lower_irq(d);
	for (i = 0; i < FDC_NDRIVES; i++) {
		d->sense_st0[i] = 0xc0 | i;
		d->sense_pcn[i] = d->pcn[i];
	}
	d->n_sense = FDC_NDRIVES;
}


static void fdc_end_reset(struct fdc82077_data *d)
{
	fdc_raise_irq(d);
}


/*  Number of bytes in each command, indexed by the low 5 bits:  */
static int fdc_command_length(uint8_t c)
{
	switch (c & 0x1f) {
	case 0x02: return 9;	/*  READ TRACK  */
	case 0x03: return 3;	/*  SPECIFY  */
	case 0x04: return 2;	/*  SENSE DRIVE STATUS  */
	case 0x05: return 9;	/*  WRITE DATA  */
	case 0x06: return 9;	/*  READ DATA  */
	case 0x07: return 2;	/*  RECALIBRATE  */
	case 0x08: return 1;	/*  SENSE INTERRUPT STATUS  */
	case 0x09: return 9;	/*  WRITE DELETED DATA  */
	case 0x0a: return 2;	/*  READ ID  */
	case 0x0c: return 9;	/*  READ DELETED DATA  */
	case 0x0d: return 6;	/*  FORMAT TRACK  */
	case 0x0e: return 1;	/*  DUMPREG  */
	case 0x0f: return 3;	/*  SEEK / RELATIVE SEEK  */
	case 0x10: return 1;	/*  VERSION  */
	case 0x11: return 9;	/*  SCAN EQUAL  */
	case 0x12: return 2;	/*  PERPENDICULAR MODE  */
	case 0x13: return 4;	/*  CONFIGURE  */
	case 0x14: return 1;	/*  LOCK  */
	case 0x16: return 9;	/*  VERIFY  */
	case 0x19: return 9;	/*  SCAN LOW OR EQUAL  */
	case 0x1d: return 9;	/*  SCAN HIGH OR EQUAL  */
	}
	return 1;		/*  invalid  */
}


static void fdc_result(struct fdc82077_data *d, const uint8_t *res, int len)
{
	memcpy(d->res, res, len);
	d->res_len = len;
	d->res_pos = 0;
	d->phase = PHASE_RESULT;
}


/*  Result for read/write type commands:  */
static void fdc_rw_result(struct fdc82077_data *d, uint8_t st0, uint8_t st1,
	uint8_t st2, uint8_t c, uint8_t h, uint8_t r, uint8_t n)
{
	uint8_t res[7];

	res[0] = st0; res[1] = st1; res[2] = st2;
	res[3] = c; res[4] = h; res[5] = r; res[6] = n;
	fdc_result(d, res, 7);
	fdc_raise_irq(d);
}


/*
 *  fdc_seek_done():
 *
 *  Completes a SEEK or RECALIBRATE.
 */
static void fdc_seek_done(struct fdc82077_data *d, int drive, int head,
	int newcyl, int recal)
{
	uint8_t st0 = ST0_SEEK_END | (head << 2) | drive;

	if (!fdc_drive_present(d, drive)) {
		/*  Track 0 never found:  */
		st0 |= ST0_ABNORMAL | ST0_EQUIP_CHECK;
		newcyl = recal ? 0 : newcyl;
	} else {
		/*  A step pulse with a disk in the drive clears DSKCHG:  */
		if (fdc_medium(d, drive) != NULL)
			d->dskchg[drive] = 0;
	}

	if (newcyl < 0)
		newcyl = 0;
	if (newcyl > 255)
		newcyl = 255;
	d->pcn[drive] = newcyl;

	/*  Replace any older status for this drive:  */
	d->n_sense = 1;
	d->sense_st0[0] = st0;
	d->sense_pcn[0] = d->pcn[drive];

	d->phase = PHASE_COMMAND;
	fdc_raise_irq(d);
}


/*
 *  fdc_dma():
 *
 *  Moves one sector over DMA.  Returns 1 when the sector was fully
 *  transferred; *tc is set when terminal count was reached.
 */
static int fdc_dma(struct fdc82077_data *d, unsigned char *buf, int len,
	int to_memory, int *tc)
{
	int n;

	*tc = 0;
	if (d->dma == NULL)
		return 0;

	n = isadma_transfer(d->dma, d->dma_channel, buf, len, to_memory, tc);
	return n == len;
}


/*
 *  fdc_rw():
 *
 *  READ DATA, WRITE DATA, VERIFY and READ TRACK.
 */
static void fdc_rw(struct fdc82077_data *d)
{
	uint8_t op = d->cmd[0] & 0x1f;
	int mt = d->cmd[0] & 0x80;
	int drive = d->cmd[1] & 3, head = (d->cmd[1] >> 2) & 1;
	uint8_t c = d->cmd[2], h = d->cmd[3], r = d->cmd[4], n = d->cmd[5];
	uint8_t eot = d->cmd[6];
	int writing = (op == 0x05 || op == 0x09);
	int verify = (op == 0x16);
	int pio = d->specify[1] & 1;
	struct diskimage *di;
	uint8_t st0, st1 = 0, st2 = 0;
	unsigned char sector[FDC_SECTOR_SIZE];
	int tc = 0, end_of_track = 0, error = 0;

	d->dor = (d->dor & ~DOR_DRIVE_MASK) | drive;
	st0 = (head << 2) | drive;

	debug("[ fdc: %s drive %i C=%i H=%i R=%i N=%i EOT=%i%s%s ]\n",
	    writing? "WRITE" : verify? "VERIFY" : "READ", drive, c, h, r, n,
	    eot, mt? " MT" : "", pio? " (non-DMA)" : "");

	/*  Implied seek:  */
	if (d->config[0] & 0x40)
		d->pcn[drive] = c;

	di = fdc_medium(d, drive);
	if (di == NULL || !fdc_drive_present(d, drive) ||
	    !fdc_rate_ok(d, di)) {
		fdc_rw_result(d, st0 | ST0_ABNORMAL, ST1_MA, 0, c, h, r, n);
		return;
	}

	if (writing && !di->writable) {
		fdc_rw_result(d, st0 | ST0_ABNORMAL, ST1_NW, 0, c, h, r, n);
		return;
	}

	if (pio) {
		fdc_free_pio(d);
		CHECK_ALLOCATION(d->pio_buf = (unsigned char *)
		    malloc(2 * 256 * FDC_SECTOR_SIZE));
	}

	for (;;) {
		off_t ofs;

		if (n != 2 || c != d->pcn[drive] || h != head || r < 1 ||
		    r > di->sectors_per_track || c >= di->cylinders ||
		    head >= di->heads) {
			st1 |= ST1_ND;
			if (c != d->pcn[drive])
				st2 |= ST2_WC;
			error = 1;
			break;
		}

		ofs = (((off_t)c * di->heads + head) * di->sectors_per_track
		    + (r - 1)) * FDC_SECTOR_SIZE;

		if (pio) {
			if (d->pio_len + FDC_SECTOR_SIZE >
			    2 * 256 * FDC_SECTOR_SIZE)
				break;
			if (!writing)
				diskimage_access(d->machine, drive,
				    DISKIMAGE_FLOPPY, 0, ofs,
				    d->pio_buf + d->pio_len, FDC_SECTOR_SIZE);
			d->pio_len += FDC_SECTOR_SIZE;
		} else if (writing) {
			if (!fdc_dma(d, sector, FDC_SECTOR_SIZE, 0, &tc)) {
				st1 |= ST1_OR;
				error = 1;
				break;
			}
			diskimage_access(d->machine, drive, DISKIMAGE_FLOPPY,
			    1, ofs, sector, FDC_SECTOR_SIZE);
		} else {
			diskimage_access(d->machine, drive, DISKIMAGE_FLOPPY,
			    0, ofs, sector, FDC_SECTOR_SIZE);
			if (!verify && !fdc_dma(d, sector, FDC_SECTOR_SIZE,
			    1, &tc)) {
				st1 |= ST1_OR;
				error = 1;
				break;
			}
		}

		/*  Next sector:  */
		if (r == eot) {
			r = 1;
			if (mt && head == 0) {
				head = h = 1;
			} else {
				if (mt)
					head = h = 0;
				c ++;
				end_of_track = 1;
			}
		} else
			r ++;

		if (tc || end_of_track)
			break;
	}

	if (pio && !error) {
		/*  Hand the execution phase over to the CPU:  */
		d->cmd[2] = c; d->cmd[3] = h; d->cmd[4] = r;
		d->phase = writing? PHASE_EXEC_PIO_IN : PHASE_EXEC_PIO_OUT;
		d->pio_pos = 0;
		fdc_raise_irq(d);
		return;
	}

	if (pio)
		fdc_free_pio(d);

	if (error)
		st0 |= ST0_ABNORMAL;
	else if (end_of_track && !tc && !verify) {
		/*  Ran off the end of the track without terminal count:  */
		st0 |= ST0_ABNORMAL;
		st1 |= ST1_EN;
	}

	fdc_rw_result(d, st0, st1, st2, c, h, r, n);
}


/*
 *  fdc_pio_done():
 *
 *  End of a non-DMA execution phase.
 */
static void fdc_pio_done(struct fdc82077_data *d)
{
	uint8_t op = d->cmd[0] & 0x1f;
	int drive = d->cmd[1] & 3, head = (d->cmd[1] >> 2) & 1;
	struct diskimage *di = fdc_medium(d, drive);

	if (d->phase == PHASE_EXEC_PIO_IN && di != NULL) {
		if (op == 0x0d) {
			/*  FORMAT: 4 bytes (C,H,R,N) per sector:  */
			unsigned char sector[FDC_SECTOR_SIZE];
			int i;
			memset(sector, d->cmd[5], sizeof(sector));
			for (i = 0; i + 4 <= d->pio_len; i += 4) {
				uint8_t r = d->pio_buf[i + 2];
				if (r < 1 || r > di->sectors_per_track)
					continue;
				diskimage_access(d->machine, drive,
				    DISKIMAGE_FLOPPY, 1, (((off_t)d->pcn[drive]
				    * di->heads + head) * di->sectors_per_track
				    + r - 1) * FDC_SECTOR_SIZE, sector,
				    FDC_SECTOR_SIZE);
			}
		} else {
			/*  WRITE DATA: from the start sector, in order:  */
			int c = d->pcn[drive], h = head;
			int r = d->cmd[8], i;	/*  start sector  */
			for (i = 0; i < d->pio_len; i += FDC_SECTOR_SIZE) {
				diskimage_access(d->machine, drive,
				    DISKIMAGE_FLOPPY, 1, (((off_t)c * di->heads
				    + h) * di->sectors_per_track + r - 1) *
				    FDC_SECTOR_SIZE, d->pio_buf + i,
				    FDC_SECTOR_SIZE);
				if (++r > di->sectors_per_track) {
					r = 1;
					if (h == 0 && (d->cmd[0] & 0x80))
						h = 1;
					else
						break;
				}
			}
		}
	}

	fdc_free_pio(d);

	/*
	 *  Without DMA there is no terminal count, so a non-DMA read or
	 *  write always runs to EOT and ends with "abnormal termination,
	 *  end of cylinder", which drivers treat as success.
	 */
	if (op == 0x0d)
		fdc_rw_result(d, (head << 2) | drive, 0, 0, d->cmd[2],
		    d->cmd[3], d->cmd[4], d->cmd[5]);
	else
		fdc_rw_result(d, ST0_ABNORMAL | (head << 2) | drive, ST1_EN,
		    0, d->cmd[2], d->cmd[3], d->cmd[4], d->cmd[5]);
}


/*
 *  fdc_format():
 *
 *  FORMAT TRACK: the sector IDs (C,H,R,N) come over DMA, four bytes per
 *  sector.  Each sector is filled with the data pattern byte.
 */
static void fdc_format(struct fdc82077_data *d)
{
	int drive = d->cmd[1] & 3, head = (d->cmd[1] >> 2) & 1;
	uint8_t n = d->cmd[2], sc = d->cmd[3], fill = d->cmd[5];
	struct diskimage *di = fdc_medium(d, drive);
	unsigned char sector[FDC_SECTOR_SIZE], id[4] = { 0, 0, 0, 0 };
	uint8_t st0 = (head << 2) | drive;
	int i, tc;

	d->dor = (d->dor & ~DOR_DRIVE_MASK) | drive;

	debug("[ fdc: FORMAT drive %i cyl %i head %i, %i sectors ]\n",
	    drive, d->pcn[drive], head, sc);

	if (di == NULL || !fdc_rate_ok(d, di)) {
		fdc_rw_result(d, st0 | ST0_ABNORMAL, ST1_MA, 0, 0, 0, 0, n);
		return;
	}
	if (!di->writable) {
		fdc_rw_result(d, st0 | ST0_ABNORMAL, ST1_NW, 0, 0, 0, 0, n);
		return;
	}

	if (d->specify[1] & 1) {
		/*  The non-DMA format reuses the PIO write path:  */
		fdc_free_pio(d);
		d->pio_len = 4 * sc;
		CHECK_ALLOCATION(d->pio_buf = (unsigned char *)
		    malloc(d->pio_len + 1));
		d->pio_pos = 0;
		d->cmd[2] = d->pcn[drive];
		d->cmd[3] = head;
		d->cmd[4] = sc;
		d->cmd[5] = fill;
		d->phase = PHASE_EXEC_PIO_IN;
		fdc_raise_irq(d);
		return;
	}

	memset(sector, fill, sizeof(sector));
	for (i = 0; i < sc; i++) {
		if (!fdc_dma(d, id, 4, 0, &tc)) {
			fdc_rw_result(d, st0 | ST0_ABNORMAL, ST1_OR, 0,
			    id[0], id[1], id[2], n);
			return;
		}
		if (id[2] >= 1 && id[2] <= di->sectors_per_track &&
		    d->pcn[drive] < di->cylinders && head < di->heads)
			diskimage_access(d->machine, drive, DISKIMAGE_FLOPPY,
			    1, (((off_t)d->pcn[drive] * di->heads + head) *
			    di->sectors_per_track + id[2] - 1) *
			    FDC_SECTOR_SIZE, sector, FDC_SECTOR_SIZE);
		if (tc)
			break;
	}

	fdc_rw_result(d, st0, 0, 0, id[0], id[1], id[2], n);
}


/*
 *  fdc_execute():
 *
 *  Called when all command bytes have been received.
 */
static void fdc_execute(struct fdc82077_data *d)
{
	uint8_t op = d->cmd[0] & 0x1f, res[10];
	int drive = d->cmd[1] & 3, head = (d->cmd[1] >> 2) & 1;
	struct diskimage *di;

	switch (op) {

	case 0x03:	/*  SPECIFY  */
		d->specify[0] = d->cmd[1];
		d->specify[1] = d->cmd[2];
		d->phase = PHASE_COMMAND;
		break;

	case 0x04:	/*  SENSE DRIVE STATUS  */
		di = fdc_medium(d, drive);
		res[0] = (head << 2) | drive;
		if (fdc_drive_present(d, drive)) {
			res[0] |= ST3_RDY | ST3_TS;
			if (d->pcn[drive] == 0)
				res[0] |= ST3_T0;
			if (di == NULL || !di->writable)
				res[0] |= ST3_WP;
		}
		fdc_result(d, res, 1);
		break;

	case 0x07:	/*  RECALIBRATE  */
		d->dor = (d->dor & ~DOR_DRIVE_MASK) | drive;
		debug("[ fdc: RECALIBRATE drive %i ]\n", drive);
		fdc_seek_done(d, drive, 0, 0, 1);
		break;

	case 0x08:	/*  SENSE INTERRUPT STATUS  */
		fdc_lower_irq(d);
		if (d->n_sense == 0) {
			res[0] = ST0_INVALID;
			fdc_result(d, res, 1);
			break;
		}
		res[0] = d->sense_st0[0];
		res[1] = d->sense_pcn[0];
		d->n_sense --;
		memmove(d->sense_st0, d->sense_st0 + 1, d->n_sense);
		memmove(d->sense_pcn, d->sense_pcn + 1, d->n_sense);
		fdc_result(d, res, 2);
		break;

	case 0x0a:	/*  READ ID  */
		d->dor = (d->dor & ~DOR_DRIVE_MASK) | drive;
		di = fdc_medium(d, drive);
		if (di == NULL || head >= di->heads || !fdc_rate_ok(d, di))
			fdc_rw_result(d, ST0_ABNORMAL | (head << 2) | drive,
			    ST1_MA, 0, d->pcn[drive], head, 1, 2);
		else
			fdc_rw_result(d, (head << 2) | drive, 0, 0,
			    d->pcn[drive], head, 1, 2);
		break;

	case 0x0d:	/*  FORMAT TRACK  */
		fdc_format(d);
		break;

	case 0x0e:	/*  DUMPREG  */
		res[0] = d->pcn[0]; res[1] = d->pcn[1];
		res[2] = d->pcn[2]; res[3] = d->pcn[3];
		res[4] = d->specify[0]; res[5] = d->specify[1];
		res[6] = 0;	/*  SC/EOT  */
		res[7] = (d->lock ? 0x80 : 0) | (d->perp & 0x7f);
		res[8] = d->config[0];
		res[9] = d->config[1];
		fdc_result(d, res, 10);
		break;

	case 0x0f:	/*  SEEK / RELATIVE SEEK  */
		d->dor = (d->dor & ~DOR_DRIVE_MASK) | drive;
		if (d->cmd[0] & 0x80) {
			int ncn = d->pcn[drive];
			if (d->cmd[0] & 0x40)
				ncn += d->cmd[2];
			else
				ncn -= d->cmd[2];
			fdc_seek_done(d, drive, head, ncn, 0);
		} else
			fdc_seek_done(d, drive, head, d->cmd[2], 0);
		debug("[ fdc: SEEK drive %i to cyl %i ]\n", drive,
		    d->pcn[drive]);
		break;

	case 0x10:	/*  VERSION  */
		res[0] = 0x90;	/*  82077AA  */
		fdc_result(d, res, 1);
		break;

	case 0x12:	/*  PERPENDICULAR MODE  */
		d->perp = d->cmd[1];
		d->phase = PHASE_COMMAND;
		break;

	case 0x13:	/*  CONFIGURE  */
		d->config[0] = d->cmd[2];
		d->config[1] = d->cmd[3];
		d->phase = PHASE_COMMAND;
		break;

	case 0x14:	/*  LOCK  */
		d->lock = (d->cmd[0] & 0x80) ? 1 : 0;
		res[0] = d->lock << 4;
		fdc_result(d, res, 1);
		break;

	case 0x02:	/*  READ TRACK  */
	case 0x05:	/*  WRITE DATA  */
	case 0x06:	/*  READ DATA  */
	case 0x09:	/*  WRITE DELETED DATA  */
	case 0x0c:	/*  READ DELETED DATA  */
	case 0x16:	/*  VERIFY  */
		/*  Remember the start sector for non-DMA writes:  */
		d->cmd[8] = d->cmd[4];
		fdc_rw(d);
		break;

	default:
		fatal("[ fdc: unimplemented command 0x%02x ]\n", d->cmd[0]);
		res[0] = ST0_INVALID;
		fdc_result(d, res, 1);
	}
}


static uint8_t fdc_msr(struct fdc82077_data *d)
{
	if (d->in_reset)
		return 0;

	switch (d->phase) {
	case PHASE_EXEC_PIO_OUT:
		return MSR_RQM | MSR_DIO | MSR_NDMA | MSR_CB;
	case PHASE_EXEC_PIO_IN:
		return MSR_RQM | MSR_NDMA | MSR_CB;
	case PHASE_RESULT:
		return MSR_RQM | MSR_DIO | MSR_CB;
	default:
		return MSR_RQM | (d->cmd_pos > 0 ? MSR_CB : 0);
	}
}


static uint8_t fdc_read_fifo(struct fdc82077_data *d)
{
	uint8_t b = 0;

	switch (d->phase) {
	case PHASE_RESULT:
		/*  Reading the result clears the interrupt:  */
		if (d->res_pos == 0)
			fdc_lower_irq(d);
		b = d->res[d->res_pos++];
		if (d->res_pos >= d->res_len) {
			d->phase = PHASE_COMMAND;
			d->cmd_pos = 0;
		}
		break;
	case PHASE_EXEC_PIO_OUT:
		b = d->pio_buf[d->pio_pos++];
		if (d->pio_pos >= d->pio_len)
			fdc_pio_done(d);
		break;
	default:
		break;
	}

	return b;
}


static void fdc_write_fifo(struct fdc82077_data *d, uint8_t b)
{
	switch (d->phase) {
	case PHASE_COMMAND:
		if (d->cmd_pos == 0)
			d->cmd_len = fdc_command_length(b);
		d->cmd[d->cmd_pos++] = b;
		if (d->cmd_pos >= d->cmd_len) {
			d->cmd_pos = 0;
			fdc_execute(d);
		}
		break;
	case PHASE_EXEC_PIO_IN:
		d->pio_buf[d->pio_pos++] = b;
		if (d->pio_pos >= d->pio_len)
			fdc_pio_done(d);
		break;
	default:
		/*  Writing during the result phase: ignored.  */
		break;
	}
}


DEVICE_ACCESS(fdc82077)
{
	struct fdc82077_data *d = (struct fdc82077_data *) extra;
	uint8_t b = 0, idata = 0;

	if (writeflag == MEM_WRITE)
		idata = data[0];

	switch (relative_addr) {

	case 0:		/*  SRA (PS/2 mode only)  */
		if (writeflag == MEM_READ)
			b = (d->irq_pending ? 0x80 : 0) | 0x40;
		break;

	case 1:		/*  SRB (PS/2 mode only)  */
		if (writeflag == MEM_READ)
			b = 0xc0 | ((d->dor >> 4) & 3);
		break;

	case 2:		/*  DOR  */
		if (writeflag == MEM_READ)
			b = d->dor;
		else {
			uint8_t old = d->dor;
			d->dor = idata;
			if (!(idata & DOR_NRESET)) {
				if (!d->in_reset)
					fdc_reset(d);
				d->in_reset = 1;
			} else if (d->in_reset || !(old & DOR_NRESET)) {
				d->in_reset = 0;
				fdc_end_reset(d);
			}
			fdc_update_irq(d);
		}
		break;

	case 3:		/*  TDR  */
		if (writeflag == MEM_READ)
			b = d->tdr;
		else
			d->tdr = idata & 3;
		break;

	case 4:		/*  MSR (read) / DSR (write)  */
		if (writeflag == MEM_READ)
			b = fdc_msr(d);
		else {
			d->dsr = idata & 0x7f;
			d->ccr = idata & 3;
			if (idata & 0x80) {
				fdc_reset(d);
				fdc_end_reset(d);
			}
		}
		break;

	case 5:		/*  Data (FIFO)  */
		if (d->in_reset)
			break;
		if (writeflag == MEM_READ)
			b = fdc_read_fifo(d);
		else
			fdc_write_fifo(d, idata);
		break;

	case 7:		/*  DIR (read) / CCR (write)  */
		if (writeflag == MEM_READ) {
			int drive = d->dor & DOR_DRIVE_MASK;
			if (fdc_medium(d, drive) == NULL)
				d->dskchg[drive] = 1;
			b = d->dskchg[drive] ? 0x80 : 0x00;
		} else
			d->ccr = idata & 3;
		break;

	default:
		break;
	}

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, b);

	return 1;
}


DEVICE_ACCESS(fdc82077_dir)
{
	return dev_fdc82077_access(cpu, mem, relative_addr + 7, data, len,
	    writeflag, extra);
}


/*
 *  dev_fdc_init():
 *
 *  Registers an 82077AA at baseaddr (normally ISA port 0x3f0), using the
 *  given ISA DMA controller and channel (normally 2).  irq_path is the
 *  full interrupt path, e.g. "machine[0].jensen.isa.6".
 */
struct fdc82077_data *dev_fdc_init(struct machine *machine,
	struct memory *mem, uint64_t baseaddr, const char *irq_path,
	struct isadma_data *dma, int dma_channel)
{
	struct fdc82077_data *d;
	int i;

	CHECK_ALLOCATION(d = (struct fdc82077_data *) malloc(sizeof(struct fdc82077_data)));
	memset(d, 0, sizeof(struct fdc82077_data));

	d->machine = machine;
	d->dma = dma;
	d->dma_channel = dma_channel;
	d->dor = DOR_NRESET;
	d->config[0] = 0x20;

	for (i = 0; i < FDC_NDRIVES; i++)
		d->dskchg[i] = 1;

	INTERRUPT_CONNECT(irq_path, d->irq);

	/*  Port 0x3f6 belongs to an IDE controller, if any:  */
	memory_device_register(mem, "fdc", baseaddr, 6,
	    dev_fdc82077_access, d, DM_DEFAULT, NULL);
	memory_device_register(mem, "fdc_dir", baseaddr + 7, 1,
	    dev_fdc82077_dir_access, d, DM_DEFAULT, NULL);

	return d;
}
