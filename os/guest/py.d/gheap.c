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

/* gheap.c — a heap, buffers, strings and integer math for the guest.
 *
 * Everything nom.h promises and the machine does not provide. Written for
 * this job rather than lifted, because there was nothing to lift: the host's
 * versions are three lines each on top of libc, and there is no libc here.
 */
#include "gsys.h"
#include "nom.h"

/* ------------------------------------------------------------------ heap
 *
 * TUX ROCKET SCIENCE: boundary tags and bins. Each block says its size and
 * the size of the one before it, so a block freed joins whichever of its
 * neighbours are free (no slivers: a script growing a list or a string
 * over and over doesn't run the heap out). Free blocks sit in bins by size
 * -- one per size up to SMALL_MAX, then one per power of two -- so taking
 * one is a look in the right bin, not a walk past every object a script
 * has alive (the first fit over every block this replaced did that walk on
 * every allocation). */
unsigned char g_heap[HEAP_BYTES] __attribute__((aligned(16)));
u64 g_heap_used = 0, g_heap_peak = 0;

#define ALIGN(n) (((n) + 15u) & ~(u64)15u)
#define SMALL_MAX 512u
#define NSMALL (SMALL_MAX / 16u + 1u)
#define NBIN (NSMALL + 24u)
#define USED 1u

typedef struct FreeLinks { Blk *next, *prev; } FreeLinks;
#define LINKS(b) ((FreeLinks *)((unsigned char *)(b) + sizeof(Blk)))
#define SIZE(b) ((b)->size & ~(u64)15u)
#define NEXT_PHYS(b) ((Blk *)((unsigned char *)(b) + sizeof(Blk) + SIZE(b)))
#define HEAP_END ((Blk *)(g_heap + HEAP_BYTES))

static Blk *g_bin[NBIN];
static int g_ready;

static u32 bin_of(u64 size)
{
    if (size <= SMALL_MAX) return (u32)(size / 16u);
    u32 k = NSMALL;
    for (u64 s = SMALL_MAX * 2; s < size && k < NBIN - 1; s <<= 1) k++;
    return k;
}

static void bin_put(Blk *b)
{
    u32 k = bin_of(SIZE(b));
    LINKS(b)->prev = 0;
    LINKS(b)->next = g_bin[k];
    if (g_bin[k]) LINKS(g_bin[k])->prev = b;
    g_bin[k] = b;
}

static void bin_take(Blk *b)
{
    FreeLinks *l = LINKS(b);
    if (l->prev) LINKS(l->prev)->next = l->next;
    else g_bin[bin_of(SIZE(b))] = l->next;
    if (l->next) LINKS(l->next)->prev = l->prev;
}

/* b's size is now `size`: the one after it is told */
static void set_size(Blk *b, u64 size, u64 used)
{
    b->size = size | used;
    Blk *n = NEXT_PHYS(b);
    if (n < HEAP_END) n->prev = size;
}

static void heap_init(void)
{
    Blk *b = (Blk *)g_heap;
    b->prev = 0;
    set_size(b, HEAP_BYTES - sizeof(Blk), 0);
    bin_put(b);
    g_ready = 1;
}

/* b (free, taken from its bin) holding n: the rest split off, if worth
 * it; zeroed from `from` on (an allocation: all of it) */
static void *carve(Blk *b, u64 n, u64 from)
{
    u64 have = SIZE(b);
    if (have >= n + sizeof(Blk) + 32u) {
        set_size(b, n, USED);
        Blk *rest = NEXT_PHYS(b);
        rest->prev = n;
        set_size(rest, have - n - sizeof(Blk), 0);
        bin_put(rest);
    } else set_size(b, have, USED);
    g_heap_used += SIZE(b) + sizeof(Blk);
    if (g_heap_used > g_heap_peak) g_heap_peak = g_heap_used;
    u64 *w = (u64 *)((unsigned char *)b + sizeof(Blk));
    for (u64 i = from / 8; i < n / 8; i++) w[i] = 0;
    return w;
}

void *nom_alloc(size_t want)
{
    if (!g_ready) heap_init();
    u64 n = ALIGN(want ? want : 1);
    if (n < 16) n = 16;
    u32 k = bin_of(n);
    /* the exact small bin, then the bins above (the first block in a big
     * bin that's big enough) */
    if (n <= SMALL_MAX && g_bin[k]) { Blk *b = g_bin[k]; bin_take(b); return carve(b, n, 0); }
    for (u32 j = k; j < NBIN; j++)
        for (Blk *b = g_bin[j]; b; b = LINKS(b)->next)
            if (SIZE(b) >= n) { bin_take(b); return carve(b, n, 0); }
    return 0;      /* out of memory: every caller checks, because it must */
}

void nom_free(void *p)
{
    if (!p) return;
    Blk *b = (Blk *)((unsigned char *)p - sizeof(Blk));
    if (!(b->size & USED)) return;          /* (freed twice: let it be) */
    u64 size = SIZE(b);
    if (g_heap_used >= size + sizeof(Blk)) g_heap_used -= size + sizeof(Blk);
    /* join the free neighbours */
    Blk *n = NEXT_PHYS(b);
    if (n < HEAP_END && !(n->size & USED)) { bin_take(n); size += sizeof(Blk) + SIZE(n); }
    if (b != (Blk *)g_heap) {
        Blk *pv = (Blk *)((unsigned char *)b - sizeof(Blk) - b->prev);
        if (!(pv->size & USED)) { bin_take(pv); size += sizeof(Blk) + SIZE(pv); b = pv; }
    }
    set_size(b, size, 0);
    bin_put(b);
}

void *nom_realloc(void *p, size_t want)
{
    if (!p) return nom_alloc(want);
    Blk *b = (Blk *)((unsigned char *)p - sizeof(Blk));
    u64 n = ALIGN(want ? want : 1), have = SIZE(b);
    if (have >= n) return p;
    /* the free block after it, if that makes room: grown where it is */
    Blk *nx = NEXT_PHYS(b);
    if (nx < HEAP_END && !(nx->size & USED) && have + sizeof(Blk) + SIZE(nx) >= n) {
        bin_take(nx);
        g_heap_used -= have + sizeof(Blk);
        set_size(b, have + sizeof(Blk) + SIZE(nx), 0);
        return carve(b, n, have);
    }
    void *q = nom_alloc(want);
    if (!q) return 0;
    nom_memcpy(q, p, have);
    nom_free(p);
    return q;
}

char *nom_strdup(const char *s)
{
    size_t n = nom_strlen(s) + 1;
    char *d = nom_alloc(n);
    if (d) nom_memcpy(d, s, n);
    return d;
}

/* ----------------------------------------------------------------- meter
 * TUX ROCKET SCIENCE: what the script has allocated since it began (it
 * counted nothing before: a runaway list ran the heap out instead) */
static size_t meter_base, meter_cap;
static int    meter_on;

void nom_meter_begin(size_t cap) { meter_base = g_heap_used; meter_cap = cap; meter_on = 1; }
void nom_meter_end(void)         { meter_on = 0; }
bool nom_meter_over(void)        { return meter_on && g_heap_used > meter_base + meter_cap; }
size_t nom_meter_used(void)      { return g_heap_used > meter_base ? g_heap_used - meter_base : 0; }

/* --------------------------------------------------------------- strings */
size_t nom_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

void *nom_memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = d; const unsigned char *b = s;
    size_t i = 0;
    /* eight at a time when both line up (a byte at a time was most of a
     * string's cost) */
    if ((((u64)a | (u64)b) & 7) == 0)
        for (; i + 8 <= n; i += 8) *(u64 *)(a + i) = *(const u64 *)(b + i);
    for (; i < n; i++) a[i] = b[i];
    return d;
}

int nom_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int nom_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

int nom_strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

/* THE COMPILER EMITS CALLS TO THESE, whatever -fno-builtin says: zeroing a
 * struct or copying one becomes a memset/memcpy call once it is big enough,
 * and a freestanding program has to provide them by those names or the link
 * fails with no line number. The macros in nom.h are undefined around them so
 * these definitions keep the names the linker is looking for. */
#undef memset
#undef memcpy
#undef memmove
void *memset(void *d, int c, size_t n) { return nom_memset(d, c, n); }
void *memcpy(void *d, const void *s, size_t n) { return nom_memcpy(d, s, n); }
void *memmove(void *d, const void *s, size_t n) { return nom_memmove(d, s, n); }
#define memset  nom_memset
#define memcpy  nom_memcpy
#define memmove nom_memmove

void *nom_memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    size_t i = 0;
    if (((u64)p & 7) == 0) {
        u64 w = (unsigned char)c * 0x0101010101010101ull;
        for (; i + 8 <= n; i += 8) *(u64 *)(p + i) = w;
    }
    for (; i < n; i++) p[i] = (unsigned char)c;
    return d;
}

void *nom_memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    if (a == b) return d;
    if (a < b) { for (size_t i = 0; i < n; i++) a[i] = b[i]; }
    else       { for (size_t i = n; i > 0; i--) a[i - 1] = b[i - 1]; }
    return d;
}

/* The number parser: digits, a point, more digits, an exponent. Exact for
 * every literal anybody types (it builds the digits as an integer and
 * scales once), which is all a script's 0.5 and 9.81 need. */
double nom_strtod(const char *s, char **end)
{
    double sign = 1.0;
    while (*s == ' ' || *s == '\t') s++;
    const char *start = s;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') s++;
    u64 digits = 0;
    int scale = 0, any = 0;
    while (*s >= '0' && *s <= '9') {
        if (digits < 100000000000000000ull) digits = digits * 10 + (u64)(*s - '0');
        else scale++;
        s++; any = 1;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (digits < 100000000000000000ull) { digits = digits * 10 + (u64)(*s - '0'); scale--; }
            s++; any = 1;
        }
    }
    if (any && (*s == 'e' || *s == 'E')) {
        const char *q = s + 1;
        int es = 1, ev = 0;
        if (*q == '-') { es = -1; q++; }
        else if (*q == '+') q++;
        if (*q >= '0' && *q <= '9') {
            while (*q >= '0' && *q <= '9') { if (ev < 400) ev = ev * 10 + (*q - '0'); q++; }
            scale += es * ev;
            s = q;
        }
    }
    double v = (double)digits;
    double p10 = 1.0;
    int k = scale < 0 ? -scale : scale;
    double b = 10.0;
    while (k) { if (k & 1) p10 *= b; b *= b; k >>= 1; }
    v = scale < 0 ? v / p10 : v * p10;
    /* (that is close; the host's strtod is exact: 0.1 must be the double
     * nearest 0.1, or 0.1 + 0.2 prints wrong) */
    if (any) {
        char t[64];
        size_t n = (size_t)(s - start);
        if (n < sizeof t) {
            memcpy(t, start, n);
            t[n] = 0;
            i64 bits = sysc(SYS_num, NUM_PARSE, (i64)t, 0);
            memcpy(&v, &bits, 8);
            if (end) *end = (char *)s;
            return v;
        }
    }
    if (end) *end = (char *)s;
    return sign * v;
}

/* Base 10 and base 16, which is every base a source literal can be in. */
int64_t nom_strtoll(const char *s, char **end, int base)
{
    i64 sign = 1, v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    if (base == 16 || (base == 0 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        for (;;) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            v = v * 16 + d;
            s++;
        }
    } else {
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    }
    if (end) *end = (char *)s;
    return sign * v;
}

/* --------------------------------------------------------------- buffers */
void buf_init(Buf *b)  { b->p = 0; b->len = b->cap = 0; }
void buf_free(Buf *b)  { nom_free(b->p); buf_init(b); }
void buf_clear(Buf *b) { b->len = 0; if (b->p) b->p[0] = 0; }

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->p && b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra + 1) cap *= 2;
    char *q = nom_realloc(b->p, cap);
    if (!q) return;             /* out of memory: the write is dropped, and
                                 * the meter will trip on the next check */
    b->p = q;
    b->cap = cap;
}

void buf_put(Buf *b, const void *data, size_t n)
{
    if (!n) return;
    buf_reserve(b, n);
    if (!b->p || b->len + n + 1 > b->cap) return;
    nom_memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = 0;
}

void buf_puts(Buf *b, const char *s) { if (s) buf_put(b, s, nom_strlen(s)); }
void buf_putc(Buf *b, char c) { buf_put(b, &c, 1); }

static void buf_putll(Buf *b, i64 v)
{
    char t[24];
    int k = 0;
    int neg = v < 0;
    u64 u = neg ? (u64)(-(v + 1)) + 1u : (u64)v;
    if (!u) t[k++] = '0';
    while (u) { t[k++] = (char)('0' + (u % 10u)); u /= 10u; }
    if (neg) buf_putc(b, '-');
    while (k) buf_putc(b, t[--k]);
}

/* %s %d %ld %lld %u %c %% -- every format the language actually uses. */
void buf_vprintf(Buf *b, const char *fmt, va_list ap)
{
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { buf_putc(b, *p); continue; }
        p++;
        /* %.*s -- a length and a pointer. The lexer and compiler use it for
         * every error message they produce, because their identifiers are
         * slices of the source rather than terminated strings. Without it
         * every syntax error read `undefined name '%.*s'`, which is a
         * compiler telling you it has found a problem and refusing to say
         * which. It cost twenty minutes and it was the second thing to fix. */
        int star = 0;
        if (*p == '.' && p[1] == '*') { star = 1; p += 2; }
        /* TUX ROCKET SCIENCE: %lld and %zu read a 64-bit value (the l was
         * skipped, and 10**18 printed as its low 32 bits) */
        int wide = 0;
        while (*p == 'l' || *p == 'z') { wide = 1; p++; }
        switch (*p) {
        case 'u': buf_putll(b, wide ? (i64)va_arg(ap, u64) : (i64)va_arg(ap, unsigned)); break;
        case 's': {
            if (star) {
                int n = va_arg(ap, int);
                const char *s = va_arg(ap, const char *);
                if (s && n > 0) buf_put(b, s, (size_t)n);
                break;
            }
            const char *s = va_arg(ap, const char *); buf_puts(b, s ? s : "(null)"); break;
        }
        case 'd': case 'i': buf_putll(b, wide ? (i64)va_arg(ap, i64) : (i64)va_arg(ap, int)); break;
        case 'c': buf_putc(b, (char)va_arg(ap, int)); break;
        case '%': buf_putc(b, '%'); break;
        default:  buf_putc(b, '%'); if (*p) buf_putc(b, *p); break;
        }
        if (!*p) break;
    }
}

void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(b, fmt, ap);
    va_end(ap);
}

/* The language calls this to print a number: ten significant figures,
 * trailing zeros dropped but never the point (2.0 is a real, 2 is an
 * integer, as in Python), and an exponent outside 1e-5 .. 1e15. */
void buf_putnum(Buf *b, double v, int decimals)
{
    /* TUX ROCKET SCIENCE: as Python prints a float -- the shortest text
     * that reads back to exactly v (0.1 + 0.2 is 0.30000000000000004) --
     * which takes a C library's care, so the host does it (SYS_num) */
    (void)decimals;
    char t[48];
    i64 bits;
    memcpy(&bits, &v, 8);
    i64 n = sysc(SYS_num, NUM_REPR, bits, (i64)t);
    if (n > 0) { buf_put(b, t, (size_t)n); return; }
    buf_puts(b, "?");
}

/* A cut-down (v)snprintf over the same formatter the buffer uses, so there is
 * one implementation of "%s means this" in the program. */
int nom_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap)
{
    Buf b;
    buf_init(&b);
    buf_vprintf(&b, fmt, ap);
    size_t n = b.len;
    if (cap) {
        if (n > cap - 1) n = cap - 1;
        if (b.p) nom_memcpy(out, b.p, n);
        out[n] = 0;
    }
    size_t full = b.len;
    buf_free(&b);
    return (int)full;
}

int nom_snprintf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = nom_vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------ math */
double nom_floor(double x)
{
    if (x != x || x > 4.5e15 || x < -4.5e15) return x; /* already whole */
    double t = (double)(i64)x;
    return t > x ? t - 1.0 : t;
}

static double host_math(int op, double x, double y)
{
    i64 a, c, r;
    double out;
    nom_memcpy(&a, &x, 8);
    nom_memcpy(&c, &y, 8);
    r = sysc(SYS_math, op, a, c);
    nom_memcpy(&out, &r, 8);
    return out;
}

double nom_pow(double b, double e)
{
    return host_math(MATH_POW, b, e);
}

double nom_math(int op, double x, double y) { return host_math(op, x, y); }
