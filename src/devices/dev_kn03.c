/*
 *  Copyright (C) 2003-2018  Anders Gavare.  All rights reserved.
 *  Copyright (C) 2026  Antoni Sawicki.  All rights reserved.
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
 *  COMMENT: DEC KN03 "3MAX+" system module (interrupts, memory registers)
 *
 *  Used in the DECstation 5000/240 and the DECsystem 5900 "3MAX+".  The
 *  devices of the system slot and the three TURBOchannel option slots
 *  interrupt through the IOASIC's system interrupt register, which
 *  interrupts the CPU on KN03_INT_ASIC (int 0); the RTC and memory errors
 *  have CPU interrupts of their own.  The memory error address, ECC check
 *  and syndrome, and system control and status registers are in the system
 *  slot too.  See include/thirdparty/dec_kn03.h for more info.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "device.h"
#include "devices.h"
#include "interrupt.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "thirdparty/dec_kn03.h"
#include "thirdparty/tc_ioasicreg.h"


struct kn03_data {
	struct dec_ioasic_data *dec_ioasic;

	uint32_t	erradr;		/*  Memory Error Address Register  */
	uint32_t	errsyn;		/*  ECC Check/Syndrome Register  */
	uint32_t	csr;		/*  System Control and Status Register  */

	struct interrupt irq;
};


/*
 *  kn03_interrupt_assert(), kn03_interrupt_deassert():
 *
 *  Called whenever an interrupt of the IOASIC's system interrupt register
 *  is asserted/deasserted.
 */
void kn03_interrupt_assert(struct interrupt *interrupt)
{
	struct kn03_data *d = (struct kn03_data *) interrupt->extra;
	struct dec_ioasic_data *r = (struct dec_ioasic_data *) d->dec_ioasic;
	r->intr |= interrupt->line;
	dec_ioasic_reassert(r);
}
void kn03_interrupt_deassert(struct interrupt *interrupt)
{
	struct kn03_data *d = (struct kn03_data *) interrupt->extra;
	struct dec_ioasic_data *r = (struct dec_ioasic_data *) d->dec_ioasic;
	r->intr &= ~interrupt->line;
	dec_ioasic_reassert(r);
}


/*
 *  The memory error address register holds the address of the first
 *  memory error or bus timeout until it is written, and the ECC check and
 *  syndrome register the ECC bits of that error.  (The emulator has no
 *  memory errors, so both stay zero.)
 */
DEVICE_ACCESS(kn03_erradr)
{
	struct kn03_data *d = (struct kn03_data *) extra;
	uint64_t odata = 0;

	if (writeflag == MEM_WRITE) {
		d->erradr = 0;
		d->errsyn = 0;
	} else
		odata = d->erradr;

	debug("[ kn03_erradr: %s ERRADR, data=0x%08x ]\n",
	    writeflag == MEM_WRITE ? "write to" : "read from",
	    (int)d->erradr);

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, odata);

	return 1;
}


DEVICE_ACCESS(kn03_errsyn)
{
	struct kn03_data *d = (struct kn03_data *) extra;
	uint64_t odata = 0;

	if (writeflag == MEM_READ)
		odata = d->errsyn;

	debug("[ kn03_errsyn: %s ERRSYN, data=0x%08x ]\n",
	    writeflag == MEM_WRITE ? "write to" : "read from",
	    (int)d->errsyn);

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, odata);

	return 1;
}


/*
 *  The system control and status register: the diagnostic LEDs, the
 *  memory bank stride, and the ECC mode.
 */
DEVICE_ACCESS(kn03_csr)
{
	struct kn03_data *d = (struct kn03_data *) extra;
	uint64_t idata = 0, odata = 0;

	if (writeflag == MEM_WRITE) {
		idata = memory_readmax64(cpu, data, len);
		d->csr = idata;
	} else
		odata = d->csr;

	debug("[ kn03_csr: %s CSR, data=0x%08x ]\n",
	    writeflag == MEM_WRITE ? "write to" : "read from",
	    (int)d->csr);

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, odata);

	return 1;
}


DEVINIT(kn03)
{
	struct kn03_data *d;
	int i;

	CHECK_ALLOCATION(d = (struct kn03_data *) malloc(sizeof(struct kn03_data)));
	memset(d, 0, sizeof(struct kn03_data));

	/*  Connect the KN03 to a specific MIPS CPU interrupt line:  */
	INTERRUPT_CONNECT(devinit->interrupt_path, d->irq);

	/*  Register the interrupts:  */
	for (i = 0; i < 32; i++) {
		struct interrupt templ;
		char tmpstr[300];
		snprintf(tmpstr, sizeof(tmpstr), "%s.kn03.0x%x",
		    devinit->interrupt_path, 1 << i);
		memset(&templ, 0, sizeof(templ));
		templ.line = 1 << i;
		templ.name = tmpstr;
		templ.extra = d;
		templ.interrupt_assert = kn03_interrupt_assert;
		templ.interrupt_deassert = kn03_interrupt_deassert;
		interrupt_handler_register(&templ);
	}

	memory_device_register(devinit->machine->memory, "kn03_erradr",
	    KN03_SYS_ERRADR, sizeof(uint32_t), dev_kn03_erradr_access, d,
	    DM_DEFAULT, NULL);

	memory_device_register(devinit->machine->memory, "kn03_errsyn",
	    KN03_SYS_ERRSYN, sizeof(uint32_t), dev_kn03_errsyn_access, d,
	    DM_DEFAULT, NULL);

	memory_device_register(devinit->machine->memory, "kn03_csr",
	    KN03_SYS_CSR, sizeof(uint32_t), dev_kn03_csr_access, d,
	    DM_DEFAULT, NULL);

	d->dec_ioasic = dev_dec_ioasic_init(devinit->machine->cpus[0],
	    devinit->machine->memory, KN03_SYS_ASIC, 0, &d->irq);

	/*
	 *  The status bits of the system interrupt register: the jumpers
	 *  for normal mode (not manufacturing mode) and the 3MAX+ model
	 *  (a 5000/240, not a 5900).  The NVRAM clear jumper is not set.
	 */
	d->dec_ioasic->intr = KN03_INTR_NRMOD_JUMPER | KN03_INTR_PROD_JUMPER;

	devinit->return_ptr = d->dec_ioasic;

	return 1;
}

