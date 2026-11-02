#ifndef	CPU_ALPHA_H
#define	CPU_ALPHA_H

/*
 *  Copyright (C) 2005-2021  Anders Gavare.  All rights reserved.
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
 *  Alpha CPU definitions.
 */

#include "misc.h"

#include "thirdparty/alpha_cpu.h"


/*  ALPHA CPU types:  */
struct alpha_cpu_type_def { 
	const char	*name;
	uint64_t	pcs_type;	/*  See alpha_rpb.h  */
	int		features;
	int		implver;
	int		icache_shift;
	int		ilinesize;
	int		iway;
	int		dcache_shift;
	int		dlinesize;
	int		dway;
	int		l2cache_shift;
	int		l2linesize;
	int		l2way;
};

/*
 *  Architecture extensions (the bits are the same as in AMASK). Without
 *  ALPHA_FEATURE_BWX, the byte/word instructions are reserved opcodes,
 *  as on the 21064 and 21164.
 */
#define	ALPHA_FEATURE_BWX		1

#define ALPHA_CPU_TYPE_DEFS	{					\
	{ "21064",	0x000000002ULL, 0, 0, 16,5,2, 16,5,2,  0,0,0 },	\
	{ "21066",	0x200000004ULL, 0, 0, 16,5,2, 16,5,2,  0,0,0 },	\
	{ "21164",	0x000000005ULL, 0, 1, 16,5,2, 16,5,2,  0,0,0 },	\
	{ "21164A-2",	0x000000007ULL, 1, 1, 16,5,2, 16,5,2,  0,0,0 },	\
	{ "21164PC",	0x000000009ULL, 1, 1, 16,5,2, 16,5,2,  0,0,0 },	\
	{ "21264",	0x00000000dULL, 1, 2, 16,5,2, 16,5,2,  0,0,0 },	\
	{ "21364",	0x000000000ULL, 1, 3, 16,5,2, 16,5,2,  0,0,0 },	\
	{ NULL,		0x000000000ULL, 0, 0,  0,0,0,  0,0,0,  0,0,0 }	}


struct cpu_family;

/*  ALPHA_KENTRY_INT .. ALPHA_KENTRY_SYS  */
#define	N_ALPHA_KENTRY		6

#define	ALPHA_V0		0
#define	ALPHA_T0		1
#define	ALPHA_T1		2
#define	ALPHA_T2		3
#define	ALPHA_T3		4
#define	ALPHA_T4		5
#define	ALPHA_T5		6
#define	ALPHA_T6		7

#define	ALPHA_T7		8
#define	ALPHA_S0		9
#define	ALPHA_S1		10
#define	ALPHA_S2		11
#define	ALPHA_S3		12
#define	ALPHA_S4		13
#define	ALPHA_S5		14
#define	ALPHA_FP		15

#define	ALPHA_A0		16
#define	ALPHA_A1		17
#define	ALPHA_A2		18
#define	ALPHA_A3		19
#define	ALPHA_A4		20
#define	ALPHA_A5		21
#define	ALPHA_T8		22
#define	ALPHA_T9		23

#define	ALPHA_T10		24
#define	ALPHA_T11		25
#define	ALPHA_RA		26
#define	ALPHA_T12		27
#define	ALPHA_AT		28
#define	ALPHA_GP		29
#define	ALPHA_SP		30
#define	ALPHA_ZERO		31

#define	N_ALPHA_REGS		32

#define ALPHA_REG_NAMES		{				\
	"v0", "t0", "t1", "t2", "t3", "t4", "t5", "t6",		\
	"t7", "s0", "s1", "s2", "s3", "s4", "s5", "fp",		\
	"a0", "a1", "a2", "a3", "a4", "a5", "t8", "t9",		\
	"t10", "t11", "ra", "t12", "at", "gp", "sp", "zero" 	}


/*  Dyntrans definitions:  */

#define	ALPHA_N_IC_ARGS			3
#define	ALPHA_INSTR_ALIGNMENT_SHIFT	2
#define	ALPHA_IC_ENTRIES_SHIFT		11
#define	ALPHA_IC_ENTRIES_PER_PAGE	(1 << ALPHA_IC_ENTRIES_SHIFT)
#define	ALPHA_PC_TO_IC_ENTRY(a)		(((a)>>ALPHA_INSTR_ALIGNMENT_SHIFT) \
					& (ALPHA_IC_ENTRIES_PER_PAGE-1))
#define	ALPHA_ADDR_TO_PAGENR(a)		((a) >> (ALPHA_IC_ENTRIES_SHIFT \
					+ ALPHA_INSTR_ALIGNMENT_SHIFT))

#define	ALPHA_MAX_VPH_TLB_ENTRIES	128

#define	ALPHA_L2N		17
#define	ALPHA_L3N		17

DYNTRANS_MISC_DECLARATIONS(alpha,ALPHA,uint64_t)
DYNTRANS_MISC64_DECLARATIONS(alpha,ALPHA,uint8_t)


#define	ALPHA_PAGESHIFT		13


/*
 *  Exceptions (see alpha_exception()):
 */
#define	ALPHA_EXC_ARITH		1	/*  Arithmetic trap  */
#define	ALPHA_EXC_OPCDEC	2	/*  Reserved or privileged instr.  */
#define	ALPHA_EXC_UNALIGN	3	/*  Unaligned access  */
#define	ALPHA_EXC_FEN		4	/*  Floating point disabled  */

/*  Exception summary bits of an arithmetic trap (EXC_SUM):  */
#define	ALPHA_EXC_SUM_SWC	0x01	/*  Software completion possible  */
#define	ALPHA_EXC_SUM_INV	0x02	/*  Invalid operation  */
#define	ALPHA_EXC_SUM_DZE	0x04	/*  Division by zero  */
#define	ALPHA_EXC_SUM_FOV	0x08	/*  Floating point overflow  */
#define	ALPHA_EXC_SUM_UNF	0x10	/*  Underflow  */
#define	ALPHA_EXC_SUM_INE	0x20	/*  Inexact result  */
#define	ALPHA_EXC_SUM_IOV	0x40	/*  Integer overflow  */

/*  Returned by alpha_fp_operate() for an instruction that doesn't exist,
    or that the hardware leaves to software (FEN exception on the EV4):  */
#define	ALPHA_FP_OPCDEC		(-1)
#define	ALPHA_FP_FEN		(-2)


/*****************************************************************************
 *
 *  21064 (EV4) privileged architecture: PALmode, internal processor
 *  registers (IPRs), translation buffers. See cpu_alpha_ev4.c and the
 *  "Alpha 21064 and 21064A Microprocessors Hardware Reference Manual"
 *  (EC-Q9ZUC-TE), chapters 4 and 5.
 */

/*
 *  The PC as seen by dyntrans is tagged, so that the three kinds of
 *  instruction fetches get separate entries in the virtual-to-host
 *  translation tables, and none of them can be mistaken for a data
 *  access (all data addresses are sign extended from bit 42):
 *
 *	PALmode:	ALPHA_PALMODE_TAG | physical address
 *	native mode:	virtual address ^ ALPHA_ISTREAM_TAG
 *
 *  alpha_pc_to_arch() and alpha_arch_to_pc() convert. With emulated
 *  PALcode (ev4_palcode == 0), there are no tags.
 */
#define	ALPHA_PALMODE_TAG	0x2000000000000000ULL
#define	ALPHA_ISTREAM_TAG	0x4000000000000000ULL
#define	ALPHA_PA_MASK		0x00000003ffffffffULL	/*  34 bits  */

/*  PALcode entry points (offsets from PAL_BASE):  */
#define	EV4_ENTRY_RESET		0x0000
#define	EV4_ENTRY_MCHK		0x0020
#define	EV4_ENTRY_ARITH		0x0060
#define	EV4_ENTRY_INTERRUPT	0x00e0
#define	EV4_ENTRY_D_FAULT	0x01e0
#define	EV4_ENTRY_ITB_MISS	0x03e0
#define	EV4_ENTRY_ITB_ACV	0x07e0
#define	EV4_ENTRY_DTB_MISS	0x08e0	/*  native mode  */
#define	EV4_ENTRY_DTB_MISS_PAL	0x09e0
#define	EV4_ENTRY_UNALIGN	0x11e0
#define	EV4_ENTRY_OPCDEC	0x13e0
#define	EV4_ENTRY_FEN		0x17e0
#define	EV4_ENTRY_CALL_PAL	0x2000	/*  privileged; unprivileged 0x3000  */

/*  IPR numbers: the low 8 bits of HW_MFPR/HW_MTPR (PAL, ABX, IBX, index)  */
#define	EV4_IPR_PAL		0x80
#define	EV4_IPR_ABX		0x40
#define	EV4_IPR_IBX		0x20

#define	EV4_IPR_TB_TAG		0	/*  Ibox  */
#define	EV4_IPR_ITB_PTE		1
#define	EV4_IPR_ICCSR		2
#define	EV4_IPR_ITB_PTE_TEMP	3
#define	EV4_IPR_EXC_ADDR	4
#define	EV4_IPR_SL_RCV		5
#define	EV4_IPR_ITBZAP		6
#define	EV4_IPR_ITBASM		7
#define	EV4_IPR_ITBIS		8
#define	EV4_IPR_PS		9
#define	EV4_IPR_EXC_SUM		10
#define	EV4_IPR_PAL_BASE	11
#define	EV4_IPR_HIRR		12
#define	EV4_IPR_SIRR		13
#define	EV4_IPR_ASTRR		14
#define	EV4_IPR_HIER		16
#define	EV4_IPR_SIER		17
#define	EV4_IPR_ASTER		18
#define	EV4_IPR_SL_CLR		19
#define	EV4_IPR_SL_XMIT		22

#define	EV4_IPR_TB_CTL		0	/*  Abox  */
#define	EV4_IPR_DTB_PTE		2
#define	EV4_IPR_DTB_PTE_TEMP	3
#define	EV4_IPR_MM_CSR		4
#define	EV4_IPR_VA		5
#define	EV4_IPR_DTBZAP		6
#define	EV4_IPR_DTBASM		7
#define	EV4_IPR_DTBIS		8
#define	EV4_IPR_BIU_ADDR	9
#define	EV4_IPR_BIU_STAT	10
#define	EV4_IPR_DC_STAT		12
#define	EV4_IPR_FILL_ADDR	13
#define	EV4_IPR_ABOX_CTL	14
#define	EV4_IPR_ALT_MODE	15
#define	EV4_IPR_CC		16
#define	EV4_IPR_CC_CTL		17
#define	EV4_IPR_BIU_CTL		18
#define	EV4_IPR_FILL_SYNDROME	19
#define	EV4_IPR_BC_TAG		20
#define	EV4_IPR_FLUSH_IC	21
#define	EV4_IPR_FLUSH_IC_ASM	23

/*  ICCSR bits (write format):  */
#define	EV4_ICCSR_FPE		(1ULL << 42)
#define	EV4_ICCSR_MAP		(1ULL << 41)
#define	EV4_ICCSR_HWE		(1ULL << 40)

/*  ABOX_CTL bits:  */
#define	EV4_ABOX_MCHK_EN	(1ULL << 1)
#define	EV4_ABOX_CRD_EN		(1ULL << 2)
#define	EV4_ABOX_SPE_1		(1ULL << 4)
#define	EV4_ABOX_SPE_2		(1ULL << 5)

/*  MM_CSR bits:  */
#define	EV4_MMCSR_WR		0x01
#define	EV4_MMCSR_ACV		0x02
#define	EV4_MMCSR_FOR		0x04
#define	EV4_MMCSR_FOW		0x08

/*  BIU_STAT bits:  */
#define	EV4_BIU_HERR		0x01
#define	EV4_BIU_FATAL1		0x80

/*  Page table entry bits, as written to ITB_PTE/DTB_PTE:  */
#define	EV4_PTE_FOR		0x0002
#define	EV4_PTE_FOW		0x0004
#define	EV4_PTE_ASM		0x0010
#define	EV4_PTE_GH_SHIFT	5
#define	EV4_PTE_KRE		0x0100
#define	EV4_PTE_URE		0x0800
#define	EV4_PTE_KWE		0x1000
#define	EV4_PTE_UWE		0x8000

#define	EV4_N_ITB_SMALL		8
#define	EV4_N_ITB_LARGE		4
#define	EV4_N_DTB		32

/*  A translation buffer entry. va and pfn are page numbers (VA<42:13>,
    PA<33:13>); gh is the granularity hint (block of 8^gh pages).  */
struct ev4_tb_entry {
	int		valid;
	int		gh;
	int		asm_bit;
	uint64_t	vpn;
	uint64_t	pfn;
	uint32_t	prot;		/*  xRE/xWE, FOR, FOW (PTE bits <15:0>)  */
};


struct alpha_cpu {
	struct alpha_cpu_type_def	cpu_type;


	/*
	 *  General Purpose Registers:
	 */

	uint64_t		r[N_ALPHA_REGS];	/*  Integer  */
	uint64_t		f[N_ALPHA_REGS];	/*  Floating Point  */

	uint64_t		fpcr;			/*  FP Control Reg.  */

	/*  Misc.:  */
	uint64_t		pcc;			/*  Cycle Counter  */
	uint64_t		load_linked_addr;
	int			ll_flag;

	int			irq_asserted;

	/*  Read by RC and RS:  */
	int			intr_flag;

	/*
	 *  State of the most recent exception (until PALcode exists,
	 *  alpha_exception() just reports these):
	 *
	 *  exc_sum and exc_mask are the summary and register write mask of
	 *  an arithmetic trap (mask bit n = integer register n, bit 32+n =
	 *  floating point register n). For an unaligned access, exc_va is
	 *  the address and exc_opcode/exc_reg describe the instruction.
	 */
	uint64_t		exc_sum;
	uint64_t		exc_mask;
	uint64_t		trap_mask;	/*  Of the current FP trap  */
	int			swc_blocked;
	uint64_t		exc_va;
	int			exc_opcode;
	int			exc_reg;

	/*
	 *  21064 privileged architecture (ev4_palcode != 0): the CPU runs
	 *  real PALcode (from the firmware). Otherwise PALcode calls are
	 *  emulated in cpu_alpha_palcode.c.
	 */
	int			ev4_palcode;
	int			palmode;
	int			cm;		/*  Current mode, PS<4:3>  */

	uint64_t		exc_addr;
	uint64_t		pal_base;
	uint64_t		iccsr;
	uint64_t		pal_temp[32];

	uint32_t		hier;		/*  <5:0>: irq_h[5:0]  */
	uint32_t		hier_misc;	/*  CRE, PC0, PC1, SLE  */
	uint32_t		irq_lines;	/*  irq_h[5:0] pins  */
	uint32_t		sirr, sier;	/*  <15:1>  */
	uint32_t		astrr, aster;	/*  <3:0>: K, E, S, U  */
	int			sl_rcv;
	int			sl_xmit;
	int			crd_pending;
	int			slr_pending;

	uint64_t		tb_tag;
	int			tb_ctl_gh;
	struct ev4_tb_entry	itb[EV4_N_ITB_SMALL + EV4_N_ITB_LARGE];
	int			itb_next_small, itb_next_large;
	uint64_t		itb_pte_temp;
	struct ev4_tb_entry	dtb[EV4_N_DTB];
	int			dtb_next;
	uint64_t		dtb_pte_temp;

	uint64_t		mm_csr;
	uint64_t		va;
	int			va_locked;
	uint64_t		abox_ctl;
	int			alt_mode;
	uint64_t		biu_ctl;
	uint64_t		biu_stat;
	uint64_t		biu_addr;
	int			biu_locked;
	uint64_t		fill_addr;
	uint64_t		fill_syndrome;
	uint64_t		bc_tag;

	/*  Cycle counter: CC<63:32> is cc_offset, CC<31:0> counts cycles
	    while cc_enabled: at the machine's clock rate in host time (so
	    that it agrees with the time of day clock), or one per
	    instruction if the clock rate isn't known. See ev4_cycles().  */
	uint64_t		cc_offset;
	uint32_t		cc_base;
	uint64_t		cc_base_cycles;
	int			cc_enabled;

	/*  Modes for which entries were added to the translation tables
	    (bit n = mode n). See ev4_set_mode().  */
	int			vph_modes;

	/*  Set by a memory access to nothing (-> machine check):  */
	int			mchk_pending;
	int			irq_now;	/*  take interrupts after this
						    instruction (a device just
						    raised an error interrupt)  */

	/*  OSF1 PALcode specific:  */
	uint64_t		ps;		/*  Processor Status  */
	uint64_t		vptptr;		/*  Virtual Page Table Ptr  */
	uint64_t		sysvalue;
	uint64_t		mces;		/*  Machine Check Error Summary  */
	uint64_t		kgp;		/*  Kernel GP  */
	uint64_t		kentry[N_ALPHA_KENTRY];
	uint64_t		ctx;		/*  Ptr to current PCB (?)  */
	struct alpha_pcb	pcb;		/*  Process Control Block  */


	/*
	 *  Instruction translation cache and Virtual->Physical->Host
	 *  address translation:
	 */
	DYNTRANS_ITC(alpha)
	VPH_TLBS(alpha,ALPHA)
	VPH64(alpha,ALPHA)
};


/*  cpu_alpha.c:  */
void alpha_update_translation_table(struct cpu *cpu, uint64_t vaddr_page,
	unsigned char *host_page, int writeflag, uint64_t paddr_page);
void alpha_invalidate_translation_caches(struct cpu *cpu, uint64_t, int);
void alpha_invalidate_code_translation(struct cpu *cpu, uint64_t, int);
void alpha_init_64bit_dummy_tables(struct cpu *cpu);
int alpha_run_instr(struct cpu *cpu);
int alpha_memory_rw(struct cpu *cpu, struct memory *mem, uint64_t vaddr,
	unsigned char *data, size_t len, int writeflag, int cache_flags);
void alpha_cpu_family_init(struct cpu_family *);

void alpha_exception(struct cpu *cpu, int type);
void alpha_pc_to_pointers(struct cpu *cpu);

/*  cpu_alpha_ev4.c:  */
uint64_t alpha_pc_to_arch(struct cpu *cpu, uint64_t pc);
uint64_t alpha_arch_to_pc(struct cpu *cpu, uint64_t addr);
void ev4_reset(struct cpu *cpu, uint64_t pc);
void ev4_exception(struct cpu *cpu, int entry, uint64_t exc_addr);
void ev4_call_pal(struct cpu *cpu, uint32_t func, uint64_t next_pc);
int ev4_hw_allowed(struct cpu *cpu);
uint64_t ev4_mfpr(struct cpu *cpu, int ipr);
void ev4_mtpr(struct cpu *cpu, int ipr, uint64_t value, int rb_value_valid);
int ev4_hw_rei(struct cpu *cpu);
int ev4_translate(struct cpu *cpu, uint64_t vaddr, uint64_t *paddr,
	int flags);
int ev4_hw_ldst(struct cpu *cpu, uint32_t iword, uint64_t addr,
	uint64_t *reg);
int ev4_interrupt_pending(struct cpu *cpu);
void ev4_check_interrupts(struct cpu *cpu);
void ev4_machine_check(struct cpu *cpu, uint64_t paddr, int writeflag);
uint64_t ev4_read_cc(struct cpu *cpu);
uint64_t ev4_cycles(struct cpu *cpu);
void ev4_tlbdump(struct cpu *cpu);

/*  cpu_alpha_fp.c:  */
int alpha_fp_operate(struct cpu *cpu, uint32_t iword);
uint64_t alpha_lds_to_reg(uint32_t m);
uint32_t alpha_reg_to_sts(uint64_t r);
uint64_t alpha_ldf_to_reg(uint32_t m);
uint32_t alpha_reg_to_stf(uint64_t r);
uint64_t alpha_swap_g(uint64_t x);

/*  cpu_alpha_palcode.c:  */
void alpha_palcode_name(uint32_t palcode, char *buf, size_t buflen);
void alpha_palcode(struct cpu *cpu, uint32_t palcode);

/*  memory_alpha.c:  */
int alpha_translate_v2p(struct cpu *cpu, uint64_t vaddr,
	uint64_t *return_addr, int flags);


#endif	/*  CPU_ALPHA_H  */
