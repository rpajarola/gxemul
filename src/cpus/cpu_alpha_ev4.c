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
 *  21064 (EV4) privileged architecture: PALmode, the internal processor
 *  registers, the instruction and data translation buffers, exceptions and
 *  interrupts. This is what the real console firmware and its PALcode run
 *  on. (Without it, cpu_alpha_palcode.c emulates the OSF/1 PALcode calls.)
 *
 *  Reference: Alpha 21064 and 21064A Microprocessors Hardware Reference
 *  Manual (EC-Q9ZUC-TE), chapters 4 and 5.
 *
 *  The translation buffers are architectural state, kept here. gxemul's
 *  virtual-to-host translation tables only cache them: entries are added by
 *  ev4_translate() and removed when TB entries are invalidated or replaced,
 *  and when the current mode changes (the tables don't record which mode
 *  an entry is valid for). Instruction fetches use tagged addresses (see
 *  ALPHA_PALMODE_TAG in cpu_alpha.h), so that they have their own entries.
 *
 *  Not implemented: ASNs and the Icache (the EV4 has no ASNs in its TBs,
 *  and translations are kept coherent with memory anyway), performance
 *  counters, the serial ROM line beyond the bits themselves, cache and
 *  parity errors.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"


void alpha_pc_to_pointers(struct cpu *cpu);


/*****************************************************************************
 *
 *  Program counter tags
 */

/*  Is va sign extended from bit 42 (a valid virtual address)?  */
static int canonical(uint64_t va)
{
	uint64_t top = (uint64_t)((int64_t)va >> 42);
	return top == 0 || top == ~(uint64_t)0;
}

/*  Instruction fetches from a non-canonical native address get a tag of
    their own, which is never entered into the translation tables.  */
#define	BAD_ISTREAM_TAG		0xc000000000000000ULL
#define	TAG_MASK		0xe000000000000000ULL


/*
 *  alpha_pc_to_arch():
 *
 *  Convert a (tagged) dyntrans PC to the address software sees.
 */
uint64_t alpha_pc_to_arch(struct cpu *cpu, uint64_t pc)
{
	if (!cpu->cd.alpha.ev4_palcode)
		return pc;

	switch (pc >> 61) {
	case 1:	return pc & ~TAG_MASK;			/*  PALmode  */
	case 6:	return pc & ~TAG_MASK;			/*  bad VA  */
	default:return pc ^ ALPHA_ISTREAM_TAG;
	}
}


/*
 *  alpha_arch_to_pc():
 *
 *  Convert a jump target to a (tagged) dyntrans PC, in the current mode.
 */
uint64_t alpha_arch_to_pc(struct cpu *cpu, uint64_t addr)
{
	if (!cpu->cd.alpha.ev4_palcode)
		return addr;

	if (cpu->cd.alpha.palmode)
		return (addr & ALPHA_PA_MASK) | ALPHA_PALMODE_TAG;
	if (!canonical(addr))
		return (addr & ~TAG_MASK) | BAD_ISTREAM_TAG;
	return addr ^ ALPHA_ISTREAM_TAG;
}


/*****************************************************************************
 *
 *  Translation table maintenance
 */

static void vph_invalidate_all(struct cpu *cpu)
{
	cpu->invalidate_translation_caches(cpu, 0, INVALIDATE_ALL);
	cpu->cd.alpha.vph_modes = 0;
}

static void vph_invalidate_entry(struct cpu *cpu, struct ev4_tb_entry *e,
	int istream)
{
	uint64_t va;

	if (!e->valid)
		return;

	if (e->gh != 0) {
		/*  More than one page; not worth doing page by page.  */
		vph_invalidate_all(cpu);
		return;
	}

	va = e->vpn << ALPHA_PAGESHIFT;
	if (va & (1ULL << 42))
		va |= 0xfffff80000000000ULL;
	if (istream)
		va ^= ALPHA_ISTREAM_TAG;
	cpu->invalidate_translation_caches(cpu, va, INVALIDATE_VADDR);
}


/*
 *  Set the current mode (PS<CM>). Translation table entries made in another
 *  mode can't be used any more: they may allow accesses that this mode
 *  isn't allowed to make.
 */
static void ev4_set_mode(struct cpu *cpu, int cm)
{
	cm &= 3;
	if (cm == cpu->cd.alpha.cm)
		return;

	if (cpu->cd.alpha.vph_modes & ~(1 << cm))
		vph_invalidate_all(cpu);

	cpu->cd.alpha.cm = cm;
}


/*****************************************************************************
 *
 *  Translation buffers
 */

/*  Number of 8 KB pages covered by an entry with granularity hint gh:  */
#define	GH_PAGES_SHIFT(gh)	(3 * (gh))

static struct ev4_tb_entry *tb_lookup(struct ev4_tb_entry *tb, int n,
	uint64_t va)
{
	uint64_t vpn = (va >> ALPHA_PAGESHIFT) & ((1ULL << 30) - 1);
	int i;

	for (i = 0; i < n; i++) {
		int s = GH_PAGES_SHIFT(tb[i].gh);
		if (tb[i].valid && (tb[i].vpn >> s) == (vpn >> s))
			return &tb[i];
	}
	return NULL;
}

static uint64_t tb_paddr(struct ev4_tb_entry *e, uint64_t va)
{
	uint64_t mask = (1ULL << (ALPHA_PAGESHIFT +
	    GH_PAGES_SHIFT(e->gh))) - 1;
	return ((e->pfn << ALPHA_PAGESHIFT) & ~mask) | (va & mask);
}

static struct ev4_tb_entry *itb_lookup(struct cpu *cpu, uint64_t va)
{
	return tb_lookup(cpu->cd.alpha.itb,
	    EV4_N_ITB_SMALL + EV4_N_ITB_LARGE, va);
}

static struct ev4_tb_entry *dtb_lookup(struct cpu *cpu, uint64_t va)
{
	return tb_lookup(cpu->cd.alpha.dtb, EV4_N_DTB, va);
}

/*  Fill a TB entry from TB_TAG and a PTE (write format).  */
static void tb_fill(struct cpu *cpu, struct ev4_tb_entry *e, uint64_t pte,
	int gh, int istream)
{
	vph_invalidate_entry(cpu, e, istream);

	e->valid = 1;
	e->gh = gh;
	e->vpn = (cpu->cd.alpha.tb_tag >> ALPHA_PAGESHIFT) & ((1ULL << 30) - 1);
	e->pfn = (pte >> 32) & ((1ULL << 21) - 1);
	e->asm_bit = (pte & EV4_PTE_ASM) != 0;
	e->prot = pte & (istream? 0x0f00 : 0xff06);

	/*  The ITB has only two sizes: 8 KB, and 4 MB for GH = 3.  */
	if (istream && gh != 3)
		e->gh = 0;
}

/*  TB entries in the read format of ITB_PTE_TEMP and DTB_PTE_TEMP:  */
static uint64_t tb_read_format(struct ev4_tb_entry *e, int istream)
{
	uint64_t x = (e->pfn << 13) | ((uint64_t)e->asm_bit << 34);

	x |= ((e->prot >> 8) & 0xf) << 9;		/*  xRE  */
	if (!istream) {
		x |= ((e->prot >> 12) & 0xf) << 5;	/*  xWE  */
		if (e->prot & EV4_PTE_FOW)
			x |= 0x10;
		if (e->prot & EV4_PTE_FOR)
			x |= 0x08;
	}
	return x;
}

static void tb_invalidate(struct cpu *cpu, struct ev4_tb_entry *tb, int n,
	int istream, int only_non_asm)
{
	int i;
	for (i = 0; i < n; i++) {
		if (!tb[i].valid || (only_non_asm && tb[i].asm_bit))
			continue;
		vph_invalidate_entry(cpu, &tb[i], istream);
		tb[i].valid = 0;
	}
}

static void tb_invalidate_single(struct cpu *cpu, struct ev4_tb_entry *tb,
	int n, int istream, uint64_t va)
{
	struct ev4_tb_entry *e;

	while ((e = tb_lookup(tb, n, va)) != NULL) {
		vph_invalidate_entry(cpu, e, istream);
		e->valid = 0;
	}

	/*  The translation tables may also have the page through a superpage
	    mapping, or a stale entry:  */
	va &= ~(uint64_t)((1 << ALPHA_PAGESHIFT) - 1);
	cpu->invalidate_translation_caches(cpu, istream?
	    va ^ ALPHA_ISTREAM_TAG : va, INVALIDATE_VADDR);
}


/*****************************************************************************
 *
 *  Exceptions and interrupts
 */

/*
 *  ev4_exception():
 *
 *  Enter PALmode at the given entry point, with EXC_ADDR = exc_addr (which
 *  must include the PALmode bit). The caller then continues execution at
 *  cpu->pc (alpha_pc_to_pointers()), except when called during an
 *  instruction fetch translation.
 */
void ev4_exception(struct cpu *cpu, int entry, uint64_t exc_addr)
{
	cpu->cd.alpha.exc_addr = exc_addr;
	cpu->cd.alpha.palmode = 1;
	cpu->pc = ALPHA_PALMODE_TAG |
	    ((cpu->cd.alpha.pal_base & 0x3ffffc000ULL) + entry);

	debugmsg_cpu(cpu, SUBSYS_CPU, "exception", VERBOSITY_DEBUG,
	    "PAL entry 0x%04x, exc_addr 0x%016" PRIx64, entry, exc_addr);
}


/*  The address of the current instruction (cpu->pc), with the PAL bit:  */
static uint64_t current_exc_addr(struct cpu *cpu)
{
	return alpha_pc_to_arch(cpu, cpu->pc) | cpu->cd.alpha.palmode;
}


/*
 *  ev4_call_pal():
 *
 *  CALL_PAL. next_pc is the (tagged) address of the next instruction;
 *  cpu->pc is the CALL_PAL itself.
 */
void ev4_call_pal(struct cpu *cpu, uint32_t func, uint64_t next_pc)
{
	int entry;

	if (func <= 0x3f && cpu->cd.alpha.cm == 0)
		entry = EV4_ENTRY_CALL_PAL + (func << 6);
	else if (func >= 0x80 && func <= 0xbf)
		entry = EV4_ENTRY_CALL_PAL + 0x1000 + ((func & 0x3f) << 6);
	else {
		ev4_exception(cpu, EV4_ENTRY_OPCDEC, current_exc_addr(cpu));
		return;
	}

	ev4_exception(cpu, entry, alpha_pc_to_arch(cpu, next_pc) |
	    cpu->cd.alpha.palmode);
}


/*  Hardware interrupt requests (the HWR summary):  */
static int hw_request(struct cpu *cpu)
{
	uint32_t misc = cpu->cd.alpha.hier_misc;

	return (cpu->cd.alpha.irq_lines & cpu->cd.alpha.hier & 0x3f) ||
	    (cpu->cd.alpha.slr_pending && (misc & 1)) ||
	    (cpu->cd.alpha.crd_pending && (misc & 2));
}

static int sw_request(struct cpu *cpu)
{
	return (cpu->cd.alpha.sirr & cpu->cd.alpha.sier & 0xfffe) != 0;
}

/*  AST requests for the current mode and the less privileged ones:  */
static int ast_request(struct cpu *cpu)
{
	uint32_t modes = (0xf << cpu->cd.alpha.cm) & 0xf;

	return (cpu->cd.alpha.sier & 4) &&
	    (cpu->cd.alpha.astrr & cpu->cd.alpha.aster & modes);
}

int ev4_interrupt_pending(struct cpu *cpu)
{
	return hw_request(cpu) || sw_request(cpu) || ast_request(cpu);
}


/*
 *  ev4_check_interrupts():
 *
 *  Take a pending machine check or interrupt (interrupts are blocked in
 *  PALmode). Called between instructions, with cpu->pc being the next
 *  instruction. Continues at the exception handler if one is taken.
 */
void ev4_check_interrupts(struct cpu *cpu)
{
	if (cpu->cd.alpha.mchk_pending) {
		cpu->cd.alpha.mchk_pending = 0;
		/*  EXC_ADDR<1> set: the address is the correct restart
		    address (see EXC_ADDR in the HRM).  */
		ev4_exception(cpu, EV4_ENTRY_MCHK, current_exc_addr(cpu) | 2);
		alpha_pc_to_pointers(cpu);
		return;
	}

	if (cpu->cd.alpha.palmode || !ev4_interrupt_pending(cpu))
		return;

	ev4_exception(cpu, EV4_ENTRY_INTERRUPT, current_exc_addr(cpu));
	alpha_pc_to_pointers(cpu);
}


/*
 *  ev4_machine_check():
 *
 *  A memory access to a physical address where nothing responds. The
 *  access completes (reads return all ones); the machine check, if enabled
 *  in ABOX_CTL, is taken after the instruction.
 */
void ev4_machine_check(struct cpu *cpu, uint64_t paddr, int writeflag)
{
	debugmsg_cpu(cpu, SUBSYS_CPU, "mchk", VERBOSITY_INFO,
	    "%s nonexistent physical address 0x%09" PRIx64 " (pc 0x%016"
	    PRIx64 ")%s", writeflag? "write to" : "read from", paddr,
	    alpha_pc_to_arch(cpu, cpu->pc),
	    (cpu->cd.alpha.abox_ctl & EV4_ABOX_MCHK_EN)? "" :
	    " (machine checks disabled)");

	if (cpu->cd.alpha.biu_locked) {
		cpu->cd.alpha.biu_stat |= EV4_BIU_FATAL1;
	} else {
		/*  BIU_CMD<6:4>: 0 = read block, 7 = write block (rough).  */
		cpu->cd.alpha.biu_stat = (cpu->cd.alpha.biu_stat & ~0x7fULL) |
		    EV4_BIU_HERR | (writeflag? 0x70 : 0);
		cpu->cd.alpha.biu_addr = paddr & ALPHA_PA_MASK & ~0x1fULL;
		cpu->cd.alpha.biu_locked = 1;
	}

	if (cpu->cd.alpha.abox_ctl & EV4_ABOX_MCHK_EN)
		cpu->cd.alpha.mchk_pending = 1;
}


/*****************************************************************************
 *
 *  Address translation
 */

/*  Latch the VA and MM_CSR of a D-stream fault (unless locked).  */
static void dstream_fault(struct cpu *cpu, uint64_t va, int write,
	int bits)
{
	unsigned char ib[4];
	uint32_t iword = 0;

	if (cpu->cd.alpha.va_locked)
		return;

	if (cpu->memory_rw(cpu, cpu->mem, cpu->pc, ib, sizeof(ib),
	    MEM_READ, CACHE_INSTRUCTION | NO_EXCEPTIONS))
		iword = ib[0] | (ib[1] << 8) | (ib[2] << 16) |
		    ((uint32_t)ib[3] << 24);

	cpu->cd.alpha.va = va;
	cpu->cd.alpha.mm_csr = (bits | (write? EV4_MMCSR_WR : 0)) |
	    (((iword >> 21) & 31) << 4) | ((uint64_t)(iword >> 26) << 9);
	cpu->cd.alpha.va_locked = 1;
}


/*  Access mode and checks for the next D-stream translation (HW_LD/HW_ST
    can use ALT_MODE, and read with write check):  */
static int xlate_mode = -1;
static int xlate_rwc = 0;


/*
 *  ev4_translate():
 *
 *  Virtual to physical translation (cpu->translate_v2p) with real PALcode.
 *  Returns 0 if the access isn't possible (an exception has been taken,
 *  unless FLAG_NOEXCEPTIONS), 1 if read-only, 2 if writable.
 */
int ev4_translate(struct cpu *cpu, uint64_t vaddr, uint64_t *paddr,
	int flags)
{
	int instr = flags & FLAG_INSTR, write = flags & FLAG_WRITEFLAG;
	int noexc = flags & FLAG_NOEXCEPTIONS;
	int mode = cpu->cd.alpha.cm, entry;
	struct ev4_tb_entry *e;
	uint64_t va;

	if (instr) {
		switch (vaddr >> 61) {
		case 1:	/*  PALmode: physical.  */
			*paddr = vaddr & ALPHA_PA_MASK;
			return 1;
		case 6:	/*  Not a valid virtual address.  */
			if (!noexc)
				ev4_exception(cpu, EV4_ENTRY_ITB_ACV,
				    vaddr & ~TAG_MASK);
			return 0;
		}

		va = vaddr ^ ALPHA_ISTREAM_TAG;

		if ((cpu->cd.alpha.iccsr & EV4_ICCSR_MAP) && mode == 0 &&
		    ((va >> 41) & 3) == 2) {
			*paddr = va & ALPHA_PA_MASK;
			goto ok_ro;
		}

		e = itb_lookup(cpu, va);
		if (e == NULL) {
			if (!noexc)
				ev4_exception(cpu, EV4_ENTRY_ITB_MISS, va);
			return 0;
		}
		if (!(e->prot & (EV4_PTE_KRE << mode))) {
			if (!noexc)
				ev4_exception(cpu, EV4_ENTRY_ITB_ACV, va);
			return 0;
		}
		*paddr = tb_paddr(e, va);
		goto ok_ro;
	}

	/*  D-stream:  */
	va = vaddr;
	if (xlate_mode >= 0)
		mode = xlate_mode;

	/*  The emulator's own accesses (file loaders, the debugger) see
	    KSEG as the superpage, whether it is enabled or not:  */
	if (noexc && canonical(va) && ((va >> 41) & 3) == 2) {
		*paddr = va & ALPHA_PA_MASK;
		return 2;
	}

	if (!canonical(va)) {
		entry = EV4_ENTRY_D_FAULT;
		if (!noexc)
			dstream_fault(cpu, va, write, EV4_MMCSR_ACV);
		goto fault;
	}

	if (mode == 0) {
		if ((cpu->cd.alpha.abox_ctl & EV4_ABOX_SPE_2) &&
		    ((va >> 41) & 3) == 2) {
			*paddr = va & ALPHA_PA_MASK;
			goto ok_rw;
		}
		if ((cpu->cd.alpha.abox_ctl & EV4_ABOX_SPE_1) &&
		    ((va >> 30) & 0x1fff) == 0x1ffe) {
			*paddr = va & 0x3fffffffULL;
			goto ok_rw;
		}
	}

	e = dtb_lookup(cpu, va);
	if (e == NULL && noexc) {
		/*  The debugger, looking at code: what the ITB maps, and in
		    PALmode physical addresses (as the PALmode PC is).  */
		struct ev4_tb_entry *ie = itb_lookup(cpu, va);
		if (ie != NULL) {
			*paddr = tb_paddr(ie, va);
			return 1;
		}
		if (cpu->cd.alpha.palmode) {
			*paddr = va & ALPHA_PA_MASK;
			return 1;
		}
	}
	if (e == NULL) {
		entry = cpu->cd.alpha.palmode? EV4_ENTRY_DTB_MISS_PAL :
		    EV4_ENTRY_DTB_MISS;
		if (!noexc)
			dstream_fault(cpu, va, write, 0);
		goto fault;
	}

	{
		int bits = 0;
		int can_read = e->prot & (EV4_PTE_KRE << mode);
		int can_write = e->prot & (EV4_PTE_KWE << mode);

		if (write || xlate_rwc) {
			if (!can_write)
				bits = EV4_MMCSR_ACV;
			else if (e->prot & EV4_PTE_FOW)
				bits = EV4_MMCSR_FOW;
		}
		if (!write && !bits) {
			if (!can_read)
				bits = EV4_MMCSR_ACV;
			else if (e->prot & EV4_PTE_FOR)
				bits = EV4_MMCSR_FOR;
		}
		if (bits) {
			entry = EV4_ENTRY_D_FAULT;
			if (!noexc)
				dstream_fault(cpu, va, write, bits);
			goto fault;
		}

		*paddr = tb_paddr(e, va);
		if (!noexc)
			cpu->cd.alpha.vph_modes |= 1 << mode;

		/*  Writable in the translation tables only if a write
		    would succeed, and reads too:  */
		if (can_write && !(e->prot & EV4_PTE_FOW) &&
		    can_read && !(e->prot & EV4_PTE_FOR))
			return 2;
		return 1;
	}

ok_ro:
	if (!noexc)
		cpu->cd.alpha.vph_modes |= 1 << mode;
	return 1;

ok_rw:
	if (!noexc)
		cpu->cd.alpha.vph_modes |= 1 << mode;
	return 2;

fault:
	if (!noexc)
		ev4_exception(cpu, entry, current_exc_addr(cpu));
	return 0;
}


/*****************************************************************************
 *
 *  PALmode instructions
 */

/*  May HW_MFPR, HW_MTPR, HW_LD, HW_ST, HW_REI be executed?  */
int ev4_hw_allowed(struct cpu *cpu)
{
	return cpu->cd.alpha.palmode || ((cpu->cd.alpha.iccsr &
	    EV4_ICCSR_HWE) && cpu->cd.alpha.cm == 0);
}


/*  The request registers' and enable registers' common read formats:  */
static uint64_t read_irr(struct cpu *cpu)
{
	uint64_t x = 0;
	uint32_t hirr = cpu->cd.alpha.irq_lines & 0x3f;

	x |= (uint64_t)(cpu->cd.alpha.astrr & 0xf) << 29;
	x |= (uint64_t)(cpu->cd.alpha.sirr & 0xfffe) << 13;
	x |= (uint64_t)cpu->cd.alpha.slr_pending << 13;
	x |= (uint64_t)(hirr & 7) << 10;
	x |= (uint64_t)(hirr >> 3) << 5;
	x |= (uint64_t)cpu->cd.alpha.crd_pending << 4;
	if (ast_request(cpu))	x |= 8;
	if (sw_request(cpu))	x |= 4;
	if (hw_request(cpu))	x |= 2;
	return x;
}

static uint64_t read_ier(struct cpu *cpu)
{
	uint64_t x = 0;
	uint32_t hier = cpu->cd.alpha.hier & 0x3f;
	uint32_t misc = cpu->cd.alpha.hier_misc;

	x |= (uint64_t)(cpu->cd.alpha.aster & 0xf) << 29;
	x |= (uint64_t)(cpu->cd.alpha.sier & 0xfffe) << 13;
	x |= (uint64_t)(misc & 1) << 13;			/*  SLE  */
	x |= (uint64_t)(hier & 7) << 10;
	x |= (uint64_t)((misc >> 2) & 1) << 9;			/*  PC0  */
	x |= (uint64_t)((misc >> 3) & 1) << 8;			/*  PC1  */
	x |= (uint64_t)(hier >> 3) << 5;
	x |= (uint64_t)((misc >> 1) & 1) << 4;			/*  CRE  */
	return x;
}


/*  ICCSR is kept in its write format; this is the read format.  */
static uint64_t read_iccsr(struct cpu *cpu)
{
	uint64_t w = cpu->cd.alpha.iccsr;
	uint64_t r = (w >> 19) & 0x3ffffe000ULL;	/*  ASN .. PCMUX1  */

	r |= w & (3ULL << 44);				/*  PCE  */
	r |= ((w >> 8) & 0xf) << 9;			/*  PCMUX0  */
	r |= (w & 1) << 2;				/*  PC1  */
	r |= ((w >> 3) & 1) << 1;			/*  PC0  */
	return r;
}


uint64_t ev4_read_cc(struct cpu *cpu)
{
	uint32_t count = cpu->cd.alpha.cc_base;

	if (cpu->cd.alpha.cc_enabled)
		count += (uint32_t)(cpu->ninstrs + cpu->n_translated_instrs -
		    cpu->cd.alpha.cc_base_ninstrs);

	return (cpu->cd.alpha.cc_offset << 32) | count;
}

static void set_cc_counter(struct cpu *cpu, uint32_t count, int enabled)
{
	cpu->cd.alpha.cc_base = count;
	cpu->cd.alpha.cc_base_ninstrs = cpu->ninstrs + cpu->n_translated_instrs;
	cpu->cd.alpha.cc_enabled = enabled;
}


/*
 *  ev4_mfpr():
 *
 *  HW_MFPR. ipr is bits <7:0> of the instruction (PAL, ABX, IBX, index).
 */
uint64_t ev4_mfpr(struct cpu *cpu, int ipr)
{
	struct alpha_cpu *a = &cpu->cd.alpha;
	int index = ipr & 31;
	struct ev4_tb_entry *e;
	uint64_t x;

	if (ipr & EV4_IPR_PAL)
		return a->pal_temp[index];

	if (ipr & EV4_IPR_IBX) {
		switch (index) {
		case EV4_IPR_ITB_PTE:
			if (!a->palmode)
				return 0;
			if (a->tb_ctl_gh == 3) {
				e = &a->itb[EV4_N_ITB_SMALL + a->itb_next_large];
				a->itb_next_large = (a->itb_next_large + 1) %
				    EV4_N_ITB_LARGE;
			} else {
				e = &a->itb[a->itb_next_small];
				a->itb_next_small = (a->itb_next_small + 1) %
				    EV4_N_ITB_SMALL;
			}
			a->itb_pte_temp = tb_read_format(e, 1);
			return 0;
		case EV4_IPR_ICCSR:
			return read_iccsr(cpu);
		case EV4_IPR_ITB_PTE_TEMP:
			return a->palmode? a->itb_pte_temp : 0;
		case EV4_IPR_EXC_ADDR:
			return a->exc_addr;
		case EV4_IPR_SL_RCV:
			return (uint64_t)a->sl_rcv << 3;
		case EV4_IPR_PS:
			return ((uint64_t)(a->cm >> 1) << 34) |
			    ((uint64_t)(a->cm & 1) << 1);
		case EV4_IPR_EXC_SUM:
			/*  One bit of the register write mask per read, F31
			    first, then F30 .. F0, I31 .. I0:  */
			x = (a->exc_sum & 0x7f) << 2;
			x |= (a->exc_mask >> 63) << 33;
			a->exc_mask <<= 1;
			return x;
		case EV4_IPR_PAL_BASE:
			return a->pal_base;
		case EV4_IPR_HIRR:
		case EV4_IPR_SIRR:
		case EV4_IPR_ASTRR:
			return read_irr(cpu);
		case EV4_IPR_HIER:
		case EV4_IPR_SIER:
		case EV4_IPR_ASTER:
			return read_ier(cpu);
		}
		debugmsg_cpu(cpu, SUBSYS_CPU, "ipr", VERBOSITY_WARNING,
		    "hw_mfpr from unknown Ibox IPR %i", index);
		return 0;
	}

	if (ipr & EV4_IPR_ABX) {
		switch (index) {
		case EV4_IPR_DTB_PTE:
			e = &a->dtb[a->dtb_next];
			a->dtb_next = (a->dtb_next + 1) % EV4_N_DTB;
			a->dtb_pte_temp = tb_read_format(e, 0);
			return 0;
		case EV4_IPR_DTB_PTE_TEMP:
			return a->dtb_pte_temp;
		case EV4_IPR_MM_CSR:
			return a->mm_csr;
		case EV4_IPR_VA:
			a->va_locked = 0;
			return a->va;
		case EV4_IPR_BIU_ADDR:
			a->biu_locked = 0;
			return a->biu_addr;
		case EV4_IPR_BIU_STAT:
			return a->biu_stat;
		case EV4_IPR_DC_STAT:
			return 7;		/*  CHIP_ID: production 21064  */
		case EV4_IPR_FILL_ADDR:
			return a->fill_addr;
		case EV4_IPR_FILL_SYNDROME:
			return a->fill_syndrome;
		case EV4_IPR_BC_TAG:
			x = a->bc_tag & 1;
			a->bc_tag >>= 1;
			return x;
		case EV4_IPR_CC:
			return ev4_read_cc(cpu);
		}
		/*  The rest are write-only.  */
		return 0;
	}

	/*  PAL, ABX and IBX all clear: a NOP.  */
	return 0;
}


/*
 *  ev4_mtpr():
 *
 *  HW_MTPR.
 */
void ev4_mtpr(struct cpu *cpu, int ipr, uint64_t value, int unused)
{
	struct alpha_cpu *a = &cpu->cd.alpha;
	int index = ipr & 31, i;
	struct ev4_tb_entry *e;
	uint64_t old;

	if (ipr & EV4_IPR_PAL)
		a->pal_temp[index] = value;

	if (ipr & EV4_IPR_IBX) {
		switch (index) {
		case EV4_IPR_TB_TAG:
			if (a->palmode)
				a->tb_tag = value;
			break;
		case EV4_IPR_ITB_PTE:
			if (!a->palmode)
				break;
			if (a->tb_ctl_gh == 3) {
				e = &a->itb[EV4_N_ITB_SMALL + a->itb_next_large];
				a->itb_next_large = (a->itb_next_large + 1) %
				    EV4_N_ITB_LARGE;
			} else {
				e = &a->itb[a->itb_next_small];
				a->itb_next_small = (a->itb_next_small + 1) %
				    EV4_N_ITB_SMALL;
			}
			tb_fill(cpu, e, value, a->tb_ctl_gh, 1);
			a->itb_pte_temp = tb_read_format(e, 1);
			break;
		case EV4_IPR_ICCSR:
			old = a->iccsr;
			a->iccsr = value & 0x001fffff00000f09ULL;
			if ((old ^ a->iccsr) & EV4_ICCSR_MAP)
				vph_invalidate_all(cpu);
			break;
		case EV4_IPR_EXC_ADDR:
			a->exc_addr = value;
			break;
		case EV4_IPR_ITBZAP:
		case EV4_IPR_ITBIS:
			if (!a->palmode)
				break;
			tb_invalidate(cpu, a->itb, EV4_N_ITB_SMALL +
			    EV4_N_ITB_LARGE, 1, 0);
			a->itb_next_small = a->itb_next_large = 0;
			break;
		case EV4_IPR_ITBASM:
			if (a->palmode)
				tb_invalidate(cpu, a->itb, EV4_N_ITB_SMALL +
				    EV4_N_ITB_LARGE, 1, 1);
			break;
		case EV4_IPR_PS:
			ev4_set_mode(cpu, (value >> 3) & 3);
			break;
		case EV4_IPR_EXC_SUM:
			a->exc_sum = 0;
			a->swc_blocked = 0;
			break;
		case EV4_IPR_PAL_BASE:
			a->pal_base = value & 0x3ffffc000ULL;
			break;
		case EV4_IPR_SIRR:
			a->sirr = (value >> 32) & 0xfffe;
			break;
		case EV4_IPR_ASTRR:
			a->astrr = (value >> 48) & 0xf;
			break;
		case EV4_IPR_HIER:
			a->hier = (value >> 9) & 0x3f;
			a->hier_misc = ((value >> 32) & 1)	/*  SLE  */
			    | (((value >> 2) & 1) << 1)		/*  CRE  */
			    | (((value >> 8) & 1) << 2)		/*  PC0  */
			    | (((value >> 15) & 1) << 3);	/*  PC1  */
			break;
		case EV4_IPR_SIER:
			a->sier = (value >> 32) & 0xfffe;
			break;
		case EV4_IPR_ASTER:
			a->aster = (value >> 48) & 0xf;
			break;
		case EV4_IPR_SL_CLR:
			/*  Write zero to clear:  */
			if (!(value & (1ULL << 32)))
				a->slr_pending = 0;
			if (!(value & 4))
				a->crd_pending = 0;
			break;
		case EV4_IPR_SL_XMIT:
			a->sl_xmit = (value >> 4) & 1;
			break;
		default:
			debugmsg_cpu(cpu, SUBSYS_CPU, "ipr",
			    VERBOSITY_WARNING, "hw_mtpr to unknown Ibox"
			    " IPR %i", index);
		}
	}

	if (ipr & EV4_IPR_ABX) {
		switch (index) {
		case EV4_IPR_TB_CTL:
			a->tb_ctl_gh = (value >> 5) & 3;
			break;
		case EV4_IPR_DTB_PTE:
			e = &a->dtb[a->dtb_next];
			a->dtb_next = (a->dtb_next + 1) % EV4_N_DTB;
			tb_fill(cpu, e, value, a->tb_ctl_gh, 0);
			a->dtb_pte_temp = tb_read_format(e, 0);
			break;
		case EV4_IPR_DTBZAP:
			tb_invalidate(cpu, a->dtb, EV4_N_DTB, 0, 0);
			a->dtb_next = 0;
			/*  Superpage entries too:  */
			vph_invalidate_all(cpu);
			break;
		case EV4_IPR_DTBASM:
			tb_invalidate(cpu, a->dtb, EV4_N_DTB, 0, 1);
			break;
		case EV4_IPR_DTBIS:
			tb_invalidate_single(cpu, a->dtb, EV4_N_DTB, 0, value);
			break;
		case EV4_IPR_ABOX_CTL:
			old = a->abox_ctl;
			a->abox_ctl = value & 0xffff;
			if ((old ^ a->abox_ctl) &
			    (EV4_ABOX_SPE_1 | EV4_ABOX_SPE_2))
				vph_invalidate_all(cpu);
			break;
		case EV4_IPR_ALT_MODE:
			a->alt_mode = (value >> 3) & 3;
			break;
		case EV4_IPR_CC:
			a->cc_offset = value >> 32;
			break;
		case EV4_IPR_CC_CTL:
			set_cc_counter(cpu, value, (value >> 32) & 1);
			break;
		case EV4_IPR_BIU_CTL:
			a->biu_ctl = value;
			break;
		case EV4_IPR_BC_TAG:
			/*  Unlocks BC_TAG.  */
			break;
		case EV4_IPR_FLUSH_IC:
		case EV4_IPR_FLUSH_IC_ASM:
			/*  Translations are always coherent with memory.  */
			break;
		default:
			/*  Read-only registers: ignored.  */
			break;
		}
	}

	(void) i;
}


/*
 *  ev4_hw_rei():
 *
 *  HW_REI: continue at EXC_ADDR, in PALmode if EXC_ADDR<0> is set. Takes a
 *  pending interrupt right away when returning to native mode. Sets
 *  cpu->pc; the caller continues there.
 */
int ev4_hw_rei(struct cpu *cpu)
{
	uint64_t target = cpu->cd.alpha.exc_addr;

	cpu->cd.alpha.palmode = target & 1;
	cpu->pc = alpha_arch_to_pc(cpu, target & ~(uint64_t)3);

	if (!cpu->cd.alpha.palmode && ev4_interrupt_pending(cpu))
		ev4_exception(cpu, EV4_ENTRY_INTERRUPT,
		    alpha_pc_to_arch(cpu, cpu->pc));
	return 1;
}


/*
 *  ev4_hw_ldst():
 *
 *  HW_LD (opcode 0x1b) and HW_ST (0x1f). Returns 1 if done, 0 if an
 *  exception was taken (cpu->pc is then the handler).
 *
 *	<15> PHY	physical address
 *	<14> ALT	virtual: use ALT_MODE; physical: load-locked or
 *			store-conditional
 *	<13> RWC	virtual HW_LD: check write access too
 *	<12> QW		quadword (else longword)
 *	<11:0>		signed displacement
 */
int ev4_hw_ldst(struct cpu *cpu, uint32_t iword, uint64_t addr,
	uint64_t *reg)
{
	int store = (iword >> 26) == 0x1f;
	int phy = (iword >> 15) & 1, alt = (iword >> 14) & 1;
	int rwc = (iword >> 13) & 1, qw = (iword >> 12) & 1;
	int len = qw? 8 : 4, i, ok;
	unsigned char data[8];
	uint64_t x;

	addr &= ~(uint64_t)(qw? 7 : 3);

	/*  Physical addresses are 34 bits; the CPU ignores the rest.  */
	if (phy)
		addr &= ALPHA_PA_MASK;

	if (store) {
		if (phy && alt) {
			/*  Store conditional:  */
			if (!cpu->cd.alpha.ll_flag) {
				*reg = 0;
				return 1;
			}
			for (i = 0; i < cpu->machine->ncpus; i++)
				cpu->machine->cpus[i]->cd.alpha.ll_flag = 0;
		}
		x = *reg;
		for (i = 0; i < len; i++)
			data[i] = x >> (8 * i);
	}

	if (!phy) {
		xlate_mode = alt? cpu->cd.alpha.alt_mode : cpu->cd.alpha.cm;
		xlate_rwc = rwc && !store;
	}

	ok = cpu->memory_rw(cpu, cpu->mem, addr, data, len,
	    store? MEM_WRITE : MEM_READ, CACHE_DATA | (phy? PHYSICAL : 0));

	xlate_mode = -1;
	xlate_rwc = 0;

	if (!ok)
		return 0;

	if (store) {
		if (phy && alt)
			*reg = 1;
		return 1;
	}

	if (phy && alt) {
		cpu->cd.alpha.load_linked_addr = addr;
		cpu->cd.alpha.ll_flag = 1;
	}

	x = 0;
	for (i = len - 1; i >= 0; i--)
		x = (x << 8) | data[i];
	if (!qw)
		x = (int64_t)(int32_t)x;
	*reg = x;
	return 1;
}


/*****************************************************************************
 *
 *  Reset, debugging
 */

/*
 *  ev4_reset():
 *
 *  Switch the CPU to real PALcode and reset it: PALmode at the given
 *  physical address, translation off, interrupts disabled.
 */
void ev4_reset(struct cpu *cpu, uint64_t pc)
{
	struct alpha_cpu *a = &cpu->cd.alpha;

	a->ev4_palcode = 1;
	a->palmode = 1;
	a->cm = 0;
	a->pal_base = 0;
	a->iccsr = 0;
	a->abox_ctl = 0;
	a->biu_ctl = 0;
	a->hier = a->hier_misc = a->sier = a->aster = 0;
	a->sirr = a->astrr = 0;
	a->va_locked = 0;
	a->mchk_pending = 0;
	memset(a->itb, 0, sizeof(a->itb));
	memset(a->dtb, 0, sizeof(a->dtb));
	a->itb_next_small = a->itb_next_large = a->dtb_next = 0;

	cpu->pc = ALPHA_PALMODE_TAG | (pc & ALPHA_PA_MASK);

	if (cpu->invalidate_translation_caches != NULL)
		vph_invalidate_all(cpu);
}


static void dump_tb_entry(struct ev4_tb_entry *e, int i, int istream)
{
	uint64_t va = e->vpn << ALPHA_PAGESHIFT;
	if (va & (1ULL << 42))
		va |= 0xfffff80000000000ULL;

	debug("  %s%2i: va 0x%016" PRIx64 " pa 0x%09" PRIx64 " gh %i%s"
	    " prot 0x%04x\n", istream? "itb" : "dtb", i, va,
	    e->pfn << ALPHA_PAGESHIFT, e->gh, e->asm_bit? " asm" : "",
	    (int) e->prot);
}


void ev4_tlbdump(struct cpu *cpu)
{
	struct alpha_cpu *a = &cpu->cd.alpha;
	int i;

	debug("cpu%i: %s, mode %i, PAL_BASE 0x%" PRIx64 ", EXC_ADDR 0x%016"
	    PRIx64 "\n", cpu->cpu_id, a->palmode? "PALmode" : "native",
	    a->cm, a->pal_base, a->exc_addr);
	debug("  ICCSR 0x%016" PRIx64 " ABOX_CTL 0x%04" PRIx64 " HIER 0x%02x"
	    " SIER 0x%04x irq 0x%02x\n", a->iccsr, a->abox_ctl, a->hier,
	    a->sier, a->irq_lines);

	for (i = 0; i < EV4_N_ITB_SMALL + EV4_N_ITB_LARGE; i++)
		if (a->itb[i].valid)
			dump_tb_entry(&a->itb[i], i, 1);
	for (i = 0; i < EV4_N_DTB; i++)
		if (a->dtb[i].valid)
			dump_tb_entry(&a->dtb[i], i, 0);
}
