/*
 * Tux Rocket Science
 * Copyright (C) 2026 Tux Rocket Science contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* cpu_fp.inc -- the RISC-V F and D extensions, included into cpu.c.
 *
 * NOMINAL left them out on purpose (cpu.h): a provisioning game never needs
 * a fraction. A ship's computer does: a pitch program, a burn time, a
 * landing's square root. With them the language's own reals (it always had
 * a V_NUM; the guest build turned it into an int) come back unchanged.
 *
 * The host does the arithmetic in its own IEEE doubles, rounding to nearest
 * even, which is what every platform we build for does for + - * / sqrt and
 * fma: those results are the same bits everywhere. The rounding-mode field
 * is honoured where a program can see it most, converting to an integer
 * (C's casts are round-toward-zero); arithmetic always rounds to nearest.
 * The exception flags are kept at zero. */
#include <math.h>

static float fp_s(const Cpu *c, int r)
{
    uint64_t v = c->f[r];
    uint32_t lo = (uint32_t)v;
    float x;
    if ((v >> 32) != 0xffffffffu)
        lo = 0x7fc00000u; /* not NaN-boxed: the canonical NaN */
    memcpy(&x, &lo, 4);
    return x;
}

static void fp_set_s(Cpu *c, int r, float x)
{
    uint32_t b;
    memcpy(&b, &x, 4);
    if (x != x)
        b = 0x7fc00000u;
    c->f[r] = 0xffffffff00000000ull | b;
}

static double fp_d(const Cpu *c, int r)
{
    double x;
    memcpy(&x, &c->f[r], 8);
    return x;
}

static void fp_set_d(Cpu *c, int r, double x)
{
    uint64_t b;
    memcpy(&b, &x, 8);
    if (x != x)
        b = 0x7ff8000000000000ull;
    c->f[r] = b;
}

/* round to an integer the way rm says (7 = the dynamic mode in fcsr) */
static double fp_round(const Cpu *c, double x, int rm)
{
    if (rm == 7)
        rm = (int)((c->fcsr >> 5) & 7);
    switch (rm) {
    case 1: return trunc(x);
    case 2: return floor(x);
    case 3: return ceil(x);
    case 4: return round(x); /* to nearest, ties away */
    default: return rint(x); /* 0: to nearest, ties even (the host's default) */
    }
}

/* fcvt to an integer: saturating, NaN to the largest, as the spec says */
static uint64_t fp_to_int(const Cpu *c, double x, int rm, int kind)
{
    if (x != x)
        x = INFINITY;
    double r = fp_round(c, x, rm);
    switch (kind) {
    case 0: /* w */
        if (r >= 2147483647.0) return (uint64_t)(int64_t)2147483647;
        if (r <= -2147483648.0) return (uint64_t)(int64_t)-2147483648LL;
        return (uint64_t)(int64_t)(int32_t)r;
    case 1: /* wu */
        if (r >= 4294967295.0) return (uint64_t)(int64_t)(int32_t)0xffffffffu;
        if (r <= 0.0) return 0;
        return (uint64_t)(int64_t)(int32_t)(uint32_t)r;
    case 2: /* l */
        if (r >= 9223372036854775807.0) return (uint64_t)INT64_MAX;
        if (r <= -9223372036854775808.0) return (uint64_t)INT64_MIN;
        return (uint64_t)(int64_t)r;
    default: /* lu */
        if (r >= 18446744073709551615.0) return UINT64_MAX;
        if (r <= 0.0) return 0;
        return (uint64_t)r;
    }
}

static double fp_from_int(uint64_t v, int kind)
{
    switch (kind) {
    case 0: return (double)(int32_t)v;
    case 1: return (double)(uint32_t)v;
    case 2: return (double)(int64_t)v;
    default: return (double)v;
    }
}

/* fmin/fmax: a NaN loses to a number; -0 is below +0 */
static double fp_minmax(double a, double b, int max)
{
    if (a != a && b != b) return NAN;
    if (a != a) return b;
    if (b != b) return a;
    if (a == 0.0 && b == 0.0)
        return (signbit(a) != 0) == !max ? a : b;
    return max ? (a > b ? a : b) : (a < b ? a : b);
}

static uint64_t fp_class(double x)
{
    int neg = signbit(x) != 0;
    if (isinf(x)) return neg ? 1u << 0 : 1u << 7;
    if (x != x) {
        uint64_t b;
        memcpy(&b, &x, 8);
        return (b >> 51) & 1 ? 1u << 9 : 1u << 8;
    }
    if (x == 0.0) return neg ? 1u << 3 : 1u << 4;
    if (fpclassify(x) == FP_SUBNORMAL) return neg ? 1u << 2 : 1u << 5;
    return neg ? 1u << 1 : 1u << 6;
}

static bool fp_ill(Cpu *c, uint32_t ins)
{
    c->trap = TRAP_ILLEGAL;
    c->trap_addr = ins;
    return false;
}

static bool fp_exec(Cpu *c, uint32_t ins, int op)
{
    int rd = RD(ins), r1 = RS1(ins), r2 = RS2(ins), f3 = F3(ins);
    if (op == 0x07 || op == 0x27) { /* flw/fld, fsw/fsd */
        uint64_t imm = op == 0x07 ? (uint64_t)(ins >> 20) : (((uint64_t)F7(ins) << 5) | (uint64_t)RD(ins));
        uint64_t addr = c->x[r1] + (uint64_t)sext(imm, 12);
        int bytes = f3 == 2 ? 4 : f3 == 3 ? 8 : 0;
        if (!bytes)
            return fp_ill(c, ins);
        if (addr & (uint64_t)(bytes - 1)) {
            c->trap = TRAP_MISALIGNED;
            c->trap_addr = addr;
            return false;
        }
        if (op == 0x07) {
            uint64_t v;
            if (!load_mem(c, addr, bytes, &v))
                return false;
            c->f[rd] = bytes == 4 ? (0xffffffff00000000ull | v) : v;
            return true;
        }
        return store_mem(c, addr, bytes, c->f[r2]);
    }
    int dbl = (int)((ins >> 25) & 3);
    if (dbl > 1)
        return fp_ill(c, ins);
    if (op != 0x53) { /* the fused four: rs3 in the top five bits */
        int r3 = (int)(ins >> 27);
        if (dbl) {
            double a = fp_d(c, r1), b = fp_d(c, r2), d = fp_d(c, r3);
            double r = op == 0x43 ? fma(a, b, d) : op == 0x47 ? fma(a, b, -d) : op == 0x4b ? -fma(a, b, -d) : -fma(a, b, d);
            fp_set_d(c, rd, r);
        } else {
            float a = fp_s(c, r1), b = fp_s(c, r2), d = fp_s(c, r3);
            float r = op == 0x43 ? fmaf(a, b, d) : op == 0x47 ? fmaf(a, b, -d) : op == 0x4b ? -fmaf(a, b, -d) : -fmaf(a, b, d);
            fp_set_s(c, rd, r);
        }
        return true;
    }
    int f7 = F7(ins) & ~1;
    if (dbl) {
        double a = fp_d(c, r1), b = fp_d(c, r2);
        switch (f7) {
        case 0x00: fp_set_d(c, rd, a + b); return true;
        case 0x04: fp_set_d(c, rd, a - b); return true;
        case 0x08: fp_set_d(c, rd, a * b); return true;
        case 0x0c: fp_set_d(c, rd, a / b); return true;
        case 0x2c: fp_set_d(c, rd, sqrt(a)); return true;
        case 0x10: { /* fsgnj, fsgnjn, fsgnjx: raw bits */
            uint64_t x = c->f[r1], y = c->f[r2], s = 1ull << 63;
            uint64_t sign = f3 == 0 ? (y & s) : f3 == 1 ? (~y & s) : f3 == 2 ? ((x ^ y) & s) : 2;
            if (sign == 2)
                return fp_ill(c, ins);
            c->f[rd] = (x & ~s) | sign;
            return true;
        }
        case 0x14:
            if (f3 > 1)
                return fp_ill(c, ins);
            fp_set_d(c, rd, fp_minmax(a, b, f3));
            return true;
        case 0x20: /* fcvt.s.d */
            fp_set_s(c, rd, (float)a);
            return true;
        case 0x50:
            setreg(c, rd, f3 == 0 ? a <= b : f3 == 1 ? a < b : f3 == 2 ? a == b : 0);
            return f3 <= 2 ? true : fp_ill(c, ins);
        case 0x60:
            setreg(c, rd, fp_to_int(c, a, f3, r2 & 3));
            return true;
        case 0x68:
            fp_set_d(c, rd, fp_from_int(c->x[r1], r2 & 3));
            return true;
        case 0x70:
            if (f3 == 0) setreg(c, rd, c->f[r1]);
            else if (f3 == 1) setreg(c, rd, fp_class(a));
            else return fp_ill(c, ins);
            return true;
        case 0x78:
            c->f[rd] = c->x[r1];
            return true;
        default: return fp_ill(c, ins);
        }
    }
    float a = fp_s(c, r1), b = fp_s(c, r2);
    switch (f7) {
    case 0x00: fp_set_s(c, rd, a + b); return true;
    case 0x04: fp_set_s(c, rd, a - b); return true;
    case 0x08: fp_set_s(c, rd, a * b); return true;
    case 0x0c: fp_set_s(c, rd, a / b); return true;
    case 0x2c: fp_set_s(c, rd, sqrtf(a)); return true;
    case 0x10: {
        uint32_t x = (uint32_t)c->f[r1], y = (uint32_t)c->f[r2], s = 1u << 31;
        uint32_t sign = f3 == 0 ? (y & s) : f3 == 1 ? (~y & s) : f3 == 2 ? ((x ^ y) & s) : 2;
        if (sign == 2)
            return fp_ill(c, ins);
        c->f[rd] = 0xffffffff00000000ull | ((x & ~s) | sign);
        return true;
    }
    case 0x14:
        if (f3 > 1)
            return fp_ill(c, ins);
        fp_set_s(c, rd, (float)fp_minmax(a, b, f3));
        return true;
    case 0x20: /* fcvt.d.s */
        fp_set_d(c, rd, (double)a);
        return true;
    case 0x50:
        setreg(c, rd, f3 == 0 ? a <= b : f3 == 1 ? a < b : f3 == 2 ? a == b : 0);
        return f3 <= 2 ? true : fp_ill(c, ins);
    case 0x60:
        setreg(c, rd, fp_to_int(c, (double)a, f3, r2 & 3));
        return true;
    case 0x68:
        fp_set_s(c, rd, (float)fp_from_int(c->x[r1], r2 & 3));
        return true;
    case 0x70:
        if (f3 == 0) setreg(c, rd, (uint64_t)(int64_t)(int32_t)(uint32_t)c->f[r1]);
        else if (f3 == 1) setreg(c, rd, fp_class((double)a));
        else return fp_ill(c, ins);
        return true;
    case 0x78:
        c->f[rd] = 0xffffffff00000000ull | (uint32_t)c->x[r1];
        return true;
    default: return fp_ill(c, ins);
    }
}

/* csrrw/s/c and their immediate forms, on fflags (1), frm (2), fcsr (3) */
static bool fp_csr(Cpu *c, uint32_t ins)
{
    int csr = (int)(ins >> 20), f3 = F3(ins), rd = RD(ins);
    if (csr < 1 || csr > 3)
        return fp_ill(c, ins);
    uint32_t mask = csr == 1 ? 0x1f : csr == 2 ? 0x7 : 0xff, shift = csr == 2 ? 5 : 0;
    uint32_t old = (c->fcsr >> shift) & mask;
    uint64_t src = f3 >= 5 ? (uint64_t)RS1(ins) : c->x[RS1(ins)];
    uint32_t v = old;
    switch (f3 & 3) {
    case 1: v = (uint32_t)src; break;
    case 2: v = old | (uint32_t)src; break;
    case 3: v = old & ~(uint32_t)src; break;
    }
    c->fcsr = (c->fcsr & ~(mask << shift)) | ((v & mask) << shift);
    c->fcsr &= 0xe0; /* the flags are never raised */
    setreg(c, rd, old);
    return true;
}
