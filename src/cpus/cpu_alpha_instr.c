/*
 *  Copyright (C) 2005-2020  Anders Gavare.  All rights reserved.
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
 *  Alpha instructions.
 *
 *  Individual functions should keep track of cpu->n_translated_instrs.
 *  (If no instruction was executed, then it should be decreased. If, say, 4
 *  instructions were combined into one function and executed, then it should
 *  be increased by 3.)
 */





/*
 *  nop:  Do nothing.
 */
X(nop)
{
}


/*
 *  call_pal:  PALcode call
 *
 *  arg[0] = pal nr
 */
X(call_pal)
{
	/*  Synchronize PC first:  */
	uint64_t old_pc, low_pc = ((size_t)ic - (size_t)
	    cpu->cd.alpha.cur_ic_page) / sizeof(struct alpha_instr_call);
	cpu->pc &= ~((ALPHA_IC_ENTRIES_PER_PAGE-1) <<
	    ALPHA_INSTR_ALIGNMENT_SHIFT);
	cpu->pc += (low_pc << ALPHA_INSTR_ALIGNMENT_SHIFT);
	old_pc = cpu->pc;

	if (cpu->cd.alpha.ev4_palcode) {
		/*  Real PALcode:  */
		ev4_call_pal(cpu, ic->arg[0], cpu->pc + sizeof(uint32_t));
		alpha_pc_to_pointers(cpu);
		return;
	}

	alpha_palcode(cpu, ic->arg[0]);

	if (!cpu->running) {
		cpu->n_translated_instrs --;
		cpu->cd.alpha.next_ic = &nothing_call;
	} else if (cpu->pc != old_pc) {
		/*  The PC value was changed by the palcode call.  */
		/*  Find the new physical page and update the translation
		    pointers:  */
		alpha_pc_to_pointers(cpu);
	}
}


/*
 *  jsr:  Jump to SubRoutine
 *
 *  arg[0] = ptr to uint64_t where to store return PC
 *  arg[1] = ptr to uint64_t of new PC
 */
X(jsr)
{
	uint64_t old_pc = cpu->pc, low_pc, new_pc;
	uint64_t mask_within_page = ((ALPHA_IC_ENTRIES_PER_PAGE-1)
	    << ALPHA_INSTR_ALIGNMENT_SHIFT) |
	    ((1 << ALPHA_INSTR_ALIGNMENT_SHIFT) - 1);

	low_pc = ((size_t)ic - (size_t)
	    cpu->cd.alpha.cur_ic_page) / sizeof(struct alpha_instr_call);
	cpu->pc &= ~((ALPHA_IC_ENTRIES_PER_PAGE-1)
	    << ALPHA_INSTR_ALIGNMENT_SHIFT);
	cpu->pc += (low_pc << ALPHA_INSTR_ALIGNMENT_SHIFT) + 4;

	/*  Read the target first, in case Ra and Rb are the same:  */
	new_pc = alpha_arch_to_pc(cpu, *((int64_t *)ic->arg[1]) &
	    ~(uint64_t)3);
	*((int64_t *)ic->arg[0]) = alpha_pc_to_arch(cpu, cpu->pc);
	cpu->pc = new_pc;

	/*
	 *  If this is a jump/return into the same code page as we were
	 *  already in, then just set cpu->cd.alpha.next_ic.
	 */
	if ((old_pc & ~mask_within_page) == (cpu->pc & ~mask_within_page)) {
		cpu->cd.alpha.next_ic = cpu->cd.alpha.cur_ic_page +
		    ((cpu->pc & mask_within_page) >> 2);
	} else {
		/*  Find the new physical page and update pointers:  */
		alpha_pc_to_pointers(cpu);
	}
}


/*
 *  jsr_trace:  Jump to SubRoutine (with function call trace enabled)
 *
 *  Arguments same as for jsr.
 */
X(jsr_trace)
{
	cpu_functioncall_trace(cpu, *((int64_t *)ic->arg[1]));
	instr(jsr)(cpu, ic);
}


/*
 *  jsr_0:  JSR/RET, don't store return PC.
 *
 *  arg[0] = ignored
 *  arg[1] = ptr to uint64_t of new PC
 */
X(jsr_0)
{
	uint64_t old_pc = cpu->pc;
	uint64_t mask_within_page = ((ALPHA_IC_ENTRIES_PER_PAGE-1)
	    << ALPHA_INSTR_ALIGNMENT_SHIFT)
	    | ((1 << ALPHA_INSTR_ALIGNMENT_SHIFT) - 1);

	cpu->pc = alpha_arch_to_pc(cpu, *((int64_t *)ic->arg[1]) &
	    ~(uint64_t)3);

	/*
	 *  If this is a jump/return into the same code page as we were
	 *  already in, then just set cpu->cd.alpha.next_ic.
	 */
	if ((old_pc & ~mask_within_page) == (cpu->pc & ~mask_within_page)) {
		cpu->cd.alpha.next_ic = cpu->cd.alpha.cur_ic_page +
		    ((cpu->pc & mask_within_page) >> 2);
	} else {
		/*  Find the new physical page and update pointers:  */
		alpha_pc_to_pointers(cpu);
	}
}


/*
 *  jsr_0_trace:  JSR/RET (with function call trace enabled)
 *
 *  Arguments same as for jsr_0.
 */
X(jsr_0_trace)
{
	cpu_functioncall_trace_return(cpu);
	instr(jsr_0)(cpu, ic);
}


/*
 *  br:  Branch (to a different translated page)
 *
 *  arg[0] = relative offset (as an int32_t)
 */
X(br)
{
	uint64_t low_pc;

	/*  Calculate new PC from this instruction + arg[0]  */
	low_pc = ((size_t)ic - (size_t)
	    cpu->cd.alpha.cur_ic_page) / sizeof(struct alpha_instr_call);
	cpu->pc &= ~((ALPHA_IC_ENTRIES_PER_PAGE-1)
	    << ALPHA_INSTR_ALIGNMENT_SHIFT);
	cpu->pc += (low_pc << ALPHA_INSTR_ALIGNMENT_SHIFT);
	cpu->pc += (int32_t)ic->arg[0];

	/*  Find the new physical page and update the translation pointers:  */
	alpha_pc_to_pointers(cpu);
}


/*
 *  br:  Branch (to a different translated page), write return address
 *
 *  arg[0] = relative offset (as an int32_t)
 *  arg[1] = pointer to uint64_t where to write return address
 */
X(br_return)
{
	uint64_t low_pc;

	/*  Calculate new PC from this instruction + arg[0]  */
	low_pc = ((size_t)ic - (size_t)
	    cpu->cd.alpha.cur_ic_page) / sizeof(struct alpha_instr_call);
	cpu->pc &= ~((ALPHA_IC_ENTRIES_PER_PAGE-1)
	    << ALPHA_INSTR_ALIGNMENT_SHIFT);
	cpu->pc += (low_pc << ALPHA_INSTR_ALIGNMENT_SHIFT);

	/*  ... but first, save away the return address:  */
	*((int64_t *)ic->arg[1]) = alpha_pc_to_arch(cpu, cpu->pc + 4);

	cpu->pc += (int32_t)ic->arg[0];

	/*  Find the new physical page and update the translation pointers:  */
	alpha_pc_to_pointers(cpu);
}


/*
 *  beq:  Branch (to a different translated page) if Equal
 *
 *  arg[0] = relative offset (as an int32_t)
 *  arg[1] = pointer to int64_t register
 */
X(beq)
{
	if (*((int64_t *)ic->arg[1]) == 0)
		instr(br)(cpu, ic);
}


/*
 *  blbs:  Branch (to a different translated page) if Low Bit Set
 *
 *  arg[0] = relative offset (as an int32_t)
 *  arg[1] = pointer to int64_t register
 */
X(blbs)
{
	if (*((int64_t *)ic->arg[1]) & 1)
		instr(br)(cpu, ic);
}


/*
 *  blbc:  Branch (to a different translated page) if Low Bit Clear
 *
 *  arg[0] = relative offset (as an int32_t)
 *  arg[1] = pointer to int64_t register
 */
X(blbc)
{
	if (!(*((int64_t *)ic->arg[1]) & 1))
		instr(br)(cpu, ic);
}


/*
 *  bne:  Branch (to a different translated page) if Not Equal
 *
 *  arg[0] = relative offset (as an int32_t)
 *  arg[1] = pointer to int64_t register
 */
X(bne)
{
	if (*((int64_t *)ic->arg[1]) != 0)
		instr(br)(cpu, ic);
}


/*
 *  ble:  Branch (to a different translated page) if Less or Equal
 *
 *  arg[0] = relative offset (as an int32_t)
 *  arg[1] = pointer to int64_t register
 */
X(ble)
{
	if (*((int64_t *)ic->arg[1]) <= 0)
		instr(br)(cpu, ic);
}


/*
 *  blt:  Branch (to a different translated page) if Less Than
 *
 *  arg[0] = relative offset (as an int32_t)
 *  arg[1] = pointer to int64_t register
 */
X(blt)
{
	if (*((int64_t *)ic->arg[1]) < 0)
		instr(br)(cpu, ic);
}


/*
 *  bge:  Branch (to a different translated page) if Greater or Equal
 *
 *  arg[0] = relative offset (as an int32_t)
 *  arg[1] = pointer to int64_t register
 */
X(bge)
{
	if (*((int64_t *)ic->arg[1]) >= 0)
		instr(br)(cpu, ic);
}


/*
 *  bgt:  Branch (to a different translated page) if Greater Than
 *
 *  arg[0] = relative offset (as an int32_t)
 *  arg[1] = pointer to int64_t register
 */
X(bgt)
{
	if (*((int64_t *)ic->arg[1]) > 0)
		instr(br)(cpu, ic);
}


/*
 *  br_samepage:  Branch (to within the same translated page)
 *
 *  arg[0] = pointer to new alpha_instr_call
 */
X(br_samepage)
{
	cpu->cd.alpha.next_ic = (struct alpha_instr_call *) ic->arg[0];
}


/*
 *  br_return_samepage:  Branch (to within the same translated page),
 *                       and save return address
 *
 *  arg[0] = pointer to new alpha_instr_call
 *  arg[1] = pointer to uint64_t where to store return address
 */
X(br_return_samepage)
{
	uint64_t low_pc;

	low_pc = ((size_t)ic - (size_t)
	    cpu->cd.alpha.cur_ic_page) / sizeof(struct alpha_instr_call);
	cpu->pc &= ~((ALPHA_IC_ENTRIES_PER_PAGE-1)
	    << ALPHA_INSTR_ALIGNMENT_SHIFT);
	cpu->pc += (low_pc << ALPHA_INSTR_ALIGNMENT_SHIFT);
	*((int64_t *)ic->arg[1]) = alpha_pc_to_arch(cpu, cpu->pc + 4);

	cpu->cd.alpha.next_ic = (struct alpha_instr_call *) ic->arg[0];
}


/*
 *  beq_samepage:  Branch (to within the same translated page) if Equal
 *
 *  arg[0] = pointer to new alpha_instr_call
 *  arg[1] = pointer to int64_t register
 */
X(beq_samepage)
{
	if (*((int64_t *)ic->arg[1]) == 0)
		instr(br_samepage)(cpu, ic);
}


/*
 *  blbs_samepage:  Branch (to within the same translated page) if Low Bit Set
 *
 *  arg[0] = pointer to new alpha_instr_call
 *  arg[1] = pointer to int64_t register
 */
X(blbs_samepage)
{
	if (*((int64_t *)ic->arg[1]) & 1)
		instr(br_samepage)(cpu, ic);
}


/*
 *  blbc_samepage:  Branch (to within the same translated page) if Low Bit Clear
 *
 *  arg[0] = pointer to new alpha_instr_call
 *  arg[1] = pointer to int64_t register
 */
X(blbc_samepage)
{
	if (!(*((int64_t *)ic->arg[1]) & 1))
		instr(br_samepage)(cpu, ic);
}


/*
 *  bne_samepage:  Branch (to within the same translated page) if Not Equal
 *
 *  arg[0] = pointer to new alpha_instr_call
 *  arg[1] = pointer to int64_t register
 */
X(bne_samepage)
{
	if (*((int64_t *)ic->arg[1]) != 0)
		instr(br_samepage)(cpu, ic);
}


/*
 *  ble_samepage:  Branch (to within the same translated page) if Less or Equal
 *
 *  arg[0] = pointer to new alpha_instr_call
 *  arg[1] = pointer to int64_t register
 */
X(ble_samepage)
{
	if (*((int64_t *)ic->arg[1]) <= 0)
		instr(br_samepage)(cpu, ic);
}


/*
 *  blt_samepage:  Branch (to within the same translated page) if Less Than
 *
 *  arg[0] = pointer to new alpha_instr_call
 *  arg[1] = pointer to int64_t register
 */
X(blt_samepage)
{
	if (*((int64_t *)ic->arg[1]) < 0)
		instr(br_samepage)(cpu, ic);
}


/*
 *  bge_samepage:  Branch (to within the same translated page)
 *		   if Greater or Equal
 *
 *  arg[0] = pointer to new alpha_instr_call
 *  arg[1] = pointer to int64_t register
 */
X(bge_samepage)
{
	if (*((int64_t *)ic->arg[1]) >= 0)
		instr(br_samepage)(cpu, ic);
}


/*
 *  bgt_samepage:  Branch (to within the same translated page) if Greater Than
 *
 *  arg[0] = pointer to new alpha_instr_call
 *  arg[1] = pointer to int64_t register
 */
X(bgt_samepage)
{
	if (*((int64_t *)ic->arg[1]) > 0)
		instr(br_samepage)(cpu, ic);
}


/*
 *  Floating point branches. These look at the value as a floating point
 *  number: -0.0 is zero, and is neither less nor greater than zero.
 *
 *  arg[0] = as for the integer branches
 *  arg[1] = pointer to the floating point register
 */
#define	FP_IS_ZERO(x)	(((x) << 1) == 0)
#define	FP_IS_NEG(x)	((x) >> 63)
static void alpha_fen(struct cpu *cpu, struct alpha_instr_call *ic);

/*  Floating point instructions with FP disabled (ICCSR<FPE> clear) cause
    a FEN exception:  */
#define	FEN_CHECK							\
	if (cpu->cd.alpha.ev4_palcode &&				\
	    !(cpu->cd.alpha.iccsr & EV4_ICCSR_FPE)) {			\
		alpha_fen(cpu, ic);					\
		return;							\
	}

#define	FBRANCH(name, cond)						\
X(name) {								\
	uint64_t x = *((uint64_t *)ic->arg[1]);				\
	FEN_CHECK							\
	if (cond) instr(br)(cpu, ic);					\
}									\
X(name ## _samepage) {							\
	uint64_t x = *((uint64_t *)ic->arg[1]);				\
	FEN_CHECK							\
	if (cond) instr(br_samepage)(cpu, ic);				\
}
FBRANCH(fbeq, FP_IS_ZERO(x))
FBRANCH(fbne, !FP_IS_ZERO(x))
FBRANCH(fblt, FP_IS_NEG(x) && !FP_IS_ZERO(x))
FBRANCH(fbge, !FP_IS_NEG(x) || FP_IS_ZERO(x))
FBRANCH(fble, FP_IS_NEG(x) || FP_IS_ZERO(x))
FBRANCH(fbgt, !FP_IS_NEG(x) && !FP_IS_ZERO(x))


/*
 *  implver:  Return CPU implver value.
 *
 *  arg[0] = pointer to destination uint64_t
 */
X(implver)
{
	reg(ic->arg[0]) = cpu->cd.alpha.cpu_type.implver;
}


/*
 *  mull, mull_imm:  Signed Multiply 32x32 => 32.
 *
 *  arg[0] = pointer to destination uint64_t
 *  arg[1] = pointer to source uint64_t
 *  arg[2] = pointer to source uint64_t or immediate
 */
X(mull)
{
	int32_t a = reg(ic->arg[1]);
	int32_t b = reg(ic->arg[2]);
	reg(ic->arg[0]) = (int64_t)(int32_t)(a * b);
}
X(mull_imm)
{
	int32_t a = reg(ic->arg[1]);
	int32_t b = ic->arg[2];
	reg(ic->arg[0]) = (int64_t)(int32_t)(a * b);
}


/*
 *  mulq, mulq_imm:  Unsigned Multiply 64x64 => 64.
 *
 *  arg[0] = pointer to destination uint64_t
 *  arg[1] = pointer to source uint64_t
 *  arg[2] = pointer to source uint64_t or immediate
 */
X(mulq)
{
	reg(ic->arg[0]) = reg(ic->arg[1]) * reg(ic->arg[2]);
}
X(mulq_imm)
{
	reg(ic->arg[0]) = reg(ic->arg[1]) * ic->arg[2];
}


/*
 *  umulh:  Unsigned Multiply 64x64 => 128. Store high part in dest reg.
 *
 *  arg[0] = pointer to destination uint64_t
 *  arg[1] = pointer to source uint64_t
 *  arg[2] = pointer to source uint64_t
 */
X(umulh)
{
	uint64_t reshi = 0, reslo = 0;
	uint64_t s1 = reg(ic->arg[1]), s2 = reg(ic->arg[2]);
	int i, bit;

	for (i=0; i<64; i++) {
		bit = (s1 & 0x8000000000000000ULL)? 1 : 0;
		s1 <<= 1;

		/*  If bit in s1 set, then add s2 to reshi/lo:  */
		if (bit) {
			uint64_t old_reslo = reslo;
			reslo += s2;
			if (reslo < old_reslo)
				reshi ++;
		}

		if (i != 63) {
			reshi <<= 1;
			reshi += (reslo & 0x8000000000000000ULL? 1 : 0);
			reslo <<= 1;
		}
	}

	reg(ic->arg[0]) = reshi;
}


/*
 *  lda:  Load address.
 *
 *  arg[0] = pointer to destination uint64_t
 *  arg[1] = pointer to source uint64_t
 *  arg[2] = offset (possibly as an int32_t)
 */
X(lda)
{
	reg(ic->arg[0]) = reg(ic->arg[1]) + (int64_t)(int32_t)ic->arg[2];
}


/*
 *  lda_0:  Load address compared to the zero register.
 *
 *  arg[0] = pointer to destination uint64_t
 *  arg[1] = ignored
 *  arg[2] = offset (possibly as an int32_t)
 */
X(lda_0)
{
	reg(ic->arg[0]) = (int64_t)(int32_t)ic->arg[2];
}


/*
 *  clear:  Clear a 64-bit register.
 *
 *  arg[0] = pointer to destination uint64_t
 */
X(clear)
{
	reg(ic->arg[0]) = 0;
}


/*
 *  rdcc:  Read the Cycle Counter into a 64-bit register.
 *
 *  arg[0] = pointer to destination uint64_t
 */
X(rdcc)
{
	if (cpu->cd.alpha.ev4_palcode) {
		reg(ic->arg[0]) = ev4_read_cc(cpu);
		return;
	}

	reg(ic->arg[0]) = cpu->cd.alpha.pcc;

	/*  TODO: actually keep the pcc updated!  */
	cpu->cd.alpha.pcc += 20;
}


/*****************************************************************************
 *
 *  Exceptions
 */

/*  Set cpu->pc to the address of the current instruction.  */
#define	SYNCH_PC	{						\
	uint64_t low_pc_ = ((size_t)ic - (size_t)			\
	    cpu->cd.alpha.cur_ic_page) / sizeof(struct alpha_instr_call);\
	cpu->pc &= ~((ALPHA_IC_ENTRIES_PER_PAGE-1) <<			\
	    ALPHA_INSTR_ALIGNMENT_SHIFT);				\
	cpu->pc += (low_pc_ << ALPHA_INSTR_ALIGNMENT_SHIFT);		\
    }


/*
 *  alpha_exception():
 *
 *  Take an exception. For faults, cpu->pc must be the address of the
 *  faulting instruction; for traps (arithmetic traps), the address of the
 *  next instruction.
 *
 *  TODO: There is no PALcode mode yet, so for now this only reports the
 *  exception and stops the emulation.
 */
void alpha_exception(struct cpu *cpu, int type)
{
	if (cpu->cd.alpha.ev4_palcode) {
		uint64_t exc_addr = alpha_pc_to_arch(cpu, cpu->pc) |
		    cpu->cd.alpha.palmode;
		int entry, op = cpu->cd.alpha.exc_opcode;

		switch (type) {
		case ALPHA_EXC_ARITH:
			/*  EXC_ADDR<1> set: the address is right as it is
			    (the next instruction).  */
			entry = EV4_ENTRY_ARITH;
			exc_addr |= 2;
			break;
		case ALPHA_EXC_UNALIGN:
			entry = EV4_ENTRY_UNALIGN;
			if (!cpu->cd.alpha.va_locked) {
				int wr = (op >= 0x0d && op <= 0x0f) ||
				    (op >= 0x24 && op <= 0x27) ||
				    (op >= 0x2c && op <= 0x2f);
				cpu->cd.alpha.va = cpu->cd.alpha.exc_va;
				cpu->cd.alpha.mm_csr = (wr? EV4_MMCSR_WR : 0) |
				    (cpu->cd.alpha.exc_reg << 4) | (op << 9);
				cpu->cd.alpha.va_locked = 1;
			}
			break;
		case ALPHA_EXC_FEN:
			entry = EV4_ENTRY_FEN;
			break;
		default:entry = EV4_ENTRY_OPCDEC;
		}

		ev4_exception(cpu, entry, exc_addr);
		alpha_pc_to_pointers(cpu);
		return;
	}

	switch (type) {
	case ALPHA_EXC_ARITH:
		fatal("[ Alpha: arithmetic trap, pc 0x%016" PRIx64 ", exc_sum "
		    "0x%02" PRIx64 ", exc_mask 0x%016" PRIx64 " ]\n",
		    (uint64_t) cpu->pc, (uint64_t) cpu->cd.alpha.exc_sum,
		    (uint64_t) cpu->cd.alpha.exc_mask);
		break;
	case ALPHA_EXC_OPCDEC:
		fatal("[ Alpha: reserved instruction, pc 0x%016" PRIx64 " ]\n",
		    (uint64_t) cpu->pc);
		break;
	case ALPHA_EXC_UNALIGN:
		fatal("[ Alpha: unaligned access, pc 0x%016" PRIx64 ", va "
		    "0x%016" PRIx64 ", opcode 0x%02x, reg %i ]\n",
		    (uint64_t) cpu->pc, (uint64_t) cpu->cd.alpha.exc_va,
		    cpu->cd.alpha.exc_opcode, cpu->cd.alpha.exc_reg);
		break;
	case ALPHA_EXC_FEN:
		fatal("[ Alpha: floating point disabled, pc 0x%016" PRIx64
		    " ]\n", (uint64_t) cpu->pc);
		break;
	default:
		fatal("[ Alpha: exception %i, pc 0x%016" PRIx64 " ]\n",
		    type, (uint64_t) cpu->pc);
	}

	cpu->running = 0;
	cpu->cd.alpha.next_ic = &nothing_call;
}


/*
 *  Raise an arithmetic trap after the current instruction.
 */
static void alpha_arith_trap(struct cpu *cpu, struct alpha_instr_call *ic,
	uint64_t exc_sum, uint64_t exc_mask)
{
	SYNCH_PC;
	cpu->pc += sizeof(uint32_t);

	if (cpu->cd.alpha.ev4_palcode) {
		/*  EXC_SUM accumulates until written; SWC stays set only
		    while all the floating point traps were /S:  */
		int fp = (exc_mask >> 32) != 0;
		cpu->cd.alpha.exc_sum |= exc_sum & ~ALPHA_EXC_SUM_SWC;
		if (fp && !(exc_sum & ALPHA_EXC_SUM_SWC)) {
			cpu->cd.alpha.exc_sum &= ~ALPHA_EXC_SUM_SWC;
			cpu->cd.alpha.swc_blocked = 1;
		} else if (fp && !cpu->cd.alpha.swc_blocked)
			cpu->cd.alpha.exc_sum |= ALPHA_EXC_SUM_SWC;
		cpu->cd.alpha.exc_mask |= exc_mask;
	} else {
		cpu->cd.alpha.exc_sum = exc_sum;
		cpu->cd.alpha.exc_mask = exc_mask;
	}
	alpha_exception(cpu, ALPHA_EXC_ARITH);
}


/*
 *  alpha_unaligned():
 *
 *  Called by loads and stores (cpu_alpha_instr_loadstore.c) for an address
 *  that isn't naturally aligned. The opcode and register are taken from the
 *  instruction word.
 */
static void alpha_unaligned(struct cpu *cpu, struct alpha_instr_call *ic,
	uint64_t addr)
{
	unsigned char ib[4];
	uint32_t iword = 0;

	SYNCH_PC;
	if (cpu->memory_rw(cpu, cpu->mem, cpu->pc, ib, sizeof(ib),
	    MEM_READ, CACHE_INSTRUCTION | NO_EXCEPTIONS))
		iword = ib[0] | (ib[1] << 8) | (ib[2] << 16) |
		    ((uint32_t)ib[3] << 24);

	cpu->cd.alpha.exc_va = addr;
	cpu->cd.alpha.exc_opcode = iword >> 26;
	cpu->cd.alpha.exc_reg = (iword >> 21) & 31;
	cpu->n_translated_instrs --;
	alpha_exception(cpu, ALPHA_EXC_UNALIGN);
}


/*
 *  opcdec:  Reserved instruction (OPCDEC fault).
 */
X(opcdec)
{
	SYNCH_PC;
	cpu->n_translated_instrs --;
	alpha_exception(cpu, ALPHA_EXC_OPCDEC);
}


/*  FEN fault: floating point disabled, or left to software.  */
static void alpha_fen(struct cpu *cpu, struct alpha_instr_call *ic)
{
	SYNCH_PC;
	cpu->n_translated_instrs --;
	alpha_exception(cpu, ALPHA_EXC_FEN);
}


/*
 *  Called after a memory access in the slow path: if the access went to
 *  a physical address where nothing is, take the machine check now that
 *  the instruction is done; likewise an error interrupt that a device
 *  raised because of the access (irq_now).
 */
static void alpha_check_mchk(struct cpu *cpu, struct alpha_instr_call *ic)
{
	if (!cpu->cd.alpha.mchk_pending && !cpu->cd.alpha.irq_now)
		return;
	cpu->cd.alpha.irq_now = 0;
	SYNCH_PC;
	cpu->pc += sizeof(uint32_t);
	ev4_check_interrupts(cpu);
}


/*****************************************************************************
 *
 *  21064 PALmode instructions (opcodes 0x19, 0x1b, 0x1d, 0x1e, 0x1f). See
 *  cpu_alpha_ev4.c. Outside of PALmode they are reserved opcodes, unless
 *  ICCSR<HWE> allows them in kernel mode.
 */

/*
 *  hw_mfpr, hw_mtpr:  Move from/to an internal processor register.
 *
 *  arg[0] = pointer to the register (Ra)
 *  arg[1] = IPR number (instruction bits <7:0>)
 */
X(hw_mfpr)
{
	uint64_t x;

	if (!ev4_hw_allowed(cpu)) {
		instr(opcdec)(cpu, ic);
		return;
	}
	x = ev4_mfpr(cpu, ic->arg[1]);
	if ((uint64_t *)ic->arg[0] != &cpu->cd.alpha.r[ALPHA_ZERO])
		reg(ic->arg[0]) = x;
}

X(hw_mtpr)
{
	if (!ev4_hw_allowed(cpu)) {
		instr(opcdec)(cpu, ic);
		return;
	}
	ev4_mtpr(cpu, ic->arg[1], reg(ic->arg[0]), 1);

	/*  Outside of PALmode (with ICCSR<HWE>), this may have made an
	    interrupt deliverable:  */
	if (!cpu->cd.alpha.palmode && ev4_interrupt_pending(cpu)) {
		SYNCH_PC;
		cpu->pc += sizeof(uint32_t);
		ev4_check_interrupts(cpu);
	}
}


/*
 *  hw_rei:  Return from PALmode (or jump within it).
 */
X(hw_rei)
{
	if (!ev4_hw_allowed(cpu)) {
		instr(opcdec)(cpu, ic);
		return;
	}
	ev4_hw_rei(cpu);
	alpha_pc_to_pointers(cpu);
}


/*
 *  hw_ld, hw_st:  Physical, or alternate mode, load and store.
 *
 *  arg[0] = pointer to the register (Ra)
 *  arg[1] = pointer to the base register (Rb)
 *  arg[2] = the instruction word
 */
X(hw_ldst)
{
	uint32_t iword = ic->arg[2];
	uint64_t addr, x;
	int store = (iword >> 26) == 0x1f;

	if (!ev4_hw_allowed(cpu)) {
		instr(opcdec)(cpu, ic);
		return;
	}

	SYNCH_PC;
	addr = reg(ic->arg[1]) + (int64_t)((int32_t)(iword << 20) >> 20);
	x = reg(ic->arg[0]);
	if (!ev4_hw_ldst(cpu, iword, addr, &x)) {
		/*  An exception; continue at the handler.  */
		cpu->n_translated_instrs --;
		alpha_pc_to_pointers(cpu);
		return;
	}

	/*  Loads, and the store conditional's result:  */
	if ((!store || (iword & 0xc000) == 0xc000) &&
	    (uint64_t *)ic->arg[0] != &cpu->cd.alpha.r[ALPHA_ZERO])
		reg(ic->arg[0]) = x;

	alpha_check_mchk(cpu, ic);
}


/*****************************************************************************
 *
 *  Integer instructions that can trap on overflow (/V), and other
 *  integer instructions not generated by generate_alpha_misc.c.
 *
 *  arg[0] = pointer to destination uint64_t (Rc, possibly R31)
 *  arg[1] = pointer to source uint64_t (Ra)
 *  arg[2] = pointer to source uint64_t (Rb), or an 8-bit literal (_imm)
 *  The register number of Rc is needed for the trap's register write mask;
 *  it is found from arg[0].
 */

static void int_overflow(struct cpu *cpu, struct alpha_instr_call *ic)
{
	int rc = ((uint64_t *)ic->arg[0]) - &cpu->cd.alpha.r[0];
	alpha_arith_trap(cpu, ic, ALPHA_EXC_SUM_IOV, 1ULL << rc);
}

/*  Results are written to a scratch location when Rc is R31:  */
#define	INT_V_RESULT(x)	{						\
	if ((uint64_t *)ic->arg[0] != &cpu->cd.alpha.r[ALPHA_ZERO])	\
		reg(ic->arg[0]) = (x);					\
    }

#define	INT_V(name, expr_b, body)					\
X(name)									\
{									\
	uint64_t a = reg(ic->arg[1]), b = (expr_b), res;		\
	int ovf;							\
	body								\
	INT_V_RESULT(res);						\
	if (ovf)							\
		int_overflow(cpu, ic);					\
}

#define	ADDL_V	{ int64_t s = (int64_t)(int32_t)a + (int64_t)(int32_t)b;\
	res = (int64_t)(int32_t)s; ovf = s != (int64_t)res; }
#define	SUBL_V	{ int64_t s = (int64_t)(int32_t)a - (int64_t)(int32_t)b;\
	res = (int64_t)(int32_t)s; ovf = s != (int64_t)res; }
#define	ADDQ_V	{ res = a + b;						\
	ovf = (~(a ^ b) & (a ^ res)) >> 63; }
#define	SUBQ_V	{ res = a - b;						\
	ovf = ((a ^ b) & (a ^ res)) >> 63; }
#define	MULL_V	{ int64_t s = (int64_t)(int32_t)a * (int64_t)(int32_t)b;\
	res = (int64_t)(int32_t)s; ovf = s != (int64_t)res; }

static uint64_t umul_hi(uint64_t a, uint64_t b)
{
	uint64_t a0 = a & 0xffffffff, a1 = a >> 32;
	uint64_t b0 = b & 0xffffffff, b1 = b >> 32;
	uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
	uint64_t mid = (p00 >> 32) + (p01 & 0xffffffff) + (p10 & 0xffffffff);
	return p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/*  Signed 64x64 multiply overflows if the high half isn't the sign
    extension of the low half:  */
#define	MULQ_V	{ uint64_t hi = umul_hi(a, b);				\
	if ((int64_t)a < 0) hi -= b;					\
	if ((int64_t)b < 0) hi -= a;					\
	res = a * b;							\
	ovf = hi != (uint64_t)((int64_t)res >> 63); }

INT_V(addl_v, reg(ic->arg[2]), ADDL_V)
INT_V(addl_v_imm, ic->arg[2], ADDL_V)
INT_V(subl_v, reg(ic->arg[2]), SUBL_V)
INT_V(subl_v_imm, ic->arg[2], SUBL_V)
INT_V(addq_v, reg(ic->arg[2]), ADDQ_V)
INT_V(addq_v_imm, ic->arg[2], ADDQ_V)
INT_V(subq_v, reg(ic->arg[2]), SUBQ_V)
INT_V(subq_v_imm, ic->arg[2], SUBQ_V)
INT_V(mull_v, reg(ic->arg[2]), MULL_V)
INT_V(mull_v_imm, ic->arg[2], MULL_V)
INT_V(mulq_v, reg(ic->arg[2]), MULQ_V)
INT_V(mulq_v_imm, ic->arg[2], MULQ_V)

X(umulh_imm)
{
	reg(ic->arg[0]) = umul_hi(reg(ic->arg[1]), ic->arg[2]);
}

/*  AMASK: clear the bits of the architecture extensions that exist.  */
X(amask)
{
	reg(ic->arg[0]) = reg(ic->arg[2]) &
	    ~(uint64_t)cpu->cd.alpha.cpu_type.features;
}
X(amask_imm)
{
	reg(ic->arg[0]) = ic->arg[2] &
	    ~(uint64_t)cpu->cd.alpha.cpu_type.features;
}

/*
 *  rc, rs:  Read and clear/set the interrupt flag.
 *
 *  arg[0] = pointer to destination uint64_t (Ra, possibly R31)
 */
X(rc)
{
	reg(ic->arg[0]) = cpu->cd.alpha.intr_flag;
	cpu->cd.alpha.intr_flag = 0;
	cpu->cd.alpha.r[ALPHA_ZERO] = 0;
}
X(rs)
{
	reg(ic->arg[0]) = cpu->cd.alpha.intr_flag;
	cpu->cd.alpha.intr_flag = 1;
	cpu->cd.alpha.r[ALPHA_ZERO] = 0;
}


/*****************************************************************************
 *
 *  Floating point
 */

/*
 *  fp_operate:  Floating point operate instruction (opcode 0x15..0x17).
 *
 *  arg[0] = the instruction word
 */
X(fp_operate)
{
	int exc;

	FEN_CHECK

	exc = alpha_fp_operate(cpu, ic->arg[0]);
	if (exc == 0)
		return;

	if (exc == ALPHA_FP_OPCDEC) {
		instr(opcdec)(cpu, ic);
		return;
	}
	if (exc == ALPHA_FP_FEN) {
		alpha_fen(cpu, ic);
		return;
	}

	alpha_arith_trap(cpu, ic, exc, cpu->cd.alpha.trap_mask);
}


/*
 *  Floating point loads and stores that convert between the memory and the
 *  register format (LDF, LDG, LDS, STF, STG, STS). These go through the
 *  slow memory_rw() path. (LDT and STT, which don't convert, use the
 *  ordinary load/store functions.)
 *
 *  arg[0] = pointer to the floating point register
 *  arg[1] = pointer to the base register
 *  arg[2] = offset (as an int32_t)
 *  arg[3] is not available, so the kind of conversion is in the function.
 */
static int fp_mem(struct cpu *cpu, struct alpha_instr_call *ic,
	unsigned char *buf, int len, int writeflag)
{
	uint64_t addr = reg(ic->arg[1]) + (int32_t)ic->arg[2];

	if (cpu->cd.alpha.ev4_palcode &&
	    !(cpu->cd.alpha.iccsr & EV4_ICCSR_FPE)) {
		alpha_fen(cpu, ic);
		return 0;
	}
	if (addr & (len - 1)) {
		alpha_unaligned(cpu, ic, addr);
		return 0;
	}
	SYNCH_PC;
	if (!cpu->memory_rw(cpu, cpu->mem, addr, buf, len, writeflag,
	    CACHE_DATA)) {
		if (cpu->cd.alpha.ev4_palcode) {
			/*  A memory management fault; continue at the
			    PALcode entry point.  */
			cpu->n_translated_instrs --;
			alpha_pc_to_pointers(cpu);
			return 0;
		}
		fatal("[ Alpha: fp load/store failed: TODO ]\n");
		cpu->running = 0;
		cpu->cd.alpha.next_ic = &nothing_call;
		return 0;
	}
	return 1;
}

static uint32_t le32(unsigned char *b)
{
	return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t le64(unsigned char *b)
{
	return le32(b) | ((uint64_t)le32(b + 4) << 32);
}

static void put_le32(unsigned char *b, uint32_t x)
{
	b[0] = x; b[1] = x >> 8; b[2] = x >> 16; b[3] = x >> 24;
}

static void put_le64(unsigned char *b, uint64_t x)
{
	put_le32(b, x);
	put_le32(b + 4, x >> 32);
}

X(ldf)
{
	unsigned char b[4];
	if (fp_mem(cpu, ic, b, sizeof(b), MEM_READ))
		reg(ic->arg[0]) = alpha_ldf_to_reg(le32(b));
}
X(lds)
{
	unsigned char b[4];
	if (fp_mem(cpu, ic, b, sizeof(b), MEM_READ))
		reg(ic->arg[0]) = alpha_lds_to_reg(le32(b));
}
X(ldg)
{
	unsigned char b[8];
	if (fp_mem(cpu, ic, b, sizeof(b), MEM_READ))
		reg(ic->arg[0]) = alpha_swap_g(le64(b));
}
X(stf)
{
	unsigned char b[4];
	put_le32(b, alpha_reg_to_stf(reg(ic->arg[0])));
	fp_mem(cpu, ic, b, sizeof(b), MEM_WRITE);
}
X(sts)
{
	unsigned char b[4];
	put_le32(b, alpha_reg_to_sts(reg(ic->arg[0])));
	fp_mem(cpu, ic, b, sizeof(b), MEM_WRITE);
}
X(stg)
{
	unsigned char b[8];
	put_le64(b, alpha_swap_g(reg(ic->arg[0])));
	fp_mem(cpu, ic, b, sizeof(b), MEM_WRITE);
}

/*  LDT and STT with real PALcode (which may disable floating point):  */
X(ldt_fen)
{
	unsigned char b[8];
	if (fp_mem(cpu, ic, b, sizeof(b), MEM_READ)) {
		reg(ic->arg[0]) = le64(b);
		alpha_check_mchk(cpu, ic);
	}
}
X(stt_fen)
{
	unsigned char b[8];
	put_le64(b, reg(ic->arg[0]));
	if (fp_mem(cpu, ic, b, sizeof(b), MEM_WRITE))
		alpha_check_mchk(cpu, ic);
}


#include "tmp_alpha_misc.c"


/*****************************************************************************/


X(end_of_page)
{
	/*  Update the PC:  (offset 0, but on the next page)  */
	cpu->pc &= ~((ALPHA_IC_ENTRIES_PER_PAGE-1)
	    << ALPHA_INSTR_ALIGNMENT_SHIFT);
	cpu->pc += (ALPHA_IC_ENTRIES_PER_PAGE
	    << ALPHA_INSTR_ALIGNMENT_SHIFT);

	/*  Find the new physical page and update the translation pointers:  */
	alpha_pc_to_pointers(cpu);

	/*  end_of_page doesn't count as an executed instruction:  */
	cpu->n_translated_instrs --;
}


/*****************************************************************************/


/*
 *  alpha_instr_to_be_translated():
 *
 *  Translate an instruction word into an alpha_instr_call. ic is filled in with
 *  valid data for the translated instruction, or a "nothing" instruction if
 *  there was a translation failure. The newly translated instruction is then
 *  executed.
 */
X(to_be_translated)
{
	uint64_t addr, low_pc;
	uint32_t iword;
	unsigned char *page;
	unsigned char ib[4];
	void (*samepage_function)(struct cpu *, struct alpha_instr_call *);
	int opcode, ra, rb, func, rc, imm, load, loadstore_type, fp, llsc;

	/*  Figure out the (virtual) address of the instruction:  */
	low_pc = ((size_t)ic - (size_t)cpu->cd.alpha.cur_ic_page)
	    / sizeof(struct alpha_instr_call);
	addr = cpu->pc & ~((ALPHA_IC_ENTRIES_PER_PAGE-1) <<
	    ALPHA_INSTR_ALIGNMENT_SHIFT);
	addr += (low_pc << ALPHA_INSTR_ALIGNMENT_SHIFT);
	addr &= ~((1 << ALPHA_INSTR_ALIGNMENT_SHIFT) - 1);
	cpu->pc = addr;

	/*  Read the instruction word from memory:  */
	{
		const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
		const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
		const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
		uint32_t x1 = (addr >> (64-DYNTRANS_L1N)) & mask1;
		uint32_t x2 = (addr >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
		uint32_t x3 = (addr >> (64-DYNTRANS_L1N-DYNTRANS_L2N-
		    DYNTRANS_L3N)) & mask3;
		struct DYNTRANS_L2_64_TABLE *l2 = cpu->cd.alpha.l1_64[x1];
		struct DYNTRANS_L3_64_TABLE *l3 = l2->l3[x2];
		page = l3->host_load[x3];
	}

	if (page != NULL) {
		/*  fatal("TRANSLATION HIT!\n");  */
		memcpy(ib, page + (addr & 8191), sizeof(ib));
	} else {
		/*  fatal("TRANSLATION MISS!\n");  */
		if (!cpu->memory_rw(cpu, cpu->mem, addr, &ib[0],
		    sizeof(ib), MEM_READ, CACHE_INSTRUCTION)) {
			fatal("to_be_translated(): read failed: TODO\n");
			goto bad;
		}
	}

	/*  Alpha instruction words are always little-endian. Convert
	    to host order:  */
	{
		uint32_t *p = (uint32_t *) ib;
		iword = LE32_TO_HOST( *p );
	}

#define DYNTRANS_TO_BE_TRANSLATED_HEAD
#include "cpu_dyntrans.c"
#undef	DYNTRANS_TO_BE_TRANSLATED_HEAD


	opcode = (iword >> 26) & 63;
	ra = (iword >> 21) & 31;
	rb = (iword >> 16) & 31;
	func = (iword >> 5) & 0x7ff;
	rc = iword & 31;
	imm = iword & 0xffff;

	switch (opcode) {
	case 0x00:						/*  CALL_PAL  */
		ic->f = instr(call_pal);
		ic->arg[0] = (size_t) (iword & 0x3ffffff);
		break;
	case 0x08:						/*  LDA  */
	case 0x09:						/*  LDAH  */
		if (ra == ALPHA_ZERO) {
			ic->f = instr(nop);
			break;
		}
		/*  TODO: A special case which is common is to add or subtract
		    a small offset from sp.  */
		ic->f = instr(lda);
		ic->arg[0] = (size_t) &cpu->cd.alpha.r[ra];
		ic->arg[1] = (size_t) &cpu->cd.alpha.r[rb];
		if (rb == ALPHA_ZERO)
			ic->f = instr(lda_0);
		ic->arg[2] = (ssize_t)(int16_t)imm;
		if (opcode == 0x09)
			ic->arg[2] <<= 16;
		break;
	case 0x0b:						/*  LDQ_U  */
	case 0x0f:						/*  STQ_U  */
		if (ra == ALPHA_ZERO && opcode == 0x0b) {
			ic->f = instr(nop);
			break;
		}
		if (opcode == 0x0b)
			ic->f = instr(ldq_u);
		else
			ic->f = instr(stq_u);
		ic->arg[0] = (size_t) &cpu->cd.alpha.r[ra];
		ic->arg[1] = (size_t) &cpu->cd.alpha.r[rb];
		ic->arg[2] = (ssize_t)(int16_t)imm;
		break;
	case 0x0a:
	case 0x0c:
	case 0x0d:
	case 0x0e:
		/*  LDBU, LDWU, STW, STB: only with the BWX extension.  */
		if (!(cpu->cd.alpha.cpu_type.features & ALPHA_FEATURE_BWX)) {
			ic->f = instr(opcdec);
			break;
		}
		/*  Fall through.  */
	case 0x23:
	case 0x27:
	case 0x28:
	case 0x29:
	case 0x2a:
	case 0x2b:
	case 0x2c:
	case 0x2d:
	case 0x2e:
	case 0x2f:
		loadstore_type = 0; fp = 0; load = 0; llsc = 0;
		switch (opcode) {
		case 0x0a: loadstore_type = 0; load = 1; break;	/*  ldbu  */
		case 0x0c: loadstore_type = 1; load = 1; break;	/*  ldwu  */
		case 0x0d: loadstore_type = 1; break;		/*  stw  */
		case 0x0e: loadstore_type = 0; break;		/*  stb  */
		case 0x23: loadstore_type = 3; load = 1; fp = 1; break; /*ldt*/
		case 0x27: loadstore_type = 3; fp = 1; break;	/*  stt  */
		case 0x28: loadstore_type = 2; load = 1; break;	/*  ldl  */
		case 0x29: loadstore_type = 3; load = 1; break;	/*  ldq  */
		case 0x2a: loadstore_type = 2; load = llsc = 1; break;/* ldl_l*/
		case 0x2b: loadstore_type = 3; load = llsc = 1; break;/* ldq_l*/
		case 0x2c: loadstore_type = 2; break;		/*  stl  */
		case 0x2d: loadstore_type = 3; break;		/*  stq  */
		case 0x2e: loadstore_type = 2; llsc = 1; break;	/*  stl_c  */
		case 0x2f: loadstore_type = 3; llsc = 1; break;	/*  stq_c  */
		}
		ic->f = alpha_loadstore[
		    loadstore_type + (imm==0? 4 : 0) + 8 * load
		    + 16 * llsc];
		if (fp && cpu->cd.alpha.ev4_palcode)
			ic->f = load? instr(ldt_fen) : instr(stt_fen);
		/*  Load to the zero register is treated as a prefetch
		    hint. It is ignored here.  */
		if (load && ra == ALPHA_ZERO) {
			ic->f = instr(nop);
			break;
		}
		if (fp)
			ic->arg[0] = (size_t) &cpu->cd.alpha.f[ra];
		else
			ic->arg[0] = (size_t) &cpu->cd.alpha.r[ra];
		ic->arg[1] = (size_t) &cpu->cd.alpha.r[rb];
		ic->arg[2] = (ssize_t)(int16_t)imm;
		break;
	case 0x20:						/*  LDF  */
	case 0x21:						/*  LDG  */
	case 0x22:						/*  LDS  */
	case 0x24:						/*  STF  */
	case 0x25:						/*  STG  */
	case 0x26:						/*  STS  */
		switch (opcode) {
		case 0x20: ic->f = instr(ldf); break;
		case 0x21: ic->f = instr(ldg); break;
		case 0x22: ic->f = instr(lds); break;
		case 0x24: ic->f = instr(stf); break;
		case 0x25: ic->f = instr(stg); break;
		case 0x26: ic->f = instr(sts); break;
		}
		/*  A load to F31 is a prefetch hint.  */
		if (opcode <= 0x22 && ra == ALPHA_ZERO) {
			ic->f = instr(nop);
			break;
		}
		ic->arg[0] = (size_t) &cpu->cd.alpha.f[ra];
		ic->arg[1] = (size_t) &cpu->cd.alpha.r[rb];
		ic->arg[2] = (ssize_t)(int16_t)imm;
		break;
	case 0x10:
		/*  The /V forms (addl/v, subl/v, addq/v, subq/v) trap on
		    overflow even if Rc is R31:  */
		if (rc == ALPHA_ZERO && (func & 0x76) != 0x40 &&
		    (func & 0x76) != 0x60) {
			ic->f = instr(nop);
			break;
		}
		ic->arg[0] = (size_t) &cpu->cd.alpha.r[rc];
		ic->arg[1] = (size_t) &cpu->cd.alpha.r[ra];
		if (func & 0x80)
			ic->arg[2] = (size_t)((rb << 3) + (func >> 8));
		else
			ic->arg[2] = (size_t) &cpu->cd.alpha.r[rb];
		switch (func & 0xff) {
		case 0x00: ic->f = instr(addl); break;
		case 0x02: ic->f = instr(s4addl); break;
		case 0x09: ic->f = instr(subl); break;
		case 0x0b: ic->f = instr(s4subl); break;
		case 0x0f: ic->f = instr(cmpbge); break;
		case 0x12: ic->f = instr(s8addl); break;
		case 0x1b: ic->f = instr(s8subl); break;
		case 0x1d: ic->f = instr(cmpult); break;
		case 0x20: ic->f = instr(addq); break;
		case 0x22: ic->f = instr(s4addq); break;
		case 0x29: ic->f = instr(subq); break;
		case 0x2b: ic->f = instr(s4subq); break;
		case 0x2d: ic->f = instr(cmpeq); break;
		case 0x32: ic->f = instr(s8addq); break;
		case 0x3b: ic->f = instr(s8subq); break;
		case 0x3d: ic->f = instr(cmpule); break;
		case 0x40: ic->f = instr(addl_v); break;
		case 0x49: ic->f = instr(subl_v); break;
		case 0x4d: ic->f = instr(cmplt); break;
		case 0x60: ic->f = instr(addq_v); break;
		case 0x69: ic->f = instr(subq_v); break;
		case 0x6d: ic->f = instr(cmple); break;

		case 0x80: ic->f = instr(addl_imm); break;
		case 0x82: ic->f = instr(s4addl_imm); break;
		case 0x89: ic->f = instr(subl_imm); break;
		case 0x8b: ic->f = instr(s4subl_imm); break;
		case 0x8f: ic->f = instr(cmpbge_imm); break;
		case 0x92: ic->f = instr(s8addl_imm); break;
		case 0x9b: ic->f = instr(s8subl_imm); break;
		case 0x9d: ic->f = instr(cmpult_imm); break;
		case 0xa0: ic->f = instr(addq_imm); break;
		case 0xa2: ic->f = instr(s4addq_imm); break;
		case 0xa9: ic->f = instr(subq_imm); break;
		case 0xab: ic->f = instr(s4subq_imm); break;
		case 0xad: ic->f = instr(cmpeq_imm); break;
		case 0xb2: ic->f = instr(s8addq_imm); break;
		case 0xbb: ic->f = instr(s8subq_imm); break;
		case 0xbd: ic->f = instr(cmpule_imm); break;
		case 0xc0: ic->f = instr(addl_v_imm); break;
		case 0xc9: ic->f = instr(subl_v_imm); break;
		case 0xcd: ic->f = instr(cmplt_imm); break;
		case 0xe0: ic->f = instr(addq_v_imm); break;
		case 0xe9: ic->f = instr(subq_v_imm); break;
		case 0xed: ic->f = instr(cmple_imm); break;

		default:ic->f = instr(opcdec);
		}
		break;
	case 0x11:
		if (rc == ALPHA_ZERO) {
			ic->f = instr(nop);
			break;
		}
		ic->arg[0] = (size_t) &cpu->cd.alpha.r[rc];
		ic->arg[1] = (size_t) &cpu->cd.alpha.r[ra];
		if (func & 0x80)
			ic->arg[2] = (size_t)((rb << 3) + (func >> 8));
		else
			ic->arg[2] = (size_t) &cpu->cd.alpha.r[rb];
		switch (func & 0xff) {
		case 0x00: ic->f = instr(and); break;
		case 0x08: ic->f = instr(andnot); break;
		case 0x14: ic->f = instr(cmovlbs); break;
		case 0x16: ic->f = instr(cmovlbc); break;
		case 0x20: ic->f = instr(or);
			   if (ra == ALPHA_ZERO || rb == ALPHA_ZERO) {
				if (ra == ALPHA_ZERO)
					ra = rb;
				ic->f = alpha_mov_r_r[ra + rc*32];
			   }
			   break;
		case 0x24: ic->f = instr(cmoveq); break;
		case 0x26: ic->f = instr(cmovne); break;
		case 0x28: ic->f = instr(ornot); break;
		case 0x40: ic->f = instr(xor); break;
		case 0x44: ic->f = instr(cmovlt); break;
		case 0x46: ic->f = instr(cmovge); break;
		case 0x48: ic->f = instr(xornot); break;
		case 0x61: ic->f = instr(amask); break;
		case 0x64: ic->f = instr(cmovle); break;
		case 0x66: ic->f = instr(cmovgt); break;
		case 0x6c: ic->f = instr(implver); break;
		case 0x80: ic->f = instr(and_imm); break;
		case 0x88: ic->f = instr(andnot_imm); break;
		case 0x94: ic->f = instr(cmovlbs_imm); break;
		case 0x96: ic->f = instr(cmovlbc_imm); break;
		case 0xa0: ic->f = instr(or_imm); break;
		case 0xa4: ic->f = instr(cmoveq_imm); break;
		case 0xa6: ic->f = instr(cmovne_imm); break;
		case 0xa8: ic->f = instr(ornot_imm); break;
		case 0xc0: ic->f = instr(xor_imm); break;
		case 0xc4: ic->f = instr(cmovlt_imm); break;
		case 0xc6: ic->f = instr(cmovge_imm); break;
		case 0xc8: ic->f = instr(xornot_imm); break;
		case 0xe4: ic->f = instr(cmovle_imm); break;
		case 0xe1: ic->f = instr(amask_imm); break;
		case 0xe6: ic->f = instr(cmovgt_imm); break;
		case 0xec: ic->f = instr(implver); break;
		default:ic->f = instr(opcdec);
		}
		break;
	case 0x12:
		if (rc == ALPHA_ZERO) {
			ic->f = instr(nop);
			break;
		}
		ic->arg[0] = (size_t) &cpu->cd.alpha.r[rc];
		ic->arg[1] = (size_t) &cpu->cd.alpha.r[ra];
		if (func & 0x80)
			ic->arg[2] = (size_t)((rb << 3) + (func >> 8));
		else
			ic->arg[2] = (size_t) &cpu->cd.alpha.r[rb];
		switch (func & 0xff) {
		case 0x02: ic->f = instr(mskbl); break;
		case 0x06: ic->f = instr(extbl); break;
		case 0x0b: ic->f = instr(insbl); break;
		case 0x12: ic->f = instr(mskwl); break;
		case 0x16: ic->f = instr(extwl); break;
		case 0x1b: ic->f = instr(inswl); break;
		case 0x22: ic->f = instr(mskll); break;
		case 0x26: ic->f = instr(extll); break;
		case 0x2b: ic->f = instr(insll); break;
		case 0x30: ic->f = instr(zap); break;
		case 0x31: ic->f = instr(zapnot); break;
		case 0x32: ic->f = instr(mskql); break;
		case 0x34: ic->f = instr(srl); break;
		case 0x36: ic->f = instr(extql); break;
		case 0x39: ic->f = instr(sll); break;
		case 0x3b: ic->f = instr(insql); break;
		case 0x3c: ic->f = instr(sra); break;
		case 0x52: ic->f = instr(mskwh); break;
		case 0x57: ic->f = instr(inswh); break;
		case 0x5a: ic->f = instr(extwh); break;
		case 0x62: ic->f = instr(msklh); break;
		case 0x67: ic->f = instr(inslh); break;
		case 0x6a: ic->f = instr(extlh); break;
		case 0x72: ic->f = instr(mskqh); break;
		case 0x77: ic->f = instr(insqh); break;
		case 0x7a: ic->f = instr(extqh); break;
		case 0x82: ic->f = instr(mskbl_imm); break;
		case 0x86: ic->f = instr(extbl_imm); break;
		case 0x8b: ic->f = instr(insbl_imm); break;
		case 0x92: ic->f = instr(mskwl_imm); break;
		case 0x96: ic->f = instr(extwl_imm); break;
		case 0x9b: ic->f = instr(inswl_imm); break;
		case 0xa2: ic->f = instr(mskll_imm); break;
		case 0xa6: ic->f = instr(extll_imm); break;
		case 0xab: ic->f = instr(insll_imm); break;
		case 0xb0: ic->f = instr(zap_imm); break;
		case 0xb1: ic->f = instr(zapnot_imm); break;
		case 0xb2: ic->f = instr(mskql_imm); break;
		case 0xb4: ic->f = instr(srl_imm); break;
		case 0xb6: ic->f = instr(extql_imm); break;
		case 0xb9: ic->f = instr(sll_imm); break;
		case 0xbb: ic->f = instr(insql_imm); break;
		case 0xbc: ic->f = instr(sra_imm); break;
		case 0xd2: ic->f = instr(mskwh_imm); break;
		case 0xd7: ic->f = instr(inswh_imm); break;
		case 0xda: ic->f = instr(extwh_imm); break;
		case 0xe2: ic->f = instr(msklh_imm); break;
		case 0xe7: ic->f = instr(inslh_imm); break;
		case 0xea: ic->f = instr(extlh_imm); break;
		case 0xf2: ic->f = instr(mskqh_imm); break;
		case 0xf7: ic->f = instr(insqh_imm); break;
		case 0xfa: ic->f = instr(extqh_imm); break;
		default:ic->f = instr(opcdec);
		}
		break;
	case 0x13:
		/*  The /V forms (mull/v, mulq/v) trap on overflow even if
		    Rc is R31:  */
		if (rc == ALPHA_ZERO && (func & 0x7f) != 0x40 &&
		    (func & 0x7f) != 0x60) {
			ic->f = instr(nop);
			break;
		}
		ic->arg[0] = (size_t) &cpu->cd.alpha.r[rc];
		ic->arg[1] = (size_t) &cpu->cd.alpha.r[ra];
		if (func & 0x80)
			ic->arg[2] = (size_t)((rb << 3) + (func >> 8));
		else
			ic->arg[2] = (size_t) &cpu->cd.alpha.r[rb];
		// TODO: mulq/v etc? overflow detection
		// bit 0..6 are function, but 7 is "imm" bit.
		switch (func & 0xff) {
		case 0x00: ic->f = instr(mull); break;
		case 0x20: ic->f = instr(mulq); break;
		case 0x30: ic->f = instr(umulh); break;
		case 0x40: ic->f = instr(mull_v); break;
		case 0x60: ic->f = instr(mulq_v); break;
		case 0x80: ic->f = instr(mull_imm); break;
		case 0xa0: ic->f = instr(mulq_imm); break;
		case 0xb0: ic->f = instr(umulh_imm); break;
		case 0xc0: ic->f = instr(mull_v_imm); break;
		case 0xe0: ic->f = instr(mulq_v_imm); break;
		default:ic->f = instr(opcdec);
		}
		break;
	case 0x15:		/*  VAX floating point operate  */
	case 0x16:		/*  IEEE floating point operate  */
	case 0x17:		/*  Data type independent floating point  */
		ic->f = instr(fp_operate);
		ic->arg[0] = iword;
		break;
	case 0x14:		/*  FIX extension (not in the 21064)  */
	case 0x1c:		/*  BWX/CIX/MVI extensions  */
		if (cpu->cd.alpha.cpu_type.features == 0) {
			ic->f = instr(opcdec);
			break;
		}
		if (!cpu->translation_readahead)
			fatal("[ Alpha: unimplemented opcode 0x%02x ]\n",
			    opcode);
		goto bad;
	case 0x18:
		switch (iword & 0xffff) {
		case 0x0000:	/*  trapb  */
		case 0x0400:	/*  excb  */
		case 0x4000:	/*  mb  */
		case 0x4400:	/*  wmb  */
		case 0x8000:	/*  fetch  */
		case 0xa000:	/*  fetch_m  */
			ic->f = instr(nop);
			break;
		case 0xe000:	/*  rc  ra  */
		case 0xf000:	/*  rs  ra  */
			ic->arg[0] = (size_t) &cpu->cd.alpha.r[ra];
			ic->f = (iword & 0x1000)? instr(rs) : instr(rc);
			break;
		case 0xc000:	/*  rdcc  ra  */
			if (ra == ALPHA_ZERO) {
				ic->f = instr(nop);
				break;
			}
			ic->arg[0] = (size_t) &cpu->cd.alpha.r[ra];
			ic->f = instr(rdcc);
			break;
		default:ic->f = instr(opcdec);
		}
		break;
	case 0x1a:
		switch ((iword >> 14) & 3) {
		case 0:	/*  JMP  */
		case 1:	/*  JSR  */
		case 2:	/*  RET  */
		case 3:	/*  JSR_COROUTINE  */
			ic->arg[0] = (size_t) &cpu->cd.alpha.r[ra];
			ic->arg[1] = (size_t) &cpu->cd.alpha.r[rb];
			if (ra == ALPHA_ZERO) {
				if (cpu->machine->show_trace_tree &&
				    rb == ALPHA_RA)
					ic->f = instr(jsr_0_trace);
				else
					ic->f = instr(jsr_0);
			} else {
				if (cpu->machine->show_trace_tree)
					ic->f = instr(jsr_trace);
				else
					ic->f = instr(jsr);
			}
			break;
		}
		break;
	case 0x30:						/*  BR    */
	case 0x31:						/*  FBEQ  */
	case 0x32:						/*  FBLT  */
	case 0x33:						/*  FBLE  */
	case 0x34:						/*  BSR   */
	case 0x35:						/*  FBNE  */
	case 0x36:						/*  FBGE  */
	case 0x37:						/*  FBGT  */
	case 0x38:						/*  BLBC  */
	case 0x39:						/*  BEQ   */
	case 0x3a:						/*  BLT   */
	case 0x3b:						/*  BLE   */
	case 0x3c:						/*  BLBS  */
	case 0x3d:						/*  BNE   */
	case 0x3e:						/*  BGE   */
	case 0x3f:						/*  BGT   */
		/*  To avoid a GCC warning:  */
		samepage_function = instr(nop);
		fp = 0;
		switch (opcode) {
		case 0x30:
		case 0x34:
			ic->f = instr(br);
			samepage_function = instr(br_samepage);
			if (ra != ALPHA_ZERO) {
				ic->f = instr(br_return);
				samepage_function = instr(br_return_samepage);
			}
			break;
		case 0x38:
			ic->f = instr(blbc);
			samepage_function = instr(blbc_samepage);
			break;
		case 0x31:
			fp = 1;
			ic->f = instr(fbeq);
			samepage_function = instr(fbeq_samepage);
			break;
		case 0x32:
			fp = 1;
			ic->f = instr(fblt);
			samepage_function = instr(fblt_samepage);
			break;
		case 0x33:
			fp = 1;
			ic->f = instr(fble);
			samepage_function = instr(fble_samepage);
			break;
		case 0x35:
			fp = 1;
			ic->f = instr(fbne);
			samepage_function = instr(fbne_samepage);
			break;
		case 0x36:
			fp = 1;
			ic->f = instr(fbge);
			samepage_function = instr(fbge_samepage);
			break;
		case 0x37:
			fp = 1;
			ic->f = instr(fbgt);
			samepage_function = instr(fbgt_samepage);
			break;
		case 0x39:
			ic->f = instr(beq);
			samepage_function = instr(beq_samepage);
			break;
		case 0x3a:
			ic->f = instr(blt);
			samepage_function = instr(blt_samepage);
			break;
		case 0x3b:
			ic->f = instr(ble);
			samepage_function = instr(ble_samepage);
			break;
		case 0x3c:
			ic->f = instr(blbs);
			samepage_function = instr(blbs_samepage);
			break;
		case 0x3d:
			ic->f = instr(bne);
			samepage_function = instr(bne_samepage);
			break;
		case 0x3e:
			ic->f = instr(bge);
			samepage_function = instr(bge_samepage);
			break;
		case 0x3f:
			ic->f = instr(bgt);
			samepage_function = instr(bgt_samepage);
			break;
		}
		if (fp)
			ic->arg[1] = (size_t) &cpu->cd.alpha.f[ra];
		else
			ic->arg[1] = (size_t) &cpu->cd.alpha.r[ra];
		ic->arg[0] = (iword & 0x001fffff) << 2;
		/*  Sign-extend:  */
		if (ic->arg[0] & 0x00400000)
			ic->arg[0] |= 0xffffffffff800000ULL;
		/*  Branches are calculated as PC + 4 + offset.  */
		ic->arg[0] = (size_t)(ic->arg[0] + 4);
		/*  Special case: branch within the same page:  */
		{
			uint64_t mask_within_page =
			    ((ALPHA_IC_ENTRIES_PER_PAGE-1) << 2) | 3;
			uint64_t old_pc = addr;
			uint64_t new_pc = old_pc + (int32_t)ic->arg[0];
			if ((old_pc & ~mask_within_page) ==
			    (new_pc & ~mask_within_page)) {
				ic->f = samepage_function;
				ic->arg[0] = (size_t) (
				    cpu->cd.alpha.cur_ic_page +
				    ((new_pc & mask_within_page) >> 2));
			}
		}
		break;
	case 0x19:		/*  HW_MFPR  */
	case 0x1d:		/*  HW_MTPR  */
		if (!cpu->cd.alpha.ev4_palcode) {
			ic->f = instr(opcdec);
			break;
		}
		ic->f = opcode == 0x19? instr(hw_mfpr) : instr(hw_mtpr);
		ic->arg[0] = (size_t) &cpu->cd.alpha.r[ra];
		ic->arg[1] = iword & 0xff;
		break;
	case 0x1b:		/*  HW_LD  */
	case 0x1f:		/*  HW_ST  */
		if (!cpu->cd.alpha.ev4_palcode) {
			ic->f = instr(opcdec);
			break;
		}
		ic->f = instr(hw_ldst);
		ic->arg[0] = (size_t) &cpu->cd.alpha.r[ra];
		ic->arg[1] = (size_t) &cpu->cd.alpha.r[rb];
		ic->arg[2] = iword;
		break;
	case 0x1e:		/*  HW_REI  */
		ic->f = cpu->cd.alpha.ev4_palcode? instr(hw_rei) :
		    instr(opcdec);
		break;
	default:
		/*  Reserved opcodes:  */
		ic->f = instr(opcdec);
	}


#define DYNTRANS_TO_BE_TRANSLATED_TAIL
#include "cpu_dyntrans.c"
#undef	DYNTRANS_TO_BE_TRANSLATED_TAIL
}

