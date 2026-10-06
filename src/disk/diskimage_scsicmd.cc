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
 *  Disk image support: SCSI command emulation.
 *
 *  TODO:  There are LOTS of ugly magic values in this module. These should
 *         be replaced by proper defines.
 *
 *  TODO:  There's probably a bug in the (raw, non-SIMH) tape support:
 *         Let's say there are 10240 bytes left in a file, and 10240
 *         bytes are read. Then feof() is not true yet (?), so the next
 *         read will also return 10240 bytes (but all zeroes), and then after
 *         that return feof (which results in a filemark).  This is probably
 *         trivial to fix, but I don't feel like it right now.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cpu.h"
#include "diskimage.h"
#include "machine.h"
#include "misc.h"


static const char *diskimage_types[] = DISKIMAGE_TYPES;
static struct scsi_transfer *first_free_scsi_transfer_alloc = NULL;


/*
 *  scsi_transfer_alloc():
 *
 *  Allocates memory for a new scsi_transfer struct, and fills it with
 *  sane data (NULL pointers).
 *  The return value is a pointer to the new struct.  If allocation
 *  failed, the program exits.
 */
struct scsi_transfer *scsi_transfer_alloc(void)
{
	struct scsi_transfer *p;

	if (first_free_scsi_transfer_alloc != NULL) {
		p = first_free_scsi_transfer_alloc;
		first_free_scsi_transfer_alloc = p->next_free;
	} else {
		p = (struct scsi_transfer *) malloc(sizeof(struct scsi_transfer));
		if (p == NULL) {
			fprintf(stderr, "scsi_transfer_alloc(): out "
			    "of memory\n");
			exit(1);
		}
	}

	memset(p, 0, sizeof(struct scsi_transfer));

	return p;
}


/*
 *  scsi_transfer_free():
 *
 *  Frees the space used by a scsi_transfer struct.  All buffers refered
 *  to by the scsi_transfer struct are freed.
 */
void scsi_transfer_free(struct scsi_transfer *p)
{
	if (p == NULL) {
		fprintf(stderr, "scsi_transfer_free(): p == NULL\n");
		exit(1);
	}

	if (p->msg_out != NULL)
		free(p->msg_out);
	if (p->cmd != NULL)
		free(p->cmd);
	if (p->data_out != NULL)
		free(p->data_out);

	if (p->data_in != NULL)
		free(p->data_in);
	if (p->msg_in != NULL)
		free(p->msg_in);
	if (p->status != NULL)
		free(p->status);

	p->next_free = first_free_scsi_transfer_alloc;
	first_free_scsi_transfer_alloc = p;
}


/*
 *  scsi_transfer_allocbuf():
 *
 *  Helper function, used by diskimage_scsicommand(), and SCSI controller
 *  devices.  Example of usage:
 *
 *	scsi_transfer_allocbuf(&xferp->msg_in_len, &xferp->msg_in, 1);
 */
void scsi_transfer_allocbuf(size_t *lenp, unsigned char **pp, size_t want_len,
	int clearflag)
{
	unsigned char *p = (*pp);

	if (p != NULL) {
		printf("WARNING! scsi_transfer_allocbuf(): old pointer "
		    "was not NULL, freeing it now\n");
		free(p);
	}

	(*lenp) = want_len;
	if ((p = (unsigned char *) malloc(want_len)) == NULL) {
		fprintf(stderr, "scsi_transfer_allocbuf(): out of "
		    "memory trying to allocate %li bytes\n", (long)want_len);
		exit(1);
	}

	if (clearflag)
		memset(p, 0, want_len);

	(*pp) = p;
}


/**************************************************************************/


/*
 *  diskimage__return_default_status_and_message():
 *
 *  Set the status and msg_in parts of a scsi_transfer struct
 *  to default values (msg_in = 0x00, status = 0x00).
 */
static void diskimage__return_default_status_and_message(
	struct scsi_transfer *xferp)
{
	scsi_transfer_allocbuf(&xferp->status_len, &xferp->status, 1, 0);
	xferp->status[0] = 0x00;
	scsi_transfer_allocbuf(&xferp->msg_in_len, &xferp->msg_in, 1, 0);
	xferp->msg_in[0] = 0x00;
}


/*
 *  diskimage__switch_tape():
 *
 *  Used by the SPACE command.  (d is assumed to be non-NULL.)
 */
static void diskimage__switch_tape(struct diskimage *d)
{
	char tmpfname[1000];

	snprintf(tmpfname, sizeof(tmpfname), "%s.%i",
	    d->fname, d->tape_filenr);
	tmpfname[sizeof(tmpfname)-1] = '\0';

	if (d->f != NULL)
		fclose(d->f);

	d->f = fopen(tmpfname, d->writable? "r+" : "r");
	if (d->f == NULL) {
		fprintf(stderr, "[ diskimage__switch_tape(): could not "
		    "(re)open '%s' ]\n", tmpfname);
		/*  TODO: return error  */
	}
	d->tape_offset = 0;
}


/*
 *  SIMH .tap format tape images:
 *
 *  Each data record (tape block) is stored as a 32-bit little-endian
 *  length, the data (padded to an even number of bytes), and the length
 *  again. A length of zero is a filemark, 0xffffffff marks end of medium,
 *  and 0xfffffffe is an erase gap. The top 4 bits of a length are a class
 *  (0 = good data, 8 = bad data, 0xf = reserved marker, others = private).
 */
#define	SIMH_TAPE_MARK		0x00000000
#define	SIMH_TAPE_GAP		0xfffffffe
#define	SIMH_TAPE_EOM		0xffffffff
#define	SIMH_TAPE_LEN_MASK	0x0fffffff

#define	TAPE_REC_DATA		0
#define	TAPE_REC_FILEMARK	1
#define	TAPE_REC_EOM		2
#define	TAPE_REC_BOT		3

#define	SENSE_KEY_NO_SENSE	0x00
#define	SENSE_KEY_BLANK_CHECK	0x08
#define	SENSE_FLAG_FILEMARK	0x80
#define	SENSE_FLAG_EOM		0x40
#define	SENSE_FLAG_ILI		0x20


static int simh_tape_read_marker(struct diskimage *d, uint64_t ofs,
	uint32_t *markerp)
{
	unsigned char buf[4];

	if (d->f == NULL || fseeko(d->f, ofs, SEEK_SET) != 0 ||
	    fread(buf, 1, 4, d->f) != 4)
		return 0;

	*markerp = buf[0] + (buf[1] << 8) + (buf[2] << 16) +
	    ((uint32_t)buf[3] << 24);
	return 1;
}


/*
 *  simh_tape_forward():
 *
 *  Moves past the next record or filemark. For data records, the record
 *  length and the offset of the data in the image file are returned.
 *  At end of medium, the position is not changed.
 */
static int simh_tape_forward(struct diskimage *d, uint32_t *lenp,
	uint64_t *data_ofsp)
{
	uint32_t marker, len;
	int recclass;

	for (;;) {
		if (!simh_tape_read_marker(d, d->tape_offset, &marker) ||
		    marker == SIMH_TAPE_EOM)
			return TAPE_REC_EOM;

		if (marker == SIMH_TAPE_MARK) {
			d->tape_offset += 4;
			d->tape_filenr ++;
			return TAPE_REC_FILEMARK;
		}

		recclass = marker >> 28;
		if (recclass == 0xf) {
			/*  Erase gap or other reserved marker  */
			d->tape_offset += 4;
			continue;
		}

		len = marker & SIMH_TAPE_LEN_MASK;
		*lenp = len;
		*data_ofsp = d->tape_offset + 4;
		d->tape_offset += 8 + ((len + 1) & ~1);

		/*  Skip private records; return good and bad data.  */
		if (recclass == 0 || recclass == 8)
			return TAPE_REC_DATA;
	}
}


/*
 *  simh_tape_backward():
 *
 *  Moves backwards past the previous record or filemark.
 */
static int simh_tape_backward(struct diskimage *d)
{
	uint32_t marker, len;
	uint64_t reclen;
	int recclass;

	for (;;) {
		if (d->tape_offset < 4 || !simh_tape_read_marker(d,
		    d->tape_offset - 4, &marker)) {
			d->tape_offset = 0;
			return TAPE_REC_BOT;
		}

		if (marker == SIMH_TAPE_MARK) {
			d->tape_offset -= 4;
			d->tape_filenr --;
			return TAPE_REC_FILEMARK;
		}

		recclass = marker >> 28;
		if (recclass == 0xf) {
			d->tape_offset -= 4;
			continue;
		}

		len = marker & SIMH_TAPE_LEN_MASK;
		reclen = 8 + ((len + 1) & ~1);
		if (reclen > d->tape_offset) {
			d->tape_offset = 0;
			return TAPE_REC_BOT;
		}
		d->tape_offset -= reclen;

		if (recclass == 0 || recclass == 8)
			return TAPE_REC_DATA;
	}
}


/*
 *  tape_check_condition():
 *
 *  Returns CHECK CONDITION status, and remembers the sense data to return
 *  on the following REQUEST SENSE.
 */
static void tape_check_condition(struct diskimage *d,
	struct scsi_transfer *xferp, int key, int flags, int32_t info)
{
	xferp->status[0] = 0x02;	/*  CHECK CONDITION  */

	d->tape_sense_key = key;
	d->tape_sense_flags = flags;
	d->tape_sense_info_valid = 1;
	d->tape_sense_info = info;
}


/*
 *  diskimage__simh_tape_read():
 *
 *  READ from a SIMH-format tape. In variable block mode, exactly one
 *  record is read, no matter how short it is. In fixed block mode, each
 *  of the requested blocks is one record of logical_block_size bytes.
 */
static void diskimage__simh_tape_read(struct diskimage *d,
	struct scsi_transfer *xferp)
{
	int fixed = xferp->cmd[1] & 0x01, sili = xferp->cmd[1] & 0x02;
	uint32_t count = (xferp->cmd[2] << 16) + (xferp->cmd[3] << 8) +
	    xferp->cmd[4];
	uint32_t blocksize = fixed? d->logical_block_size : count;
	uint32_t nblocks = fixed? count : 1;
	uint32_t i, reclen, n;
	uint64_t data_ofs;
	size_t done = 0, got;
	int res;

	diskimage__return_default_status_and_message(xferp);

	debug(" READ tape, id=%i file=%i, cmd[1]=%02x count=%i ofs=%lli\n",
	    d->id, d->tape_filenr, xferp->cmd[1], (int)count,
	    (long long)d->tape_offset);

	/*  A transfer length of zero is not an error, and moves nothing.  */
	if (count == 0)
		return;

	scsi_transfer_allocbuf(&xferp->data_in_len, &xferp->data_in,
	    (size_t)nblocks * blocksize, 1);

	for (i = 0; i < nblocks; i++) {
		res = simh_tape_forward(d, &reclen, &data_ofs);

		if (res == TAPE_REC_FILEMARK) {
			tape_check_condition(d, xferp, SENSE_KEY_NO_SENSE,
			    SENSE_FLAG_FILEMARK, fixed? nblocks - i : count);
			break;
		}
		if (res != TAPE_REC_DATA) {
			tape_check_condition(d, xferp, SENSE_KEY_BLANK_CHECK,
			    0, fixed? nblocks - i : count);
			break;
		}

		n = reclen < blocksize? reclen : blocksize;
		got = 0;
		if (n > 0 && fseeko(d->f, data_ofs, SEEK_SET) == 0)
			got = fread(xferp->data_in + done, 1, n, d->f);
		if (got < n)
			fatal("[ tape id %i: short record data at ofs %lli ]\n",
			    d->id, (long long)data_ofs);
		done += n;

		if (reclen != blocksize) {
			/*  Incorrect length. The tape is still positioned
			    after the whole record.  */
			if (fixed)
				tape_check_condition(d, xferp,
				    SENSE_KEY_NO_SENSE, SENSE_FLAG_ILI,
				    nblocks - i);
			else if (reclen > blocksize || !sili)
				tape_check_condition(d, xferp,
				    SENSE_KEY_NO_SENSE, SENSE_FLAG_ILI,
				    (int32_t)(blocksize - reclen));
			break;
		}
	}

	/*  Only the data actually read from the tape is returned.  */
	xferp->data_in_len = done;
}


/*
 *  diskimage__simh_tape_space():
 *
 *  SPACE on a SIMH-format tape: over blocks (code 0), filemarks (code 1),
 *  or to end of data (code 3).
 */
static void diskimage__simh_tape_space(struct diskimage *d,
	struct scsi_transfer *xferp)
{
	int code = xferp->cmd[1] & 7;
	int32_t count = (xferp->cmd[2] << 16) + (xferp->cmd[3] << 8) +
	    xferp->cmd[4];
	int32_t todo, done = 0, sign = 1;
	uint32_t reclen;
	uint64_t data_ofs;
	int res = TAPE_REC_DATA;

	diskimage__return_default_status_and_message(xferp);

	/*  Negative counts space backwards:  */
	if (count & (1 << 23)) {
		count -= 1 << 24;
		sign = -1;
	}
	todo = count * sign;

	switch (code) {

	case 0:	/*  Blocks  */
	case 1:	/*  Filemarks  */
		while (done < todo) {
			res = sign > 0? simh_tape_forward(d, &reclen, &data_ofs)
			    : simh_tape_backward(d);

			if (res == (code == 0? TAPE_REC_DATA :
			    TAPE_REC_FILEMARK)) {
				done ++;
				continue;
			}

			if (res == TAPE_REC_FILEMARK) {
				/*  Filemark while spacing over blocks  */
				tape_check_condition(d, xferp,
				    SENSE_KEY_NO_SENSE, SENSE_FLAG_FILEMARK,
				    (todo - done) * sign);
				break;
			}
			if (res == TAPE_REC_EOM) {
				tape_check_condition(d, xferp,
				    SENSE_KEY_BLANK_CHECK, 0,
				    (todo - done) * sign);
				break;
			}
			if (res == TAPE_REC_BOT) {
				tape_check_condition(d, xferp,
				    SENSE_KEY_NO_SENSE, SENSE_FLAG_EOM,
				    (todo - done) * sign);
				break;
			}
		}
		break;

	case 3:	/*  End of data  */
		while (simh_tape_forward(d, &reclen, &data_ofs) != TAPE_REC_EOM)
			;
		break;

	default:
		fatal("[ diskimage: unimplemented SPACE type %i ]\n", code);
	}
}


/**************************************************************************/


/*
 *  diskimage_scsicommand():
 *
 *  Perform a SCSI command on a disk image.
 *
 *  The xferp points to a scsi_transfer struct, containing msg_out, command,
 *  and data_out coming from the SCSI controller device.  This function
 *  interprets the command, and (if necessary) creates responses in
 *  data_in, msg_in, and status.
 *
 *  Returns:
 *	2 if the command expects data from the DATA_OUT phase,
 *	1 if otherwise ok,
 *	0 on error.
 */
int diskimage_scsicommand(struct cpu *cpu, int id, int type,
	struct scsi_transfer *xferp)
{
	char namebuf[16];
	int retlen, i, q;
	uint64_t size;
	int64_t ofs;
	int pagecode;
	struct machine *machine = cpu->machine;
	struct diskimage *d;

	if (machine == NULL) {
		fatal("[ diskimage_scsicommand(): machine == NULL ]\n");
		return 0;
	}

	d = machine->first_diskimage;
	while (d != NULL) {
		if (d->type == type && d->id == id)
			break;
		d = d->next;
	}
	if (d == NULL) {
		fprintf(stderr, "[ diskimage_scsicommand(): %s "
		    " id %i not connected? ]\n", diskimage_types[type], id);
	}

	if (xferp->cmd == NULL) {
		fatal("[ diskimage_scsicommand(): cmd == NULL ]\n");
		return 0;
	}

	if (xferp->cmd_len < 1) {
		fatal("[ diskimage_scsicommand(): cmd_len == %i ]\n",
		    xferp->cmd_len);
		return 0;
	}

	debug("[ diskimage_scsicommand(id=%i) cmd=0x%02x: ",
	    id, xferp->cmd[0]);

#if 0
	fatal("[ diskimage_scsicommand(id=%i) cmd=0x%02x len=%i:",
	    id, xferp->cmd[0], xferp->cmd_len);
	for (i=0; i<xferp->cmd_len; i++)
		fatal(" %02x", xferp->cmd[i]);
	fatal("\n");
if (xferp->cmd_len > 7 && xferp->cmd[5] == 0x11)
	single_step = ENTER_SINGLE_STEPPING;
#endif

#if 0
{
	static FILE *f = NULL;
	if (f == NULL)
		f = fopen("scsi_log.txt", "w"); 
	if (f != NULL) {
		int i;
		fprintf(f, "id=%i cmd =", id);
		for (i=0; i<xferp->cmd_len; i++)
			fprintf(f, " %02x", xferp->cmd[i]);
		fprintf(f, "\n");
		fflush(f);
	}
}
#endif

	switch (xferp->cmd[0]) {

	case SCSICMD_TEST_UNIT_READY:
		debug("TEST_UNIT_READY");
		if (xferp->cmd_len != 6)
			debug(" (weird len=%i)", xferp->cmd_len);

		/*  TODO: bits 765 of buf[1] contains the LUN  */
		if (xferp->cmd[1] != 0x00)
			fatal("WARNING: TEST_UNIT_READY with cmd[1]=0x%02x"
			    " not yet implemented\n", (int)xferp->cmd[1]);

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICMD_INQUIRY:
		debug("INQUIRY");
		if (xferp->cmd_len != 6)
			debug(" (weird len=%i)", xferp->cmd_len);
		if (xferp->cmd[1] != 0x00) {
			debug("WARNING: INQUIRY with cmd[1]=0x%02x not yet "
			    "implemented\n", (int)xferp->cmd[1]);

			break;
		}

		/*  Return values:  */
		retlen = xferp->cmd[4];
		if (retlen < 36) {
			fatal("WARNING: SCSI inquiry len=%i, <36!\n", retlen);
			retlen = 36;
		}

		/*  Return data:  */
		scsi_transfer_allocbuf(&xferp->data_in_len, &xferp->data_in,
		    retlen, 1);
		xferp->data_in[0] = 0x00;  /*  0x00 = Direct-access disk  */
		xferp->data_in[1] = 0x00;  /*  0x00 = non-removable  */
		xferp->data_in[2] = 0x02;  /*  SCSI-2  */
#if 0
xferp->data_in[3] = 0x02;	/*  Response data format = SCSI-2  */
#endif
		xferp->data_in[4] = retlen - 4;	/*  Additional length  */
xferp->data_in[4] = 0x2c - 4;	/*  Additional length  */
		xferp->data_in[6] = 0x04;  /*  ACKREQQ  */
		xferp->data_in[7] = 0x60;  /*  WBus32, WBus16  */

		/*  These are padded with spaces:  */

		memcpy(xferp->data_in+8,  "GXemul  ", 8);
		if (diskimage_getname(cpu->machine, id,
		    type, namebuf, sizeof(namebuf))) {
			for (size_t j=0; j<sizeof(namebuf); j++) {
				if (namebuf[j] == 0) {
					for (; j<sizeof(namebuf); j++)
						namebuf[j] = ' ';
					break;
				}
			}
			
			memcpy(xferp->data_in+16, namebuf, 16);
		} else
			memcpy(xferp->data_in+16, "DISK            ", 16);
		memcpy(xferp->data_in+32, "0   ", 4);

		/*
		 *  Some Ultrix kernels want specific responses from
		 *  the drives.
		 */

		if (machine->machine_type == MACHINE_PMAX) {
			/*  DEC, RZ25 (rev 0900) = 832527 sectors  */
			/*  DEC, RZ58 (rev 2000) = 2698061 sectors  */
			memcpy(xferp->data_in+8,  "DEC     ", 8);
			memcpy(xferp->data_in+16, "RZ58     (C) DEC", 16);
			memcpy(xferp->data_in+32, "2000", 4);
		}

		/*  Some data is different for CD-ROM drives:  */
		if (d->is_a_cdrom) {
			xferp->data_in[0] = 0x05;  /*  0x05 = CD-ROM  */
			xferp->data_in[1] = 0x80;  /*  0x80 = removable  */
			/*  memcpy(xferp->data_in+16, "CD-ROM          ", 16);*/

			if (machine->machine_type == MACHINE_PMAX) {
				/*  SONY, CD-ROM:  */
				memcpy(xferp->data_in+8, "SONY    ", 8);
				memcpy(xferp->data_in+16,
				    "CD-ROM          ", 16);

				/*  ... or perhaps this:  */
				memcpy(xferp->data_in+8, "DEC     ", 8);
				memcpy(xferp->data_in+16,
				    "RRD42   (C) DEC ", 16);
				memcpy(xferp->data_in+32, "4.5d", 4);
			} else if (machine->machine_type == MACHINE_ARC) {
				/*  NEC, CD-ROM:  */
				memcpy(xferp->data_in+8, "NEC     ", 8);
				memcpy(xferp->data_in+16,
				    "CD-ROM CDR-210P ", 16);
				memcpy(xferp->data_in+32, "1.0 ", 4);
			}
		}

		/*  Data for tape devices:  */
		if (d->is_a_tape) {
			xferp->data_in[0] = 0x01;  /*  0x01 = tape  */
			xferp->data_in[1] = 0x80;  /*  0x80 = removable  */
			memcpy(xferp->data_in+16, "TAPE            ", 16);

			if (machine->machine_type == MACHINE_PMAX) {
				/*
				 *  TODO:  find out if these are correct.
				 *
				 *  The name might be TZK10, TSZ07, or TLZ04,
				 *  or something completely different.
				 */
				memcpy(xferp->data_in+8, "DEC     ", 8);
				memcpy(xferp->data_in+16,
				    "TK50     (C) DEC", 16);
				memcpy(xferp->data_in+32, "2000", 4);
			}
		}

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSIBLOCKCMD_READ_CAPACITY:
		debug("READ_CAPACITY");

		if (xferp->cmd_len != 10)
			fatal(" [ weird READ_CAPACITY len=%i, should be 10 ] ",
			    xferp->cmd_len);
		else {
			if (xferp->cmd[8] & 1) {
				/*  Partial Medium Indicator bit...  TODO  */
				fatal("WARNING: READ_CAPACITY with PMI bit"
				    " set not yet implemented\n");
			}
		}

		/*  Return data:  */
		scsi_transfer_allocbuf(&xferp->data_in_len, &xferp->data_in,
		    8, 1);

		diskimage_recalc_size(d);

		size = d->total_size / d->logical_block_size;
		if (d->total_size & (d->logical_block_size-1))
			size ++;

		xferp->data_in[0] = (size >> 24) & 255;
		xferp->data_in[1] = (size >> 16) & 255;
		xferp->data_in[2] = (size >> 8) & 255;
		xferp->data_in[3] = size & 255;

		xferp->data_in[4] = (d->logical_block_size >> 24) & 255;
		xferp->data_in[5] = (d->logical_block_size >> 16) & 255;
		xferp->data_in[6] = (d->logical_block_size >> 8) & 255;
		xferp->data_in[7] = d->logical_block_size & 255;

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICMD_MODE_SENSE:
	case SCSICMD_MODE_SENSE10:	
		debug("MODE_SENSE");
		q = 4; retlen = xferp->cmd[4];
		switch (xferp->cmd_len) {
		case 6:	break;
		case 10:q = 8;
			retlen = xferp->cmd[7] * 256 + xferp->cmd[8];
			break;
		default:fatal(" (unimplemented mode_sense len=%i)",
			    xferp->cmd_len);
		}

		/*
		 *  NOTE/TODO: This code doesn't handle too short retlens
		 *  very well. A quick hack around this is that I allocate
		 *  a bit too much memory, so that nothing is actually
		 *  written outside of xferp->data_in[].
		 */

		retlen += 100;		/*  Should be enough. (Ugly.)  */

		if ((xferp->cmd[2] & 0xc0) != 0)
			fatal("WARNING: mode sense, cmd[2] = 0x%02x\n",
			    xferp->cmd[2]);

		/*  Return data:  */
		scsi_transfer_allocbuf(&xferp->data_in_len,
		    &xferp->data_in, retlen, 1);

		xferp->data_in_len -= 100;	/*  Restore size.  */

		pagecode = xferp->cmd[2] & 0x3f;

		debug("[ MODE SENSE id %i, pagecode=%i ]\n", id, pagecode);

		/*  4 bytes of header for 6-byte command,
		    8 bytes of header for 10-byte command.  */
		xferp->data_in[0] = retlen;	/*  0: mode data length  */
		xferp->data_in[1] = d->is_a_cdrom? 0x05 : 0x00;
				/*  1: medium type  */
		xferp->data_in[2] = 0x00;	/*  device specific
						    parameter  */
		xferp->data_in[3] = 8 * 1;	/*  block descriptor
						    length: 1 page (?)  */

		xferp->data_in[q+0] = 0x00;	/*  density code  */
		xferp->data_in[q+1] = 0;	/*  nr of blocks, high  */
		xferp->data_in[q+2] = 0;	/*  nr of blocks, mid  */
		xferp->data_in[q+3] = 0;	/*  nr of blocks, low */
		xferp->data_in[q+4] = 0x00;	/*  reserved  */
		xferp->data_in[q+5] = (d->logical_block_size >> 16) & 255;
		xferp->data_in[q+6] = (d->logical_block_size >> 8) & 255;
		xferp->data_in[q+7] = d->logical_block_size & 255;
		q += 8;

		diskimage__return_default_status_and_message(xferp);

		/*  descriptors, 8 bytes (each)  */

		/*  page, n bytes (each)  */
		switch (pagecode) {
		case 0:
			/*  TODO: Nothing here?  */
			break;
		case 1:		/*  read-write error recovery page  */
			xferp->data_in[q + 0] = pagecode;
			xferp->data_in[q + 1] = 10;
			break;
		case 3:		/*  format device page  */
			xferp->data_in[q + 0] = pagecode;
			xferp->data_in[q + 1] = 22;

			/*  10,11 = sectors per track  */
			xferp->data_in[q + 10] = 0;
			xferp->data_in[q + 11] = d->sectors_per_track;

			/*  12,13 = physical sector size  */
			xferp->data_in[q + 12] =
			    (d->logical_block_size >> 8) & 255;
			xferp->data_in[q + 13] = d->logical_block_size & 255;
			break;
		case 4:		/*  rigid disk geometry page  */
			xferp->data_in[q + 0] = pagecode;
			xferp->data_in[q + 1] = 22;
			xferp->data_in[q + 2] = (d->ncyls >> 16) & 255;
			xferp->data_in[q + 3] = (d->ncyls >> 8) & 255;
			xferp->data_in[q + 4] = d->ncyls & 255;
			xferp->data_in[q + 5] = d->heads;

			xferp->data_in[q + 20] = (d->rpms >> 8) & 255;
			xferp->data_in[q + 21] = d->rpms & 255;
			break;
		case 5:		/*  flexible disk page  */
			xferp->data_in[q + 0] = pagecode;
			xferp->data_in[q + 1] = 0x1e;

			/*  2,3 = transfer rate  */
			xferp->data_in[q + 2] = ((5000) >> 8) & 255;
			xferp->data_in[q + 3] = (5000) & 255;

			xferp->data_in[q + 4] = d->heads;
			xferp->data_in[q + 5] = d->sectors_per_track;

			/*  6,7 = data bytes per sector  */
			xferp->data_in[q + 6] = (d->logical_block_size >> 8)
			    & 255;
			xferp->data_in[q + 7] = d->logical_block_size & 255;

			xferp->data_in[q + 8] = (d->ncyls >> 8) & 255;
			xferp->data_in[q + 9] = d->ncyls & 255;

			xferp->data_in[q + 28] = (d->rpms >> 8) & 255;
			xferp->data_in[q + 29] = d->rpms & 255;
			break;
		default:
			fatal("[ MODE_SENSE for page %i is not yet "
			    "implemented! ]\n", pagecode);
		}

		break;

	case SCSICMD_READ:
	case SCSICMD_READ_10:
		debug("READ");

		if (d->is_a_tape && d->tape_simh) {
			diskimage__simh_tape_read(d, xferp);
			break;
		}

		/*
		 *  For tape devices, read data at the current position.
		 *  For disk and CDROM devices, the command bytes contain
		 *  an offset telling us where to read from the device.
		 */

		if (d->is_a_tape) {
			/*  bits 7..5 of cmd[1] are the LUN bits... TODO  */

			size = (xferp->cmd[2] << 16) +
			       (xferp->cmd[3] <<  8) +
				xferp->cmd[4];

			/*  Bit 1 of cmd[1] is the SILI bit (TODO), and
			    bit 0 is the "use fixed length" bit.  */

			if (xferp->cmd[1] & 0x01) {
				/*  Fixed block length:  */
				size *= d->logical_block_size;
			}

			if (d->filemark) {
				/*  At end of file, switch to the next
				    automagically:  */
				d->tape_filenr ++;
				diskimage__switch_tape(d);

				d->filemark = 0;
			}

			ofs = d->tape_offset;

			fatal("[ READ tape, id=%i file=%i, cmd[1]=%02x size=%i"
			    ", ofs=%lli ]\n", id, d->tape_filenr,
			    xferp->cmd[1], (int)size, (long long)ofs);
		} else {
			if (xferp->cmd[0] == SCSICMD_READ) {
				if (xferp->cmd_len != 6)
					debug(" (weird len=%i)",
					    xferp->cmd_len);

				/*
				 *  bits 4..0 of cmd[1], and cmd[2] and cmd[3]
				 *  hold the logical block address.
				 *
				 *  cmd[4] holds the number of logical blocks
				 *  to transfer. (Special case if the value is
				 *  0, actually means 256.)
				 */
				ofs = ((xferp->cmd[1] & 0x1f) << 16) +
				      (xferp->cmd[2] << 8) + xferp->cmd[3];
				retlen = xferp->cmd[4];
				if (retlen == 0)
					retlen = 256;
			} else {
				if (xferp->cmd_len != 10)
					debug(" (weird len=%i)",
					    xferp->cmd_len);

				/*
				 *  cmd[2..5] hold the logical block address.
				 *  cmd[7..8] holds the number of logical
				 *  blocks to transfer. (NOTE: If the value is
				 *  0, this means 0, not 65536. :-)
				 */
				ofs = ((uint64_t)xferp->cmd[2] << 24) +
				    (xferp->cmd[3] << 16) + (xferp->cmd[4] << 8)
				    + xferp->cmd[5];
				retlen = (xferp->cmd[7] << 8) + xferp->cmd[8];
			}

			size = retlen * d->logical_block_size;
			ofs *= d->logical_block_size;
		}

		/*  Return data:  */
		scsi_transfer_allocbuf(&xferp->data_in_len, &xferp->data_in,
		    size, 0);

		debug(" READ  ofs=%lli size=%i\n", (long long)ofs, (int)size);

		diskimage__return_default_status_and_message(xferp);

		d->filemark = 0;

		/*
		 *  Failure? Then set check condition.
		 *  For tapes, error should only occur at the end of a file.
		 *
		 *  "If the logical unit encounters a filemark during
		 *   a READ command, CHECK CONDITION status shall be
		 *   returned and the filemark and valid bits shall be
		 *   set to one in the sense data. The sense key shall
		 *   be set to NO SENSE"..
		 */
		if (d->is_a_tape && d->f != NULL && feof(d->f)) {
			debug(" feof id=%i\n", id);
			xferp->status[0] = 0x02;	/*  CHECK CONDITION  */

			d->filemark = 1;
		} else
			diskimage__internal_access(d, 0, ofs,
			    xferp->data_in, size);

		if (d->is_a_tape && d->f != NULL)
			d->tape_offset = ftello(d->f);

		/*  TODO: other errors?  */
		break;

	case SCSICMD_WRITE:
	case SCSICMD_WRITE_10:
		debug("WRITE");

		/*  TODO: tape  */

		if (xferp->cmd[0] == SCSICMD_WRITE) {
			if (xferp->cmd_len != 6)
				debug(" (weird len=%i)", xferp->cmd_len);

			/*
			 *  bits 4..0 of cmd[1], and cmd[2] and cmd[3] hold the
			 *  logical block address.
			 *
			 *  cmd[4] holds the number of logical blocks to
			 *  transfer. (Special case if the value is 0, actually
			 *  means 256.)
			 */
			ofs = ((xferp->cmd[1] & 0x1f) << 16) +
			      (xferp->cmd[2] << 8) + xferp->cmd[3];
			retlen = xferp->cmd[4];
			if (retlen == 0)
				retlen = 256;
		} else {
			if (xferp->cmd_len != 10)
				debug(" (weird len=%i)", xferp->cmd_len);

			/*
			 *  cmd[2..5] hold the logical block address.
			 *  cmd[7..8] holds the number of logical blocks to
			 *  transfer. (NOTE: If the value is 0 this means 0,
			 *  not 65536.)
			 */
			ofs = ((uint64_t)xferp->cmd[2] << 24) +
			    (xferp->cmd[3] << 16) + (xferp->cmd[4] << 8) +
			    xferp->cmd[5];
			retlen = (xferp->cmd[7] << 8) + xferp->cmd[8];
		}

		size = retlen * d->logical_block_size;
		ofs *= d->logical_block_size;

		if (xferp->data_out_offset != size) {
			debug(", data_out == NULL, wanting %i bytes, \n\n",
			    (int)size);
			xferp->data_out_len = size;
			return 2;
		}

		debug(", data_out != NULL, OK :-)");

		debug("WRITE ofs=%i size=%i offset=%i\n", (int)ofs,
		    (int)size, (int)xferp->data_out_offset);

		diskimage__internal_access(d, 1, ofs,
		    xferp->data_out, size);

		/*  TODO: how about return code?  */

		/*  Is this really necessary?  */
		/*  fsync(fileno(d->f));  */

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICMD_SYNCHRONIZE_CACHE:
		debug("SYNCHRONIZE_CACHE");

		if (xferp->cmd_len != 10)
			debug(" (weird len=%i)", xferp->cmd_len);

		/*  TODO: actualy care about cmd[]  */
		fsync(fileno(d->f));

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICMD_START_STOP_UNIT:
		debug("START_STOP_UNIT");

		if (xferp->cmd_len != 6)
			debug(" (weird len=%i)", xferp->cmd_len);

		for (i=0; i<(ssize_t)xferp->cmd_len; i++)
			debug(" %02x", xferp->cmd[i]);

		/*  TODO: actualy care about cmd[]  */

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICMD_REQUEST_SENSE:
		debug("REQUEST_SENSE");

		retlen = xferp->cmd[4];

		/*  TODO: bits 765 of buf[1] contains the LUN  */
		if (xferp->cmd[1] != 0x00)
			fatal("WARNING: REQUEST_SENSE with cmd[1]=0x%02x not"
			    " yet implemented\n", (int)xferp->cmd[1]);

		if (retlen < 18) {
			fatal("WARNING: SCSI request sense len=%i, <18!\n",
			    (int)retlen);
			retlen = 18;
		}

		/*  Return data:  */
		scsi_transfer_allocbuf(&xferp->data_in_len, &xferp->data_in,
		    retlen, 1);

		xferp->data_in[0] = 0x80 + 0x70;/*  0x80 = valid,
						    0x70 = "current errors"  */
		xferp->data_in[2] = 0x00;	/*  SENSE KEY!  */

		if (d->filemark) {
			xferp->data_in[2] = 0x80;
		}

		if (d->is_a_tape && d->tape_simh) {
			xferp->data_in[2] = d->tape_sense_flags |
			    d->tape_sense_key;
			if (d->tape_sense_info_valid) {
				xferp->data_in[3] = d->tape_sense_info >> 24;
				xferp->data_in[4] = d->tape_sense_info >> 16;
				xferp->data_in[5] = d->tape_sense_info >> 8;
				xferp->data_in[6] = d->tape_sense_info;
			} else
				xferp->data_in[0] = 0x70;

			/*  Sense data is only reported once.  */
			d->tape_sense_key = SENSE_KEY_NO_SENSE;
			d->tape_sense_flags = 0;
			d->tape_sense_info_valid = 0;
			d->tape_sense_info = 0;
		}
		debug(": [2]=0x%02x ", xferp->data_in[2]);

		printf(" XXX(!) \n");

		/*  TODO  */
		xferp->data_in[7] = retlen - 7;	/*  additional sense length  */
		/*  TODO  */

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICMD_READ_BLOCK_LIMITS:
		debug("READ_BLOCK_LIMITS");

		retlen = 6;

		/*  TODO: bits 765 of buf[1] contains the LUN  */
		if (xferp->cmd[1] != 0x00)
			fatal("WARNING: READ_BLOCK_LIMITS with cmd[1]="
			    "0x%02x not yet implemented\n", (int)xferp->cmd[1]);

		/*  Return data:  */
		scsi_transfer_allocbuf(&xferp->data_in_len, &xferp->data_in,
		    retlen, 1);

		/*
		 *  data[0] is reserved, data[1..3] contain the maximum block
		 *  length limit, data[4..5] contain the minimum limit.
		 */

		{
			int max_limit = 32768;
			int min_limit = 128;

			xferp->data_in[1] = (max_limit >> 16) & 255;
			xferp->data_in[2] = (max_limit >>  8) & 255;
			xferp->data_in[3] =  max_limit        & 255;
			xferp->data_in[4] = (min_limit >>  8) & 255;
			xferp->data_in[5] =  min_limit        & 255;
		}

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICMD_REWIND:
		debug("REWIND");

		/*  TODO: bits 765 of buf[1] contains the LUN  */
		if ((xferp->cmd[1] & 0xe0) != 0x00)
			fatal("WARNING: REWIND with cmd[1]=0x%02x not yet "
			    "implemented\n", (int)xferp->cmd[1]);

		/*  Close and reopen.  */

		if (d->f != NULL)
			fclose(d->f);

		d->f = fopen(d->fname, d->writable? "r+" : "r");
		if (d->f == NULL) {
			fprintf(stderr, "[ diskimage: could not (re)open "
			    "'%s' ]\n", d->fname);
			/*  TODO: return error  */
		}

		d->tape_offset = 0;
		d->tape_filenr = 0;
		d->filemark = 0;
		d->tape_sense_key = SENSE_KEY_NO_SENSE;
		d->tape_sense_flags = 0;
		d->tape_sense_info_valid = 0;

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICMD_SPACE:
		debug("SPACE");

		/*  TODO: bits 765 of buf[1] contains the LUN  */
		if ((xferp->cmd[1] & 0xe0) != 0x00)
			fatal("WARNING: SPACE with cmd[1]=0x%02x not yet "
			    "implemented\n", (int)xferp->cmd[1]);

		/*
		 *  Bits 2..0 of buf[1] contain the 'code' which describes how
		 *  spacing should be done, and buf[2..4] contain the number of
		 *  operations.
		 */
		debug("[ SPACE: buf[] = %02x %02x %02x %02x %02x %02x ]\n",
		    xferp->cmd[0],
		    xferp->cmd[1],
		    xferp->cmd[2],
		    xferp->cmd[3],
		    xferp->cmd[4],
		    xferp->cmd[5]);

		if (d->is_a_tape && d->tape_simh) {
			diskimage__simh_tape_space(d, xferp);
			break;
		}

		switch (xferp->cmd[1] & 7) {
		case 1:	/*  Seek to a different file nr:  */
			{
				int diff = (xferp->cmd[2] << 16) +
				    (xferp->cmd[3] << 8) + xferp->cmd[4];

				/*  Negative seek offset:  */
				if (diff & (1 << 23))
					diff = - (16777216 - diff);

				d->tape_filenr += diff;
			}

			/*  At end of file, switch to the next tape file:  */
			if (d->filemark) {
				d->tape_filenr ++;
				d->filemark = 0;
			}

			debug("{ switching to tape file %i }", d->tape_filenr);
			diskimage__switch_tape(d);
			d->filemark = 0;
			break;
		default:
			fatal("[ diskimage.c: unimplemented SPACE type %i ]\n",
			    xferp->cmd[1] & 7);
		}

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICDROM_READ_SUBCHANNEL:
		/*
		 *  According to
		 *  http://mail-index.netbsd.org/port-i386/1997/03/03/0010.html:
		 *
		 *  "The READ_CD_CAPACITY, READ_SUBCHANNEL, and MODE_SELECT
		 *   commands have the same opcode in SCSI or ATAPI, but don't
		 *   have the same command structure"...
		 *
		 *  TODO: This still doesn't work. Hm.
		 */
		retlen = 48;

		debug("CDROM_READ_SUBCHANNEL/READ_CD_CAPACITY, cmd[1]=0x%02x",
		    xferp->cmd[1]);

		/*  Return data:  */
		scsi_transfer_allocbuf(&xferp->data_in_len,
		    &xferp->data_in, retlen, 1);

		diskimage_recalc_size(d);

		size = d->total_size / d->logical_block_size;
		if (d->total_size & (d->logical_block_size-1))
			size ++;

		xferp->data_in[0] = (size >> 24) & 255;
		xferp->data_in[1] = (size >> 16) & 255;
		xferp->data_in[2] = (size >> 8) & 255;
		xferp->data_in[3] = size & 255;

		xferp->data_in[4] = (d->logical_block_size >> 24) & 255;
		xferp->data_in[5] = (d->logical_block_size >> 16) & 255;
		xferp->data_in[6] = (d->logical_block_size >> 8) & 255;
		xferp->data_in[7] = d->logical_block_size & 255;

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICDROM_READ_TOC:
		debug("(CDROM_READ_TOC: ");
		debug("lun=%i msf=%i ",
		    xferp->cmd[1] >> 5, (xferp->cmd[1] >> 1) & 1);
		debug("starting_track=%i ", xferp->cmd[6]);
		retlen = xferp->cmd[7] * 256 + xferp->cmd[8];
		debug("allocation_len=%i)\n", retlen);

		/*  Return data:  */
		scsi_transfer_allocbuf(&xferp->data_in_len,
		    &xferp->data_in, retlen, 1);

		xferp->data_in[0] = 0;
		xferp->data_in[1] = 10;
		xferp->data_in[2] = 0;		/*  First track.  */
		xferp->data_in[3] = 0;		/*  Last track.  */

		/*  Track 0 data:  */
		xferp->data_in[4] = 0x00;	/*  Reserved.  */
		xferp->data_in[5] = 0x04;	/*  ADR + CTRL:
						    Data, not audio  */
		xferp->data_in[6] = 0x00;	/*  Track nr  */
		xferp->data_in[7] = 0x00;	/*  Reserved  */
		/*  8..11 = absolute CDROM address  */

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICDROM_READ_DISCINFO:
		/*  (Patch from H�vard Eidnes.)  */
		debug("CDROM_READ_DISCINFO, cmd[1]=0x%02x", xferp->cmd[1]);
		retlen = 34;
		scsi_transfer_allocbuf(&xferp->data_in_len,
		    &xferp->data_in, retlen, 1);

		/*  TODO: Make this more generic!  */
		xferp->data_in[0] = retlen-2;	/*  length of info, excl len  */
		xferp->data_in[1] = 0;	 /*  length of info-(len field), msb  */
		xferp->data_in[2] = 0xE; /*  11=complete ses., 10=fin disc  */
		xferp->data_in[3] = 0;   /*  First track on disc  */
		xferp->data_in[4] = 1;   /*  Number of sessions, lsb  */
		xferp->data_in[5] = 0;   /*  first_track_last_session_lsb  */
		xferp->data_in[6] = 0;   /*  last_track_last_session_lsb  */
		xferp->data_in[7] = 0x20;/*  various flags  */
		xferp->data_in[8] = 0;   /*  CD-ROM disc  */
		xferp->data_in[9] = 1;   /*  num sessions, msb  */
		xferp->data_in[10] = 0;  /*  first_track_last_session_msb  */
		xferp->data_in[11] = 0;  /*  last_track_last_session_msb  */

		/*  Lead-in data, for completed cd-rom:  */
		for (size_t j=16; j<=23; j++)
			xferp->data_in[j] = 0xff;

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICDROM_READ_TRACKINFO:
		/*  (Patch from H�vard Eidnes.)  */
		debug("CDROM_READ_TRACKINFO");
		retlen = 36;
		scsi_transfer_allocbuf(&xferp->data_in_len,
		    &xferp->data_in, retlen, 1);

		diskimage_recalc_size(d);

		size = d->total_size / d->logical_block_size;
		if (d->total_size & (d->logical_block_size-1))
			size ++;

		/*  TODO: Make more generic?  */
		/*  TODO: Don't use magic values.  */
		xferp->data_in[0] = retlen-2; /*  length of info, excl len  */
		xferp->data_in[1] = 0;	      /*  length of info, msb  */
		xferp->data_in[2] = 1;	      /*  track#, lsb  */
		xferp->data_in[3] = 1;	      /*  session#, lsb  */
		xferp->data_in[4] = 0;	      /*  reserved  */
		xferp->data_in[5] = 0x6;      /*  trk mode: unintr. data,
						  copyable  */
		xferp->data_in[6] = 0x81;     /*  trk info: RT + trk mode  */
		xferp->data_in[7] = 0x2;      /*  last rec=valid, next w=not
						  valid */
		/*
		 *  track start, next writable, free blcks,
		 *  blocking factor
		 */
		for(size_t j=8; j<=23; j++)
			xferp->data_in[j] = 0;

		/*  Track size:  */
		xferp->data_in[24] = (size >> 24) & 0xff;
		xferp->data_in[25] = (size >> 16) & 0xff;
		xferp->data_in[26] = (size >> 8) & 0xff;
		xferp->data_in[27] = size & 0xff;

		/*  Last recorded address, only for dvd; zero out the rest:  */
		for (size_t k=28; k<=35; k++)
			xferp->data_in[k] = 0;

		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICMD_MODE_SELECT:
		debug("[ SCSI MODE_SELECT: ");

		/*
		 *  TODO:
		 *
		 *  This is super-hardcoded for NetBSD's usage of mode_select
		 *  to set the size of CDROM sectors to 2048.
		 */

		if (xferp->data_out_offset == 0) {
			xferp->data_out_len = 12;	/*  TODO  */
			debug("data_out == NULL, wanting %i bytes ]\n",
			    (int)xferp->data_out_len);
			return 2;
		}

		debug("data_out!=NULL (OK), ");

		/*  TODO:  Care about cmd?  */

		/*  Set sector size to 2048:  */
		/*  00 05 00 08 00 03 ca 40 00 00 08 00  */
		if (xferp->data_out[0] == 0x00 &&
		    xferp->data_out[1] == 0x05 &&
		    xferp->data_out[2] == 0x00 &&
		    xferp->data_out[3] == 0x08) {
			d->logical_block_size =
			    (xferp->data_out[9] << 16) +
			    (xferp->data_out[10] << 8) +
			    xferp->data_out[11];
			debug("[ setting logical_block_size to %i ]\n",
			    d->logical_block_size);
		} else {
			int j;
			fatal("[ unknown MODE_SELECT: cmd =");
			for (j=0; j<(ssize_t)xferp->cmd_len; j++)
				fatal(" %02x", xferp->cmd[j]);
			fatal(", data_out =");
			for (j=0; j<(ssize_t)xferp->data_out_len; j++)
				fatal(" %02x", xferp->data_out[j]);
			fatal(" ]");
		}

		debug(" ]\n");
		diskimage__return_default_status_and_message(xferp);
		break;

	case SCSICMD_PREVENT_ALLOW_REMOVE:
		debug("[ SCSI 0x%02x Prevent/allow medium removal: "
		    "TODO ]\n", xferp->cmd[0]);

		diskimage__return_default_status_and_message(xferp);
		break;

	case 0xbd:
		fatal("[ SCSI 0x%02x (len %i), TODO: ", xferp->cmd[0],
		    xferp->cmd_len);
		for (i=0; i<(ssize_t)xferp->cmd_len; i++)
			fatal(" %02x", xferp->cmd[i]);
		fatal(" ]\n");

		/*
		 *  Used by Windows NT?
		 *
		 *  Not documented in http://www.danbbs.dk/~dino/
		 *		SCSI/SCSI2-D.html.
		 *  Google gave the answer "MECHANISM_STATUS" for ATAPI. Hm.
		 */

		if (xferp->cmd_len < 12) {
			fatal("WEIRD LEN?\n");
			retlen = 8;
		} else {
			retlen = xferp->cmd[8] * 256 + xferp->cmd[9];
		}

		/*  Return data:  */
		scsi_transfer_allocbuf(&xferp->data_in_len,
		    &xferp->data_in, retlen, 1);

		diskimage__return_default_status_and_message(xferp);

		break;

	default:
		fatal("[ UNIMPLEMENTED SCSI command 0x%02x, disk id=%i ]\n",
		    xferp->cmd[0], id);
		exit(1);
	}
	debug(" ]\n");

	return 1;
}


