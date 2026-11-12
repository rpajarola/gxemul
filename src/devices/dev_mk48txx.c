/*
 *  Copyright (C) 2006-2009  Anders Gavare.  All rights reserved.
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
 *  COMMENT: Mostek MK48T08 timekeeper RAM (8 KB battery-backed RAM and clock)
 *
 *  The clock registers are the top eight bytes; reading them is done with
 *  the R bit (frozen copy), setting them with the W bit. The guest's time is
 *  kept as an offset from the host's. With an NVRAM file, the RAM and the
 *  offset are kept there (8192 bytes, then the offset as text).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "device.h"
#include "devices.h"
#include "emul.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "thirdparty/mk48txxreg.h"


#define	MK48TXX_LEN		MK48T08_CLKSZ
#define	CLK			(MK48TXX_LEN - 8)	/*  control register  */

#define	BCD(x)		((((x) / 10) << 4) + ((x) % 10))
#define	FROM_BCD(x)	((((x) >> 4) & 0xf) * 10 + ((x) & 0xf))

struct mk48txx_data {
	uint8_t		ram[MK48TXX_LEN];
	int64_t		offset;		/*  guest time - host time  */
	char		*nvram_file;
	struct machine	*machine;
	time_t		boot_time;	/*  host time at power-up  */
};

static struct mk48txx_data *nvram_dev;


static void mk48txx_save(struct mk48txx_data *d)
{
	FILE *f;

	if (d->nvram_file == NULL)
		return;
	f = fopen(d->nvram_file, "wb");
	if (f == NULL)
		return;
	fwrite(d->ram, 1, MK48TXX_LEN, f);
	fprintf(f, "%lld\n", (long long) d->offset);
	fclose(f);
}

static void mk48txx_save_atexit(void)
{
	if (nvram_dev != NULL)
		mk48txx_save(nvram_dev);
}

static void mk48txx_load(struct mk48txx_data *d)
{
	FILE *f = fopen(d->nvram_file, "rb");
	long long off;

	if (f == NULL)
		return;
	if (fread(d->ram, 1, MK48TXX_LEN, f) == MK48TXX_LEN &&
	    fscanf(f, "%lld", &off) == 1)
		d->offset = off;
	fclose(f);
}


/*  The host's time, or with an MVME187, the time at power-up plus the
    board's emulated time:  */
static time_t host_now(struct mk48txx_data *d)
{
	if (d->machine->md.mvme187 != NULL &&
	    d->machine->machine_type == MACHINE_MVME88K)
		return d->boot_time + (time_t) (dev_mvme187_time_us(
		    d->machine->md.mvme187) / 1000000);
	return time(NULL);
}


/*  Copies the current time into the clock registers:  */
static void mk48txx_update_regs(struct mk48txx_data *d)
{
	time_t t = host_now(d) + d->offset;
	struct tm *tmp = gmtime(&t);

	d->ram[CLK + 1] = (d->ram[CLK + 1] & MK48TXX_SEC_STOP) |
	    BCD(tmp->tm_sec);
	d->ram[CLK + 2] = BCD(tmp->tm_min);
	d->ram[CLK + 3] = BCD(tmp->tm_hour);
	d->ram[CLK + 4] = (d->ram[CLK + 4] & 0xf8) | (tmp->tm_wday + 1);
	d->ram[CLK + 5] = BCD(tmp->tm_mday);
	d->ram[CLK + 6] = BCD(tmp->tm_mon + 1);
	d->ram[CLK + 7] = BCD(tmp->tm_year % 100);
}

/*  Sets the clock from the registers (end of a write):  */
static void mk48txx_set_time(struct mk48txx_data *d)
{
	struct tm tm;
	int year = FROM_BCD(d->ram[CLK + 7]);

	memset(&tm, 0, sizeof(tm));
	tm.tm_sec = FROM_BCD(d->ram[CLK + 1] & 0x7f);
	tm.tm_min = FROM_BCD(d->ram[CLK + 2] & 0x7f);
	tm.tm_hour = FROM_BCD(d->ram[CLK + 3] & 0x3f);
	tm.tm_mday = FROM_BCD(d->ram[CLK + 5] & 0x3f);
	tm.tm_mon = FROM_BCD(d->ram[CLK + 6] & 0x1f) - 1;
	/*  Two-digit years: 70-99 are 19xx, the others 20xx.  */
	tm.tm_year = year < 70 ? year + 100 : year;

	d->offset = (int64_t) timegm(&tm) - (int64_t) host_now(d);
	mk48txx_save(d);
}


DEVICE_ACCESS(mk48txx)
{
	struct mk48txx_data *d = (struct mk48txx_data *) extra;
	size_t i;

	if (d->machine->md.mvme187 != NULL)
		dev_mvme187_bbram_access(d->machine->md.mvme187);

	for (i = 0; i < len; i++) {
		uint32_t a = (relative_addr + i) & (MK48TXX_LEN - 1);

		if (writeflag == MEM_READ) {
			/*  Live clock registers unless R or W is set:  */
			if (a > CLK && !(d->ram[CLK] & (MK48TXX_CSR_READ |
			    MK48TXX_CSR_WRITE)))
				mk48txx_update_regs(d);
			data[i] = d->ram[a];
			continue;
		}

		if (a == CLK) {
			uint8_t old = d->ram[CLK], v = data[i];
			if ((v & MK48TXX_CSR_READ) && !(old & MK48TXX_CSR_READ))
				mk48txx_update_regs(d);
			if ((v & MK48TXX_CSR_WRITE) &&
			    !(old & MK48TXX_CSR_WRITE))
				mk48txx_update_regs(d);
			d->ram[CLK] = v;
			if (!(v & MK48TXX_CSR_WRITE) && (old & MK48TXX_CSR_WRITE))
				mk48txx_set_time(d);
		} else if (a > CLK) {
			/*  Clock registers: only while W is set.  */
			if (d->ram[CLK] & MK48TXX_CSR_WRITE)
				d->ram[a] = data[i];
			else if (a == CLK + 1)
				d->ram[a] = (d->ram[a] & 0x7f) |
				    (data[i] & MK48TXX_SEC_STOP);
		} else {
			if (d->ram[a] != data[i]) {
				d->ram[a] = data[i];
				if (d->nvram_file != NULL)
					mk48txx_save(d);
			}
		}
	}

	return 1;
}


DEVINIT(mk48txx)
{
	struct mk48txx_data *d;

	CHECK_ALLOCATION(d = (struct mk48txx_data *) malloc(sizeof(struct mk48txx_data)));
	memset(d, 0, sizeof(struct mk48txx_data));
	d->machine = devinit->machine;
	d->boot_time = time(NULL);

	if (devinit->machine->nvram_filename != NULL &&
	    devinit->machine->nvram_filename[0] && nvram_dev == NULL) {
		d->nvram_file = devinit->machine->nvram_filename;
		mk48txx_load(d);
		nvram_dev = d;
		atexit(mk48txx_save_atexit);
	}

	/*  The MVME187's board configuration, if blank:  */
	if (devinit->machine->machine_type == MACHINE_MVME88K &&
	    devinit->machine->machine_subtype == MACHINE_MVME88K_187)
		dev_mvme187_brdid_defaults(devinit->machine, d->ram);

	d->ram[CLK] = 0;
	mk48txx_update_regs(d);

	/*  The 8 KB repeat over a 64 KB window.  */
	memory_device_register(devinit->machine->memory, devinit->name,
	    devinit->addr, 0x10000, dev_mk48txx_access, (void *)d,
	    DM_DEFAULT, NULL);

	return 1;
}
