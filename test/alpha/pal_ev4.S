/*
 *  21064 privileged architecture test, for gxemul's "testalphapal" machine.
 *
 *  This is a tiny PALcode of its own: the vectors at physical 0 (PAL_BASE
 *  is 0 after reset), the reset code at 0x4000, and a test driver that runs
 *  in native kernel mode through the KSEG superpage. Every exception that
 *  isn't one of the PALcode's own services is logged (entry, EXC_ADDR and
 *  two more registers that depend on the entry) and then resumed; check()
 *  copies the last log record into the result array.
 *
 *  Linked at KSEG 0xfffffc0000000000 (= physical 0). The result lines are
 *  described in pal_ev4.list; expected/pal_ev4.txt is derived by hand from
 *  the Hardware Reference Manual.
 */

	.set	noreorder
	.set	noat

/*  21064 PALmode instructions:  */
#define	MFPR(ra, ipr)	.long (0x19 << 26) | ((ra) << 21) | ((ra) << 16) | (ipr)
#define	MTPR(ra, ipr)	.long (0x1d << 26) | ((ra) << 21) | ((ra) << 16) | (ipr)
#define	HW_LDQ_P(ra, disp, rb)	.long (0x1b << 26) | ((ra) << 21) | \
	((rb) << 16) | 0x9000 | ((disp) & 0xfff)
#define	HW_STQ_P(ra, disp, rb)	.long (0x1f << 26) | ((ra) << 21) | \
	((rb) << 16) | 0x9000 | ((disp) & 0xfff)
#define	HW_REI		.long (0x1e << 26) | (31 << 21) | (31 << 16) | 0x8000

/*  IPRs:  */
#define	PT(n)		(0x80 | (n))
#define	TB_TAG		0x20
#define	ITB_PTE		0x21
#define	ICCSR		0x22
#define	EXC_ADDR	0x24
#define	ITBZAP		0x26
#define	PS		0x29
#define	EXC_SUM		0x2a
#define	PAL_BASE	0x2b
#define	HIRR		0x2c
#define	SIRR		0x2d
#define	SIER		0x31
#define	TB_CTL		0x40
#define	DTB_PTE		0x42
#define	DTB_PTE_TEMP	0x43
#define	MM_CSR		0x44
#define	VA		0x45
#define	DTBZAP		0x46
#define	BIU_ADDR	0x49
#define	BIU_STAT	0x4a
#define	DC_STAT		0x4c
#define	ABOX_CTL	0x4e
#define	CC_CTL		0x51

/*  pal_data layout:  */
#define	D_COUNT		0
#define	D_ACTION	8	/*  0 = skip/resume, 1 = fill the TB  */
#define	D_PTE		16	/*  PTE for a fill  */
#define	D_KRET		24	/*  kernel return address (user mode)  */
#define	D_LOG		32	/*  16 records of 4 quadwords  */

/*  A vector: save r1/r2, entry offset to r1, go to the common code.  */
#define	VECTOR(off)		\
	.org	off;		\
	MTPR(1, PT(0));		\
	MTPR(2, PT(1));		\
	lda	$1, off($31);	\
	br	$31, common

	.text
	.globl	_start

/*****************************************************************************
 *  Vectors
 */
	VECTOR(0x0020)		/*  MCHK  */
	VECTOR(0x0060)		/*  ARITH  */
	VECTOR(0x00e0)		/*  INTERRUPT  */
	VECTOR(0x01e0)		/*  D_FAULT  */
	VECTOR(0x03e0)		/*  ITB_MISS  */
	VECTOR(0x07e0)		/*  ITB_ACV  */
	VECTOR(0x08e0)		/*  DTB_MISS native  */
	VECTOR(0x09e0)		/*  DTB_MISS PAL  */
	VECTOR(0x11e0)		/*  UNALIGN  */
	VECTOR(0x13e0)		/*  OPCDEC  */
	VECTOR(0x17e0)		/*  FEN  */
	VECTOR(0x2000)		/*  CALL_PAL 0x00 (halt)  */

	/*  CALL_PAL 0x01: enter user mode at a0. Kernel return address
	    (the instruction after the CALL_PAL) is kept in pal_data.  */
	.org	0x2040
	br	$1, 1f
1:	lda	$1, (pal_data - 1b)($1)
	MFPR(2, EXC_ADDR)
	HW_STQ_P(2, D_KRET, 1)
	lda	$1, 0x18($31)		/*  PS<CM> = 3, user  */
	MTPR(1, PS)
	MTPR(16, EXC_ADDR)
	HW_REI

	/*  CALL_PAL 0x02: DTB fill, a0 = va, a1 = pte (8 KB pages).  */
	.org	0x2080
	MTPR(31, TB_CTL)
	MTPR(16, TB_TAG)
	MTPR(17, DTB_PTE)
	HW_REI

	/*  CALL_PAL 0x03: ITB fill, a0 = va, a1 = pte.  */
	.org	0x20c0
	MTPR(31, TB_CTL)
	MTPR(16, TB_TAG)
	MTPR(17, ITB_PTE)
	HW_REI

	/*  CALL_PAL 0x04 .. 0x07: write SIRR, SIER, ICCSR, ABOX_CTL.  */
	.org	0x2100
	MTPR(16, SIRR)
	HW_REI
	.org	0x2140
	MTPR(16, SIER)
	HW_REI
	.org	0x2180
	MTPR(16, ICCSR)
	HW_REI
	.org	0x21c0
	MTPR(16, ABOX_CTL)
	HW_REI

	/*  CALL_PAL 0x83 (unprivileged): back from user mode.  */
	.org	0x30c0
	br	$1, 1f
1:	lda	$1, (pal_data - 1b)($1)
	MTPR(31, PS)
	HW_LDQ_P(2, D_KRET, 1)
	MTPR(2, EXC_ADDR)
	HW_REI

	/*  CALL_PAL 0x84 (unprivileged): logged like an exception.  */
	VECTOR(0x3100)


/*****************************************************************************
 *  Common exception code: log, then resume.  r1 = entry offset.
 */
	.org	0x3800
common:
	MTPR(3, PT(2))
	MTPR(4, PT(3))
	MTPR(5, PT(4))
	MTPR(6, PT(5))
	br	$2, 1f
1:	lda	$2, (pal_data - 1b)($2)		/*  r2 = pal_data (physical)  */

	HW_LDQ_P(3, D_COUNT, 2)
	sll	$3, 5, $4
	addq	$4, $2, $4			/*  r4 = record - D_LOG  */
	addq	$3, 1, $3
	HW_STQ_P(3, D_COUNT, 2)
	HW_STQ_P(1, D_LOG + 0, 4)
	MFPR(3, EXC_ADDR)
	HW_STQ_P(3, D_LOG + 8, 4)
	mov	$31, $5
	mov	$31, $6

	/*  Two more registers, depending on the entry:  */
	lda	$3, -0x60($1)
	beq	$3, log_arith
	lda	$3, -0x20($1)
	beq	$3, log_mchk
	lda	$3, -0xe0($1)
	beq	$3, log_intr
	lda	$3, -0x1e0($1)
	beq	$3, log_dstream
	lda	$3, -0x8e0($1)
	beq	$3, log_dstream
	lda	$3, -0x9e0($1)
	beq	$3, log_dstream
	lda	$3, -0x11e0($1)
	beq	$3, log_dstream
	br	$31, log_done

log_dstream:
	MFPR(6, MM_CSR)
	MFPR(5, VA)			/*  (unlocks VA and MM_CSR)  */
	br	$31, log_done
log_mchk:
	MFPR(6, BIU_STAT)
	MFPR(5, BIU_ADDR)		/*  (unlocks)  */
	br	$31, log_done
log_intr:
	MFPR(5, HIRR)
	MTPR(31, SIRR)			/*  clear software interrupts  */
	br	$31, log_done
log_arith:
	/*  EXC_SUM; each read also gives one bit of the write mask, F31
	    first, then F30 .. F0, I31 .. I0 (64 reads in all).  */
	MFPR(5, EXC_SUM)
	srl	$5, 33, $6
	and	$6, 1, $6
	srl	$5, 2, $5		/*  keep bits <8:2>  */
	and	$5, 0x7f, $5
	sll	$5, 2, $5
	lda	$3, 63($31)
2:	MFPR(4, EXC_SUM)
	srl	$4, 33, $4
	and	$4, 1, $4
	sll	$6, 1, $6
	or	$6, $4, $6
	subq	$3, 1, $3
	bne	$3, 2b
	MTPR(31, EXC_SUM)
	HW_LDQ_P(3, D_COUNT, 2)		/*  r4 was clobbered: recompute  */
	subq	$3, 1, $3
	sll	$3, 5, $4
	addq	$4, $2, $4

log_done:
	HW_STQ_P(5, D_LOG + 16, 4)
	HW_STQ_P(6, D_LOG + 24, 4)

	/*  Resume.  */
	MFPR(3, EXC_ADDR)
	lda	$4, -0x60($1)
	beq	$4, resume_trap
	lda	$4, -0x20($1)
	beq	$4, resume_trap
	lda	$4, -0xe0($1)
	beq	$4, resume
	lda	$4, -0x3100($1)
	beq	$4, resume
	lda	$4, -0x3e0($1)
	beq	$4, fill_itb
	HW_LDQ_P(4, D_ACTION, 2)
	beq	$4, skip
	lda	$4, -0x8e0($1)
	beq	$4, fill_dtb
	lda	$4, -0x9e0($1)
	beq	$4, fill_dtb
skip:
	addq	$3, 4, $3
	br	$31, resume_at

resume_trap:
	/*  EXC_ADDR<1> says whether the address is 4 too high.  */
	subq	$3, 2, $3
	bic	$3, 2, $3
	br	$31, resume_at

fill_dtb:
	HW_LDQ_P(4, D_PTE, 2)
	HW_STQ_P(31, D_ACTION, 2)
	MFPR(5, VA)
	MTPR(31, TB_CTL)
	MTPR(5, TB_TAG)
	MTPR(4, DTB_PTE)
	br	$31, resume_at

fill_itb:
	HW_LDQ_P(4, D_PTE, 2)
	MTPR(31, TB_CTL)
	MTPR(3, TB_TAG)
	MTPR(4, ITB_PTE)
	br	$31, resume_at

resume_at:
	MTPR(3, EXC_ADDR)
resume:
	MFPR(1, PT(0))
	MFPR(2, PT(1))
	MFPR(3, PT(2))
	MFPR(4, PT(3))
	MFPR(5, PT(4))
	MFPR(6, PT(5))
	HW_REI


/*****************************************************************************
 *  Reset: PALmode, physical. IPR tests, then native mode.
 */
	.org	0x4000
_start:
	br	$10, 1f
1:	lda	$11, (ipr_res - 1b)($10)	/*  r11 = ipr_res (physical)  */
	lda	$12, (consts - 1b)($10)

	MTPR(31, DTBZAP)
	MTPR(31, ITBZAP)
	MTPR(31, EXC_SUM)

	/*  1: PAL_TEMP  */
	HW_LDQ_P(1, 0, 12)
	MTPR(1, PT(17))
	MFPR(31, 0)
	MFPR(2, PT(17))
	HW_STQ_P(2, 0, 11)

	/*  2: PAL_BASE keeps bits <33:14>  */
	lda	$1, -1($31)
	MTPR(1, PAL_BASE)
	MFPR(2, PAL_BASE)
	HW_STQ_P(2, 8, 11)
	MTPR(31, PAL_BASE)

	/*  3: ICCSR: write FPE and MAP, read format has them at 23, 22  */
	lda	$1, 3($31)
	sll	$1, 41, $1
	MTPR(1, ICCSR)
	MFPR(2, ICCSR)
	HW_STQ_P(2, 16, 11)

	/*  4: EXC_ADDR  */
	lda	$1, 0x1234($31)
	MTPR(1, EXC_ADDR)
	MFPR(2, EXC_ADDR)
	HW_STQ_P(2, 24, 11)

	/*  5: DC_STAT (CHIP_ID)  */
	MFPR(2, DC_STAT)
	HW_STQ_P(2, 32, 11)

	/*  6: PS read format: CM1 at 34, CM0 at 1  */
	lda	$1, 0x18($31)
	MTPR(1, PS)
	MFPR(2, PS)
	HW_STQ_P(2, 40, 11)
	MTPR(31, PS)

	/*  7: DTB entry read back through DTB_PTE_TEMP: entry 0 is
	    written, then 32 reads go around to it.  */
	HW_LDQ_P(1, 8, 12)		/*  PTE  */
	lda	$2, 0x4000($31)
	sll	$2, 16, $2		/*  tag 0x40000000  */
	MTPR(31, TB_CTL)
	MTPR(2, TB_TAG)
	MTPR(1, DTB_PTE)
	lda	$3, 32($31)
2:	MFPR(31, DTB_PTE)
	subq	$3, 1, $3
	bne	$3, 2b
	MFPR(2, DTB_PTE_TEMP)
	HW_STQ_P(2, 48, 11)
	MTPR(31, DTBZAP)

	/*  8: the cycle counter counts when enabled  */
	lda	$1, 1($31)
	sll	$1, 32, $1
	MTPR(1, CC_CTL)
	rpcc	$1
	lda	$3, 1000($31)
2:	subq	$3, 1, $3
	bne	$3, 2b
	rpcc	$2
	subl	$2, $1, $2
	cmplt	$31, $2, $2
	HW_STQ_P(2, 56, 11)

	/*  Superpages, and off to native mode (KSEG):  */
	lda	$1, 0x20($31)		/*  ABOX_CTL<SPE_2>  */
	MTPR(1, ABOX_CTL)
	lda	$1, (native_main - 1b)($10)
	lda	$2, -1024($31)
	sll	$2, 32, $2		/*  0xfffffc0000000000  */
	or	$1, $2, $1
	MTPR(1, EXC_ADDR)
	MFPR(31, 0)
	HW_REI

	.align	3
consts:
	.quad	0x0123456789abcdef
	.quad	0x0000012300001102	/*  PFN 0x123, KWE|KRE, FOR  */
ipr_res:
	.space	64
pal_data:
	.space	D_LOG + 16 * 32


/*****************************************************************************
 *  Test driver: native kernel mode, KSEG.
 */
	.align	4
native_main:
	br	$29, 1f
1:	ldgp	$29, 0($29)
	lda	$30, stack_top
	lda	$9, results

	/*  The IPR results:  */
	lda	$1, ipr_res
	lda	$2, 8($31)
2:	ldq	$3, 0($1)
	stq	$3, 0($9)
	lda	$1, 8($1)
	lda	$9, 8($9)
	subq	$2, 1, $2
	bne	$2, 2b

	/*  CALL_PAL 0x84 (unprivileged): EXC_ADDR is the next instruction  */
	call_pal 0x84
t_callpal:
	lda	$16, t_callpal
	bsr	$26, check

	/*  HW_MFPR outside of PALmode: OPCDEC, EXC_ADDR is the instruction  */
t_opcdec:
	MFPR(1, EXC_ADDR)
	lda	$16, t_opcdec
	bsr	$26, check

	/*  Unaligned load: UNALIGN, VA and MM_CSR  */
	lda	$1, data_page
t_unal:	ldq	$2, 4($1)
	lda	$16, t_unal
	bsr	$26, check
	ldq	$3, -16($9)		/*  VA, relative to data_page  */
	lda	$4, data_page
	subq	$3, $4, $3
	stq	$3, -16($9)

	/*  Not a valid virtual address: D_FAULT with ACV  */
	lda	$1, 1($31)
	sll	$1, 62, $1
t_badva:
	ldq	$3, 0($1)
	lda	$16, t_badva
	bsr	$26, check

	/*  DTB miss, filled by the handler, then the load succeeds  */
	lda	$1, data_page
	zapnot	$1, 0x1f, $1		/*  physical address of data_page  */
	srl	$1, 13, $1
	sll	$1, 32, $1
	lda	$10, 0x1100($1)		/*  KRE|KWE  */
	lda	$2, pal_data
	stq	$10, D_PTE($2)
	lda	$3, 1($31)
	stq	$3, D_ACTION($2)
	ldah	$4, 0x2000($31)		/*  va 0x20000000  */
t_dtbmiss:
	ldq	$5, 8($4)
	lda	$16, t_dtbmiss
	bsr	$26, check
	stq	$5, 0($9)		/*  the value loaded  */
	lda	$9, 8($9)

	/*  Write to a page with FOW: D_FAULT, MM_CSR has FOW and WR  */
	lda	$17, 4($10)		/*  KRE|KWE|FOW  */
	ldah	$16, 0x2200($31)	/*  va 0x22000000  */
	call_pal 0x02
t_fow:	stq	$31, 0($16)
	lda	$16, t_fow
	bsr	$26, check

	/*  ITB miss: jump to va 0x30000000, which the handler maps to
	    itb_page (KRE); the code there returns.  */
	lda	$1, itb_page
	zapnot	$1, 0x1f, $1
	srl	$1, 13, $1
	sll	$1, 32, $1
	lda	$1, 0x100($1)		/*  KRE  */
	lda	$2, pal_data
	stq	$1, D_PTE($2)
	ldah	$27, 0x3000($31)
	jsr	$26, ($27)
	ldah	$16, 0x3000($31)
	bsr	$26, check

	/*  Integer overflow trap: EXC_SUM<IOV>, mask bit 3 (r3)  */
	lda	$1, 1($31)
	sll	$1, 62, $1
t_iov:	addq/v	$1, $1, $3
	lda	$16, t_iov
	bsr	$26, check

	/*  Division by zero (DZE), mask bit 32+5 (f5)  */
	lda	$1, fp_consts
	ldt	$f1, 0($1)
t_dze:	divt	$f1, $f31, $f5
	trapb
	lda	$16, t_dze
	bsr	$26, check

	/*  DIVT/SUI always traps with INE on the 21064, and SWC  */
	lda	$1, fp_consts
	ldt	$f2, 8($1)
t_divi:	divt/sui $f1, $f2, $f6
	trapb
	lda	$16, t_divi
	bsr	$26, check

	/*  The FPCR records exceptions that don't trap (here underflow and
	    inexact, without /U), and SUM  */
	lda	$1, fp_consts
	ldt	$f3, 16($1)
	mt_fpcr	$f31
	mult	$f3, $f3, $f4
	mf_fpcr	$f5
	stt	$f4, 0($9)		/*  the product: a true zero  */
	stt	$f5, 8($9)
	lda	$9, 16($9)

	/*  Rounding towards minus infinity: FEN on the 21064  */
t_minf:	addt/m	$f1, $f2, $f7
	lda	$16, t_minf
	bsr	$26, check

	/*  Floating point disabled: FEN  */
	lda	$16, 1($31)
	sll	$16, 41, $16		/*  MAP only  */
	call_pal 0x06
t_fen:	addt	$f1, $f2, $f7
	lda	$16, t_fen
	bsr	$26, check
	lda	$16, 3($31)
	sll	$16, 41, $16
	call_pal 0x06

	/*  Software interrupt, level 3: taken when CALL_PAL 0x04 returns,
	    HIRR shows SIRR<3> and SWR  */
	lda	$16, 1($31)
	sll	$16, 35, $16
	call_pal 0x05			/*  SIER  */
	call_pal 0x04			/*  SIRR  */
t_intr:	lda	$16, t_intr
	bsr	$26, check

	/*  Machine check: a load from a physical address with nothing,
	    after enabling machine checks; EXC_ADDR is the next instruction
	    (with bit 1 set)  */
	lda	$16, 0x22($31)		/*  SPE_2 | MCHK_EN  */
	call_pal 0x07
	lda	$1, -1024($31)
	sll	$1, 32, $1
	lda	$2, 1($31)
	sll	$2, 33, $2
	or	$1, $2, $1		/*  KSEG + 0x200000000  */
	ldq	$11, 0($1)
t_mchk:	lda	$16, t_mchk
	bsr	$26, check
	stq	$11, 0($9)		/*  the load completed, all ones  */
	lda	$9, 8($9)

	/*  User mode: map user_page at va 0x10000 (ITB, URE) and data_page
	    at va 0x12000 (DTB, URE only), and run user_code there.  */
	lda	$1, user_page
	zapnot	$1, 0x1f, $1
	srl	$1, 13, $1
	sll	$1, 32, $1
	lda	$17, 0x900($1)		/*  URE|KRE  */
	lda	$16, 0x1000($31)
	sll	$16, 4, $16		/*  0x10000  */
	call_pal 0x03
	lda	$1, data_page
	zapnot	$1, 0x1f, $1
	srl	$1, 13, $1
	sll	$1, 32, $1
	lda	$17, 0x900($1)
	lda	$16, 0x1200($31)
	sll	$16, 4, $16		/*  0x12000  */
	call_pal 0x02
	lda	$2, pal_data
	stq	$31, D_COUNT($2)
	lda	$16, 0x1000($31)
	sll	$16, 4, $16
	call_pal 0x01
	/*  Back in kernel mode: the value read, and the four log records  */
	stq	$20, 0($9)
	lda	$9, 8($9)
	lda	$2, pal_data
	ldq	$3, D_COUNT($2)
	stq	$3, 0($9)
	lda	$9, 8($9)
	lda	$4, D_LOG($2)
	lda	$5, 16($31)
2:	ldq	$3, 0($4)
	stq	$3, 0($9)
	lda	$4, 8($4)
	lda	$9, 8($9)
	subq	$5, 1, $5
	bne	$5, 2b

	/*  Print the results and stop.  */
	lda	$16, results
	subq	$9, $16, $17
	srl	$17, 3, $17
	bsr	$26, print_results
	bsr	$26, cons_base
	stl	$31, 0x10($0)
3:	br	3b


/*
 *  check(a0 = address of the instruction): the last log record, with
 *  EXC_ADDR relative to a0, and the number of records since the previous
 *  check(), to the results.
 */
check:
	lda	$1, pal_data
	ldq	$2, D_COUNT($1)
	stq	$2, 0($9)
	beq	$2, 1f
	subq	$2, 1, $2
	sll	$2, 5, $2
	addq	$2, $1, $2
	ldq	$3, D_LOG + 0($2)
	stq	$3, 8($9)
	ldq	$3, D_LOG + 8($2)
	subq	$3, $16, $3
	stq	$3, 16($9)
	ldq	$3, D_LOG + 16($2)
	stq	$3, 24($9)
	ldq	$3, D_LOG + 24($2)
	stq	$3, 32($9)
	br	$31, 2f
1:	stq	$31, 8($9)
	stq	$31, 16($9)
	stq	$31, 24($9)
	stq	$31, 32($9)
2:	lda	$9, 40($9)
	stq	$31, D_COUNT($1)
	ret	$31, ($26), 1


/*  Console (KSEG address of the test machine's console device).  */
	.globl	rt_putc
cons_base:
	lda	$0, -1024($31)
	sll	$0, 32, $0
	ldah	$0, 0x1000($0)
	ret	$31, ($26), 1
rt_putc:
	mov	$26, $1
	bsr	$26, cons_base
	mov	$1, $26
	stl	$16, 0($0)
	ret	$31, ($26), 1
	.globl	rt_flush
rt_flush:
	ret	$31, ($26), 1

	.align	3
fp_consts:
	.double	1.0
	.double	3.0
	.quad	0x1ff0000000000000	/*  2^-512: its square underflows  */


/*****************************************************************************
 *  Pages that get mapped by the translation buffers.
 */
	.align	13
itb_page:
	ret	$31, ($26), 1

	.align	13
user_page:
	/*  va 0x10000, user mode  */
	lda	$1, 0x1200($31)
	sll	$1, 4, $1		/*  0x12000  */
	ldq	$20, 8($1)		/*  0x10008: readable  */
	stq	$20, 0($1)		/*  0x1000c: D_FAULT, ACV (read-only)  */
	call_pal 0x00			/*  0x10010: OPCDEC (privileged)  */
	lda	$2, -1024($31)		/*  0x10014  */
	sll	$2, 32, $2		/*  0x10018  */
	ldq	$3, 0($2)		/*  0x1001c: no superpage in user mode,
					    so a DTB miss  */
	call_pal 0x83			/*  0x10020: back to kernel mode  */

	.align	13
data_page:
	.quad	0x1111111111111111
	.quad	0x2222222222222222

	.data
	.align	3
results:
	.space	8 * 200

	.bss
	.align	4
	.space	16384
stack_top:
