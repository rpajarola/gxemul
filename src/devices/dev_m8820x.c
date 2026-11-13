/*
 *  Copyright (C) 2007-2021  Anders Gavare.  All rights reserved.
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
 *  COMMENT: M88200/M88204 CMMU (Cache/Memory Management Unit)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "device.h"
#include "emul.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "thirdparty/m8820x.h"
#include "thirdparty/m8820x_pte.h"


uint32_t m8820x_probe(struct cpu *cpu, struct m8820x_cmmu *cmmu,
	uint32_t vaddr, int supervisor, uint32_t *paddr);


/*  Cache: 256 sets of 4 lines of 16 bytes (the 88204 has 1024 sets).  */
#define	N_SETS		256

struct m8820x_data {
	int		cpu_nr;
	int		cmmu_nr;

	/*  The cache data, tag and set status ports read and write this
	    (no cache is emulated; diagnostics only see their own data):  */
	uint32_t	cache_data[N_SETS][4][4];
	uint32_t	cache_tag[N_SETS][4];
	uint32_t	cache_status[N_SETS];
};


/*
 *  m8820x_command():
 *
 *  Handle M8820x commands written to the System Command Register.
 */
static void m8820x_command(struct cpu *cpu, struct m8820x_data *d)
{
	struct m8820x_cmmu *cmmu = cpu->cd.m88k.cmmu[d->cmmu_nr];
	uint32_t *regs = cmmu->reg;
	int cmd = regs[CMMU_SCR] & 0x3f;
	uint32_t sar = regs[CMMU_SAR];
	size_t i;
	uint32_t super, mask;

	/*  Cache flushes, copybacks and invalidates (no cache contents):  */
	if (cmd >= 0x10 && cmd <= 0x1f) {
		if ((cmd & 3) == 3 && (cmd & 0x0c) != 0x08)
			for (i = 0; i < N_SETS; i++)
				d->cache_status[i] |= 0x0000ff00;
		return;
	}

	/*  Probe user (0x20-0x23) or supervisor (0x24-0x27) address:  */
	if (cmd >= 0x20 && cmd <= 0x27) {
		uint32_t paddr;
		regs[CMMU_SSR] = m8820x_probe(cpu, cmmu, sar, cmd >= 0x24,
		    &paddr);
		if (regs[CMMU_SSR] & CMMU_SSR_V)
			regs[CMMU_SAR] = paddr;
		return;
	}

	/*  Invalidate user (0x30-0x33) or supervisor (0x34-0x37) PATC
	    entries: line (= page), page, segment, all.  */
	if (cmd >= 0x30 && cmd <= 0x37) {
		super = cmd >= 0x34 ? M8820X_PATC_SUPERVISOR_BIT : 0;
		switch (cmd & 3) {
		case 0: case 1:	mask = 0xfffff000; break;
		case 2:		mask = 0xffc00000; break;
		default:	mask = 0;
		}

		cpu->invalidate_translation_caches(cpu, 0, INVALIDATE_ALL);

		for (i=0; i<N_M88200_PATC_ENTRIES; i++) {
			uint32_t v = cmmu->patc_v_and_control[i];
			uint32_t p = cmmu->patc_p_and_supervisorbit[i];

			if (!(v & PG_V))
				continue;
			if ((p & M8820X_PATC_SUPERVISOR_BIT) != super)
				continue;
			if ((sar & mask) != (v & mask))
				continue;

			cmmu->patc_v_and_control[i] = v & ~PG_V;
		}
		return;
	}

	/*  Other codes are no-ops.  */
	debug("[ m8820x: command 0x%02x ignored ]\n", cmd);
}


DEVICE_ACCESS(m8820x)
{
	// NOTE:
	//	cpu is the struct cpu* of the processor doing the device access.
	//	c   is the struct cpu* where the M88200 CMMU is located.
	uint64_t idata = 0, odata = 0;
	struct m8820x_data *d = (struct m8820x_data *) extra;
	struct cpu* c = cpu->machine->cpus[d->cpu_nr];
	struct m8820x_cmmu *cmmu = c->cd.m88k.cmmu[d->cmmu_nr];
	uint32_t *regs = cmmu->reg;
	uint32_t *batc = cmmu->batc;
	int r = relative_addr / sizeof(uint32_t);
	int set = (regs[CMMU_SAR] >> 4) & (N_SETS - 1);

	/*  Only 32-bit accesses.  */
	if (len != 4 || (relative_addr & 3))
		return 0;

	if (writeflag == MEM_WRITE)
		idata = memory_readmax64(cpu, data, len);

	if (writeflag == MEM_READ)
		odata = regs[r];

	switch (r) {

	case CMMU_IDR:
		/*  The ID field is writable.  */
		if (writeflag == MEM_WRITE)
			regs[r] = (regs[r] & 0x00ffffff) | (idata & 0xff000000);
		break;

	case CMMU_SCR:
		if (writeflag == MEM_WRITE) {
			regs[r] = idata & 0x3f;
			m8820x_command(c, d);
		}
		break;

	case CMMU_SSR:
		if (writeflag == MEM_WRITE)
			regs[r] = idata & 0xc3df;
		break;

	case CMMU_SAR:
	case CMMU_PFAR:
		if (writeflag == MEM_WRITE)
			regs[r] = idata;
		break;

	case CMMU_SCTR:
		if (writeflag == MEM_WRITE)
			regs[r] = idata & 0xe000;
		break;

	case CMMU_PFSR:
		if (writeflag == MEM_WRITE)
			regs[r] = idata & 0x00070000;
		break;

	case CMMU_SAPR:
	case CMMU_UAPR:
		if (writeflag == MEM_WRITE) {
			regs[r] = idata & 0xfffff2c1;
			c->invalidate_translation_caches(c, 0, INVALIDATE_ALL);
		}
		break;

	case CMMU_BWP0:
	case CMMU_BWP1:
	case CMMU_BWP2:
	case CMMU_BWP3:
	case CMMU_BWP4:
	case CMMU_BWP5:
	case CMMU_BWP6:
	case CMMU_BWP7:
		if (writeflag == MEM_WRITE) {
			int i = r - CMMU_BWP0;
			regs[r] = idata;
			if (batc[i] != idata) {
				batc[i] = idata;
				c->invalidate_translation_caches(
				    c, 0, INVALIDATE_ALL);
			}
		}
		break;

	case CMMU_CDP0:
	case CMMU_CDP1:
	case CMMU_CDP2:
	case CMMU_CDP3: {
		/*  Line (port) and word (SAR bits 3-2):  */
		int line = r - CMMU_CDP0, word = (regs[CMMU_SAR] >> 2) & 3;
		if (writeflag == MEM_WRITE)
			d->cache_data[set][line][word] = idata;
		else
			odata = d->cache_data[set][line][word];
		break;
	}

	case CMMU_CTP0:
	case CMMU_CTP1:
	case CMMU_CTP2:
	case CMMU_CTP3:
		if (writeflag == MEM_WRITE)
			d->cache_tag[set][r - CMMU_CTP0] = idata & 0xfffff000;
		else
			odata = d->cache_tag[set][r - CMMU_CTP0];
		break;

	case CMMU_CSSP0:
		if (writeflag == MEM_WRITE)
			d->cache_status[set] = idata & 0x3ffff000;
		else
			odata = d->cache_status[set];
		break;

	default:
		/*  Unused register addresses read as zero.  */
		if (writeflag == MEM_WRITE)
			debug("[ m8820x: write to offset 0x%x: 0x%x ]\n",
			    (int) relative_addr, (int) idata);
		odata = 0;
	}

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, odata);

	return 1;
}


DEVINIT(m8820x)
{
	struct m8820x_data *d;
	int i;

	CHECK_ALLOCATION(d = (struct m8820x_data *) malloc(sizeof(struct m8820x_data)));
	memset(d, 0, sizeof(struct m8820x_data));

	// Hack: Use the addr2 field to select cpu and cmmu numbers.
	// cmmu nr 0 = instruction, 1 = data.
	d->cmmu_nr = devinit->addr2 & 1;
	d->cpu_nr = devinit->addr2 >> 1;

	/*  All lines invalid after reset:  */
	for (i = 0; i < N_SETS; i++)
		d->cache_status[i] = 0x0000ff00;

	memory_device_register(devinit->machine->memory, devinit->name,
	    devinit->addr, M8820X_LENGTH, dev_m8820x_access, (void *)d,
	    DM_DEFAULT, NULL);

	return 1;
}
