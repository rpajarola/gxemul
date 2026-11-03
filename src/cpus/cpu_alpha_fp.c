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
 *  Alpha floating point operate instructions (opcodes 0x15, 0x16, 0x17),
 *  as implemented by the 21064 (EV4).
 *
 *  All arithmetic is done in software on an unpacked format, so that the
 *  results are the same on every host, and so that the VAX formats (F, G,
 *  D) and the EV4's handling of IEEE special values can be done exactly:
 *
 *    o  IEEE operands that are not finite numbers (infinity, NaN) or that
 *	 are denormal cause an invalid operation trap. (The 21064 leaves
 *	 these to software completion, i.e. to the operating system.)
 *    o  Results that underflow are a true zero (+0, even where IEEE would
 *	 give -0), unless underflow traps are enabled (/U), in which case
 *	 there is an underflow trap.
 *    o  Overflow, invalid operation and division by zero always trap.
 *	 Inexact traps only with /SUI.
 *    o  The FPCR records every exception that occurs, whether it traps or
 *	 not. Divides never set FPCR[INE]; DIVS/SUI and DIVT/SUI always
 *	 trap with EXC_SUM[INE] instead (the 21064 doesn't compute the
 *	 inexact flag of a division).
 *    o  IEEE rounding towards plus or minus infinity (/M, or /D with
 *	 FPCR[DYN] = 1 or 3), and IEEE instructions with the reserved data
 *	 type 1, raise a FEN exception; the operating system completes them
 *	 in software. (Other Alphas implement these rounding modes.)
 *
 *  Hardware Reference Manual sections 2.6, 2.7 and Table 4-1.
 *
 *  Traps are reported precisely, right after the instruction. (Real
 *  hardware reports them some instructions later; precise reporting is
 *  one of the allowed behaviours.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "misc.h"


/*  Value classes:  */
#define	AFP_ZERO	0
#define	AFP_NUM		1
#define	AFP_SPECIAL	2	/*  IEEE Inf/NaN/denormal, VAX reserved operand  */

/*
 *  An unpacked value: for AFP_NUM, value = mant * 2^(exp - 63), with bit 63
 *  of mant set. Bit 0 of mant may hold a "sticky" bit (something nonzero
 *  below), which never matters for anything but rounding.
 */
struct afp {
	int		sign;
	int		cls;
	int		exp;
	uint64_t	mant;
};

/*  Rounding modes:  */
#define	RND_CHOPPED	0
#define	RND_MINUS	1
#define	RND_NORMAL	2	/*  IEEE: to nearest, ties to even  */
#define	RND_PLUS	3
#define	RND_VAX		4	/*  VAX: to nearest, ties away from zero  */

/*  Formats:  */
#define	FMT_S		0
#define	FMT_T		1
#define	FMT_F		2
#define	FMT_G		3
#define	FMT_D		4


static int count_leading_zeros64(uint64_t x)
{
	int n = 0;
	if (x == 0)
		return 64;
	while (!(x & 0x8000000000000000ULL)) {
		x <<= 1;
		n ++;
	}
	return n;
}


/*  Shift right, keeping a sticky bit for anything shifted out.  */
static uint64_t shr_sticky(uint64_t x, int n)
{
	if (n <= 0)
		return x;
	if (n >= 64)
		return x != 0;
	return (x >> n) | ((x & ((1ULL << n) - 1)) != 0);
}


/*  Normalize: value = m * 2^k (m != 0).  */
static void afp_normalize(struct afp *r, uint64_t m, int k)
{
	int lz = count_leading_zeros64(m);
	r->cls = AFP_NUM;
	r->mant = m << lz;
	r->exp = k + 63 - lz;
}


static void mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
	uint64_t a0 = a & 0xffffffff, a1 = a >> 32;
	uint64_t b0 = b & 0xffffffff, b1 = b >> 32;
	uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
	uint64_t mid = (p00 >> 32) + (p01 & 0xffffffff) + (p10 & 0xffffffff);

	*lo = (mid << 32) | (p00 & 0xffffffff);
	*hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}


/*****************************************************************************
 *
 *  Unpacking
 */

/*  IEEE S or T, register format (S is held in T layout).  */
static void unpack_ieee(uint64_t x, struct afp *r)
{
	int e = (x >> 52) & 0x7ff;
	uint64_t f = x & 0x000fffffffffffffULL;

	r->sign = x >> 63;
	if (e == 0x7ff || (e == 0 && f != 0)) {
		r->cls = AFP_SPECIAL;
		return;
	}
	if (e == 0) {
		r->cls = AFP_ZERO;
		return;
	}
	r->cls = AFP_NUM;
	r->exp = e - 1023;
	r->mant = 0x8000000000000000ULL | (f << 11);
}

/*
 *  VAX F or G, register format. Both have an 11-bit exponent in the
 *  register (F's 8-bit exponent is mapped into it when loaded), with
 *  value = 0.1fff... * 2^(exp - 1024). An exponent of zero is a zero if
 *  the sign is clear, and a reserved operand otherwise.
 */
static void unpack_vax_fg(uint64_t x, struct afp *r, int fmt)
{
	int e = (x >> 52) & 0x7ff;
	uint64_t f = x & 0x000fffffffffffffULL;

	if (fmt == FMT_F)
		f &= 0x000fffffe0000000ULL;
	r->sign = x >> 63;
	if (e == 0) {
		r->cls = r->sign? AFP_SPECIAL : AFP_ZERO;
		return;
	}
	r->cls = AFP_NUM;
	r->exp = e - 1025;
	r->mant = 0x8000000000000000ULL | (f << 11);
}

/*  VAX D, register format: exponent<62:55>, fraction<54:0>.  */
static void unpack_vax_d(uint64_t x, struct afp *r)
{
	int e = (x >> 55) & 0xff;
	uint64_t f = x & 0x007fffffffffffffULL;

	r->sign = x >> 63;
	if (e == 0) {
		r->cls = r->sign? AFP_SPECIAL : AFP_ZERO;
		return;
	}
	r->cls = AFP_NUM;
	r->exp = e - 129;
	r->mant = 0x8000000000000000ULL | (f << 8);
}

static void unpack_int(uint64_t x, struct afp *r)
{
	uint64_t mag = x;

	r->sign = x >> 63;
	if (r->sign)
		mag = -x;
	if (mag == 0) {
		r->cls = AFP_ZERO;
		return;
	}
	afp_normalize(r, mag, 0);
}


/*****************************************************************************
 *
 *  Rounding and packing
 */

/*
 *  Round a AFP_NUM value to p significant bits. Returns non-zero if the
 *  result is inexact.
 */
static int afp_round(struct afp *x, int p, int mode)
{
	int shift = 64 - p;
	uint64_t keep = x->mant >> shift;
	uint64_t lost = x->mant & ((1ULL << shift) - 1);
	uint64_t half = 1ULL << (shift - 1);
	int up = 0;

	switch (mode) {
	case RND_CHOPPED: up = 0; break;
	case RND_MINUS:   up = x->sign && lost; break;
	case RND_PLUS:    up = !x->sign && lost; break;
	case RND_NORMAL:  up = lost > half || (lost == half && (keep & 1));
			  break;
	case RND_VAX:     up = lost >= half; break;
	}

	if (up) {
		keep ++;
		if (keep == (1ULL << p)) {
			keep >>= 1;
			x->exp ++;
		}
	}
	x->mant = keep << shift;
	return lost != 0;
}

/*  Exception bits, as in EXC_SUM:  */
#define	EXC_SWC		ALPHA_EXC_SUM_SWC
#define	EXC_INV		ALPHA_EXC_SUM_INV
#define	EXC_DZE		ALPHA_EXC_SUM_DZE
#define	EXC_FOV		ALPHA_EXC_SUM_FOV
#define	EXC_UNF		ALPHA_EXC_SUM_UNF
#define	EXC_INE		ALPHA_EXC_SUM_INE
#define	EXC_IOV		ALPHA_EXC_SUM_IOV

/*
 *  Round and pack a value into a register. *exc gets the exceptions that
 *  occurred (EXC_FOV, EXC_UNF, EXC_INE); the caller decides which trap.
 *  Underflow gives a true zero.
 */
static uint64_t afp_pack(struct afp *x, int fmt, int mode, int *exc)
{
	int p, emin, emax, inexact;
	uint64_t sign = (uint64_t)x->sign << 63;
	uint64_t frac;

	if (x->cls == AFP_ZERO) {
		if (fmt == FMT_S || fmt == FMT_T)
			return sign;
		return 0;
	}

	switch (fmt) {
	case FMT_S: p = 24; emin = -126;  emax = 127;  break;
	case FMT_T: p = 53; emin = -1022; emax = 1023; break;
	case FMT_F: p = 24; emin = -128;  emax = 126;  break;
	case FMT_G: p = 53; emin = -1024; emax = 1022; break;
	default:    p = 56; emin = -128;  emax = 126;  break;	/*  D  */
	}

	inexact = afp_round(x, p, mode);
	if (inexact)
		*exc |= EXC_INE;

	if (x->exp > emax) {
		*exc |= EXC_FOV | EXC_INE;
		if (fmt == FMT_S || fmt == FMT_T) {
			/*  Infinity, or the largest number when rounding
			    towards zero:  */
			int to_inf = mode == RND_NORMAL ||
			    (mode == RND_PLUS && !x->sign) ||
			    (mode == RND_MINUS && x->sign);
			if (to_inf)
				return sign | 0x7ff0000000000000ULL;
			if (fmt == FMT_T)
				return sign | 0x7fefffffffffffffULL;
			return sign | 0x47efffffe0000000ULL;
		}
		return 0;
	}
	if (x->exp < emin) {
		/*  A nonzero result flushed to zero is also inexact.  */
		*exc |= EXC_UNF | EXC_INE;
		return 0;
	}

	frac = (x->mant << 1);		/*  Drop the hidden bit.  */
	switch (fmt) {
	case FMT_S:
		return sign | ((uint64_t)(x->exp + 1023) << 52) |
		    ((frac >> 12) & 0x000fffffe0000000ULL);
	case FMT_T:
		return sign | ((uint64_t)(x->exp + 1023) << 52) | (frac >> 12);
	case FMT_F:
		return sign | ((uint64_t)(x->exp + 1025) << 52) |
		    ((frac >> 12) & 0x000fffffe0000000ULL);
	case FMT_G:
		return sign | ((uint64_t)(x->exp + 1025) << 52) | (frac >> 12);
	default:
		return sign | ((uint64_t)(x->exp + 129) << 55) | (frac >> 9);
	}
}

/*
 *  Convert to a 64-bit integer, rounding as given. *exc gets EXC_INE if
 *  inexact and EXC_IOV if the integer doesn't fit; the result is then the
 *  low 64 bits of the integer.
 */
static uint64_t afp_to_int(struct afp *x, int mode, int *exc)
{
	uint64_t ival, frac;
	int half, rest, up = 0, overflow = 0;

	if (x->cls == AFP_ZERO)
		return 0;

	if (x->exp < 0) {
		ival = 0;
		half = x->exp == -1;
		rest = x->exp == -1? (x->mant << 1) != 0 : 1;
	} else if (x->exp < 63) {
		ival = x->mant >> (63 - x->exp);
		frac = x->mant << (x->exp + 1);
		half = frac >> 63;
		rest = (frac << 1) != 0;
	} else if (x->exp == 63) {
		/*  Only -2^63 fits.  */
		ival = x->mant;
		half = rest = 0;
	} else {
		ival = x->exp < 127? x->mant << (x->exp - 63) : 0;
		half = rest = 0;
		overflow = 1;
	}

	switch (mode) {
	case RND_CHOPPED: up = 0; break;
	case RND_MINUS:   up = x->sign && (half || rest); break;
	case RND_PLUS:    up = !x->sign && (half || rest); break;
	case RND_NORMAL:  up = half && (rest || (ival & 1)); break;
	case RND_VAX:     up = half; break;
	}

	if (half || rest)
		*exc |= EXC_INE;
	ival += up;

	if (!overflow) {
		if (x->sign)
			overflow = ival > 0x8000000000000000ULL;
		else
			overflow = ival > 0x7fffffffffffffffULL;
	}
	if (overflow)
		*exc |= EXC_IOV;

	return x->sign? -ival : ival;
}


/*****************************************************************************
 *
 *  Arithmetic on unpacked values (AFP_ZERO or AFP_NUM only)
 */

static void afp_add(struct afp *a, struct afp *b, int mode, struct afp *r)
{
	uint64_t ma, mb, m;
	int ea, eb;

	if (a->cls == AFP_ZERO && b->cls == AFP_ZERO) {
		r->cls = AFP_ZERO;
		if (a->sign == b->sign)
			r->sign = a->sign;
		else
			r->sign = mode == RND_MINUS;
		return;
	}
	if (a->cls == AFP_ZERO) {
		*r = *b;
		return;
	}
	if (b->cls == AFP_ZERO) {
		*r = *a;
		return;
	}

	if (a->exp < b->exp) {
		struct afp *tmp = a;
		a = b;
		b = tmp;
	}

	/*  Shift both right by one so that the sum can't overflow:  */
	ma = a->mant >> 1;
	mb = b->mant >> 1;
	ea = a->exp;
	eb = b->exp;
	mb = shr_sticky(mb, ea - eb);

	if (a->sign == b->sign) {
		m = ma + mb;
		r->sign = a->sign;
	} else if (ma >= mb) {
		m = ma - mb;
		r->sign = a->sign;
	} else {
		m = mb - ma;
		r->sign = b->sign;
	}

	if (m == 0) {
		r->cls = AFP_ZERO;
		r->sign = mode == RND_MINUS;
		return;
	}

	afp_normalize(r, m, ea - 62);
}

static void afp_mul(struct afp *a, struct afp *b, struct afp *r)
{
	uint64_t hi, lo;

	r->sign = a->sign ^ b->sign;
	if (a->cls == AFP_ZERO || b->cls == AFP_ZERO) {
		r->cls = AFP_ZERO;
		return;
	}

	mul64(a->mant, b->mant, &hi, &lo);
	afp_normalize(r, hi | (lo != 0), a->exp + b->exp - 62);
}

/*  b must not be zero.  */
static void afp_div(struct afp *a, struct afp *b, struct afp *r)
{
	uint64_t q = 0, rem, mb = b->mant;
	int i, carry = 0;

	r->sign = a->sign ^ b->sign;
	if (a->cls == AFP_ZERO) {
		r->cls = AFP_ZERO;
		return;
	}

	/*  q = floor(ma / mb * 2^63), by long division:  */
	rem = a->mant;
	for (i = 63; i >= 0; i--) {
		if (carry || rem >= mb) {
			rem -= mb;
			q |= 1ULL << i;
		}
		carry = rem >> 63;
		rem <<= 1;
	}

	afp_normalize(r, q, a->exp - b->exp - 63);
	r->mant |= (rem != 0 || carry);
}

/*  Returns -1, 0 or 1.  */
static int afp_compare(struct afp *a, struct afp *b)
{
	int sa, sb;

	if (a->cls == AFP_ZERO && b->cls == AFP_ZERO)
		return 0;

	sa = a->cls == AFP_ZERO? 0 : (a->sign? -1 : 1);
	sb = b->cls == AFP_ZERO? 0 : (b->sign? -1 : 1);
	if (sa != sb)
		return sa < sb? -1 : 1;

	/*  Same sign, both non-zero:  */
	if (a->exp != b->exp)
		return ((a->exp < b->exp) ^ a->sign)? -1 : 1;
	if (a->mant != b->mant)
		return ((a->mant < b->mant) ^ a->sign)? -1 : 1;
	return 0;
}


/*****************************************************************************
 *
 *  Instruction execution
 */

#define	FPCR_SUM	0x8000000000000000ULL
#define	FPCR_DYN_SHIFT	58
#define	FPCR_IOV	0x0200000000000000ULL
#define	FPCR_INE	0x0100000000000000ULL
#define	FPCR_UNF	0x0080000000000000ULL
#define	FPCR_OVF	0x0040000000000000ULL
#define	FPCR_DZE	0x0020000000000000ULL
#define	FPCR_INV	0x0010000000000000ULL

/*  The FPCR bits that the 21064 implements:  */
#define	FPCR_EV4_MASK	0x8ff0000000000000ULL


/*  Record exceptions in the FPCR.  */
static void fpcr_set_exc(struct cpu *cpu, int exc)
{
	uint64_t bits = 0;

	if (exc & EXC_INV)	bits |= FPCR_INV;
	if (exc & EXC_DZE)	bits |= FPCR_DZE;
	if (exc & EXC_FOV)	bits |= FPCR_OVF;
	if (exc & EXC_UNF)	bits |= FPCR_UNF;
	if (exc & EXC_INE)	bits |= FPCR_INE;
	if (exc & EXC_IOV)	bits |= FPCR_IOV;
	if (bits)
		cpu->cd.alpha.fpcr |= bits | FPCR_SUM;
}


/*
 *  Which of the exceptions that occurred will trap, given the trap
 *  qualifier field (func<10:8>) of an IEEE or VAX instruction.
 */
static int trapping_exc(int exc, int trp, int ieee)
{
	int enabled = EXC_INV | EXC_DZE | EXC_FOV;

	if (trp & 1)
		enabled |= EXC_UNF | EXC_IOV;	/*  /U, or /V for CVTxQ  */
	if (ieee && trp == 7)
		enabled |= EXC_INE;		/*  /SUI  */

	return exc & enabled;
}


static int ieee_rounding(struct cpu *cpu, int rnd)
{
	if (rnd == 3)
		return (cpu->cd.alpha.fpcr >> FPCR_DYN_SHIFT) & 3;
	return rnd;
}


/*  The EV4 family (21064, 21066, ...) leaves some things to software:  */
static int is_ev4(struct cpu *cpu)
{
	return cpu->cd.alpha.cpu_type.implver == 0;
}


/*  Rounding towards +/- infinity, which the EV4 doesn't implement:  */
static int inf_rounding(struct cpu *cpu, int mode)
{
	return is_ev4(cpu) && (mode == RND_MINUS || mode == RND_PLUS);
}


/*
 *  Finish an operate instruction: write the result, record the exceptions
 *  in the FPCR (fpcr_exc), and return the ones that trap (exc).
 */
static int fp_finish(struct cpu *cpu, int fc, uint64_t result, int fpcr_exc,
	int exc, int swc)
{
	if (fc != 31)
		cpu->cd.alpha.f[fc] = result;

	fpcr_set_exc(cpu, fpcr_exc);

	if (exc) {
		if (swc)
			exc |= EXC_SWC;
		cpu->cd.alpha.trap_mask = 1ULL << (32 + fc);
	}
	return exc;
}


/*
 *  IEEE operate (opcode 0x16).
 */
static int fp_ieee(struct cpu *cpu, uint32_t iword)
{
	int fa = (iword >> 21) & 31, fb = (iword >> 16) & 31, fc = iword & 31;
	int func = (iword >> 5) & 0x7ff;
	int fnc = func & 0xf, src = (func >> 4) & 3;
	int rnd = (func >> 6) & 3, trp = (func >> 8) & 7;
	int mode = ieee_rounding(cpu, rnd), exc = 0, fmt, cmp;
	struct afp a, b, r;
	uint64_t result;
	int fpcr_exc;

	if (src == 1 && is_ev4(cpu))
		return ALPHA_FP_FEN;

	if (fnc <= 3) {
		/*  ADDx, SUBx, MULx, DIVx  */
		if (src != 0 && src != 2)
			return ALPHA_FP_OPCDEC;
		if (inf_rounding(cpu, mode))
			return ALPHA_FP_FEN;
		fmt = src == 0? FMT_S : FMT_T;
		unpack_ieee(cpu->cd.alpha.f[fa], &a);
		unpack_ieee(cpu->cd.alpha.f[fb], &b);
		if (a.cls == AFP_SPECIAL || b.cls == AFP_SPECIAL)
			exc = EXC_INV;
		else switch (fnc) {
		case 0:	afp_add(&a, &b, mode, &r); break;
		case 1:	b.sign ^= 1;
			afp_add(&a, &b, mode, &r); break;
		case 2:	afp_mul(&a, &b, &r); break;
		case 3:	if (b.cls == AFP_ZERO)
				exc = a.cls == AFP_ZERO? EXC_INV : EXC_DZE;
			else
				afp_div(&a, &b, &r);
			break;
		}
		if (exc)
			result = 0;
		else
			result = afp_pack(&r, fmt, mode, &exc);
	} else if (fnc <= 7) {
		/*  CMPTUN, CMPTEQ, CMPTLT, CMPTLE  */
		if (src != 2)
			return ALPHA_FP_OPCDEC;
		unpack_ieee(cpu->cd.alpha.f[fa], &a);
		unpack_ieee(cpu->cd.alpha.f[fb], &b);
		result = 0;
		if (a.cls == AFP_SPECIAL || b.cls == AFP_SPECIAL)
			exc = EXC_INV;
		else {
			cmp = afp_compare(&a, &b);
			switch (fnc) {
			case 4: result = 0; break;
			case 5: result = cmp == 0; break;
			case 6: result = cmp < 0; break;
			case 7: result = cmp <= 0; break;
			}
			result = result? 0x4000000000000000ULL : 0;
		}
	} else if (fnc == 0xc && src == 2 && (trp == 2 || trp == 6)) {
		/*  CVTST: S to T is exact.  */
		unpack_ieee(cpu->cd.alpha.f[fb], &b);
		if (b.cls == AFP_SPECIAL) {
			exc = EXC_INV;
			result = 0;
		} else
			result = cpu->cd.alpha.f[fb];
		trp = 0;
	} else if (fnc == 0xc || fnc == 0xe) {
		/*  CVTTS, CVTQS, CVTQT  */
		fmt = fnc == 0xc? FMT_S : FMT_T;
		if (src == 3)
			unpack_int(cpu->cd.alpha.f[fb], &b);
		else if (src == 2 && fnc == 0xc)
			unpack_ieee(cpu->cd.alpha.f[fb], &b);
		else
			return ALPHA_FP_OPCDEC;
		if (inf_rounding(cpu, mode))
			return ALPHA_FP_FEN;
		if (b.cls == AFP_SPECIAL) {
			exc = EXC_INV;
			result = 0;
		} else
			result = afp_pack(&b, fmt, mode, &exc);
	} else if (fnc == 0xf && src == 2) {
		/*  CVTTQ  */
		if (inf_rounding(cpu, mode))
			return ALPHA_FP_FEN;
		unpack_ieee(cpu->cd.alpha.f[fb], &b);
		if (b.cls == AFP_SPECIAL) {
			exc = EXC_INV;
			result = 0;
		} else
			result = afp_to_int(&b, mode, &exc);
	} else
		return ALPHA_FP_OPCDEC;

	fpcr_exc = exc;
	exc = trapping_exc(exc, trp, 1);

	if (fnc == 3 && is_ev4(cpu) && !(fpcr_exc & (EXC_INV | EXC_DZE))) {
		/*  The 21064 doesn't compute the inexact flag of a divide:  */
		fpcr_exc &= ~EXC_INE;
		exc &= ~EXC_INE;
		if (trp == 7)
			exc |= EXC_INE;
	}

	return fp_finish(cpu, fc, result, fpcr_exc, exc, trp & 4);
}


/*
 *  VAX operate (opcode 0x15).
 */
static int fp_vax(struct cpu *cpu, uint32_t iword)
{
	int fa = (iword >> 21) & 31, fb = (iword >> 16) & 31, fc = iword & 31;
	int func = (iword >> 5) & 0x7ff;
	int fnc = func & 0xf, src = (func >> 4) & 3;
	int rnd = (func >> 6) & 3, trp = (func >> 8) & 7;
	int mode = rnd == 0? RND_CHOPPED : RND_VAX, exc = 0, fmt = FMT_G, cmp;
	struct afp a, b, r;
	uint64_t result = 0;

	if (rnd != 0 && rnd != 2)
		return ALPHA_FP_OPCDEC;

	if (fnc <= 3) {
		/*  ADDx, SUBx, MULx, DIVx  */
		if (src != 0 && src != 2)
			return ALPHA_FP_OPCDEC;
		fmt = src == 0? FMT_F : FMT_G;
		unpack_vax_fg(cpu->cd.alpha.f[fa], &a, fmt);
		unpack_vax_fg(cpu->cd.alpha.f[fb], &b, fmt);
		if (a.cls == AFP_SPECIAL || b.cls == AFP_SPECIAL)
			exc = EXC_INV;
		else switch (fnc) {
		case 0:	afp_add(&a, &b, mode, &r); break;
		case 1:	b.sign ^= 1;
			afp_add(&a, &b, mode, &r); break;
		case 2:	afp_mul(&a, &b, &r); break;
		case 3:	if (b.cls == AFP_ZERO)
				exc = EXC_DZE;
			else
				afp_div(&a, &b, &r);
			break;
		}
		if (!exc)
			result = afp_pack(&r, fmt, mode, &exc);
	} else if (fnc >= 5 && fnc <= 7) {
		/*  CMPGEQ, CMPGLT, CMPGLE  */
		if (src != 2)
			return ALPHA_FP_OPCDEC;
		unpack_vax_fg(cpu->cd.alpha.f[fa], &a, FMT_G);
		unpack_vax_fg(cpu->cd.alpha.f[fb], &b, FMT_G);
		if (a.cls == AFP_SPECIAL || b.cls == AFP_SPECIAL)
			exc = EXC_INV;
		else {
			cmp = afp_compare(&a, &b);
			switch (fnc) {
			case 5: result = cmp == 0; break;
			case 6: result = cmp < 0; break;
			case 7: result = cmp <= 0; break;
			}
			result = result? 0x4000000000000000ULL : 0;
		}
	} else if (fnc >= 0xc) {
		/*  Conversions:  */
		switch (src) {
		case 1:	unpack_vax_d(cpu->cd.alpha.f[fb], &b); break;
		case 2:	unpack_vax_fg(cpu->cd.alpha.f[fb], &b, FMT_G); break;
		case 3:	unpack_int(cpu->cd.alpha.f[fb], &b); break;
		default:return ALPHA_FP_OPCDEC;
		}
		switch (fnc) {
		case 0xc:	/*  CVTGF, CVTQF  */
			if (src == 1)
				return ALPHA_FP_OPCDEC;
			fmt = FMT_F;
			break;
		case 0xd:	/*  CVTGD  */
			if (src != 2)
				return ALPHA_FP_OPCDEC;
			fmt = FMT_D;
			break;
		case 0xe:	/*  CVTDG, CVTQG  */
			if (src == 2)
				return ALPHA_FP_OPCDEC;
			fmt = FMT_G;
			break;
		case 0xf:	/*  CVTGQ  */
			if (src != 2)
				return ALPHA_FP_OPCDEC;
			break;
		}
		if (b.cls == AFP_SPECIAL)
			exc = EXC_INV;
		else if (fnc == 0xf)
			result = afp_to_int(&b, mode, &exc);
		else
			result = afp_pack(&b, fmt, mode, &exc);
	} else
		return ALPHA_FP_OPCDEC;

	return fp_finish(cpu, fc, result, exc, trapping_exc(exc, trp, 0),
	    trp & 4);
}


static int fcmov_cond(int cond, uint64_t x)
{
	int zero = (x & 0x7fffffffffffffffULL) == 0;
	int neg = x >> 63;

	switch (cond) {
	case 0xa: return zero;			/*  FCMOVEQ  */
	case 0xb: return !zero;			/*  FCMOVNE  */
	case 0xc: return neg && !zero;		/*  FCMOVLT  */
	case 0xd: return !neg || zero;		/*  FCMOVGE  */
	case 0xe: return neg || zero;		/*  FCMOVLE  */
	default:  return !neg && !zero;		/*  FCMOVGT  */
	}
}


/*
 *  Data-type independent operate (opcode 0x17).
 */
static int fp_misc(struct cpu *cpu, uint32_t iword)
{
	int fa = (iword >> 21) & 31, fb = (iword >> 16) & 31, fc = iword & 31;
	int func = (iword >> 5) & 0x7ff;
	uint64_t a = cpu->cd.alpha.f[fa], b = cpu->cd.alpha.f[fb], result;
	int exc = 0;

	switch (func) {
	case 0x010:	/*  CVTLQ  */
		result = (uint64_t)(int64_t)(int32_t)
		    ((((b >> 62) & 3) << 30) | ((b >> 29) & 0x3fffffff));
		break;
	case 0x020:	/*  CPYS  */
		result = (a & 0x8000000000000000ULL) |
		    (b & 0x7fffffffffffffffULL);
		break;
	case 0x021:	/*  CPYSN  */
		result = (~a & 0x8000000000000000ULL) |
		    (b & 0x7fffffffffffffffULL);
		break;
	case 0x022:	/*  CPYSE  */
		result = (a & 0xfff0000000000000ULL) |
		    (b & 0x000fffffffffffffULL);
		break;
	case 0x024:	/*  MT_FPCR  */
		cpu->cd.alpha.fpcr = a & FPCR_EV4_MASK;
		return 0;
	case 0x025:	/*  MF_FPCR  */
		if (fa != 31)
			cpu->cd.alpha.f[fa] = cpu->cd.alpha.fpcr;
		return 0;
	case 0x02a: case 0x02b: case 0x02c:
	case 0x02d: case 0x02e: case 0x02f:	/*  FCMOVxx  */
		if (!fcmov_cond(func & 0xf, a))
			return 0;
		result = b;
		break;
	case 0x030:	/*  CVTQL  */
	case 0x130:	/*  CVTQL/V  */
	case 0x530:	/*  CVTQL/SV  */
		result = ((b >> 30) & 3) << 62 | (b & 0x3fffffff) << 29;
		/*  Overflow sets FPCR[IOV] and FPCR[INE]; only /V traps.  */
		if ((int64_t)b != (int64_t)(int32_t)b)
			exc = EXC_IOV | EXC_INE;
		break;
	default:
		return ALPHA_FP_OPCDEC;
	}

	return fp_finish(cpu, fc, result, exc,
	    func == 0x030? 0 : exc & EXC_IOV, func & 0x400);
}


/*
 *  alpha_fp_operate():
 *
 *  Executes a floating point operate instruction (opcode 0x15, 0x16 or
 *  0x17). Returns 0 normally, ALPHA_FP_OPCDEC if the instruction doesn't
 *  exist, ALPHA_FP_FEN if the hardware leaves it to software (EV4), or the
 *  EXC_SUM bits of an arithmetic trap (with the register write mask in
 *  cpu->cd.alpha.exc_mask).
 */
int alpha_fp_operate(struct cpu *cpu, uint32_t iword)
{
	switch (iword >> 26) {
	case 0x15:	return fp_vax(cpu, iword);
	case 0x16:	return fp_ieee(cpu, iword);
	case 0x17:	return fp_misc(cpu, iword);
	}
	return ALPHA_FP_OPCDEC;
}


/*
 *  Conversions between the memory and register formats of S and F.
 *  (T and G are the same in memory as in registers, except that G has its
 *  16-bit words in the opposite order.)
 */
uint64_t alpha_lds_to_reg(uint32_t m)
{
	uint64_t exp = (m >> 23) & 0xff, mid;

	if (exp == 0xff)
		mid = 7;
	else if (exp & 0x80)
		mid = 0;
	else
		mid = exp != 0? 7 : 0;
	return ((uint64_t)(m >> 30) << 62) | (mid << 59) |
	    ((uint64_t)(m & 0x3fffffff) << 29);
}

uint32_t alpha_reg_to_sts(uint64_t r)
{
	return ((r >> 62) << 30) | ((r >> 29) & 0x3fffffff);
}

uint64_t alpha_ldf_to_reg(uint32_t m)
{
	uint64_t emsb = (m >> 14) & 1, mid = 0;

	if (!emsb && ((m >> 7) & 0x7f) != 0)
		mid = 7;
	return ((uint64_t)((m >> 15) & 1) << 63) | (emsb << 62) | (mid << 59) |
	    ((uint64_t)(m & 0x3fff) << 45) | ((uint64_t)(m >> 16) << 29);
}

uint32_t alpha_reg_to_stf(uint64_t r)
{
	return (uint32_t)((((r >> 62) & 3) << 14) | ((r >> 45) & 0x3fff)) |
	    (uint32_t)(((r >> 29) & 0xffff) << 16);
}

uint64_t alpha_swap_g(uint64_t x)
{
	return ((x >> 48) & 0xffff) | (((x >> 32) & 0xffff) << 16) |
	    (((x >> 16) & 0xffff) << 32) | ((x & 0xffff) << 48);
}
