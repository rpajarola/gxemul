#!/usr/bin/env python3
#
#  Reference model for the Alpha floating-point tests made by gen_tests.py.
#
#  expected(desc) takes a test description line (".list" file) and
#  returns the expected 64-bit result, or None if the model doesn't cover
#  that test. Arithmetic is done exactly with fractions and then rounded,
#  following the Alpha Architecture Reference Manual. Only non-trapping
#  cases are modelled (the generator only makes those).
#
#  QEMU's VAX floating point is wrong, so the VAX tests use this model as
#  their reference. For the IEEE tests the model is a cross-check of QEMU.

from fractions import Fraction
import re

MASK64 = (1 << 64) - 1


def sext(x, bits):
    x &= (1 << bits) - 1
    return x - (1 << bits) if x >> (bits - 1) else x


class Unsupported(Exception):
    pass


#############################################################################
#  Rounding

CHOPPED, MINUS, NORMAL, PLUS, VAXNORMAL = range(5)
DYN = {'chopped': CHOPPED, 'minus': MINUS, 'normal': NORMAL, 'plus': PLUS}


def round_to(r, p, mode):
    """Round the exact value r to p significant bits.
    Returns (negative, n, e) with |result| = n * 2^e, 2^(p-1) <= n < 2^p,
    or n == 0 for zero."""
    neg = r < 0
    a = -r if neg else r
    if a == 0:
        return neg, 0, 0
    e = a.numerator.bit_length() - a.denominator.bit_length()
    #  Now 2^(e-1) < a < 2^(e+1); find e with 2^e <= a < 2^(e+1):
    if a < Fraction(2) ** e:
        e -= 1
    shift = e - p + 1
    scaled = a / (Fraction(2) ** shift)
    n = scaled.numerator // scaled.denominator
    rem = scaled - n
    up = False
    if mode == CHOPPED:
        up = False
    elif mode == MINUS:
        up = neg and rem > 0
    elif mode == PLUS:
        up = (not neg) and rem > 0
    elif mode == NORMAL:
        up = rem > Fraction(1, 2) or (rem == Fraction(1, 2) and n & 1)
    elif mode == VAXNORMAL:
        up = rem >= Fraction(1, 2)
    if up:
        n += 1
        if n == 1 << p:
            n >>= 1
            shift += 1
    return neg, n, shift


#############################################################################
#  IEEE S and T

def t_decode(x):
    s = x >> 63
    e = (x >> 52) & 0x7ff
    f = x & ((1 << 52) - 1)
    if e == 0x7ff:
        raise Unsupported('inf/nan')
    if e == 0:
        if f:
            raise Unsupported('denormal')
        return Fraction(0), s
    v = Fraction((1 << 52) | f) * Fraction(2) ** (e - 1023 - 52)
    return (-v if s else v), s


def ieee_pack(r, fmt, mode, zero_sign):
    """Round exact r to S or T; return register (T-layout) bits."""
    p = 24 if fmt == 's' else 53
    emin, emax = (-126, 127) if fmt == 's' else (-1022, 1023)
    neg, n, shift = round_to(r, p, mode)
    if n == 0:
        return (zero_sign << 63)
    #  value = n * 2^shift, normalized exponent E = shift + p - 1
    E = shift + p - 1
    if E > emax:
        raise Unsupported('overflow')
    if E < emin:
        if 'u' in UNF_QUAL[0]:
            raise Unsupported('underflow trap')
        return 0            #  The 21064 gives a true zero.
    frac = (n - (1 << (p - 1))) << (53 - p)
    return (neg << 63) | ((E + 1023) << 52) | frac


def ieee_int_round(r, mode):
    """Round exact r to an integer."""
    fl = r.numerator // r.denominator           # floor
    rem = r - fl
    if rem == 0:
        q = fl
    elif mode == CHOPPED:
        q = fl if r >= 0 else fl + 1
    elif mode == MINUS:
        q = fl
    elif mode == PLUS:
        q = fl + 1
    elif mode == NORMAL:
        if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and fl & 1):
            q = fl + 1
        else:
            q = fl
    elif mode == VAXNORMAL:
        #  Round half away from zero.
        a = abs(r)
        q = a.numerator // a.denominator
        if a - q >= Fraction(1, 2):
            q += 1
        if r < 0:
            q = -q
    return q


#############################################################################
#  VAX F and G (register formats)

def vax_f_decode(x):
    s = x >> 63
    e11 = (x >> 52) & 0x7ff
    e = ((e11 >> 10) << 7) | (e11 & 0x7f)
    f = (x >> 29) & ((1 << 23) - 1)
    if e == 0:
        if s:
            raise Unsupported('reserved operand')
        return Fraction(0)
    v = Fraction((1 << 23) | f) * Fraction(2) ** (e - 128 - 24)
    return -v if s else v


def vax_g_decode(x):
    s = x >> 63
    e = (x >> 52) & 0x7ff
    f = x & ((1 << 52) - 1)
    if e == 0:
        if s:
            raise Unsupported('reserved operand')
        return Fraction(0)
    v = Fraction((1 << 52) | f) * Fraction(2) ** (e - 1024 - 53)
    return -v if s else v


def vax_d_decode(x):
    s = x >> 63
    e = (x >> 55) & 0xff
    f = x & ((1 << 55) - 1)
    if e == 0:
        if s:
            raise Unsupported('reserved operand')
        return Fraction(0)
    v = Fraction((1 << 55) | f) * Fraction(2) ** (e - 128 - 56)
    return -v if s else v


def vax_pack(r, fmt, mode):
    p = {'f': 24, 'g': 53, 'd': 56}[fmt]
    neg, n, shift = round_to(r, p, mode)
    if n == 0:
        return 0
    #  value = 0.1fff * 2^(e - bias), i.e. n * 2^shift with n < 2^p
    #  => e = shift + p + bias
    if fmt == 'f':
        e = shift + p + 128
        if e < 1 or e > 255:
            raise Unsupported('over/underflow')
        if e & 0x80:
            e11 = 0x400 | (e & 0x7f)
        else:
            e11 = 0x380 | (e & 0x7f)
        frac = n - (1 << 23)
        return (neg << 63) | (e11 << 52) | (frac << 29)
    if fmt == 'd':
        e = shift + p + 128
        if e < 1 or e > 255:
            raise Unsupported('over/underflow')
        frac = n - (1 << 55)
        return (neg << 63) | (e << 55) | frac
    e = shift + p + 1024
    if e < 1 or e > 2047:
        raise Unsupported('over/underflow')
    frac = n - (1 << 52)
    return (neg << 63) | (e << 52) | frac


def stf_mem(fa):
    lo = ((fa >> 62) & 3) << 14 | ((fa >> 45) & 0x3fff)
    hi = (fa >> 29) & 0xffff
    return lo | (hi << 16)


def ldf_reg(m):
    sign = (m >> 15) & 1
    emsb = (m >> 14) & 1
    elow = (m >> 7) & 0x7f
    if emsb:
        mid = 0
    else:
        mid = 7 if elow != 0 else 0
    return ((sign << 63) | (emsb << 62) | (mid << 59) |
            ((m & 0x3fff) << 45) | (((m >> 16) & 0xffff) << 29))


def swap_words(q):
    return (((q >> 48) & 0xffff) | (((q >> 32) & 0xffff) << 16) |
            (((q >> 16) & 0xffff) << 32) | ((q & 0xffff) << 48))


def lds_reg(m):
    sign = (m >> 31) & 1
    exp = (m >> 23) & 0xff
    emsb = exp >> 7
    if emsb:
        mid = 7 if exp == 0xff else 0
    else:
        mid = 7 if exp != 0 else 0
    return ((sign << 63) | (emsb << 62) | (mid << 59) |
            ((m & 0x3fffffff) << 29))


def sts_mem(fa):
    return (((fa >> 62) & 3) << 30) | ((fa >> 29) & 0x3fffffff)


#############################################################################
#  Description parsing and evaluation

DESC_RE = re.compile(r'^(\S+?)(?:\[(\w+)\])? (.*)$')


def ieee_mode(qual, dyn):
    if 'd' in qual:
        return DYN[dyn]
    if 'c' in qual:
        return CHOPPED
    if 'm' in qual:
        return MINUS
    return NORMAL


def fp_is_zero(x):
    return (x & ~(1 << 63)) == 0


def fcond(cond, x):
    neg = x >> 63
    z = fp_is_zero(x)
    return {'eq': z, 'ne': not z, 'lt': neg and not z, 'ge': not neg or z,
            'le': z or neg, 'gt': not neg and not z}[cond]


def expected(desc):
    m = DESC_RE.match(desc)
    if not m:
        return None
    mnem, dyn, rest = m.group(1), m.group(2) or 'normal', m.group(3)
    if '/' in mnem:
        base, qual = mnem.split('/', 1)
    else:
        base, qual = mnem, ''
    ops = [int(x, 16) for x in re.findall(r'\b[0-9a-f]{8,16}\b', rest)]
    try:
        return _expected(base, qual, dyn, ops, rest)
    except Unsupported:
        return None


UNF_QUAL = ['']


def _expected(base, qual, dyn, ops, rest):
    UNF_QUAL[0] = qual
    #  IEEE arithmetic:
    mt = re.match(r'^(add|sub|mul|div)([st])$', base)
    if mt:
        fn, fmt = mt.groups()
        a, sa = t_decode(ops[0])
        b, sb = t_decode(ops[1])
        mode = ieee_mode(qual, dyn)
        if fn == 'add':
            r = a + b
        elif fn == 'sub':
            r = a - b
        elif fn == 'mul':
            r = a * b
        else:
            r = a / b
        if r == 0:
            if fn in ('mul', 'div'):
                zs = sa ^ sb
            else:
                bs = sb ^ (1 if fn == 'sub' else 0)
                if a == 0 and b == 0 and sa == bs:
                    zs = sa
                else:
                    zs = 1 if mode == MINUS else 0
        else:
            zs = 0
        return ieee_pack(r, fmt, mode, zs)
    mt = re.match(r'^cmpt(eq|lt|le|un)$', base)
    if mt:
        a, _ = t_decode(ops[0])
        b, _ = t_decode(ops[1])
        res = {'eq': a == b, 'lt': a < b, 'le': a <= b, 'un': False}[
            mt.group(1)]
        return 0x4000000000000000 if res else 0
    mt = re.match(r'^cvtq([st])$', base)
    if mt:
        return ieee_pack(Fraction(sext(ops[0], 64)), mt.group(1),
                         ieee_mode(qual, dyn), 0)
    if base == 'cvttq':
        a, _ = t_decode(ops[0])
        q = ieee_int_round(a, ieee_mode(qual.replace('v', ''), dyn))
        if q < -(1 << 63) or q >= (1 << 63):
            raise Unsupported('overflow')
        return q & MASK64
    if base == 'cvtts':
        a, s = t_decode(ops[0])
        return ieee_pack(a, 's', ieee_mode(qual, dyn), s)
    if base == 'cvtlq':
        x = ops[0]
        v = (((x >> 62) & 3) << 30) | ((x >> 29) & 0x3fffffff)
        return sext(v, 32) & MASK64
    if base == 'cvtql':
        x = ops[0]
        return (((x >> 30) & 3) << 62) | ((x & 0x3fffffff) << 29)
    if base == 'lds':
        return lds_reg(ops[0])
    if base == 'sts':
        w = ops[0]
        return w | (sts_mem(lds_reg(w)) << 32)
    if base in ('cpys', 'cpysn', 'cpyse'):
        a, b = ops
        if base == 'cpys':
            return (a & (1 << 63)) | (b & ~(1 << 63))
        if base == 'cpysn':
            return ((~a) & (1 << 63)) | (b & ~(1 << 63))
        return (a & (0xfff << 52)) | (b & ((1 << 52) - 1))
    mt = re.match(r'^fcmov(\w\w)$', base)
    if mt:
        return (0x1111222233334444 if fcond(mt.group(1), ops[0])
                else 0x5555666677778888)
    mt = re.match(r'^fb(\w\w)$', base)
    if mt and rest.endswith('taken'):
        return 1 if fcond(mt.group(1), ops[0]) else 0
    if base == 'fpcr':
        return int(rest.split()[-1])

    #  VAX:
    vmode = CHOPPED if 'c' in qual else VAXNORMAL
    mt = re.match(r'^(add|sub|mul|div)([fg])$', base)
    if mt:
        fn, fmt = mt.groups()
        dec = vax_f_decode if fmt == 'f' else vax_g_decode
        a = dec(ops[0])
        b = dec(ops[1])
        r = {'add': lambda: a + b, 'sub': lambda: a - b,
             'mul': lambda: a * b, 'div': lambda: a / b}[fn]()
        return vax_pack(r, fmt, vmode)
    mt = re.match(r'^cmpg(eq|lt|le)$', base)
    if mt:
        a = vax_g_decode(ops[0])
        b = vax_g_decode(ops[1])
        res = {'eq': a == b, 'lt': a < b, 'le': a <= b}[mt.group(1)]
        return 0x4000000000000000 if res else 0
    if base == 'cvtgd':
        return vax_pack(vax_g_decode(ops[0]), 'd', vmode)
    if base == 'cvtdg':
        return vax_pack(vax_d_decode(ops[0]), 'g', vmode)
    if base == 'cvtst':
        t_decode(ops[0])
        return ops[0]
    if base == 'cvtgf':
        return vax_pack(vax_g_decode(ops[0]), 'f', vmode)
    if base == 'cvtgq':
        q = ieee_int_round(vax_g_decode(ops[0]), vmode)
        if q < -(1 << 63) or q >= (1 << 63):
            raise Unsupported('overflow')
        return q & MASK64
    mt = re.match(r'^cvtq([fg])$', base)
    if mt:
        return vax_pack(Fraction(sext(ops[0], 64)), mt.group(1), vmode)
    if base == 'stf':
        fb = ops[0]
        return (fb & 0xffffffff00000000) | stf_mem(fb)
    if base == 'stg':
        return swap_words(ops[0])
    if base == 'ldf' and rest.startswith('(stf'):
        return ldf_reg(stf_mem(ops[0]))
    if base == 'ldg' and rest.startswith('(stg'):
        return swap_words(swap_words(ops[0]))
    return None
