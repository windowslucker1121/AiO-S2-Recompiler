/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bit-exact AArch64 behaviour for the few floating-point operations where x86 differs: reciprocal estimates, the sign
 * of generated NaNs, FMAX/FMIN, half-precision conversion. Included by ctx.h. Adapted from smo-recomp
 * runtime/core/fpexact.h (GPL-3.0-or-later); the estimate algorithms follow the Arm architecture manual pseudocode. */
#ifndef NXRECOMP_FPEXACT_H
#define NXRECOMP_FPEXACT_H

/* x86 produces a negative quiet NaN for invalid operations, AArch64 a positive one. */
NX_HELPER float fnan32(float r) {
    uint32_t b;
    memcpy(&b, &r, 4);
    if (b == 0xffc00000u) { b = 0x7fc00000u; memcpy(&r, &b, 4); }
    return r;
}
NX_HELPER double fnan64(double r) {
    uint64_t b;
    memcpy(&b, &r, 8);
    if (b == 0xfff8000000000000ull) { b = 0x7ff8000000000000ull; memcpy(&r, &b, 8); }
    return r;
}

static inline uint32_t recip_estimate(uint32_t a) {            /* a in 256..511 */
    a = a * 2 + 1;
    uint32_t b = (1u << 19) / a;
    return (b + 1) / 2;
}
static inline uint32_t recip_sqrt_estimate(uint32_t a) {       /* a in 128..511 */
    if (a < 256) a = a * 2 + 1;
    else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    uint32_t b = 512;
    while ((uint64_t)a * (b + 1) * (b + 1) < (1ull << 28)) b++;
    return (b + 1) / 2;
}

/* Generic implementations on (sign, biased exponent, fraction left-aligned to 52 bits). */
static inline uint64_t fp_recpe_bits(uint64_t bits, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint64_t sign = bits >> (ebits + fbits);
    int64_t exp = (int64_t)((bits >> fbits) & ((1u << ebits) - 1));
    uint64_t frac = bits & ((1ull << fbits) - 1);
    int64_t emax = (1 << ebits) - 1;
    uint64_t signbit = sign << (ebits + fbits);
    if (exp == emax) {
        if (frac) return bits | (1ull << (fbits - 1));          /* NaN: quieten */
        return signbit;                                         /* infinity -> zero */
    }
    if (exp == 0 && frac == 0) return signbit | ((uint64_t)emax << fbits);       /* zero -> infinity */
    if (exp == 0 && frac < (1ull << (fbits - 2))) return signbit | ((uint64_t)emax << fbits);  /* overflow */
    uint64_t f52 = frac << (52 - fbits);
    if (exp == 0) {
        if ((f52 >> 51) == 0) { exp = -1; f52 = (f52 << 2) & ((1ull << 52) - 1); }
        else f52 = (f52 << 1) & ((1ull << 52) - 1);
    }
    uint32_t scaled = 0x100 | (uint32_t)(f52 >> 44);
    int64_t rexp = (dbl ? 2045 : 253) - exp;
    uint64_t rf = (uint64_t)(recip_estimate(scaled) & 0xff) << 44;
    if (rexp == 0) rf = (1ull << 51) | (rf >> 1);
    else if (rexp == -1) { rf = (1ull << 50) | (rf >> 2); rexp = 0; }
    return signbit | ((uint64_t)rexp << fbits) | (rf >> (52 - fbits));
}
static inline uint64_t fp_rsqrte_bits(uint64_t bits, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint64_t sign = bits >> (ebits + fbits);
    int64_t exp = (int64_t)((bits >> fbits) & ((1u << ebits) - 1));
    uint64_t frac = bits & ((1ull << fbits) - 1);
    int64_t emax = (1 << ebits) - 1;
    uint64_t default_nan = ((uint64_t)emax << fbits) | (1ull << (fbits - 1));
    if (exp == emax && frac) return bits | (1ull << (fbits - 1));                 /* NaN: quieten */
    if (exp == 0 && frac == 0) return (sign << (ebits + fbits)) | ((uint64_t)emax << fbits);  /* zero -> infinity */
    if (sign) return default_nan;                                                 /* negative -> NaN */
    if (exp == emax) return 0;                                                    /* +infinity -> +0 */
    uint64_t f52 = frac << (52 - fbits);
    if (exp == 0) {
        while ((f52 >> 51) == 0) { f52 = (f52 << 1) & ((1ull << 52) - 1); exp--; }
        f52 = (f52 << 1) & ((1ull << 52) - 1);
    }
    uint32_t scaled = (exp & 1) == 0 ? 0x100 | (uint32_t)(f52 >> 44) : 0x80 | (uint32_t)(f52 >> 45);
    int64_t rexp = ((dbl ? 3068 : 380) - exp) / 2;
    uint64_t est = recip_sqrt_estimate(scaled) & 0xff;
    return ((uint64_t)rexp << fbits) | (est << (fbits - 8));
}

NX_HELPER float recpe_f(float x) { uint32_t b; memcpy(&b, &x, 4); b = (uint32_t)fp_recpe_bits(b, 0); memcpy(&x, &b, 4); return x; }
NX_HELPER double recpe_d(double x) { uint64_t b; memcpy(&b, &x, 8); b = fp_recpe_bits(b, 1); memcpy(&x, &b, 8); return x; }
NX_HELPER float rsqrte_f(float x) { uint32_t b; memcpy(&b, &x, 4); b = (uint32_t)fp_rsqrte_bits(b, 0); memcpy(&x, &b, 4); return x; }
NX_HELPER double rsqrte_d(double x) { uint64_t b; memcpy(&b, &x, 8); b = fp_rsqrte_bits(b, 1); memcpy(&x, &b, 8); return x; }

/* FCVT between half precision (IEEE binary16, FPCR.AHP = 0) and single / double: FPConvert of the Arm manual with
 * round-to-nearest-even and FPCR.DN = 0. A NaN keeps its sign and the top bits of its payload and becomes quiet
 * (FPConvertNaN). Done on the bit patterns, so there is one rounding and no dependence on the host's conversions. */
static inline uint16_t fp_to_half_bits(uint64_t bits, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint16_t sign = (uint16_t)(((bits >> (ebits + fbits)) & 1) << 15);
    int64_t exp = (int64_t)((bits >> fbits) & ((1u << ebits) - 1));
    uint64_t frac = bits & ((1ull << fbits) - 1);
    int64_t emax = (1 << ebits) - 1, bias = emax >> 1;
    if (exp == emax) {
        if (frac) return (uint16_t)(sign | 0x7e00 | ((frac >> (fbits - 10)) & 0x1ff));   /* NaN: quiet, payload bits below the quiet bit */
        return (uint16_t)(sign | 0x7c00);                                                 /* infinity */
    }
    if (exp == 0 && frac == 0) return sign;
    uint64_t m;                                    /* value = m * 2^(e - fbits), bit fbits of m set */
    int64_t e;
    if (exp) { m = frac | (1ull << fbits); e = exp - bias; }
    else { m = frac; e = 1 - bias; while (!(m >> fbits)) { m <<= 1; e--; } }
    if (e > 15) return (uint16_t)(sign | 0x7c00);                                         /* overflow: infinity (round to nearest) */
    int64_t sh = fbits - 10 + (e < -14 ? -14 - e : 0);                                    /* below 2^-14 the result is subnormal */
    if (sh > fbits + 1) return sign;                                                      /* less than half the smallest subnormal */
    uint64_t q = m >> sh, rem = m & ((1ull << sh) - 1), half = 1ull << (sh - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    uint32_t r = e < -14 ? (uint32_t)q : (uint32_t)(((uint64_t)(e + 15) << 10) + (q - 0x400));   /* a carry out of the fraction moves into the exponent */
    if (r >= 0x7c00) r = 0x7c00;
    return (uint16_t)(sign | r);
}
static inline uint64_t fp_from_half_bits(uint16_t h, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint64_t sign = (uint64_t)(h >> 15) << (ebits + fbits), frac = h & 0x3ff, emax = (1u << ebits) - 1;
    int64_t exp = (h >> 10) & 31, bias = (int64_t)(emax >> 1), e;
    if (exp == 31) {
        if (frac) return sign | (emax << fbits) | (1ull << (fbits - 1)) | ((frac & 0x1ff) << (fbits - 10));
        return sign | (emax << fbits);
    }
    if (exp == 0) {
        if (!frac) return sign;
        e = -14;
        while (!(frac & 0x400)) { frac <<= 1; e--; }
        frac &= 0x3ff;
    } else e = exp - 15;
    return sign | ((uint64_t)(e + bias) << fbits) | (frac << (fbits - 10));
}
NX_HELPER uint16_t f32_to_half(float x) { uint32_t b; memcpy(&b, &x, 4); return fp_to_half_bits(b, 0); }
NX_HELPER uint16_t f64_to_half(double x) { uint64_t b; memcpy(&b, &x, 8); return fp_to_half_bits(b, 1); }
NX_HELPER float half_to_f32(uint16_t h) { uint32_t b = (uint32_t)fp_from_half_bits(h, 0); float x; memcpy(&x, &b, 4); return x; }
NX_HELPER double half_to_f64(uint16_t h) { uint64_t b = fp_from_half_bits(h, 1); double x; memcpy(&x, &b, 8); return x; }

/* FMAX / FMIN / FMAXNM / FMINNM (FPMax, FPMin, FPMaxNum, FPMinNum of the Arm manual, FPCR.DN = 0), on bit patterns.
 * A signalling NaN operand wins (first operand first) and is returned quiet; then a quiet NaN (first operand first),
 * except that the NM forms return the other operand when exactly one operand is a quiet NaN and none is signalling.
 * +0 is greater than -0. The C library's fmax / fmin do neither. */
NX_HELPER uint32_t fp_maxmin32_bits(uint32_t a, uint32_t b, int is_min, int num) {
    int na = (a & 0x7fffffffu) > 0x7f800000u, nb = (b & 0x7fffffffu) > 0x7f800000u;
    if (na | nb) {
        int sa = na && !(a & 0x00400000u), sb = nb && !(b & 0x00400000u);
        if (num && !sa && !sb && na != nb) return na ? b : a;
        return (sa ? a : sb ? b : na ? a : b) | 0x00400000u;
    }
    if (((a | b) & 0x7fffffffu) == 0) return is_min ? (a | b) : (a & b);
    float fa, fb;
    memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
    return (is_min ? fa < fb : fa > fb) ? a : b;
}
NX_HELPER uint64_t fp_maxmin64_bits(uint64_t a, uint64_t b, int is_min, int num) {
    int na = (a & 0x7fffffffffffffffull) > 0x7ff0000000000000ull, nb = (b & 0x7fffffffffffffffull) > 0x7ff0000000000000ull;
    if (na | nb) {
        int sa = na && !(a & 0x0008000000000000ull), sb = nb && !(b & 0x0008000000000000ull);
        if (num && !sa && !sb && na != nb) return na ? b : a;
        return (sa ? a : sb ? b : na ? a : b) | 0x0008000000000000ull;
    }
    if (((a | b) & 0x7fffffffffffffffull) == 0) return is_min ? (a | b) : (a & b);
    double fa, fb;
    memcpy(&fa, &a, 8); memcpy(&fb, &b, 8);
    return (is_min ? fa < fb : fa > fb) ? a : b;
}
NX_HELPER float fp_maxmin32(float x, float y, int is_min, int num) {
    uint32_t a, b;
    memcpy(&a, &x, 4); memcpy(&b, &y, 4);
    a = fp_maxmin32_bits(a, b, is_min, num);
    memcpy(&x, &a, 4);
    return x;
}
NX_HELPER double fp_maxmin64(double x, double y, int is_min, int num) {
    uint64_t a, b;
    memcpy(&a, &x, 8); memcpy(&b, &y, 8);
    a = fp_maxmin64_bits(a, b, is_min, num);
    memcpy(&x, &a, 8);
    return x;
}

/* ---- Exact NaN rules of the arithmetic (FPCR.DN = 0). The host result is used whenever it is not a NaN: x86 SSE /
 * the C library already give the IEEE value for every non-NaN case. When the host result is a NaN, the result is
 * recomputed from the operand bit patterns, in Arm operand order: the first signalling NaN operand, quietened (quiet
 * bit set, payload and sign kept); else the first quiet NaN operand, unchanged; else (invalid operation, no NaN
 * operand) the positive default NaN. Operands that the Arm pseudocode negates (FPNeg) are passed already negated, so a
 * NaN taken from them keeps the flipped sign. Pure sign operations (fneg, fabs, fcsel, fmov) never go through here. */
#define FP_QBIT32 0x00400000u
#define FP_QBIT64 0x0008000000000000ull

NX_HELPER uint32_t fp_bits32(float x) { uint32_t b; memcpy(&b, &x, 4); return b; }
NX_HELPER float fp_from_bits32(uint32_t b) { float x; memcpy(&x, &b, 4); return x; }
NX_HELPER uint64_t fp_bits64(double x) { uint64_t b; memcpy(&b, &x, 8); return b; }
NX_HELPER double fp_from_bits64(uint64_t b) { double x; memcpy(&x, &b, 8); return x; }

NX_HELPER int fp_nan32b(uint32_t b) { return (b & 0x7fffffffu) > 0x7f800000u; }
NX_HELPER int fp_snan32b(uint32_t b) { return fp_nan32b(b) && !(b & FP_QBIT32); }
NX_HELPER int fp_inf32b(uint32_t b) { return (b & 0x7fffffffu) == 0x7f800000u; }
NX_HELPER int fp_zero32b(uint32_t b) { return (b & 0x7fffffffu) == 0; }
NX_HELPER int fp_nan64b(uint64_t b) { return (b & 0x7fffffffffffffffull) > 0x7ff0000000000000ull; }
NX_HELPER int fp_snan64b(uint64_t b) { return fp_nan64b(b) && !(b & FP_QBIT64); }
NX_HELPER int fp_inf64b(uint64_t b) { return (b & 0x7fffffffffffffffull) == 0x7ff0000000000000ull; }
NX_HELPER int fp_zero64b(uint64_t b) { return (b & 0x7fffffffffffffffull) == 0; }

/* The NaN result for n operands (ARM order; unused operands are ignored). */
NX_COLD_HELPER float fp_nan_result32(int n, float x0, float x1, float x2) {
    uint32_t op[3] = { fp_bits32(x0), fp_bits32(x1), fp_bits32(x2) };
    uint32_t res = 0x7fc00000u;
    int i, found = 0;
    for (i = 0; i < n && !found; i++) if (fp_snan32b(op[i])) { res = op[i] | FP_QBIT32; found = 1; }
    for (i = 0; i < n && !found; i++) if (fp_nan32b(op[i])) { res = op[i]; found = 1; }
    return fp_from_bits32(res);
}
NX_COLD_HELPER double fp_nan_result64(int n, double x0, double x1, double x2) {
    uint64_t op[3] = { fp_bits64(x0), fp_bits64(x1), fp_bits64(x2) };
    uint64_t res = 0x7ff8000000000000ull;
    int i, found = 0;
    for (i = 0; i < n && !found; i++) if (fp_snan64b(op[i])) { res = op[i] | FP_QBIT64; found = 1; }
    for (i = 0; i < n && !found; i++) if (fp_nan64b(op[i])) { res = op[i]; found = 1; }
    return fp_from_bits64(res);
}

NX_HELPER float fp_add32(float a, float b) { float r = a + b; return r == r ? r : fp_nan_result32(2, a, b, 0); }
NX_HELPER float fp_sub32(float a, float b) { float r = a - b; return r == r ? r : fp_nan_result32(2, a, b, 0); }
NX_HELPER float fp_mul32(float a, float b) { float r = a * b; return r == r ? r : fp_nan_result32(2, a, b, 0); }
NX_HELPER float fp_div32(float a, float b) { float r = a / b; return r == r ? r : fp_nan_result32(2, a, b, 0); }
NX_HELPER float fp_sqrt32(float a) { float r = sqrtf(a); return r == r ? r : fp_nan_result32(1, a, 0, 0); }
NX_HELPER float fp_neg32(float a) { return fp_from_bits32(fp_bits32(a) ^ 0x80000000u); }
NX_HELPER float fp_abs32(float a) { return fp_from_bits32(fp_bits32(a) & 0x7fffffffu); }

NX_HELPER double fp_add64(double a, double b) { double r = a + b; return r == r ? r : fp_nan_result64(2, a, b, 0); }
NX_HELPER double fp_sub64(double a, double b) { double r = a - b; return r == r ? r : fp_nan_result64(2, a, b, 0); }
NX_HELPER double fp_mul64(double a, double b) { double r = a * b; return r == r ? r : fp_nan_result64(2, a, b, 0); }
NX_HELPER double fp_div64(double a, double b) { double r = a / b; return r == r ? r : fp_nan_result64(2, a, b, 0); }
NX_HELPER double fp_sqrt64(double a) { double r = sqrt(a); return r == r ? r : fp_nan_result64(1, a, 0, 0); }
NX_HELPER double fp_neg64(double a) { return fp_from_bits64(fp_bits64(a) ^ 0x8000000000000000ull); }
NX_HELPER double fp_abs64(double a) { return fp_from_bits64(fp_bits64(a) & 0x7fffffffffffffffull); }

/* Fused n * m + a. NaN order is addend, n, m (measured on Unicorn, not the order of the operands in the encoding). A quiet
 * NaN addend with an invalid product (inf * 0) gives the default NaN, not the addend. */
NX_COLD_HELPER float fp_fma_nan32(float n, float m, float a) {
    uint32_t bn = fp_bits32(n), bm = fp_bits32(m), ba = fp_bits32(a);
    if (fp_nan32b(ba) && !fp_snan32b(ba) &&
        ((fp_inf32b(bn) && fp_zero32b(bm)) || (fp_zero32b(bn) && fp_inf32b(bm))))
        return fp_from_bits32(0x7fc00000u);
    return fp_nan_result32(3, a, n, m);       /* the addend has the first place (Unicorn, probed), then n, then m */
}
NX_COLD_HELPER double fp_fma_nan64(double n, double m, double a) {
    uint64_t bn = fp_bits64(n), bm = fp_bits64(m), ba = fp_bits64(a);
    if (fp_nan64b(ba) && !fp_snan64b(ba) &&
        ((fp_inf64b(bn) && fp_zero64b(bm)) || (fp_zero64b(bn) && fp_inf64b(bm))))
        return fp_from_bits64(0x7ff8000000000000ull);
    return fp_nan_result64(3, a, n, m);
}
/* With FMA3 (-march=x86-64-v3) fmaf / fma compile to one vfmadd, which rounds once like the Arm instruction. */
NX_HELPER float fp_fma32(float n, float m, float a) {
    float r = fmaf(n, m, a);
    return r == r ? r : fp_fma_nan32(n, m, a);
}
NX_HELPER double fp_fma64(double n, double m, double a) {
    double r = fma(n, m, a);
    return r == r ? r : fp_fma_nan64(n, m, a);
}

/* FPRecipStepFused (FRECPS) and FPRecipSqrtStepFused (FRSQRTS): the inf * 0 cases are fixed values, else a fused step. */
NX_HELPER float fp_recps32(float a, float b) {
    uint32_t ba = fp_bits32(a), bb = fp_bits32(b);
    if ((fp_inf32b(ba) && fp_zero32b(bb)) || (fp_zero32b(ba) && fp_inf32b(bb))) return 2.0f;
    return fp_fma32(fp_neg32(a), b, 2.0f);
}
NX_HELPER float fp_rsqrts32(float a, float b) {
    uint32_t ba = fp_bits32(a), bb = fp_bits32(b);
    if ((fp_inf32b(ba) && fp_zero32b(bb)) || (fp_zero32b(ba) && fp_inf32b(bb))) return 1.5f;
    /* (3 - a*b) / 2, rounded once. Halving a normal operand is exact, so fold the / 2 into it: the fused multiply-add then
     * cannot overflow in the intermediate product (a = 1.5, b = FLT_MAX has the finite result -0.75 * FLT_MAX). */
    float na = fp_neg32(a);
    if (a == a && b == b) {
        if (fabsf(a) >= 2.0f * 1.17549435e-38f) return fmaf(na * 0.5f, b, 1.5f);
        if (fabsf(b) >= 2.0f * 1.17549435e-38f) return fmaf(na, b * 0.5f, 1.5f);
    }
    return fp_fma32(na, b, 3.0f) * 0.5f;
}
NX_HELPER double fp_recps64(double a, double b) {
    uint64_t ba = fp_bits64(a), bb = fp_bits64(b);
    if ((fp_inf64b(ba) && fp_zero64b(bb)) || (fp_zero64b(ba) && fp_inf64b(bb))) return 2.0;
    return fp_fma64(fp_neg64(a), b, 2.0);
}
NX_HELPER double fp_rsqrts64(double a, double b) {
    uint64_t ba = fp_bits64(a), bb = fp_bits64(b);
    if ((fp_inf64b(ba) && fp_zero64b(bb)) || (fp_zero64b(ba) && fp_inf64b(bb))) return 1.5;
    double na = fp_neg64(a);
    if (a == a && b == b) {
        if (fabs(a) >= 2.0 * 2.2250738585072014e-308) return fma(na * 0.5, b, 1.5);
        if (fabs(b) >= 2.0 * 2.2250738585072014e-308) return fma(na, b * 0.5, 1.5);
    }
    return fp_fma64(na, b, 3.0) * 0.5;
}

/* FCVT between single and double (FPConvert, FPCR.DN = 0). A signalling NaN becomes quiet with its payload kept (the
 * top payload bits), sign kept. The host conversion does not quieten every such NaN (measured on Unicorn), so the NaN
 * case is done on the bit patterns. */
NX_HELPER float fp_d2s(double x) {
    uint64_t b = fp_bits64(x);
    if (fp_nan64b(b))
        return fp_from_bits32(((uint32_t)(b >> 32) & 0x80000000u) | 0x7f800000u | 0x00400000u | ((uint32_t)(b >> 29) & 0x3fffffu));
    return (float)x;
}
NX_HELPER double fp_s2d(float x) {
    uint32_t b = fp_bits32(x);
    if (fp_nan32b(b))
        return fp_from_bits64(((uint64_t)(b & 0x80000000u) << 32) | 0x7ff0000000000000ull | FP_QBIT64 | ((uint64_t)(b & 0x3fffffu) << 29));
    return (double)x;
}

/* FRINTx: mode 0 nearest-even (FRINTN, FRINTX, FRINTI in the default rounding mode), 1 nearest ties away (FRINTA),
 * 2 floor (FRINTM), 3 ceiling (FRINTP), 4 towards zero (FRINTZ). A quiet NaN stays; a signalling NaN is quietened
 * (payload and sign kept); the sign of a zero result is that of the operand. */
NX_HELPER float fp_frint32(float x, int mode) {
    uint32_t b = fp_bits32(x);
    if (fp_nan32b(b)) return fp_from_bits32(b | FP_QBIT32);
    switch (mode) {
    case 1: return roundf(x);
    case 2: return floorf(x);
    case 3: return ceilf(x);
    case 4: return truncf(x);
    default: return rintf(x);
    }
}
NX_HELPER double fp_frint64(double x, int mode) {
    uint64_t b = fp_bits64(x);
    if (fp_nan64b(b)) return fp_from_bits64(b | FP_QBIT64);
    switch (mode) {
    case 1: return round(x);
    case 2: return floor(x);
    case 3: return ceil(x);
    case 4: return trunc(x);
    default: return rint(x);
    }
}

#endif
