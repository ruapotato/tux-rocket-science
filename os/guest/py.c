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

/* /bin/py — the scripting language, on the machine.
 *
 * A Python subset: indentation, if/elif/else, while, for/in, def/return,
 * and/or/not, True/False/nil, integers, strings, lists and dicts. Handoff
 * decision 14 asked for exactly this -- "the audience knows Python" -- and
 * NOMINAL already had a lexer, compiler and bytecode VM for it, so what this
 * program does is put them on a RISC-V disk.
 *
 *     py script.py        run a file
 *     py -c 'print(1+1)'  run a line
 *
 * WHY IT IS NOT THE SHELL. /bin/sh has loops and pipes and gets a long way,
 * and the first automation anybody writes here will be a shell for loop. What
 * a shell cannot do is hold a parsed answer: `rb ticket.get TCK-1` comes back
 * as a line of JSON, and picking two fields out of it and branching on them
 * is where sh runs out and a language begins. Hence dicts, and hence json().
 */
#include "gsys.h"
#include "nom.h"
#include "lang.h"

/* The language files are compiled as SEPARATE translation units and linked,
 * not #included here. They were included at first, which is the obvious thing
 * when the build takes one .c per program -- and lex.c and compile.c both
 * have a static `emit`, so the first attempt failed to compile in a way that
 * looked like a type error in somebody else's file. Separate units also mean
 * they stay byte-identical to NOMINAL's, which is the whole point. */

static char src[NOM_SRC_MAX];
static char argbuf[GARG_MAX];
static char apibuf[65536];

/* TUX ROCKET SCIENCE: a keyword argument of the call being made, by name
 * (print(..., end="")); reading one says the native took them */
static bool kw_get(VM *v, const char *name, Value *out)
{
    if (v->kw.k == V_NIL) return false;
    v->kw_used = true;
    Value key = str_newz(name), got;
    bool ok = dict_get(AS_DICT(v->kw), key, &got);
    val_release(key);
    if (ok) *out = got;
    return ok;
}

/* ------------------------------------------------------------- natives --
 *
 * The whole library, and it is short on purpose. Everything the game can do
 * is one call away through api(), so this does not need a function per verb
 * -- it needs the handful of things you cannot write in the language itself.
 */

/* print(...) -- to stdout, which on this machine is the terminal. */
static VmStatus n_print(VM *v, Value *a, int n, Value *out)
{
    Value sep = VAL_NIL, end = VAL_NIL;
    kw_get(v, "sep", &sep);
    kw_get(v, "end", &end);
    Buf b;
    buf_init(&b);
    for (int i = 0; i < n; i++) {
        if (i) { if (IS_STR(sep)) buf_put(&b, AS_STR(sep)->s, AS_STR(sep)->len); else buf_putc(&b, ' '); }
        val_tostr(&b, a[i]);
    }
    if (IS_STR(end)) buf_put(&b, AS_STR(end)->s, AS_STR(end)->len); else buf_putc(&b, '\n');
    if (b.p) g_write(1, b.p, b.len);
    buf_free(&b);
    *out = VAL_NIL;
    return VM_OK;
}

/* do(command) -- THE ONE THAT MATTERS.
 *
 * The same command the button sends. `do("power shields 3")` is what clicking
 * on the shields does, and there is nothing a button can send that this
 * cannot -- which is the whole reason the console can honestly mirror your
 * clicking, and the reason a script is a first-class way to play rather than
 * a bolted-on convenience.
 *
 * It is called do() and not api() because a player reading their own recorded
 * script should see a verb, not an acronym. */
static VmStatus n_do(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    if (!IS_STR(a[0])) { *out = VAL_NIL; return VM_OK; }
    i64 got = sysc(SYS_rbapi, (i64)AS_STR(a[0])->s, (i64)apibuf, (i64)sizeof apibuf);
    if (got < 0) { *out = str_newz("-ERR no world attached"); return VM_OK; }

    /* THE ENVELOPE COMES OFF HERE, ONCE, instead of in every script.
     *
     * The wire format is a status line, then any body, then a lone dot. That
     * is right for a protocol and wrong for a person: the first script
     * anybody writes is `for line in lines(do("rooms"))`, and the first thing
     * it printed was "+OK rooms" -- a line that is not a room, does not parse
     * as one, and is entirely about plumbing they did not ask about.
     *
     * So: a command that answered with a body gives you the body. A command
     * that just did something gives you what it said it did. An error gives
     * you the error, with its marker left on so a script can test for it.
     *
     * A player should never have to know there was an envelope. */
    size_t len = (size_t)got;
    const char *p = apibuf;

    size_t first = 0;
    while (first < len && p[first] != '\n') first++;

    if (len >= 4 && p[0] == '-' && p[1] == 'E') {
        *out = str_new(p, first);          /* "-ERR why", one line */
        return VM_OK;
    }

    size_t bstart = (first < len) ? first + 1 : len;
    size_t bend = len;
    /* Drop the terminating ".\n", and any trailing newline after it. */
    while (bend > bstart && (p[bend - 1] == '\n' || p[bend - 1] == '\r')) bend--;
    if (bend > bstart && p[bend - 1] == '.' &&
        (bend - 1 == bstart || p[bend - 2] == '\n')) {
        bend--;
        while (bend > bstart && (p[bend - 1] == '\n' || p[bend - 1] == '\r')) bend--;
    }

    if (bend > bstart) {
        *out = str_new(p + bstart, bend - bstart);
        return VM_OK;
    }

    /* No body: the status line IS the answer. "+OK shields at 3, 0 spare"
     * becomes "shields at 3, 0 spare", which is a sentence a script can
     * print straight at somebody. */
    size_t s = 0;
    if (first >= 3 && p[0] == '+' && p[1] == 'O' && p[2] == 'K') {
        s = 3;
        while (s < first && p[s] == ' ') s++;
    }
    *out = str_new(p + s, first - s);
    return VM_OK;
}

/* ship() -- the whole ship, as one answer, ready for json().
 *
 * A script that acts without looking is a macro. A script that looks is a
 * program, and this is the line that separates them:
 *
 *     s = json(ship())
 *     if s["weapon"] == "100":
 *         do("fire")
 */
static VmStatus n_ship(VM *v, Value *a, int n, Value *out)
{
    (void)a; (void)n;
    Value line = str_newz("ship");
    Value args[1] = { line };
    VmStatus st = n_do(v, args, 1, out);
    val_release(line);
    return st;
}

/* len, str, int -- the three conversions every script needs in its first
 * five lines. */
static const char *kind_of(Value x);
static bool is_fn(Value x);
static VmStatus n_len(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    if (IS_STR(a[0]))  { *out = VAL_INT(AS_STR(a[0])->len);  return VM_OK; }
    if (IS_LIST(a[0])) { *out = VAL_INT(AS_LIST(a[0])->len); return VM_OK; }
    if (IS_DICT(a[0])) { *out = VAL_INT(AS_DICT(a[0])->len); return VM_OK; }
    if (IS_RANGE(a[0])) { *out = VAL_INT(range_len(AS_RANGE(a[0]))); return VM_OK; }
    Value fn;
    if (vm_method(a[0], "__len__", &fn)) return vm_call(v, fn, a, 1, out) ? VM_OK : VM_ERROR;
    vm_runtime_error(v, "TypeError: a %s has no len()", kind_of(a[0]));
    return VM_ERROR;
}

static VmStatus n_str(VM *v, Value *a, int n, Value *out)
{
    (void)v;
    if (!n) { *out = str_newz(""); return VM_OK; }
    Buf b;
    buf_init(&b);
    val_tostr(&b, a[0]);
    *out = str_new(b.p ? b.p : "", b.len);
    buf_free(&b);
    return VM_OK;
}

static bool nom_memchr_(const char *cs, char c) { for (; *cs; cs++) if (*cs == c) return true; return false; }
static bool set_has(List *l, Value x) { for (u32 i = 0; i < l->len; i++) if (val_equal(l->v[i], x)) return true; return false; }
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/* int("42"), int(" -7 "), int("ff", 16), int("0x1f", 0), int(3.9): as
 * Python does (a string that isn't a whole number is a ValueError) */
static VmStatus n_int(VM *v, Value *a, int n, Value *out)
{
    if (!n) { *out = VAL_INT(0); return VM_OK; }
    if (n > 1 && !IS_STR(a[0])) { vm_runtime_error(v, "TypeError: int() can't convert non-string with explicit base"); return VM_ERROR; }
    if (IS_STR(a[0])) {
        Str *s = AS_STR(a[0]);
        int base = n > 1 ? (int)val_int(a[1]) : 10;
        u32 i = 0, j = s->len;
        while (i < j && is_space(s->s[i])) i++;
        while (j > i && is_space(s->s[j - 1])) j--;
        bool neg = false;
        if (i < j && (s->s[i] == '-' || s->s[i] == '+')) { neg = s->s[i] == '-'; i++; }
        if (j - i > 2 && s->s[i] == '0') {
            char c = (char)(s->s[i + 1] | 32);
            int pb = c == 'x' ? 16 : c == 'o' ? 8 : c == 'b' ? 2 : 0;
            if (pb && (base == pb || base == 0)) { base = pb; i += 2; }
        }
        if (base == 0) base = 10;
        if (base < 2 || base > 36) { vm_runtime_error(v, "ValueError: int() base must be >= 2 and <= 36, or 0"); return VM_ERROR; }
        i64 val = 0;
        double dv = 0;
        bool any = false, over = false, bad = i >= j;
        for (u32 k = i; k < j && !bad; k++) {
            char c = s->s[k];
            if (c == '_' && any && k + 1 < j && s->s[k + 1] != '_') continue;
            int d = c >= '0' && c <= '9' ? c - '0' : ((c | 32) >= 'a' && (c | 32) <= 'z') ? (c | 32) - 'a' + 10 : 99;
            if (d >= base) { bad = true; break; }
            any = true;
            dv = dv * base + d;
            if (!over && (__builtin_mul_overflow(val, (i64)base, &val) || __builtin_add_overflow(val, (i64)d, &val))) over = true;
        }
        if (bad || !any) {
            Buf b; buf_init(&b);
            val_repr(&b, a[0]);
            buf_putc(&b, 0);
            bool point = false;
            for (u32 k = i; k < j; k++) point |= s->s[k] == '.' || s->s[k] == 'e';
            if (point && base == 10) vm_runtime_error(v, "ValueError: invalid literal for int() with base %d: %s (a number with a point: int(float(x)))", base, b.p);
            else vm_runtime_error(v, "ValueError: invalid literal for int() with base %d: %s", base, b.p);
            buf_free(&b);
            return VM_ERROR;
        }
        *out = over ? VAL_NUM(neg ? -dv : dv) : VAL_INT(neg ? -val : val);
        return VM_OK;
    }
    if (a[0].k == V_NUM) {
        double d = a[0].as.d;
        if (d != d) { vm_runtime_error(v, "ValueError: cannot convert float NaN to integer"); return VM_ERROR; }
        if (d > 1.7e308 || d < -1.7e308) { vm_runtime_error(v, "OverflowError: cannot convert float infinity to integer"); return VM_ERROR; }
        *out = (d >= 9.2e18 || d <= -9.2e18) ? VAL_NUM(d < 0 ? -nom_floor(-d) : nom_floor(d)) : VAL_INT((i64)d);
        return VM_OK;
    }
    if (a[0].k == V_INT || a[0].k == V_BOOL) { *out = VAL_INT(val_int(a[0])); return VM_OK; }
    vm_runtime_error(v, "TypeError: int() needs a number or text, not a %s", kind_of(a[0]));
    return VM_ERROR;
}

/* split(s, sep) -- a list of pieces. The workhorse of reading anything back
 * from the API, and the thing sh can only fake. */
static VmStatus n_split(VM *v, Value *a, int n, Value *out)
{
    (void)v;
    if (!IS_STR(a[0])) { *out = list_new(); return VM_OK; }
    const char *s = AS_STR(a[0])->s;
    u32 slen = AS_STR(a[0])->len;
    /* TUX ROCKET SCIENCE: "a  b".split() -- on any run of white space */
    if (n < 2 || a[1].k == V_NIL) {
        Value l = list_new();
        u32 i = 0;
        while (i < slen) {
            while (i < slen && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
            u32 st = i;
            while (i < slen && !(s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
            if (i > st) list_push(AS_LIST(l), str_new(s + st, i - st));
        }
        *out = l;
        return VM_OK;
    }
    const char *sep = (n > 1 && IS_STR(a[1])) ? AS_STR(a[1])->s : "\n";
    size_t seplen = nom_strlen(sep);
    if (!seplen) seplen = 1;

    Value list = list_new();
    size_t start = 0;
    for (size_t i = 0; i + seplen <= slen; ) {
        if (nom_strncmp(s + i, sep, seplen) == 0) {
            list_push(AS_LIST(list), str_new(s + start, i - start));
            i += seplen;
            start = i;
        } else i++;
    }
    list_push(AS_LIST(list), str_new(s + start, slen - start));
    *out = list;
    return VM_OK;
}

/* find(haystack, needle) -- the index, or -1. */
static VmStatus n_find(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    *out = VAL_INT(-1);
    if (!IS_STR(a[0]) || !IS_STR(a[1])) return VM_OK;
    const char *h = AS_STR(a[0])->s, *nd = AS_STR(a[1])->s;
    u32 hl = AS_STR(a[0])->len, nl = AS_STR(a[1])->len;
    if (!nl || nl > hl) return VM_OK;
    for (u32 i = 0; i + nl <= hl; i++)
        if (nom_strncmp(h + i, nd, nl) == 0) { *out = VAL_INT((i64)i); return VM_OK; }
    return VM_OK;
}

/* json(text) -- every "key":"value" pair in one flat dict.
 *
 * FLAT, AND NESTING IS WALKED INTO, which is the same decision the desktop's
 * reader made and for the same reason: a ticket carries
 * `"subject":{"ref":"u_00041"}` and what a script wants from that is `ref`.
 * Arrays are kept as their raw text, because walking into one turns its
 * second element into a key.
 *
 * It is not a JSON parser and does not pretend to be. It is the thing that
 * turns one line of an API answer into something you can index.
 */
static VmStatus n_json(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    Value d = dict_new();
    *out = d;
    if (!IS_STR(a[0])) return VM_OK;
    const char *s = AS_STR(a[0])->s;
    u32 len = AS_STR(a[0])->len;

    u32 i = 0;
    while (i < len) {
        while (i < len && s[i] != '"') i++;
        if (i >= len) break;
        u32 k0 = ++i;
        while (i < len && s[i] != '"') i++;
        if (i >= len) break;
        u32 k1 = i++;
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= len || s[i] != ':') continue;      /* a string, not a key */
        i++;
        while (i < len && s[i] == ' ') i++;
        if (i >= len) break;

        Value key = str_new(s + k0, k1 - k0);
        if (s[i] == '"') {
            u32 v0 = ++i;
            while (i < len && s[i] != '"') i++;
            dict_set(AS_DICT(d), key, str_new(s + v0, i - v0));
            if (i < len) i++;
        } else if (s[i] == '[') {
            u32 v0 = i;
            int depth = 0;
            while (i < len) {
                if (s[i] == '[') depth++;
                else if (s[i] == ']') { depth--; if (!depth) { i++; break; } }
                i++;
            }
            dict_set(AS_DICT(d), key, str_new(s + v0, i - v0));
        } else if (s[i] == '{') {
            /* walked into: the keys inside land in this same dict */
            i++;
        } else {
            u32 v0 = i;
            while (i < len && s[i] != ',' && s[i] != '}' && s[i] != ']') i++;
            u32 v1 = i;
            while (v1 > v0 && (s[v1 - 1] == ' ')) v1--;
            dict_set(AS_DICT(d), key, str_new(s + v0, v1 - v0));
        }
        /* NO val_release(key) HERE. dict_set TAKES OWNERSHIP of both the key
         * and the value -- it stores them without retaining, and releases the
         * key itself when it overwrites an existing entry. Releasing it here
         * as well freed the string the dict was still pointing at, and the
         * script then failed with `key "id" not present` about a dict that
         * had just been given one. */
    }
    return VM_OK;
}

/* lines(text) -- split on newline, dropping empties. Half of every script
 * starts with it, so it is not worth making everybody write it. */
static VmStatus n_lines(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value sep = str_newz("\n");
    Value args[2] = { a[0], sep };
    Value all;
    n_split(v, args, 2, &all);
    val_release(sep);

    Value keep = list_new();
    List *src_l = AS_LIST(all);
    for (u32 i = 0; i < src_l->len; i++) {
        if (IS_STR(src_l->v[i]) && AS_STR(src_l->v[i])->len > 0)
            list_push(AS_LIST(keep), val_retain(src_l->v[i]));
    }
    val_release(all);
    *out = keep;
    return VM_OK;
}

/* lower(s), upper(s) -- the org's naming convention is lowercase, and every
 * script here starts by applying it. */
static VmStatus n_case(VM *v, Value *a, int n, Value *out, int up)
{
    (void)v; (void)n;
    if (!IS_STR(a[0])) { *out = val_retain(a[0]); return VM_OK; }
    Str *s = AS_STR(a[0]);
    Buf b;
    buf_init(&b);
    for (u32 i = 0; i < s->len; i++) {
        char c = s->s[i];
        if (up  && c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!up && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        buf_putc(&b, c);
    }
    *out = str_new(b.p ? b.p : "", b.len);
    buf_free(&b);
    return VM_OK;
}

static VmStatus n_lower(VM *v, Value *a, int n, Value *out) { return n_case(v, a, n, out, 0); }
static VmStatus n_upper(VM *v, Value *a, int n, Value *out) { return n_case(v, a, n, out, 1); }

/* sub(s, from, to) -- a slice, spelled as a call.
 *
 * The language has no slice syntax and adding one would mean editing the
 * compiler, which is lifted. A function does the same work and reads no
 * worse, and `sub(family, 0, 1)` says what it does. */
static VmStatus n_sub(VM *v, Value *a, int n, Value *out)
{
    (void)v;
    *out = str_newz("");
    if (!IS_STR(a[0])) return VM_OK;
    Str *s = AS_STR(a[0]);
    i64 from = val_int(a[1]);
    i64 to = (n > 2) ? val_int(a[2]) : (i64)s->len;
    if (from < 0) from += (i64)s->len;
    if (to < 0)   to += (i64)s->len;
    if (from < 0) from = 0;
    if (to > (i64)s->len) to = (i64)s->len;
    if (to <= from) return VM_OK;
    val_release(*out);
    *out = str_new(s->s + from, (size_t)(to - from));
    return VM_OK;
}

/* append(list, value) -- BUILD a list, not just read one.
 *
 * The selftest found this missing by not being able to count itself, which is
 * a small embarrassment standing in for a large one: a language where lists
 * are read-only is a language you cannot collect anything in, and collecting
 * things is most of what a provisioning script does. Every one of the natives
 * from here down is in the same category -- not features, but the difference
 * between a language and a demonstration. */
static VmStatus n_append(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    if (IS_LIST(a[0])) list_push(AS_LIST(a[0]), val_retain(a[1]));
    *out = val_retain(a[0]);
    return VM_OK;
}

/* keys(dict) -- because a dict you cannot iterate is a dict you can only ask
 * questions you already know the answer to. */
static VmStatus n_keys(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    Value l = list_new();
    if (IS_DICT(a[0])) {
        Dict *d = AS_DICT(a[0]);
        for (u32 i = 0; i < d->len; i++) list_push(AS_LIST(l), val_retain(d->k[i]));
    }
    *out = l;
    return VM_OK;
}

/* has(dict, key) -- indexing a key that is not there is an ERROR, and it
 * should be: a script quietly reading nil out of a typo'd field is exactly
 * the kind of bug this game is about. So there has to be a way to ask. */
static VmStatus n_has(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    Value found;
    *out = VAL_BOOL(IS_DICT(a[0]) && dict_get(AS_DICT(a[0]), a[1], &found));
    return VM_OK;
}

/* join(list, sep) -- the other half of split. */
static VmStatus n_join(VM *v, Value *a, int n, Value *out)
{
    const char *sep = (n > 1 && IS_STR(a[1])) ? AS_STR(a[1])->s : "";
    Buf b;
    buf_init(&b);
    Value items;
    if (!vm_items(v, a[0], &items)) return VM_ERROR;
    List *l = AS_LIST(items);
    for (u32 i = 0; i < l->len; i++) {
        if (!IS_STR(l->v[i])) {
            vm_runtime_error(v, "TypeError: join() wants text, and item %d is a %s (str(x) makes it text)", (int)i, kind_of(l->v[i]));
            buf_free(&b);
            val_release(items);
            return VM_ERROR;
        }
        if (i) buf_puts(&b, sep);
        val_tostr(&b, l->v[i]);
    }
    val_release(items);
    *out = str_new(b.p ? b.p : "", b.len);
    buf_free(&b);
    return VM_OK;
}

/* strip(s) -- every line read back from anything has whitespace on it. */
static VmStatus n_strip(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    if (!IS_STR(a[0])) { *out = val_retain(a[0]); return VM_OK; }
    Str *s = AS_STR(a[0]);
    u32 i = 0, j = s->len;
    while (i < j && (s->s[i] == ' ' || s->s[i] == '\t' || s->s[i] == '\n' || s->s[i] == '\r')) i++;
    while (j > i && (s->s[j-1] == ' ' || s->s[j-1] == '\t' || s->s[j-1] == '\n' || s->s[j-1] == '\r')) j--;
    *out = str_new(s->s + i, j - i);
    return VM_OK;
}

/* replace(s, from, to) */
static VmStatus n_replace(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    if (!IS_STR(a[0]) || !IS_STR(a[1])) { *out = val_retain(a[0]); return VM_OK; }
    Str *s = AS_STR(a[0]);
    Str *f = AS_STR(a[1]);
    const char *t = (n > 2 && IS_STR(a[2])) ? AS_STR(a[2])->s : "";
    Buf b;
    buf_init(&b);
    if (!f->len) { *out = val_retain(a[0]); buf_free(&b); return VM_OK; }
    for (u32 i = 0; i < s->len; ) {
        if (i + f->len <= s->len && nom_strncmp(s->s + i, f->s, f->len) == 0) {
            buf_puts(&b, t);
            i += f->len;
        } else {
            buf_putc(&b, s->s[i]);
            i++;
        }
    }
    *out = str_new(b.p ? b.p : "", b.len);
    buf_free(&b);
    return VM_OK;
}

/* read(path) / write(path, text) -- THE MACHINE IS A COMPUTER, and a script
 * that cannot keep a note between runs is a script that has to be told
 * everything twice. A list of people to onboard is a file. */
static char io_buf[32768];

static VmStatus n_read(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    *out = VAL_NIL;
    if (!IS_STR(a[0])) return VM_OK;
    i64 got = g_slurp(AS_STR(a[0])->s, io_buf, sizeof io_buf);
    if (got < 0) return VM_OK;          /* nil, so `if read(p):` reads well */
    *out = str_new(io_buf, (size_t)got);
    return VM_OK;
}

static VmStatus n_write(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    *out = VAL_BOOL(false);
    if (!IS_STR(a[0])) return VM_OK;
    Buf b;
    buf_init(&b);
    val_tostr(&b, a[1]);
    int fd = g_open(AS_STR(a[0])->s, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        g_write(fd, b.p ? b.p : "", b.len);
        g_close(fd);
        *out = VAL_BOOL(true);
    }
    buf_free(&b);
    return VM_OK;
}

/* exit(code) -- because a script that has decided it is finished should be
 * able to say so. */
static VmStatus n_exit(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)out;
    g_exit(n > 0 ? (int)val_int(a[0]) : 0);
    return VM_OK;
}


/* ============================================================ TUX ROCKET SCIENCE
 * NUMBERS, AND THE FLIGHT LIBRARY.
 *
 * The machine has reals now (machine/cpu_fp.inc), so the language does: 2.5
 * is 2.5. These are the functions a flight needs on top: the maths, and the
 * ship's programs as plain calls that wait for the ship --
 *
 *     launch(80000)
 *     wait_for_window("krill")
 *     until_orbit_crosses("krill")
 *     warp_to("krill")
 *     capture(30000)
 *     land()                 (or land("chime"), land(-10, -24): down by it)
 *
 * Every one of them is a `ship` command (launch() is do("launch 80000")),
 * and the waiting is the only thing added: the game flies the program frame
 * by frame, and the call returns when it is done. One that fails stops the
 * script and says why, so a landing never goes on after a capture that did
 * not happen.
 *
 * WAITING NEEDS A SHIP THAT MOVES ON. A script started with `ship run` runs
 * in the background between the game's frames, and gives up its slice while
 * it waits (SYS_yield). Typed at a terminal, a command holds the machine
 * until it is done and the ship cannot move on, so the waiting calls say so
 * instead of hanging. */

static double arg_num(Value v) { return val_num(v); }

/* float("2.5"), float(" 1e3 "), float("inf"), float(2): as Python does */
static VmStatus n_float(VM *v, Value *a, int n, Value *out)
{
    if (!n) { *out = VAL_NUM(0.0); return VM_OK; }
    if (IS_STR(a[0])) {
        Str *s = AS_STR(a[0]);
        u32 i = 0, j = s->len;
        while (i < j && is_space(s->s[i])) i++;
        while (j > i && is_space(s->s[j - 1])) j--;
        char t[64];
        u32 k = 0;
        bool ok = j > i && j - i < sizeof t;
        for (u32 q = i; ok && q < j; q++) t[k++] = (char)(s->s[q] >= 'A' && s->s[q] <= 'Z' ? s->s[q] + 32 : s->s[q]);
        t[k] = 0;
        if (ok) {
            const char *w = t + (t[0] == '-' || t[0] == '+');
            double sg = t[0] == '-' ? -1.0 : 1.0;
            if (!nom_strcmp(w, "inf") || !nom_strcmp(w, "infinity")) { *out = VAL_NUM(sg * __builtin_inf()); return VM_OK; }
            if (!nom_strcmp(w, "nan")) { *out = VAL_NUM(__builtin_nan("")); return VM_OK; }
            char *end = 0;
            double d = nom_strtod(t, &end);
            if (end && *end == 0 && end != t && (w[0] == '.' || (w[0] >= '0' && w[0] <= '9'))) { *out = VAL_NUM(d); return VM_OK; }
        }
        Buf b; buf_init(&b);
        val_repr(&b, a[0]);
        buf_putc(&b, 0);
        vm_runtime_error(v, "ValueError: could not convert string to float: %s", b.p);
        buf_free(&b);
        return VM_ERROR;
    }
    if (a[0].k != V_INT && a[0].k != V_NUM && a[0].k != V_BOOL) {
        vm_runtime_error(v, "TypeError: float() needs a number or text, not a %s", kind_of(a[0]));
        return VM_ERROR;
    }
    *out = VAL_NUM(val_num(a[0]));
    return VM_OK;
}

static VmStatus n_abs(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value fn;
    if (vm_method(a[0], "__abs__", &fn)) return vm_call(v, fn, a, 1, out) ? VM_OK : VM_ERROR;
    if (a[0].k == V_INT || a[0].k == V_BOOL) { i64 x = val_int(a[0]); *out = VAL_INT(x < 0 ? -x : x); }
    else if (a[0].k == V_NUM) { double x = a[0].as.d; *out = VAL_NUM(x < 0 || (x == 0 && 1.0 / x < 0) ? -x : x); }
    else { vm_runtime_error(v, "TypeError: abs() of a %s (it wants a number)", kind_of(a[0])); return VM_ERROR; }
    return VM_OK;
}

static int py_cmp(VM *v, Value a, Value b, bool *ok);

static VmStatus n_minmax(VM *v, Value *a, int n, Value *out, int max)
{
    Value key = VAL_NIL;
    kw_get(v, "key", &key);
    Value tmp = VAL_NIL;
    if (n == 1 && IS_RANGE(a[0])) {
        Range *r = AS_RANGE(a[0]);
        i64 len = range_len(r);
        if (!len) { vm_runtime_error(v, "%s() of an empty range", max ? "max" : "min"); return VM_ERROR; }
        i64 last = r->start + (len - 1) * r->step;
        *out = VAL_INT(max ? (r->step > 0 ? last : r->start) : (r->step > 0 ? r->start : last));
        return VM_OK;
    }
    Value dflt = VAL_NIL, items = VAL_NIL;
    bool has_d = kw_get(v, "default", &dflt);
    if (n == 1) {
        if (!vm_items(v, a[0], &items)) return VM_ERROR;
        a = AS_LIST(items)->v;
        n = (int)AS_LIST(items)->len;
    }
    if (!n) {
        val_release(items);
        if (has_d) { *out = val_retain(dflt); return VM_OK; }
        vm_runtime_error(v, "ValueError: %s() of nothing (an empty list) -- %s(xs, default=0) gives 0 then", max ? "max" : "min", max ? "max" : "min");
        return VM_ERROR;
    }
    int best = 0;
    Value bk = VAL_NIL;
    if (key.k != V_NIL && !vm_call(v, key, &a[0], 1, &bk)) { val_release(items); return VM_ERROR; }
    for (int i = 1; i < n; i++) {
        Value xk = a[i];
        if (key.k != V_NIL && !vm_call(v, key, &a[i], 1, &xk)) { val_release(bk); val_release(items); return VM_ERROR; }
        bool ok = true;
        int c = py_cmp(v, key.k != V_NIL ? xk : a[i], key.k != V_NIL ? bk : a[best], &ok);
        if (!ok) { if (key.k != V_NIL) { val_release(xk); val_release(bk); } val_release(items); return VM_ERROR; }
        if (max ? c > 0 : c < 0) {
            best = i;
            if (key.k != V_NIL) { val_release(bk); bk = xk; }
        } else if (key.k != V_NIL) val_release(xk);
    }
    if (key.k != V_NIL) val_release(bk);
    (void)tmp;
    *out = val_retain(a[best]);
    val_release(items);
    return VM_OK;
}
static VmStatus n_min(VM *v, Value *a, int n, Value *out) { return n_minmax(v, a, n, out, 0); }
static VmStatus n_max(VM *v, Value *a, int n, Value *out) { return n_minmax(v, a, n, out, 1); }

static VmStatus n_floor(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    *out = VAL_INT((i64)nom_floor(val_num(a[0])));
    return VM_OK;
}

static VmStatus n_ceil(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    double x = val_num(a[0]), f = nom_floor(x);
    *out = VAL_INT((i64)(f < x ? f + 1 : f));
    return VM_OK;
}

/* round(x) to a whole number; round(x, 2) to two places */
/* half to even, as Python rounds: round(2.5) is 2, round(3.5) is 4 */
static double half_even(double x)
{
    double f = nom_floor(x), d = x - f;
    if (d > 0.5) return f + 1;
    if (d < 0.5) return f;
    return (f / 2 == nom_floor(f / 2)) ? f : f + 1;
}

static bool host_fixed(double x, int places, char *out);
static VmStatus n_round(VM *v, Value *a, int n, Value *out)
{
    (void)v;
    double x = val_num(a[0]);
    bool whole = a[0].k == V_INT || a[0].k == V_BOOL;
    if (!whole && a[0].k != V_NUM) { vm_runtime_error(v, "TypeError: round() of a %s (it wants a number)", kind_of(a[0])); return VM_ERROR; }
    if (n < 2 || a[1].k == V_NIL) {
        if (whole) { *out = VAL_INT(val_int(a[0])); return VM_OK; }
        if (x != x || x > 1.7e308 || x < -1.7e308) { vm_runtime_error(v, "ValueError: can't round %s to a whole number", x != x ? "nan" : "infinity"); return VM_ERROR; }
        *out = VAL_INT((i64)half_even(x));
        return VM_OK;
    }
    double p = 1;
    i64 k = val_int(a[1]);
    if (whole) {
        /* round(1234, -2) is 1200: a whole number stays one */
        if (k >= 0) { *out = VAL_INT(val_int(a[0])); return VM_OK; }
        for (i64 i = 0; i > k; i--) p *= 10;
        *out = VAL_INT((i64)(half_even(x / p) * p));
        return VM_OK;
    }
    for (i64 i = 0; i < k; i++) p *= 10;
    for (i64 i = 0; i > k; i--) p /= 10;
    char hf[64];
    if (k >= 0 && k <= 40 && host_fixed(x, (int)k, hf)) {
        /* the decimal digits, correctly rounded, read back (as Python does) */
        *out = VAL_NUM(nom_strtod(hf, 0));
        return VM_OK;
    }
    *out = VAL_NUM(half_even(x * p) / p);
    return VM_OK;
}

static VmStatus n_sqrt(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    double x = val_num(a[0]);
    if (x < 0) { vm_runtime_error(v, "sqrt of a negative number (%d)", (int)x); return VM_ERROR; }
    *out = VAL_NUM(__builtin_sqrt(x));
    return VM_OK;
}

#define MATH1(fn, op) \
    static VmStatus fn(VM *v, Value *a, int n, Value *out) \
    { (void)v; (void)n; *out = VAL_NUM(nom_math(op, val_num(a[0]), 0)); return VM_OK; }
MATH1(n_sin, MATH_SIN)
MATH1(n_cos, MATH_COS)
MATH1(n_tan, MATH_TAN)
MATH1(n_asin, MATH_ASIN)
MATH1(n_acos, MATH_ACOS)
MATH1(n_atan, MATH_ATAN)
MATH1(n_exp, MATH_EXP)
MATH1(n_log, MATH_LOG)

static VmStatus n_atan2(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    *out = VAL_NUM(nom_math(MATH_ATAN2, val_num(a[0]), val_num(a[1])));
    return VM_OK;
}

/* degrees and radians: the flight speaks degrees, sin() speaks radians */
static VmStatus n_rad(VM *v, Value *a, int n, Value *out)
{ (void)v; (void)n; *out = VAL_NUM(val_num(a[0]) * 0.017453292519943295); return VM_OK; }
static VmStatus n_deg(VM *v, Value *a, int n, Value *out)
{ (void)v; (void)n; *out = VAL_NUM(val_num(a[0]) * 57.29577951308232); return VM_OK; }

/* ---- the ship ---- */

/* one line to the ship, its answer into buf (the envelope off, as do()) */
static const char *ask(VM *v, const char *line)
{
    static char said[16384];
    Value l = str_newz(line), r;
    Value args[1] = { l };
    n_do(v, args, 1, &r);
    val_release(l);
    u32 k = 0;
    if (IS_STR(r)) {
        Str *st = AS_STR(r);
        for (; k < st->len && k < sizeof said - 1; k++) said[k] = st->s[k];
    }
    said[k] = 0;
    val_release(r);
    return said;
}

static bool is_err(const char *s) { return s[0] == '-' && s[1] == 'E' && s[2] == 'R' && s[3] == 'R'; }

/* a reading as text, or NULL */
static const char *reading(VM *v, const char *key)
{
    char line[96];
    nom_snprintf(line, sizeof line, "get %s", key);
    const char *r = ask(v, line);
    return is_err(r) ? 0 : r;
}

static double reading_num(VM *v, const char *key)
{
    const char *r = reading(v, key);
    return r ? nom_strtod(r, 0) : 0;
}

/* a reading as the number it is (or the word) */
static Value reading_value(const char *r)
{
    const char *q = r;
    if (*q == '-') q++;
    bool num = *q >= '0' && *q <= '9', dot = false;
    for (; num && *q; q++) {
        if (*q == '.' || *q == 'e' || *q == 'E' || ((*q == '-' || *q == '+') && (q[-1] == 'e' || q[-1] == 'E'))) dot = true;
        else if (*q < '0' || *q > '9') num = false;
    }
    if (!num) return str_newz(r);
    return dot ? VAL_NUM(nom_strtod(r, 0)) : VAL_INT(nom_strtoll(r, 0, 10));
}

/* get("apoapsis") -> 80211.5; get("situation") -> "orbiting" */
static VmStatus n_get(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    if (!IS_STR(a[0])) { *out = VAL_NIL; return VM_OK; }
    const char *r = reading(v, AS_STR(a[0])->s);
    *out = r ? reading_value(r) : VAL_NIL;
    return VM_OK;
}

/* the rest of this slice, given up; false in the foreground */
static void dbg_waiting(VM *v);

static bool yield(VM *v)
{
    dbg_waiting(v);
    if (sysc(SYS_yield, 0, 0, 0) == 1) return true;
    vm_runtime_error(v, "this waits for the ship to move on, and typed at a terminal the ship "
                        "waits for the command: run the script in the background (ship run <script>)");
    return false;
}

/* wait(seconds): of game time */
static bool in_background(void);
static VmStatus n_wait(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    if (a[0].k != V_INT && a[0].k != V_NUM && a[0].k != V_BOOL) { vm_runtime_error(v, "TypeError: sleep() wants seconds, a number"); return VM_ERROR; }
    double until = reading_num(v, "time") + arg_num(a[0]);
    char wake[64];
    nom_snprintf(wake, sizeof wake, "wake %d", (int)until + 1);
    /* at least a frame, as Python's sleep(0) gives the others a turn: a
     * `while True: ... sleep(0)` goes at the game's pace, not flat out */
    bool once = in_background();
    while (once || reading_num(v, "time") < until) {
        once = false;
        if (reading_num(v, "time") < until) ask(v, wake);    /* so a warp eases off before it */
        if (!yield(v)) return VM_ERROR;
    }
    *out = VAL_NIL;
    return VM_OK;
}

static bool test(VM *v, const char *key, const char *op, Value want, bool *ok)
{
    const char *r = reading(v, key);
    if (!r) { vm_runtime_error(v, "no such reading: %s (ship status lists them)", key); return false; }
    Value got = reading_value(r);
    double x = 0, y = 0;
    int c;
    if (IS_STR(got) || IS_STR(want)) {
        Buf b; buf_init(&b);
        val_tostr(&b, want);
        c = nom_strcmp(r, b.p ? b.p : "");
        buf_free(&b);
    } else {
        x = val_num(got); y = val_num(want);
        c = x < y ? -1 : x > y ? 1 : 0;
    }
    val_release(got);
    if (!nom_strcmp(op, ">")) *ok = c > 0;
    else if (!nom_strcmp(op, ">=")) *ok = c >= 0;
    else if (!nom_strcmp(op, "<")) *ok = c < 0;
    else if (!nom_strcmp(op, "<=")) *ok = c <= 0;
    else if (!nom_strcmp(op, "==")) *ok = c == 0;
    else if (!nom_strcmp(op, "!=")) *ok = c != 0;
    else { vm_runtime_error(v, "'%s' is not a comparison: > >= < <= == !=", op); return false; }
    return true;
}

/* wait_until("altitude", "<", 5000) */
static VmStatus n_wait_until(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    if (!IS_STR(a[0]) || !IS_STR(a[1])) { vm_runtime_error(v, "wait_until(reading, comparison, value)"); return VM_ERROR; }
    for (;;) {
        bool ok;
        if (!test(v, AS_STR(a[0])->s, AS_STR(a[1])->s, a[2], &ok)) return VM_ERROR;
        if (ok) break;
        if (!yield(v)) return VM_ERROR;
    }
    *out = VAL_BOOL(1);
    return VM_OK;
}

/* A program on the game's side, started and waited for. */
static VmStatus program(VM *v, const char *line, Value *out)
{
    double id0 = reading_num(v, "program_id");
    const char *r = ask(v, line);
    if (is_err(r)) { vm_runtime_error(v, "%s", r + 5); return VM_ERROR; }
    /* the game takes it up at the end of this frame */
    for (;;) {
        if (!yield(v)) return VM_ERROR;
        if (reading_num(v, "program_id") != id0) break;
    }
    for (;;) {
        const char *st = reading(v, "program_status");
        if (!st || nom_strcmp(st, "running")) break;
        if (!yield(v)) return VM_ERROR;
    }
    char name[40], st[16];
    nom_snprintf(name, sizeof name, "%s", reading(v, "program") ? reading(v, "program") : "?");
    nom_snprintf(st, sizeof st, "%s", reading(v, "program_status") ? reading(v, "program_status") : "?");
    const char *note = reading(v, "program_note");
    if (!nom_strcmp(st, "failed")) {
        vm_runtime_error(v, "%s failed: %s", name, note ? note : "");
        return VM_ERROR;
    }
    g_puts(name); g_puts(": "); g_puts(note ? note : st); g_puts("\n");
    *out = VAL_BOOL(1);
    return VM_OK;
}

/* "verb a b c": the arguments as the shell would type them */
static void line_of(char *out, size_t cap, const char *verb, Value *a, int n)
{
    Buf b; buf_init(&b);
    buf_puts(&b, verb);
    for (int i = 0; i < n; i++) {
        buf_putc(&b, ' ');
        val_tostr(&b, a[i]);
    }
    nom_snprintf(out, cap, "%s", b.p ? b.p : verb);
    buf_free(&b);
}

#define PROGRAM(fn, verb) \
    static VmStatus fn(VM *v, Value *a, int n, Value *out) \
    { char line[256]; line_of(line, sizeof line, verb, a, n); return program(v, line, out); }
PROGRAM(n_launch, "launch")
PROGRAM(n_circularize, "circularize")
PROGRAM(n_transfer, "transfer")
PROGRAM(n_window, "window")
PROGRAM(n_warp_to, "warp")
PROGRAM(n_capture, "capture")
PROGRAM(n_land, "land")
PROGRAM(n_node, "node")
PROGRAM(n_hover, "hover")
PROGRAM(n_drive_to, "drive_to")

/* burn_until("prograde", "apoapsis", ">", 2000000) */
static VmStatus n_burn_until(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value parts[5] = { a[0], str_newz("until"), a[1], a[2], a[3] };
    char line[256];
    line_of(line, sizeof line, "burn", parts, 5);
    val_release(parts[1]);
    return program(v, line, out);
}

/* until_orbit_crosses("krill"): prograde until the path meets it */
static VmStatus n_until_crosses(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value parts[5] = { str_newz("prograde"), str_newz("until"), str_newz("encounter"), str_newz("=="), a[0] };
    char line[256];
    line_of(line, sizeof line, "burn", parts, 5);
    for (int i = 0; i < 4; i++) val_release(parts[i]);
    return program(v, line, out);
}

/* The quick ones: said, and (in the background) a frame waited, so the
 * next line sees what this one did. */
static VmStatus quick(VM *v, const char *verb, Value *a, int n, Value *out)
{
    char line[256];
    line_of(line, sizeof line, verb, a, n);
    const char *r = ask(v, line);
    if (is_err(r)) { vm_runtime_error(v, "%s", r + 5); return VM_ERROR; }
    if (sysc(SYS_yield, 0, 0, 0) == 1) {
        double f0 = reading_num(v, "frame");
        while (reading_num(v, "frame") == f0)
            if (!yield(v)) return VM_ERROR;
    }
    *out = VAL_NIL;
    return VM_OK;
}

#define QUICK(fn, verb) \
    static VmStatus fn(VM *v, Value *a, int n, Value *out) { return quick(v, verb, a, n, out); }
QUICK(n_stage, "stage")
QUICK(n_sas, "sas")
QUICK(n_point, "point")
QUICK(n_chutes, "chutes")
/* legs(): out; legs(False): in (a script says which; a toggle would put
 * them away if something else had put them out) */
static VmStatus n_legs(VM *v, Value *a, int n, Value *out)
{
    Value w = str_newz(n == 0 || val_truthy(a[0]) ? "on" : "off");
    VmStatus st = quick(v, "legs", &w, 1, out);
    val_release(w);
    return st;
}
/* mining() / mining(False): the drills and refineries on or off (C) */
static VmStatus n_mining(VM *v, Value *a, int n, Value *out)
{
    Value w = str_newz(n == 0 || val_truthy(a[0]) ? "on" : "off");
    VmStatus st = quick(v, "mining", &w, 1, out);
    val_release(w);
    return st;
}
QUICK(n_rcs, "rcs")
QUICK(n_say, "say")
QUICK(n_beep, "beep")
QUICK(n_abort, "abort")
QUICK(n_autostage, "autostage")
QUICK(n_target, "target")


/* ======================================================= TUX ROCKET SCIENCE
 * THE DOT. The ship and every part aboard is a thing with readings and
 * things it does, by a path in the game's table (os/core/computer.c):
 *
 *     engine = ship.part("Swivel")
 *     engine.throttle = 100          # set: the game does it this frame
 *     if engine.fuel < 50:           # read: what the game said this frame
 *         booster_drop.decouple()    # do: done, then the next frame
 *     print(ship.orbit.top)
 *
 * A dict's keys read the same way (orbit.top or orbit["top"]), and a
 * string's or a list's functions are its methods ("a,b".split(","),
 * names.append(x)). */

/* the answer to `obj get`, as a value: "@part.12:Swivel" a thing,
 * "@[a:x|b:y]" a list of them, true/false, a number, or a word */
static Value decode(const char *r)
{
    if (r[0] == '@' && r[1] == '[') {
        Value l = list_new();
        const char *q = r + 2;
        while (*q && *q != ']') {
            const char *e = q;
            while (*e && *e != '|' && *e != ']') e++;
            const char *c = q;
            while (c < e && *c != ':') c++;
            list_push(AS_LIST(l), handle_new(q, (size_t)(c - q), c < e ? c + 1 : c, c < e ? (size_t)(e - c - 1) : 0));
            q = *e == '|' ? e + 1 : e;
        }
        return l;
    }
    if (r[0] == '@') {
        const char *c = r + 1;
        while (*c && *c != ':') c++;
        return handle_new(r + 1, (size_t)(c - r - 1), *c ? c + 1 : c, *c ? nom_strlen(c + 1) : 0);
    }
    if (!nom_strcmp(r, "true")) return VAL_BOOL(1);
    if (!nom_strcmp(r, "false")) return VAL_BOOL(0);
    if (!nom_strcmp(r, "nil") || !nom_strcmp(r, "none")) return VAL_NIL;
    return reading_value(r);
}

static void child(char *out, size_t cap, Handle *h, const char *name)
{
    nom_snprintf(out, cap, "%s%s%s", h->path, h->path[0] ? "." : "", name);
}

/* run with `ship run` (a slice a frame, between the game's frames) or typed
 * at a terminal (to the end, the ship waiting): asked once, at the start */
static int g_bg = -1;
static bool in_background(void)
{
    if (g_bg < 0) g_bg = sysc(SYS_yield, 0, 0, 0) == 1;
    return g_bg == 1;
}

/* the game has moved on a frame (in the background; at a terminal, not) */
static bool frame_passes(VM *v)
{
    if (!in_background()) return true;
    double f0 = reading_num(v, "frame");
    while (reading_num(v, "frame") == f0)
        if (!yield(v)) return false;
    return true;
}

/* a part that went with a stage (or broke off) */
static bool gone(VM *v, Value obj, const char *r)
{
    if (nom_strcmp(r, "-ERR not aboard")) return false;
    if (!nom_strncmp(AS_HANDLE(obj)->path, "thing.", 6))
        vm_runtime_error(v, "%s is gone (down, or out of the ship's sight): check .alive first",
                         AS_HANDLE(obj)->label[0] ? AS_HANDLE(obj)->label : "that");
    else
        vm_runtime_error(v, "%s isn't aboard any more (it went with a stage, or broke off)",
                         AS_HANDLE(obj)->label[0] ? AS_HANDLE(obj)->label : "that part");
    return true;
}

/* the methods each kind of value has (the ones a dot finds) */
static const char *LIST_M[] = { "append", "insert", "remove", "pop", "index", "count", "extend", "sort", "reverse", "copy", "clear", NULL };
static const char *TUPLE_M[] = { "count", "index", NULL };
static const char *SET_M[] = { "add", "discard", "remove", "pop", "clear", "copy", "update", "union", "intersection", "difference",
                               "symmetric_difference", "issubset", "issuperset", "isdisjoint", "intersection_update", "difference_update", NULL };
static const char *DICT_M[] = { "items", "values", "keys", "get", "pop", "popitem", "update", "setdefault", "copy", "clear", NULL };
static const char *STR_M[] = { "split", "rsplit", "splitlines", "join", "strip", "lstrip", "rstrip", "lower", "upper", "replace", "find", "rfind",
                               "format", "count", "index", "rindex", "startswith", "endswith", "isdigit", "isnumeric", "isdecimal", "isalpha",
                               "isalnum", "isspace", "isupper", "islower", "rjust", "ljust", "center", "zfill", "title", "capitalize",
                               "swapcase", "casefold", "partition", "rpartition", "removeprefix", "removesuffix", NULL };
static const char *NUM_M[] = { "is_integer", "bit_length", NULL };

static const char **methods_of(Value x)
{
    if (IS_LIST(x)) return AS_LIST(x)->tuple == 1 ? TUPLE_M : AS_LIST(x)->tuple == 2 ? SET_M : LIST_M;
    if (IS_DICT(x)) return DICT_M;
    if (IS_STR(x)) return STR_M;
    if (x.k == V_INT || x.k == V_NUM || x.k == V_BOOL) return NUM_M;
    return NULL;
}

static bool in_names(const char **ms, const char *n)
{
    for (int i = 0; ms && ms[i]; i++) if (!nom_strcmp(ms[i], n)) return true;
    return false;
}

/* how many edits make one name the other (for "did you mean") */
static int edit_dist(const char *a, int al, const char *b, int bl)
{
    if (al > 30 || bl > 30) return 99;
    int d[31][31];
    for (int x = 0; x <= al; x++) d[x][0] = x;
    for (int y = 0; y <= bl; y++) d[0][y] = y;
    for (int x = 1; x <= al; x++)
        for (int y = 1; y <= bl; y++) {
            int c = (a[x - 1] | 32) != (b[y - 1] | 32);
            int m = d[x - 1][y] + 1;
            if (d[x][y - 1] + 1 < m) m = d[x][y - 1] + 1;
            if (d[x - 1][y - 1] + c < m) m = d[x - 1][y - 1] + c;
            d[x][y] = m;
        }
    return d[al][bl];
}

/* the nearest of some names to a mistyped one, or NULL */
static const char *g_near;
static int g_near_d;
static void near_try(const char *cand, const char *n)
{
    if (cand[0] == '_' && n[0] != '_') return;
    int e = edit_dist(n, (int)nom_strlen(n), cand, (int)nom_strlen(cand));
    int most = nom_strlen(n) <= 3 ? 1 : nom_strlen(n) <= 6 ? 2 : 3;
    if (e > 0 && e <= most && e < g_near_d) { g_near_d = e; g_near = cand; }
}
static const char *nearest_of(const char **ms, const char *n)
{
    g_near = NULL;
    g_near_d = 99;
    for (int i = 0; ms && ms[i]; i++) near_try(ms[i], n);
    return g_near;
}

/* a name on an instance's fields or its classes, near a mistyped one */
static const char *nearest_attr(Value obj, const char *n)
{
    g_near = NULL;
    g_near_d = 99;
    if (IS_INSTANCE(obj)) {
        Dict *fd = AS_DICT(((Instance *)obj.as.o)->fields);
        for (u32 i = 0; i < fd->len; i++) if (IS_STR(fd->k[i])) near_try(AS_STR(fd->k[i])->s, n);
    }
    for (Value c = IS_INSTANCE(obj) ? ((Instance *)obj.as.o)->cls : obj; IS_CLASS(c); c = ((Klass *)c.as.o)->base) {
        Dict *cd = AS_DICT(((Klass *)c.as.o)->dict);
        for (u32 i = 0; i < cd->len; i++) if (IS_STR(cd->k[i])) near_try(AS_STR(cd->k[i])->s, n);
    }
    return g_near;
}

static VmStatus n_type(VM *v, Value *a, int n, Value *out);

bool host_getattr(VM *v, Value obj, Str *name, Value *out)
{
    const char *nm = name->s;
    if (!nom_strcmp(nm, "__name__") && obj.k == V_OBJ) {
        if (IS_CLASS(obj)) { *out = str_newz(((Klass *)obj.as.o)->name); return true; }
        if (obj.as.o->kind == O_NATIVE) { int c; *out = str_newz(native_table(&c)[func_obj_index(obj)].name); return true; }
        if (obj.as.o->kind == O_FUNC || obj.as.o->kind == O_CLOSURE) { *out = str_newz(v->prog->func[func_obj_index(obj)].name); return true; }
    }
    if (!nom_strcmp(nm, "__class__")) return n_type(v, &obj, 1, out) == VM_OK;
    if (IS_INSTANCE(obj) && !nom_strcmp(nm, "__dict__")) { *out = val_retain(((Instance *)obj.as.o)->fields); return true; }
    if (IS_INSTANCE(obj) || IS_CLASS(obj)) {
        /* self.kp: the instance's own, then its class's (and the bases'):
         * a def there comes with self (a bound method), a @property is
         * worked out */
        Value got, key = str_new(name->s, name->len);
        bool own = IS_INSTANCE(obj) && dict_get(AS_DICT(((Instance *)obj.as.o)->fields), key, &got);
        val_release(key);
        if (own) { *out = val_retain(got); return true; }
        Value cls = IS_INSTANCE(obj) ? ((Instance *)obj.as.o)->cls : obj;
        if (class_find(cls, name->s, name->len, &got)) {
            if (IS_BOUND(got) && ((Bound *)got.as.o)->self.k == V_NIL && IS_STR(((Bound *)got.as.o)->name)) {
                const char *mk = AS_STR(((Bound *)got.as.o)->name)->s;
                Value fn = ((Bound *)got.as.o)->fn;
                if (!nom_strcmp(mk, "$property")) {
                    if (IS_INSTANCE(obj)) return vm_call(v, fn, &obj, 1, out);
                    *out = val_retain(got);
                    return true;
                }
                if (!nom_strcmp(mk, "$static")) { *out = val_retain(fn); return true; }
                if (!nom_strcmp(mk, "$class")) { *out = bound_new(O_BOUND, cls, fn, VAL_OBJ(name)); return true; }
            }
            if (IS_INSTANCE(obj) && is_fn(got)) { *out = bound_new(O_BOUND, obj, got, VAL_OBJ(name)); return true; }
            *out = val_retain(got);
            return true;
        }
        const char *cn = ((Klass *)cls.as.o)->name;
        const char *near = nearest_attr(obj, nm);
        if (near) vm_runtime_error(v, "AttributeError: a %s has no .%s -- did you mean .%s?", cn, nm, near);
        else if (IS_INSTANCE(obj)) vm_runtime_error(v, "AttributeError: a %s has no .%s (set it first: self.%s = ...)", cn, nm, nm);
        else vm_runtime_error(v, "AttributeError: class %s has no .%s", cn, nm);
        return false;
    }
    if (obj.k == V_OBJ && obj.as.o->kind == O_SUPER) {
        Bound *sp = (Bound *)obj.as.o;
        Value got;
        if (class_find(sp->fn, name->s, name->len, &got)) {
            *out = is_fn(got) ? bound_new(O_BOUND, sp->self, got, VAL_OBJ(name)) : val_retain(got);
            return true;
        }
        vm_runtime_error(v, "AttributeError: super() has no .%s", nm);
        return false;
    }
    if (IS_DICT(obj)) {
        Value key = str_new(name->s, name->len), got;
        bool ok = dict_get(AS_DICT(obj), key, &got);
        val_release(key);
        if (ok) { *out = val_retain(got); return true; }
        if (in_names(DICT_M, nm)) { *out = bound_new(O_BOUND, obj, VAL_NIL, VAL_OBJ(name)); return true; }
        /* what it does have, to choose from */
        Buf b; buf_init(&b);
        Dict *d = AS_DICT(obj);
        g_near = NULL;
        g_near_d = 99;
        for (u32 i = 0; i < d->len; i++) {
            if (!IS_STR(d->k[i])) continue;
            near_try(AS_STR(d->k[i])->s, nm);
            if (i < 8) { if (b.len) buf_puts(&b, ", "); buf_puts(&b, AS_STR(d->k[i])->s); }
        }
        if (d->len > 8) buf_puts(&b, "...");
        buf_putc(&b, 0);
        if (g_near) vm_runtime_error(v, "KeyError: no '%s' in it -- did you mean '%s'?", nm, g_near);
        else vm_runtime_error(v, "KeyError: no '%s' in it (it has: %s)", nm, b.len > 1 ? b.p : "nothing");
        buf_free(&b);
        return false;
    }
    if (IS_HANDLE(obj)) {
        char path[128], line[200];
        child(path, sizeof path, AS_HANDLE(obj), name->s);
        nom_snprintf(line, sizeof line, "obj get %s", path);
        const char *r = ask(v, line);
        if (gone(v, obj, r)) return false;
        if (is_err(r)) { vm_runtime_error(v, "%s", r + 5); return false; }
        *out = decode(r);
        return true;
    }
    const char **ms = methods_of(obj);
    if (in_names(ms, nm)) { *out = bound_new(O_BOUND, obj, VAL_NIL, VAL_OBJ(name)); return true; }
    const char *near = nearest_of(ms, nm);
    if (obj.k == V_NIL) vm_runtime_error(v, "AttributeError: None has no .%s (the value isn't set -- a function that returned nothing?)", nm);
    else if (near) vm_runtime_error(v, "AttributeError: a %s has no .%s -- did you mean .%s?", kind_of(obj), nm, near);
    else vm_runtime_error(v, "AttributeError: a %s has no .%s", kind_of(obj), nm);
    return false;
}

bool host_setattr(VM *v, Value obj, Str *name, Value val)
{
    if (IS_INSTANCE(obj)) {
        dict_set(AS_DICT(((Instance *)obj.as.o)->fields), str_new(name->s, name->len), val_retain(val));
        return true;
    }
    if (IS_CLASS(obj)) {
        dict_set(AS_DICT(((Klass *)obj.as.o)->dict), str_new(name->s, name->len), val_retain(val));
        return true;
    }
    if (IS_DICT(obj)) {
        dict_set(AS_DICT(obj), str_new(name->s, name->len), val_retain(val));
        return true;
    }
    if (!IS_HANDLE(obj)) { vm_runtime_error(v, "can't set .%s on that", name->s); return false; }
    if (val.k != V_INT && val.k != V_NUM && val.k != V_BOOL) {
        vm_runtime_error(v, ".%s wants a number", name->s);
        return false;
    }
    char path[128], line[220];
    child(path, sizeof path, AS_HANDLE(obj), name->s);
    Buf b; buf_init(&b);
    if (val.k == V_BOOL) buf_puts(&b, val.as.i ? "1" : "0");
    else val_tostr(&b, val);
    buf_putc(&b, 0);
    nom_snprintf(line, sizeof line, "obj set %s %s", path, b.p);
    buf_free(&b);
    const char *r = ask(v, line);
    if (gone(v, obj, r)) return false;
    if (is_err(r)) { vm_runtime_error(v, "%s", r + 5); return false; }
    return true;
}

static VmStatus n_join(VM *v, Value *a, int n, Value *out);
static VmStatus n_ship(VM *v, Value *a, int n, Value *out);

/* ship.part("Swivel"): the one part called that */
static VmStatus find_parts(VM *v, Value name, bool one, Value *out)
{
    if (!IS_STR(name)) { vm_runtime_error(v, "part() wants a name: ship.part(\"Swivel\")"); return VM_ERROR; }
    char line[200];
    nom_snprintf(line, sizeof line, "obj find - %s", AS_STR(name)->s);
    Value l = decode(ask(v, line));
    if (!one) { *out = l; return VM_OK; }
    List *ls = AS_LIST(l);
    if (ls->len == 1) { *out = val_retain(ls->v[0]); val_release(l); return VM_OK; }
    if (ls->len == 0)
        vm_runtime_error(v, "no part called '%s' aboard (print(ship.parts) lists them)", AS_STR(name)->s);
    else {
        Buf b; buf_init(&b);
        for (u32 i = 0; i < ls->len; i++) { if (i) buf_puts(&b, ", "); val_tostr(&b, ls->v[i]); }
        buf_putc(&b, 0);
        vm_runtime_error(v, "%d parts are called '%s': %s (name one, or ship.parts(\"%s\") for them all)",
                         (int)ls->len, AS_STR(name)->s, b.p, AS_STR(name)->s);
        buf_free(&b);
    }
    val_release(l);
    return VM_ERROR;
}

static VmStatus value_method(VM *v, Value obj, Str *name, Value *a, int n, Value *out, bool *found);

VmStatus host_callmethod(VM *v, Value obj, Str *name, Value *args, int n, Value *out)
{
    if (IS_HANDLE(obj)) {
        Handle *h = AS_HANDLE(obj);
        if (!name) {
            if (h->path[0]) { vm_runtime_error(v, "%s is a part, not a call", h->label); return VM_ERROR; }
            return n_ship(v, args, n, out);          /* ship(): the old json */
        }
        if (!h->path[0] && (!nom_strcmp(name->s, "part") || !nom_strcmp(name->s, "parts")) && n == 1)
            return find_parts(v, args[0], name->s[4] == 0, out);
        if (n == 0 && !h->path[0] && !nom_strcmp(name->s, "parts"))
            return host_getattr(v, obj, name, out) ? VM_OK : VM_ERROR;
        char path[128];
        child(path, sizeof path, h, name->s);
        Buf b; buf_init(&b);
        buf_puts(&b, "obj call ");
        buf_puts(&b, path);
        for (int i = 0; i < n; i++) {
            buf_putc(&b, ' ');
            /* a thing passed to a call goes as itself (ship.fire_at(beast)), not its name */
            if (IS_HANDLE(args[i])) { buf_putc(&b, '@'); buf_puts(&b, AS_HANDLE(args[i])->path); }
            else val_tostr(&b, args[i]);
        }
        buf_putc(&b, 0);
        const char *r = ask(v, b.p);
        buf_free(&b);
        if (gone(v, obj, r)) return VM_ERROR;
        if (is_err(r)) { vm_runtime_error(v, "%s", r + 5); return VM_ERROR; }
        if (!frame_passes(v)) return VM_ERROR;
        *out = VAL_NIL;
        return VM_OK;
    }
    if (!name) { vm_runtime_error(v, "that can't be called"); return VM_ERROR; }
    {
        bool found;
        VmStatus st = value_method(v, obj, name, args, n, out, &found);
        if (found) return st;
    }
    /* Python's own spellings that put the thing in a different place */
    if (IS_STR(obj) && !nom_strcmp(name->s, "join") && n == 1) {
        Value as[2] = { args[0], obj };
        return n_join(v, as, 2, out);
    }
    if (IS_DICT(obj) && !nom_strcmp(name->s, "get") && (n == 1 || n == 2)) {
        Value got;
        *out = dict_get(AS_DICT(obj), args[0], &got) ? val_retain(got) : (n == 2 ? val_retain(args[1]) : VAL_NIL);
        return VM_OK;
    }
    if (IS_LIST(obj) && !nom_strcmp(name->s, "pop") && n == 0) {
        List *l = AS_LIST(obj);
        if (!l->len) { vm_runtime_error(v, "pop from an empty list"); return VM_ERROR; }
        *out = l->v[--l->len];
        return VM_OK;
    }
    /* the rest: x.f(a) is f(x, a), for the methods a value has */
    const char **ms = methods_of(obj);
    int k = native_find(name->s, (int)name->len);
    int count = 0;
    const Native *tbl = native_table(&count);
    if (!in_names(ms, name->s) || k < 0 || n + 1 > NOM_ARGS_MAX || n + 1 < tbl[k].minargs || n + 1 > tbl[k].maxargs) {
        const char *near = in_names(ms, name->s) ? NULL : nearest_of(ms, name->s);
        if (in_names(ms, name->s))
            vm_runtime_error(v, "TypeError: .%s() was given %d argument%s (that's not what it takes)", name->s, n, n == 1 ? "" : "s");
        else if (obj.k == V_NIL)
            vm_runtime_error(v, "AttributeError: None has no .%s() (the value isn't set -- a function that returned nothing?)", name->s);
        else if (near) vm_runtime_error(v, "AttributeError: a %s has no .%s() -- did you mean .%s()?", kind_of(obj), name->s, near);
        else vm_runtime_error(v, "AttributeError: a %s has no .%s()", kind_of(obj), name->s);
        return VM_ERROR;
    }
    Value all[NOM_ARGS_MAX];
    all[0] = obj;
    for (int i = 0; i < n; i++) all[i + 1] = args[i];
    return tbl[k].fn(v, all, n + 1, out);
}

/* format(x, ".1f") -- what f"{x:.1f}" does: [,][.places][f d % s], and a
 * width with < > ^ before it */
/* x with so many decimals, as the host's printf gets it (correctly
 * rounded: 2.675 is 2.67499999... and so "2.67", as Python says), or
 * false */
static bool host_fixed(double x, int places, char *out)
{
    i64 bits;
    nom_memcpy(&bits, &x, 8);
    return sysc(SYS_num, NUM_FIXED | ((i64)places << 8), bits, (i64)out) > 0;
}

static void put_fixed(Buf *b, double x, int places, bool commas)
{
    if (x != x) { buf_puts(b, "nan"); return; }
    if (x - x != 0) { buf_puts(b, x < 0 ? "-inf" : "inf"); return; }
    if (x < 0 || (x == 0 && 1.0 / x < 0)) { buf_putc(b, '-'); x = -x; }
    char hf[64];
    if (places <= 40 && host_fixed(x, places, hf)) {
        int w = 0;
        while (hf[w] && hf[w] != '.') w++;
        for (int i = 0; i < w; i++) {
            buf_putc(b, hf[i]);
            if (commas && w - 1 - i > 0 && (w - 1 - i) % 3 == 0) buf_putc(b, ',');
        }
        buf_puts(b, hf + w);
        return;
    }
    if (x > 9e15) { buf_putnum(b, x, places); return; }
    double p = 1;
    for (int i = 0; i < places; i++) p *= 10;
    /* half to even, as Python does: 62.5 is "62" */
    double xp = x * p, fl = nom_floor(xp);
    u64 all = (u64)fl + (xp - fl > 0.5 || (xp - fl == 0.5 && ((u64)fl & 1)) ? 1 : 0);
    u64 whole = (u64)((double)all / p), frac = all - (u64)((double)whole * p);
    char dig[32];
    int nd = 0;
    do { dig[nd++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    for (int i = nd - 1; i >= 0; i--) {
        buf_putc(b, dig[i]);
        if (commas && i && i % 3 == 0) buf_putc(b, ',');
    }
    if (places > 0) {
        buf_putc(b, '.');
        char fd[24];
        for (int i = places - 1; i >= 0; i--) { fd[i] = (char)('0' + frac % 10); frac /= 10; }
        buf_put(b, fd, (size_t)places);
    }
}

/* x as "e" format with places decimals: 1.50e+03 */
static void put_exp(Buf *b, double x, int places, char ech)
{
    if (x < 0) { buf_putc(b, '-'); x = -x; }
    int ex = 0;
    if (x != 0) {
        while (x >= 10.0) { x /= 10.0; ex++; }
        while (x < 1.0) { x *= 10.0; ex--; }
    }
    double p = 1;
    for (int i = 0; i < places; i++) p *= 10;
    if (half_even(x * p) / p >= 10.0) { x /= 10.0; ex++; }
    put_fixed(b, x, places, false);
    buf_putc(b, ech);
    buf_putc(b, ex < 0 ? '-' : '+');
    if (ex < 0) ex = -ex;
    if (ex < 10) buf_putc(b, '0');
    buf_printf(b, "%d", ex);
}

/* format(x, spec) -- what f"{x:spec}" does, Python's spec:
 * [[fill]align][sign][0][width][,][.precision][type], type d f e g % x s */
static VmStatus n_chr(VM *v, Value *a, int n, Value *out);
static VmStatus n_format(VM *v, Value *a, int n, Value *out)
{
    const char *s = (n > 1 && IS_STR(a[1])) ? AS_STR(a[1])->s : "";
    char align = 0, type = 0, fill = ' ', sign = '-';
    int width = 0, places = -1;
    bool commas = false, zero = false, alt = false;
    if (s[0] && (s[1] == '<' || s[1] == '>' || s[1] == '^' || s[1] == '=')) { fill = s[0]; align = s[1]; s += 2; }
    else if (*s == '<' || *s == '>' || *s == '^' || *s == '=') align = *s++;
    if (*s == '+' || *s == '-' || *s == ' ') sign = *s++;
    if (*s == '#') { alt = true; s++; }
    if (*s == '0') { zero = true; s++; }
    while (*s >= '0' && *s <= '9') width = width * 10 + (*s++ - '0');
    if (*s == ',' || *s == '_') { commas = true; s++; }
    if (*s == '.') { s++; places = 0; while (*s >= '0' && *s <= '9') places = places * 10 + (*s++ - '0'); }
    if (*s) type = *s++;
    if (*s) { vm_runtime_error(v, "ValueError: '%s' isn't a format (try .1f, 8.2f, >10, 05d, ,)", AS_STR(a[1])->s); return VM_ERROR; }
    Buf b; buf_init(&b);
    bool num = a[0].k == V_INT || a[0].k == V_NUM || a[0].k == V_BOOL;
    if (num && type != 's') {
        double x = val_num(a[0]);
        bool neg = x < 0 || (x == 0 && a[0].k == V_NUM && 1.0 / x < 0);
        double ax = neg ? -x : x;
        Buf d; buf_init(&d);
        if (type == 'x' || type == 'X' || type == 'b' || type == 'o') {
            /* {255:x} ff, {5:08b} 00000101, {8:o} 10, {255:#x} 0xff */
            if (a[0].k == V_NUM) { vm_runtime_error(v, "ValueError: format '%c' is for whole numbers, not %s", type, "a float"); buf_free(&d); buf_free(&b); return VM_ERROR; }
            int base = type == 'b' ? 2 : type == 'o' ? 8 : 16;
            i64 iv = val_int(a[0]);
            u64 u = iv < 0 ? (u64)(-(iv + 1)) + 1 : (u64)iv;
            char t[72]; int k = 0;
            do { int q = (int)(u % (u64)base); t[k++] = (char)(q < 10 ? '0' + q : (type == 'X' ? 'A' : 'a') + q - 10); u /= (u64)base; } while (u);
            if (alt) { buf_putc(&d, '0'); buf_putc(&d, type == 'X' ? 'X' : type); }
            while (k) buf_putc(&d, t[--k]);
        } else if (type == 'c') {
            i64 c = val_int(a[0]);
            Value cv = VAL_INT(c), cs;
            if (n_chr(v, &cv, 1, &cs) != VM_OK) { buf_free(&d); buf_free(&b); return VM_ERROR; }
            buf_put(&d, AS_STR(cs)->s, AS_STR(cs)->len);
            val_release(cs);
        } else if (type == 'e' || type == 'E') {
            put_exp(&d, ax, places >= 0 ? places : 6, type);
        } else if (type == 'g' || type == 'G' || (!type && places >= 0 && a[0].k == V_NUM)) {
            /* the number in so many significant figures, zeros at the end dropped */
            int pr = places > 0 ? places : places == 0 ? 1 : 6;
            int ex = 0;
            double m = ax;
            if (m != 0) { while (m >= 10.0) { m /= 10.0; ex++; } while (m < 1.0) { m *= 10.0; ex--; } }
            if (ex >= -4 && ex < pr) put_fixed(&d, ax, pr - 1 - ex, false);
            else put_exp(&d, ax, pr - 1, type == 'G' ? 'E' : 'e');
            if (d.len) {
                /* trailing zeros (and a lone point) off, as %g does */
                char *ep = NULL;
                for (size_t i = 0; i < d.len; i++) if (d.p[i] == 'e' || d.p[i] == 'E') { ep = d.p + i; break; }
                size_t mant = ep ? (size_t)(ep - d.p) : d.len;
                bool dot = false;
                for (size_t i = 0; i < mant; i++) dot |= d.p[i] == '.';
                if (dot) {
                    size_t k = mant;
                    while (k > 0 && d.p[k - 1] == '0') k--;
                    if (k > 0 && d.p[k - 1] == '.') k--;
                    Buf t; buf_init(&t);
                    buf_put(&t, d.p, k);
                    if (ep) buf_put(&t, ep, d.len - mant);
                    buf_free(&d);
                    d = t;
                }
            }
        } else if (type == 'f' || type == 'F' || type == '%' || type == 'd' || type == 'n' || places >= 0 || commas) {
            double y = type == '%' ? ax * 100 : ax;
            int pl = (type == 'd' || type == 'n') ? 0 : places >= 0 ? places : (type == 'f' || type == 'F' || type == '%') ? 6
                   : (a[0].k == V_NUM ? 1 : 0);
            put_fixed(&d, y, pl, commas);
            if (type == '%') buf_putc(&d, '%');
        } else {
            if (a[0].k == V_BOOL && !type && width == 0) { buf_free(&d); val_tostr(&b, a[0]); goto padded; }
            Value pos = a[0].k == V_NUM ? VAL_NUM(ax) : a[0].k == V_BOOL ? VAL_INT(a[0].as.i) : VAL_INT(neg ? -a[0].as.i : a[0].as.i);
            val_tostr(&d, pos);
        }
        const char *sg = neg ? "-" : sign == '+' ? "+" : sign == ' ' ? " " : "";
        /* the sign, then zeros up to the width (05d, +08.2f) */
        size_t sl = nom_strlen(sg);
        if (zero && !align) {
            buf_puts(&b, sg);
            for (int i = (int)(d.len + sl); i < width; i++) buf_putc(&b, '0');
            buf_put(&b, d.p ? d.p : "", d.len);
        } else {
            buf_puts(&b, sg);
            buf_put(&b, d.p ? d.p : "", d.len);
        }
        buf_free(&d);
    } else {
        val_tostr(&b, a[0]);
        if (places >= 0 && (size_t)places < b.len) b.len = (size_t)places;   /* {:.3} of a string: its first 3 */
    }
padded:;
    int pad = width - (int)b.len;
    Buf o; buf_init(&o);
    if (!align) align = num && type != 's' ? '>' : '<';
    int left = pad <= 0 ? 0 : align == '>' || align == '=' ? pad : align == '^' ? pad / 2 : 0;
    int right = pad <= 0 ? 0 : pad - left;
    for (int i = 0; i < left; i++) buf_putc(&o, fill);
    if (b.len) buf_put(&o, b.p, b.len);
    for (int i = 0; i < right; i++) buf_putc(&o, fill);
    *out = str_new(o.p ? o.p : "", o.len);
    buf_free(&b);
    buf_free(&o);
    return VM_OK;
}

static VmStatus n_clamp(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    double x = val_num(a[0]), lo = val_num(a[1]), hi = val_num(a[2]);
    *out = val_retain(x < lo ? a[1] : x > hi ? a[2] : a[0]);
    return VM_OK;
}

static VmStatus n_affix(VM *v, Value *a, int n, Value *out, bool end)
{
    (void)v; (void)n;
    if (!IS_STR(a[0]) || !IS_STR(a[1])) { *out = VAL_BOOL(0); return VM_OK; }
    Str *s = AS_STR(a[0]), *f = AS_STR(a[1]);
    *out = VAL_BOOL(f->len <= s->len && !nom_memcmp(s->s + (end ? s->len - f->len : 0), f->s, f->len));
    return VM_OK;
}
static VmStatus n_startswith(VM *v, Value *a, int n, Value *out) { return n_affix(v, a, n, out, false); }
static VmStatus n_endswith(VM *v, Value *a, int n, Value *out) { return n_affix(v, a, n, out, true); }

/* ---- aiming, burning, and the maths of an orbit ---- */

static double obj_num(VM *v, const char *path, bool *ok)
{
    char line[160];
    nom_snprintf(line, sizeof line, "obj get %s", path);
    const char *r = ask(v, line);
    if (is_err(r)) { *ok = false; return 0; }
    return nom_strtod(r, 0);
}

/* until the nose is where it was told (2 degrees) and has stopped turning,
 * or a minute of game time: true if it got there */
static VmStatus wait_aimed(VM *v, Value *out)
{
    *out = VAL_BOOL(1);
    if (!in_background()) return VM_OK;
    if (!frame_passes(v)) return VM_ERROR;
    double t0 = reading_num(v, "time");
    for (;;) {
        if (reading_num(v, "aim_error") < 2.0 && reading_num(v, "turn_rate") < 1.0) return VM_OK;
        if (reading_num(v, "time") - t0 > 60.0) {
            char m[96];
            nom_snprintf(m, sizeof m, "aim: still %d degrees off after a minute\n", (int)reading_num(v, "aim_error"));
            g_puts(m);
            *out = VAL_BOOL(0);
            return VM_OK;
        }
        if (!yield(v)) return VM_ERROR;
    }
}

/* aim("prograde"): points along it and holds it there (SAS) */
static VmStatus n_aim(VM *v, Value *a, int n, Value *out)
{
    if (!IS_STR(a[0])) { vm_runtime_error(v, "aim() wants a direction: aim(\"prograde\")"); return VM_ERROR; }
    char line[96];
    nom_snprintf(line, sizeof line, "aim %s", AS_STR(a[0])->s);
    const char *r = ask(v, line);
    if (is_err(r)) { vm_runtime_error(v, "%s", r + 5); return VM_ERROR; }
    (void)n;
    return wait_aimed(v, out);
}

/* aim_at(pitch, heading): degrees above the horizon, degrees from north */
static VmStatus n_aim_at(VM *v, Value *a, int n, Value *out)
{
    char line[96];
    line_of(line, sizeof line, "point", a, n);
    const char *r = ask(v, line);
    if (is_err(r)) { vm_runtime_error(v, "%s", r + 5); return VM_ERROR; }
    return wait_aimed(v, out);
}

/* aim_along(forward, sideways, outward): along a burn what_if() found */
static VmStatus n_aim_along(VM *v, Value *a, int n, Value *out)
{
    char line[160];
    line_of(line, sizeof line, "aim_along", a, n);
    const char *r = ask(v, line);
    if (is_err(r)) { vm_runtime_error(v, "%s", r + 5); return VM_ERROR; }
    return wait_aimed(v, out);
}

/* throttle(100): every engine, in percent (an engine's own: engine.throttle) */
static VmStatus n_throttle_pct(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    double p = val_num(a[0]);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    Value f = VAL_NUM(p / 100.0);
    return quick(v, "throttle", &f, 1, out);
}

/* burn(120): this many m/s along the nose as it points; burn("prograde",
 * 120) points it there first */
static VmStatus n_burn_dv(VM *v, Value *a, int n, Value *out)
{
    char line[256];
    if (n == 1) {
        Value parts[2] = { str_newz("here"), a[0] };
        line_of(line, sizeof line, "burn", parts, 2);
        val_release(parts[0]);
    } else {
        line_of(line, sizeof line, "burn", a, n);
    }
    return program(v, line, out);
}

/* speed_needed(height): the speed of a circle that high (m/s) */
static VmStatus n_speed_needed(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    bool ok = true;
    double mu = obj_num(v, "body.mu", &ok), R = obj_num(v, "body.radius", &ok);
    if (!ok) { vm_runtime_error(v, "speed_needed: there is no body here"); return VM_ERROR; }
    *out = VAL_NUM(__builtin_sqrt(mu / (R + val_num(a[0]))));
    return VM_OK;
}

/* speed_at(height): how fast this orbit goes when it is that high */
static VmStatus n_speed_at(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    bool ok = true;
    double mu = obj_num(v, "body.mu", &ok), R = obj_num(v, "body.radius", &ok);
    double sma = obj_num(v, "orbit.semi_major_axis", &ok);
    if (!ok) { vm_runtime_error(v, "speed_at: no orbit to work it out from"); return VM_ERROR; }
    double r = R + val_num(a[0]), v2 = mu * (2.0 / r - 1.0 / sma);
    if (v2 < 0) { vm_runtime_error(v, "speed_at: this orbit never gets %d m up", (int)val_num(a[0])); return VM_ERROR; }
    *out = VAL_NUM(__builtin_sqrt(v2));
    return VM_OK;
}

/* burn_time(dv): seconds at full thrust for that much, on the engines
 * burning (or the next stage's), as the rocket equation has it */
static VmStatus n_burn_time(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    double m = reading_num(v, "mass"), ve = reading_num(v, "exhaust_speed"), f = reading_num(v, "stage_thrust_kn") * 1000.0;
    if (f <= 0 || ve <= 0) { vm_runtime_error(v, "burn_time: no engine to burn with (light one, or stage)"); return VM_ERROR; }
    *out = VAL_NUM(m * ve / f * (1.0 - nom_math(MATH_EXP, -val_num(a[0]) / ve, 0)));
    return VM_OK;
}

/* time_to("top"): seconds until it (top, bottom, apoapsis, periapsis), or
 * until a game time */
static VmStatus n_time_to(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    if (!IS_STR(a[0])) { *out = VAL_NUM(val_num(a[0]) - reading_num(v, "time")); return VM_OK; }
    const char *w = AS_STR(a[0])->s;
    const char *key = (!nom_strcmp(w, "top") || !nom_strcmp(w, "apoapsis")) ? "time_to_apoapsis"
                    : (!nom_strcmp(w, "bottom") || !nom_strcmp(w, "periapsis")) ? "time_to_periapsis" : 0;
    if (!key) { vm_runtime_error(v, "time_to(\"top\") or time_to(\"bottom\"), not '%s'", w); return VM_ERROR; }
    *out = VAL_NUM(reading_num(v, key));
    return VM_OK;
}

/* stopping_distance(): how far, flat out from the speed it is doing, it
 * takes to stop against this gravity (m); huge if it can't */
static VmStatus n_stopping(VM *v, Value *a, int n, Value *out)
{
    (void)a; (void)n;
    double vs = reading_num(v, "surface_speed"), m = reading_num(v, "mass");
    double f = reading_num(v, "stage_thrust_kn") * 1000.0, g = reading_num(v, "gravity");
    double acc = m > 0 ? f / m - g : 0;
    *out = VAL_NUM(acc > 0 ? vs * vs / (2 * acc) : 1e12);
    return VM_OK;
}

/* A question the game works out (it takes the path, and a frame): the
 * answer as a dict, or a runtime error with the game's reason. */
static VmStatus ask_game(VM *v, const char *what, Value *out)
{
    static int count;
    char id[24], line[256];
    nom_snprintf(id, sizeof id, "%d_%d", (int)sysc(SYS_getpid, 0, 0, 0), ++count);
    nom_snprintf(line, sizeof line, "ask %s %s", id, what);
    const char *r = ask(v, line);
    if (is_err(r)) { vm_runtime_error(v, "%s", r + 5); return VM_ERROR; }
    if (!in_background()) {
        vm_runtime_error(v, "the game works this out between frames: run the script in the background (ship run <script>)");
        return VM_ERROR;
    }
    char path[64];
    nom_snprintf(path, sizeof path, "answer.%s", id);
    for (;;) {
        if (!yield(v)) return VM_ERROR;
        nom_snprintf(line, sizeof line, "obj dict %s", path);
        const char *body = ask(v, line);
        /* not worked out yet: the game's answer ends with done */
        bool done = false;
        for (const char *z = body; *z && !done; z++)
            done = (z == body || z[-1] == '\n') && !nom_strncmp(z, "done\t", 5);
        if (is_err(body) || !done) continue;
        Value d = dict_new();
        char k[64], val[512];
        for (const char *q = body; *q; ) {
            size_t i = 0;
            while (*q && *q != '\t' && *q != '\n' && i < sizeof k - 1) k[i++] = *q++;
            k[i] = 0;
            size_t j = 0;
            if (*q == '\t') q++;
            while (*q && *q != '\n' && j < sizeof val - 1) val[j++] = *q++;
            val[j] = 0;
            if (*q == '\n') q++;
            if (nom_strcmp(k, "done")) dict_set(AS_DICT(d), str_newz(k), decode(val));
        }
        Value err, ek = str_newz("error");
        if (dict_get(AS_DICT(d), ek, &err) && IS_STR(err)) {
            vm_runtime_error(v, "%s", AS_STR(err)->s);
            val_release(ek);
            val_release(d);
            return VM_ERROR;
        }
        val_release(ek);
        *out = d;
        return VM_OK;
    }
}

/* when_to_leave("krill"): {wait, at, dv, travel, closest, body} -- worked out */
static VmStatus n_when_to_leave(VM *v, Value *a, int n, Value *out)
{
    char line[160];
    line_of(line, sizeof line, "leave", a, n);
    return ask_game(v, line, out);
}

/* what_if(forward, sideways, outward, when): {top, bottom, meets, closest,
 * arrive} -- the path as if that burn were made then; nothing is flown */
static VmStatus n_what_if(VM *v, Value *a, int n, Value *out)
{
    char line[200];
    line_of(line, sizeof line, "whatif", a, n);
    return ask_game(v, line, out);
}

/* chart("height", ship.height): a point on the Flight app's graph */
static VmStatus n_chart(VM *v, Value *a, int n, Value *out)
{
    char line[160];
    line_of(line, sizeof line, "chart", a, n);
    ask(v, line);
    *out = VAL_NIL;
    return VM_OK;
}

/* wait_user_trigger(): until the pilot presses space (the press is the
 * script's, not a stage) */
static VmStatus n_wait_space(VM *v, Value *a, int n, Value *out)
{
    (void)a; (void)n;
    if (!in_background()) {
        vm_runtime_error(v, "this waits for the pilot's space: run the script in the background (ship run <script>)");
        return VM_ERROR;
    }
    double k0 = reading_num(v, "space_presses");
    g_puts("waiting for you to press space...\n");
    ask(v, "say Press space when ready");
    for (;;) {
        ask(v, "await_space");
        if (!yield(v)) return VM_ERROR;
        if (reading_num(v, "space_presses") != k0) break;
    }
    *out = VAL_NIL;
    return VM_OK;
}

/* the ship itself, as `ship` (ship.orbit.top; ship() is still its json) */
static VmStatus n_the_ship(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)a; (void)n;
    *out = handle_new("", 0, "ship", 4);
    return VM_OK;
}


/* ============================================================ TUX ROCKET SCIENCE
 * THE REST OF PYTHON'S BUILTINS a player reaches for: range, sum, sorted,
 * enumerate, zip, map, isinstance... and the modules math, time, random. */

/* Python's order: numbers by value, strings by their letters, lists item
 * by item; anything else can't be ordered */
static int py_cmp(VM *v, Value a, Value b, bool *ok)
{
    bool an = a.k == V_INT || a.k == V_NUM || a.k == V_BOOL, bn = b.k == V_INT || b.k == V_NUM || b.k == V_BOOL;
    if (an && bn) { double x = val_num(a), y = val_num(b); return x < y ? -1 : x > y ? 1 : 0; }
    if (IS_STR(a) && IS_STR(b)) {
        Str *x = AS_STR(a), *y = AS_STR(b);
        u32 m = x->len < y->len ? x->len : y->len;
        int c = nom_memcmp(x->s, y->s, m);
        return c ? (c < 0 ? -1 : 1) : (x->len < y->len ? -1 : x->len > y->len ? 1 : 0);
    }
    if (IS_LIST(a) && IS_LIST(b)) {
        List *x = AS_LIST(a), *y = AS_LIST(b);
        for (u32 i = 0; i < x->len && i < y->len; i++) {
            int c = py_cmp(v, x->v[i], y->v[i], ok);
            if (c || !*ok) return c;
        }
        return x->len < y->len ? -1 : x->len > y->len ? 1 : 0;
    }
    vm_runtime_error(v, "can't put these in order (a number and a word?)");
    *ok = false;
    return 0;
}

/* any sequence as its items, in a new list (vm.c: a generator too) */
static bool items_of(VM *v, Value s, Value *out) { return vm_items(v, s, out); }

static VmStatus n_range(VM *v, Value *a, int n, Value *out)
{
    for (int i = 0; i < n; i++)
        if (a[i].k != V_INT && !(a[i].k == V_NUM && a[i].as.d == nom_floor(a[i].as.d))) {
            vm_runtime_error(v, "range() wants whole numbers");
            return VM_ERROR;
        }
    i64 st = n > 2 ? val_int(a[2]) : 1;
    if (!st) { vm_runtime_error(v, "range() step can't be 0"); return VM_ERROR; }
    *out = n == 1 ? range_new(0, val_int(a[0]), 1) : range_new(val_int(a[0]), val_int(a[1]), st);
    return VM_OK;
}

static VmStatus n_list(VM *v, Value *a, int n, Value *out)
{
    if (!n) { *out = list_new(); return VM_OK; }
    return items_of(v, a[0], out) ? VM_OK : VM_ERROR;
}

static VmStatus n_tuple(VM *v, Value *a, int n, Value *out)
{
    if (!n) { *out = list_new(); AS_LIST(*out)->tuple = 1; return VM_OK; }
    if (!items_of(v, a[0], out)) return VM_ERROR;
    AS_LIST(*out)->tuple = 1;
    return VM_OK;
}

static VmStatus n_dict(VM *v, Value *a, int n, Value *out)
{
    *out = dict_new();
    if (n && IS_DICT(a[0]))
        for (u32 i = 0; i < AS_DICT(a[0])->len; i++)
            dict_set(AS_DICT(*out), val_retain(AS_DICT(a[0])->k[i]), val_retain(AS_DICT(a[0])->v[i]));
    else if (n) {
        /* dict(zip(names, values)), dict([("a", 1)]): pairs */
        Value l;
        if (!items_of(v, a[0], &l)) { val_release(*out); return VM_ERROR; }
        for (u32 i = 0; i < AS_LIST(l)->len; i++) {
            Value pr = AS_LIST(l)->v[i];
            if (!IS_LIST(pr) || AS_LIST(pr)->len != 2) {
                vm_runtime_error(v, "TypeError: dict() of a list wants pairs: [(key, value), ...]");
                val_release(l);
                val_release(*out);
                return VM_ERROR;
            }
            dict_set(AS_DICT(*out), val_retain(AS_LIST(pr)->v[0]), val_retain(AS_LIST(pr)->v[1]));
        }
        val_release(l);
    }
    if (v->kw.k != V_NIL) {
        v->kw_used = true;
        for (u32 i = 0; i < AS_DICT(v->kw)->len; i++)
            dict_set(AS_DICT(*out), val_retain(AS_DICT(v->kw)->k[i]), val_retain(AS_DICT(v->kw)->v[i]));
    }
    return VM_OK;
}

static VmStatus n_sum(VM *v, Value *a, int n, Value *out)
{
    Value l, start = n > 1 ? a[1] : VAL_INT(0);
    kw_get(v, "start", &start);
    if (IS_STR(start)) { vm_runtime_error(v, "TypeError: sum() can't add up text: use ''.join(xs)"); return VM_ERROR; }
    if (!items_of(v, a[0], &l)) return VM_ERROR;
    if (IS_LIST(start)) {
        /* sum(lists, []): them end to end */
        Value r = list_new();
        for (u32 i = 0; i < AS_LIST(start)->len; i++) list_push(AS_LIST(r), val_retain(AS_LIST(start)->v[i]));
        for (u32 i = 0; i < AS_LIST(l)->len; i++) {
            Value x = AS_LIST(l)->v[i];
            if (!IS_LIST(x)) { vm_runtime_error(v, "TypeError: sum() with a list to start adds lists, and this is a %s", kind_of(x)); val_release(l); val_release(r); return VM_ERROR; }
            for (u32 k = 0; k < AS_LIST(x)->len; k++) list_push(AS_LIST(r), val_retain(AS_LIST(x)->v[k]));
        }
        AS_LIST(r)->tuple = AS_LIST(start)->tuple == 1;
        val_release(l);
        *out = r;
        return VM_OK;
    }
    Value acc = val_retain(start);
    for (u32 i = 0; i < AS_LIST(l)->len; i++) {
        Value x = AS_LIST(l)->v[i];
        if (x.k != V_INT && x.k != V_NUM && x.k != V_BOOL) { vm_runtime_error(v, "TypeError: sum() of a %s (it adds up numbers)", kind_of(x)); val_release(l); return VM_ERROR; }
        acc = (acc.k == V_NUM || x.k == V_NUM) ? VAL_NUM(val_num(acc) + val_num(x)) : VAL_INT(val_int(acc) + val_int(x));
    }
    val_release(l);
    *out = acc;
    return VM_OK;
}

/* sorts l in place, steadily (equal ones keep their order), by key= */
static bool sort_list(VM *v, List *l, Value key, bool rev)
{
    Value *k = nom_alloc(sizeof(Value) * (l->len + 1));
    for (u32 i = 0; i < l->len; i++) {
        if (key.k != V_NIL) { if (!vm_call(v, key, &l->v[i], 1, &k[i])) { for (u32 j = 0; j < i; j++) val_release(k[j]); nom_free(k); return false; } }
        else k[i] = val_retain(l->v[i]);
    }
    bool ok = true;
    for (u32 i = 1; i < l->len && ok; i++) {
        Value kv = k[i], vv = l->v[i];
        u32 j = i;
        while (j > 0) {
            int c = py_cmp(v, k[j - 1], kv, &ok);
            if (!ok) break;
            if (rev ? c >= 0 : c <= 0) break;
            k[j] = k[j - 1];
            l->v[j] = l->v[j - 1];
            j--;
        }
        k[j] = kv;
        l->v[j] = vv;
    }
    for (u32 i = 0; i < l->len; i++) val_release(k[i]);
    nom_free(k);
    return ok;
}

static VmStatus n_sorted(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value key = VAL_NIL, rev = VAL_NIL;
    kw_get(v, "key", &key);
    kw_get(v, "reverse", &rev);
    if (!items_of(v, a[0], out)) return VM_ERROR;
    if (!sort_list(v, AS_LIST(*out), key, val_truthy(rev))) { val_release(*out); *out = VAL_NIL; return VM_ERROR; }
    return VM_OK;
}

static VmStatus n_reversed(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    if (!items_of(v, a[0], out)) return VM_ERROR;
    List *l = AS_LIST(*out);
    for (u32 i = 0; i < l->len / 2; i++) { Value t = l->v[i]; l->v[i] = l->v[l->len - 1 - i]; l->v[l->len - 1 - i] = t; }
    return VM_OK;
}

static VmStatus n_anyall(VM *v, Value *a, int n, Value *out, bool all)
{
    (void)n;
    Value l;
    if (!items_of(v, a[0], &l)) return VM_ERROR;
    bool r = all;
    for (u32 i = 0; i < AS_LIST(l)->len; i++)
        if (val_truthy(AS_LIST(l)->v[i]) != all) { r = !all; break; }
    val_release(l);
    *out = VAL_BOOL(r);
    return VM_OK;
}
static VmStatus n_any(VM *v, Value *a, int n, Value *out) { return n_anyall(v, a, n, out, false); }
static VmStatus n_all(VM *v, Value *a, int n, Value *out) { return n_anyall(v, a, n, out, true); }

static VmStatus n_enumerate(VM *v, Value *a, int n, Value *out)
{
    Value l;
    if (!items_of(v, a[0], &l)) return VM_ERROR;
    i64 start = n > 1 ? val_int(a[1]) : 0;
    Value r = list_new();
    for (u32 i = 0; i < AS_LIST(l)->len; i++) {
        Value pr = list_new();
        list_push(AS_LIST(pr), VAL_INT(start + (i64)i));
        list_push(AS_LIST(pr), val_retain(AS_LIST(l)->v[i]));
        AS_LIST(pr)->tuple = 1;
        list_push(AS_LIST(r), pr);
    }
    val_release(l);
    *out = r;
    return VM_OK;
}

static VmStatus n_zip(VM *v, Value *a, int n, Value *out)
{
    Value ls[NOM_ARGS_MAX];
    u32 m = 0xffffffffu;
    for (int i = 0; i < n; i++) {
        if (!items_of(v, a[i], &ls[i])) { for (int j = 0; j < i; j++) val_release(ls[j]); return VM_ERROR; }
        if (AS_LIST(ls[i])->len < m) m = AS_LIST(ls[i])->len;
    }
    Value r = list_new();
    for (u32 k = 0; n && k < m; k++) {
        Value t = list_new();
        for (int i = 0; i < n; i++) list_push(AS_LIST(t), val_retain(AS_LIST(ls[i])->v[k]));
        AS_LIST(t)->tuple = 1;
        list_push(AS_LIST(r), t);
    }
    for (int i = 0; i < n; i++) val_release(ls[i]);
    *out = r;
    return VM_OK;
}

static VmStatus n_mapfilter(VM *v, Value *a, int n, Value *out, bool filter)
{
    if (!filter && n > 2) {
        /* map(f, xs, ys): f(x, y) of each pair, to the shortest */
        Value ls[NOM_ARGS_MAX];
        u32 m = 0xffffffffu;
        for (int i = 1; i < n; i++) {
            if (!items_of(v, a[i], &ls[i - 1])) { for (int j = 0; j < i - 1; j++) val_release(ls[j]); return VM_ERROR; }
            if (AS_LIST(ls[i - 1])->len < m) m = AS_LIST(ls[i - 1])->len;
        }
        Value r = list_new();
        bool ok = true;
        for (u32 k = 0; k < m && ok; k++) {
            Value args[NOM_ARGS_MAX], y;
            for (int i = 0; i < n - 1; i++) args[i] = AS_LIST(ls[i])->v[k];
            ok = vm_call(v, a[0], args, n - 1, &y);
            if (ok) list_push(AS_LIST(r), y);
        }
        for (int i = 0; i < n - 1; i++) val_release(ls[i]);
        if (!ok) { val_release(r); return VM_ERROR; }
        *out = r;
        return VM_OK;
    }
    Value l;
    if (!items_of(v, a[1], &l)) return VM_ERROR;
    Value r = list_new();
    for (u32 i = 0; i < AS_LIST(l)->len; i++) {
        Value x = AS_LIST(l)->v[i], y;
        if (filter && a[0].k == V_NIL) { if (val_truthy(x)) list_push(AS_LIST(r), val_retain(x)); continue; }
        if (!vm_call(v, a[0], &x, 1, &y)) { val_release(l); val_release(r); return VM_ERROR; }
        if (!filter) list_push(AS_LIST(r), y);
        else { if (val_truthy(y)) list_push(AS_LIST(r), val_retain(x)); val_release(y); }
    }
    val_release(l);
    *out = r;
    return VM_OK;
}
static VmStatus n_map(VM *v, Value *a, int n, Value *out) { return n_mapfilter(v, a, n, out, false); }
static VmStatus n_filter(VM *v, Value *a, int n, Value *out) { return n_mapfilter(v, a, n, out, true); }

static VmStatus n_repr(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    Buf b; buf_init(&b);
    val_repr(&b, a[0]);
    *out = str_new(b.p ? b.p : "", b.len);
    buf_free(&b);
    return VM_OK;
}

/* dir(x): the names x has, sorted: a ship's part's readings and what it
 * does (with ()), a dict's keys, a string's or a list's methods */
static VmStatus n_dir(VM *v, Value *a, int n, Value *out)
{
    Value l = list_new();
    *out = l;
    if (!n) {
        for (int g = NBUILTIN_GLOBALS; g < v->prog->nglobals; g++)
            if (v->gdef[g] && v->prog->global[g][0] != '$') list_push(AS_LIST(l), str_newz(v->prog->global[g]));
    } else if (IS_HANDLE(a[0])) {
        char line[160];
        nom_snprintf(line, sizeof line, "obj keys %s", AS_HANDLE(a[0])->path[0] ? AS_HANDLE(a[0])->path : "-");
        const char *r = ask(v, line);
        for (const char *q = r; *q; ) {
            const char *e = q;
            while (*e && *e != '\n') e++;
            if (e > q) list_push(AS_LIST(l), str_new(q, (size_t)(e - q)));
            q = *e ? e + 1 : e;
        }
        if (!AS_HANDLE(a[0])->path[0]) {
            list_push(AS_LIST(l), str_newz("part()"));
            list_push(AS_LIST(l), str_newz("parts()"));
        }
    } else if (IS_DICT(a[0])) {
        for (u32 i = 0; i < AS_DICT(a[0])->len; i++) list_push(AS_LIST(l), val_retain(AS_DICT(a[0])->k[i]));
    } else if (IS_INSTANCE(a[0])) {
        Dict *fd = AS_DICT(((Instance *)a[0].as.o)->fields);
        for (u32 i = 0; i < fd->len; i++)
            if (IS_STR(fd->k[i]) && AS_STR(fd->k[i])->s[0] != '_') list_push(AS_LIST(l), val_retain(fd->k[i]));
        for (Value c = ((Instance *)a[0].as.o)->cls; IS_CLASS(c); c = ((Klass *)c.as.o)->base) {
            Dict *cd = AS_DICT(((Klass *)c.as.o)->dict);
            for (u32 i = 0; i < cd->len; i++)
                if (IS_STR(cd->k[i]) && !set_has(AS_LIST(l), cd->k[i]) && AS_STR(cd->k[i])->s[0] != '_') list_push(AS_LIST(l), val_retain(cd->k[i]));
        }
    } else {
        const char **m = methods_of(a[0]);
        for (int i = 0; m && m[i]; i++) list_push(AS_LIST(l), str_newz(m[i]));
    }
    sort_list(v, AS_LIST(l), VAL_NIL, false);
    return VM_OK;
}

/* help(): where to look */
static VmStatus n_help(VM *v, Value *a, int n, Value *out)
{
    *out = VAL_NIL;
    if (n) {
        Value d;
        if (n_dir(v, a, n, &d) != VM_OK) return VM_ERROR;
        Buf b; buf_init(&b);
        buf_puts(&b, "it has: ");
        for (u32 i = 0; i < AS_LIST(d)->len; i++) { if (i) buf_puts(&b, ", "); val_tostr(&b, AS_LIST(d)->v[i]); }
        buf_puts(&b, "\n");
        g_write(1, b.p, b.len);
        buf_free(&b);
        val_release(d);
        return VM_OK;
    }
    g_puts("The ship is `ship`: ship.height, ship.orbit.top, ship.engines, ship.part(\"Walrus\").\n"
           "dir(x) lists what x has; in the Script Editor, type a dot for a list and hover a name for its help.\n"
           "Flying: aim(\"prograde\") aim_at(pitch, heading) throttle(100) burn(dv) stage() sleep(s)\n"
           "Maths: speed_needed(h) speed_at(h) burn_time(dv) stopping_distance() when_to_leave(\"krill\") what_if(...)\n"
           "Seeing: print(...) say(...) chart(\"name\", value) beep()   Examples: /root/examples (first_script.py first)\n");
    return VM_OK;
}

static VmStatus n_bool(VM *v, Value *a, int n, Value *out) { (void)v; *out = VAL_BOOL(n && val_truthy(a[0])); return VM_OK; }

/* type(x) is the conversion that makes one (int, float, str, list, dict,
 * bool), so type(2.5) == float and isinstance(3, int) say what Python does */
static const char *type_fn(Value x)
{
    switch (x.k) {
    case V_NIL: return NULL;
    case V_BOOL: return "bool";
    case V_INT: return "int";
    case V_NUM: return "float";
    default: break;
    }
    switch (x.as.o->kind) {
    case O_STR: return "str";
    case O_LIST: return AS_LIST(x)->tuple == 1 ? "tuple" : AS_LIST(x)->tuple == 2 ? "set" : "list";
    case O_DICT: return "dict";
    case O_RANGE: return "range";
    default: return NULL;
    }
}

/* the kinds that aren't a conversion: classes made when first asked for */
static Value g_kinds[6];
static Value hidden_kind(int i)
{
    static const char *N[] = { "NoneType", "function", "generator", "method", "type", "part" };
    if (g_kinds[i].k == V_NIL) g_kinds[i] = class_new(N[i], VAL_NIL);
    return val_retain(g_kinds[i]);
}

static bool is_fn(Value x)
{
    return x.k == V_OBJ && (x.as.o->kind == O_FUNC || x.as.o->kind == O_CLOSURE || x.as.o->kind == O_NATIVE);
}

/* what a value is, in words, for a message: "number", "list" */
static const char *kind_of(Value x)
{
    if (x.k == V_NIL) return "None";
    if (x.k == V_INT || x.k == V_NUM) return "number";
    if (x.k == V_BOOL) return "bool";
    if (IS_INSTANCE(x)) return ((Klass *)((Instance *)x.as.o)->cls.as.o)->name;
    if (IS_HANDLE(x)) return "ship's part";
    const char *t = type_fn(x);
    return t ? (!nom_strcmp(t, "str") ? "string" : t) : IS_GEN(x) ? "generator" : IS_CLASS(x) ? "class" : "function";
}

static VmStatus n_type(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    if (IS_INSTANCE(a[0])) { *out = val_retain(((Instance *)a[0].as.o)->cls); return VM_OK; }
    const char *t = type_fn(a[0]);
    int k = t ? native_find(t, (int)nom_strlen(t)) : -1;
    if (k >= 0) { *out = func_obj_new(k, true); return VM_OK; }
    Value x = a[0];
    *out = hidden_kind(x.k == V_NIL ? 0 : IS_GEN(x) ? 2 : IS_BOUND(x) ? 3 : IS_CLASS(x) ? 4 : IS_HANDLE(x) ? 5 : 1);
    return VM_OK;
}

static VmStatus n_isinstance(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    const char *t = type_fn(a[0]);
    Value kinds = a[1];
    bool any = false;
    Value one[1] = { kinds };
    Value *ks = IS_LIST(kinds) ? AS_LIST(kinds)->v : one;
    u32 nk = IS_LIST(kinds) ? AS_LIST(kinds)->len : 1;
    for (u32 i = 0; i < nk; i++) {
        if (IS_CLASS(ks[i])) {
            /* an instance of it, or of a class built on it */
            for (Value c = IS_INSTANCE(a[0]) ? ((Instance *)a[0].as.o)->cls : VAL_NIL; IS_CLASS(c); c = ((Klass *)c.as.o)->base)
                any |= c.as.o == ks[i].as.o;
            continue;
        }
        if (ks[i].k != V_OBJ || ks[i].as.o->kind != O_NATIVE) { vm_runtime_error(v, "isinstance() wants a kind: int, float, str, list, dict, bool"); return VM_ERROR; }
        int cnt = 0;
        const char *nm = native_table(&cnt)[func_obj_index(ks[i])].name;
        any |= t && !nom_strcmp(nm, t);
        any |= t && !nom_strcmp(nm, "int") && !nom_strcmp(t, "bool");   /* (a bool is an int, in Python) */
    }
    *out = VAL_BOOL(any);
    return VM_OK;
}

/* ValueError("why") and the rest are classes (vm.c); these are
 * BaseException's __init__, __str__ and __repr__, self first: e.args are
 * what it was made with, str(e) the words (a KeyError's key as repr shows
 * it), repr(e) ValueError('why') */
static Value field_of(Value inst, const char *name)
{
    if (!IS_INSTANCE(inst)) return VAL_NIL;
    Value k = str_newz(name), got = VAL_NIL;
    bool ok = dict_get(AS_DICT(((Instance *)inst.as.o)->fields), k, &got);
    val_release(k);
    return ok ? got : VAL_NIL;
}

static VmStatus n_exc_init(VM *v, Value *a, int n, Value *out)
{
    (void)v;
    *out = VAL_NIL;
    if (!IS_INSTANCE(a[0])) return VM_OK;
    Value t = list_new();
    for (int i = 1; i < n; i++) list_push(AS_LIST(t), val_retain(a[i]));
    AS_LIST(t)->tuple = 1;
    dict_set(AS_DICT(((Instance *)a[0].as.o)->fields), str_newz("args"), t);
    return VM_OK;
}

static VmStatus n_exc_str(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value m = field_of(a[0], "_msg"), args = field_of(a[0], "args");
    if (IS_STR(m)) { *out = val_retain(m); return VM_OK; }
    Buf b; buf_init(&b);
    if (IS_LIST(args) && AS_LIST(args)->len == 1) {
        if (vm_isinstance(a[0], vm_exc_class(v, "KeyError"))) val_repr(&b, AS_LIST(args)->v[0]);
        else val_tostr(&b, AS_LIST(args)->v[0]);
    } else if (IS_LIST(args) && AS_LIST(args)->len > 1) val_repr(&b, args);
    *out = str_new(b.p ? b.p : "", b.len);
    buf_free(&b);
    return VM_OK;
}

static VmStatus n_exc_repr(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    Value args = field_of(a[0], "args");
    Buf b; buf_init(&b);
    buf_puts(&b, IS_INSTANCE(a[0]) ? ((Klass *)((Instance *)a[0].as.o)->cls.as.o)->name : "Exception");
    buf_putc(&b, '(');
    for (u32 i = 0; IS_LIST(args) && i < AS_LIST(args)->len; i++) { if (i) buf_puts(&b, ", "); val_repr(&b, AS_LIST(args)->v[i]); }
    buf_putc(&b, ')');
    *out = str_new(b.p, b.len);
    buf_free(&b);
    return VM_OK;
}

/* super(): self, and the class to look in (the base of the method's) */
static VmStatus n_super(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    if (!IS_CLASS(a[0])) { vm_runtime_error(v, "TypeError: super() works inside a class's method"); return VM_ERROR; }
    *out = bound_new(O_SUPER, a[1], ((Klass *)a[0].as.o)->base, VAL_NIL);
    return VM_OK;
}

/* @staticmethod, @classmethod, @property: the def, marked */
static VmStatus mark(VM *v, Value *a, Value *out, const char *how)
{
    if (!is_fn(a[0])) { vm_runtime_error(v, "TypeError: @%s goes on a def", how + 1); return VM_ERROR; }
    Value nm = str_newz(how);
    *out = bound_new(O_BOUND, VAL_NIL, a[0], nm);
    val_release(nm);
    return VM_OK;
}
static VmStatus n_staticmethod(VM *v, Value *a, int n, Value *out) { (void)n; return mark(v, a, out, "$static"); }
static VmStatus n_classmethod(VM *v, Value *a, int n, Value *out) { (void)n; return mark(v, a, out, "$class"); }
static VmStatus n_property(VM *v, Value *a, int n, Value *out) { (void)n; return mark(v, a, out, "$property"); }

/* next(g), next(g, default): a generator's next value */
static VmStatus n_next(VM *v, Value *a, int n, Value *out)
{
    if (!IS_GEN(a[0])) { vm_runtime_error(v, "TypeError: next() wants a generator (a def with yield, called), not a %s", kind_of(a[0])); return VM_ERROR; }
    bool got;
    if (!gen_next(v, a[0], out, &got)) return VM_ERROR;
    if (got) return VM_OK;
    if (n > 1) { *out = val_retain(a[1]); return VM_OK; }
    vm_runtime_error(v, "StopIteration: the generator has no more");
    return VM_ERROR;
}

/* iter(x): a generator stays itself; anything else, its items */
static VmStatus n_iter(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    if (IS_GEN(a[0])) { *out = val_retain(a[0]); return VM_OK; }
    /* (as a list of them: for, list() and next()-less code go through it the same) */
    return items_of(v, a[0], out) ? VM_OK : VM_ERROR;
}

static VmStatus n_ord(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    if (!IS_STR(a[0]) || !AS_STR(a[0])->len) { vm_runtime_error(v, "TypeError: ord() wants one letter"); return VM_ERROR; }
    const unsigned char *s = (const unsigned char *)AS_STR(a[0])->s;
    u32 len = AS_STR(a[0])->len, c = s[0], k = 1;
    if (c >= 0xf0 && len >= 4) { c = ((c & 7) << 18) | ((s[1] & 63u) << 12) | ((s[2] & 63u) << 6) | (s[3] & 63u); k = 4; }
    else if (c >= 0xe0 && len >= 3) { c = ((c & 15) << 12) | ((s[1] & 63u) << 6) | (s[2] & 63u); k = 3; }
    else if (c >= 0xc0 && len >= 2) { c = ((c & 31) << 6) | (s[1] & 63u); k = 2; }
    if (k != len) { vm_runtime_error(v, "TypeError: ord() wants one letter, got %d", (int)len); return VM_ERROR; }
    *out = VAL_INT(c);
    return VM_OK;
}

static VmStatus n_chr(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    i64 c = val_int(a[0]);
    if (c < 0 || c > 0x10ffff) { vm_runtime_error(v, "ValueError: chr() arg not in range(0x110000)"); return VM_ERROR; }
    char t[4];
    int k = 0;
    if (c < 0x80) t[k++] = (char)c;
    else if (c < 0x800) { t[k++] = (char)(0xc0 | (c >> 6)); t[k++] = (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { t[k++] = (char)(0xe0 | (c >> 12)); t[k++] = (char)(0x80 | ((c >> 6) & 63)); t[k++] = (char)(0x80 | (c & 63)); }
    else { t[k++] = (char)(0xf0 | (c >> 18)); t[k++] = (char)(0x80 | ((c >> 12) & 63)); t[k++] = (char)(0x80 | ((c >> 6) & 63)); t[k++] = (char)(0x80 | (c & 63)); }
    *out = str_new(t, (size_t)k);
    return VM_OK;
}

/* hex(255) '0xff', bin(5) '0b101', oct(8) '0o10' */
static VmStatus based(VM *v, Value *a, Value *out, int base, const char *pre)
{
    if (a[0].k != V_INT && a[0].k != V_BOOL) { vm_runtime_error(v, "TypeError: %s() wants a whole number, not a %s", pre[1] == 'x' ? "hex" : pre[1] == 'b' ? "bin" : "oct", kind_of(a[0])); return VM_ERROR; }
    i64 x = val_int(a[0]);
    u64 u = x < 0 ? (u64)(-(x + 1)) + 1 : (u64)x;
    char t[72];
    int k = 0;
    do { int d = (int)(u % (u64)base); t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (u64)base; } while (u);
    Buf b; buf_init(&b);
    if (x < 0) buf_putc(&b, '-');
    buf_puts(&b, pre);
    while (k) buf_putc(&b, t[--k]);
    *out = str_new(b.p, b.len);
    buf_free(&b);
    return VM_OK;
}
static VmStatus n_hex(VM *v, Value *a, int n, Value *out) { (void)n; return based(v, a, out, 16, "0x"); }
static VmStatus n_bin(VM *v, Value *a, int n, Value *out) { (void)n; return based(v, a, out, 2, "0b"); }
static VmStatus n_oct(VM *v, Value *a, int n, Value *out) { (void)n; return based(v, a, out, 8, "0o"); }

static VmStatus n_hash(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value x = a[0];
    if (x.k == V_INT || x.k == V_BOOL) { *out = VAL_INT(val_int(x)); return VM_OK; }
    if (x.k == V_NUM) {
        if (x.as.d == nom_floor(x.as.d) && x.as.d < 9e18 && x.as.d > -9e18) { *out = VAL_INT((i64)x.as.d); return VM_OK; }
        u64 bits;
        nom_memcpy(&bits, &x.as.d, 8);
        *out = VAL_INT((i64)(bits >> 1));
        return VM_OK;
    }
    if (x.k == V_NIL) { *out = VAL_INT(0x5f3759df); return VM_OK; }
    if (IS_STR(x)) {
        u64 h = 1469598103934665603ull;
        for (u32 i = 0; i < AS_STR(x)->len; i++) { h ^= (unsigned char)AS_STR(x)->s[i]; h *= 1099511628211ull; }
        *out = VAL_INT((i64)(h >> 1));
        return VM_OK;
    }
    if (IS_LIST(x) && AS_LIST(x)->tuple != 1) { vm_runtime_error(v, "TypeError: a %s can't be hashed (a tuple can)", kind_of(x)); return VM_ERROR; }
    if (IS_DICT(x)) { vm_runtime_error(v, "TypeError: a dict can't be hashed"); return VM_ERROR; }
    *out = VAL_INT((i64)(u64)x.as.o >> 3);
    return VM_OK;
}

static VmStatus n_id(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    *out = a[0].k == V_OBJ ? VAL_INT((i64)(u64)a[0].as.o) : VAL_INT(val_int(a[0]) * 32 + 8);
    return VM_OK;
}

static VmStatus n_callable(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    Value fn;
    *out = VAL_BOOL(is_fn(a[0]) || IS_CLASS(a[0]) || IS_BOUND(a[0]) || IS_HANDLE(a[0]) || vm_method(a[0], "__call__", &fn));
    return VM_OK;
}

/* getattr(x, "name"[, default]), hasattr, setattr: the dot, by a name */
static VmStatus n_getattr(VM *v, Value *a, int n, Value *out)
{
    if (!IS_STR(a[1])) { vm_runtime_error(v, "TypeError: getattr()'s name must be text"); return VM_ERROR; }
    if (host_getattr(v, a[0], AS_STR(a[1]), out)) return VM_OK;
    if (n > 2) { v->err[0] = 0; *out = val_retain(a[2]); return VM_OK; }
    return VM_ERROR;
}

static VmStatus n_hasattr(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    if (!IS_STR(a[1])) { vm_runtime_error(v, "TypeError: hasattr()'s name must be text"); return VM_ERROR; }
    Value got;
    bool ok = host_getattr(v, a[0], AS_STR(a[1]), &got);
    if (ok) val_release(got); else v->err[0] = 0;
    *out = VAL_BOOL(ok);
    return VM_OK;
}

static VmStatus n_setattr(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    *out = VAL_NIL;
    if (!IS_STR(a[1])) { vm_runtime_error(v, "TypeError: setattr()'s name must be text"); return VM_ERROR; }
    return host_setattr(v, a[0], AS_STR(a[1]), a[2]) ? VM_OK : VM_ERROR;
}

/* open(path, "w"): a file on the ship's computer, written as you go (or
 * "r", read whole when opened; "a" to add to the end). with open(...) as f:
 * closes it when the block is done. */
static Value g_file_cls;
static Value fileclass(void)
{
    if (g_file_cls.k != V_NIL) return g_file_cls;
    g_file_cls = class_new("file", VAL_NIL);
    static const char *M[][2] = {
        { "write", "_f_write" }, { "read", "_f_read" }, { "readline", "_f_readline" }, { "readlines", "_f_readlines" },
        { "close", "_f_close" }, { "flush", "_f_flush" }, { "__enter__", "_f_enter" }, { "__exit__", "_f_exit" },
        { "__iter__", "_f_readlines" }, { "__repr__", "_f_repr" },
    };
    for (u32 i = 0; i < sizeof M / sizeof M[0]; i++)
        dict_set(AS_DICT(((Klass *)g_file_cls.as.o)->dict), str_newz(M[i][0]), func_obj_new(native_find(M[i][1], (int)nom_strlen(M[i][1])), true));
    return g_file_cls;
}

static void set_field(Value inst, const char *name, Value x)
{
    dict_set(AS_DICT(((Instance *)inst.as.o)->fields), str_newz(name), x);
}

static VmStatus n_open(VM *v, Value *a, int n, Value *out)
{
    Value modev = n > 1 ? a[1] : VAL_NIL;
    kw_get(v, "mode", &modev);
    const char *mode = IS_STR(modev) ? AS_STR(modev)->s : "r";
    if (!IS_STR(a[0])) { vm_runtime_error(v, "TypeError: open() wants a file's path, as text"); return VM_ERROR; }
    const char *path = AS_STR(a[0])->s;
    bool rd = false, wr = false, ap = false;
    for (const char *q = mode; *q; q++) { rd |= *q == 'r'; wr |= *q == 'w'; ap |= *q == 'a'; }
    if ((int)rd + (int)wr + (int)ap != 1) { vm_runtime_error(v, "ValueError: open()'s mode is \"r\", \"w\" or \"a\", not \"%s\"", mode); return VM_ERROR; }
    Value f = instance_new(fileclass());
    set_field(f, "name", val_retain(a[0]));
    set_field(f, "mode", str_newz(mode));
    set_field(f, "closed", VAL_BOOL(0));
    if (rd) {
        i64 got = g_slurp(path, io_buf, sizeof io_buf);
        if (got < 0) {
            val_release(f);
            vm_runtime_error(v, "FileNotFoundError: [Errno 2] No such file or directory: '%s'", path);
            return VM_ERROR;
        }
        set_field(f, "_data", str_new(io_buf, (size_t)got));
        set_field(f, "_pos", VAL_INT(0));
        set_field(f, "_fd", VAL_INT(-1));
    } else {
        int fd = g_open(path, O_WRONLY | O_CREAT | (ap ? O_APPEND : O_TRUNC));
        if (fd < 0) { val_release(f); vm_runtime_error(v, "OSError: can't write to '%s'", path); return VM_ERROR; }
        set_field(f, "_fd", VAL_INT(fd));
    }
    *out = f;
    return VM_OK;
}

static bool file_ok(VM *v, Value f, bool reading)
{
    if (!IS_INSTANCE(f)) { vm_runtime_error(v, "TypeError: that's not a file"); return false; }
    if (val_truthy(field_of(f, "closed"))) { vm_runtime_error(v, "ValueError: the file is closed"); return false; }
    if (reading && !IS_STR(field_of(f, "_data"))) { vm_runtime_error(v, "OSError: the file was opened to write, not to read (open(path) reads)"); return false; }
    if (!reading && val_int(field_of(f, "_fd")) < 0) { vm_runtime_error(v, "OSError: the file was opened to read, not to write (open(path, \"w\"))"); return false; }
    return true;
}

static VmStatus n_f_write(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    if (!file_ok(v, a[0], false)) return VM_ERROR;
    if (!IS_STR(a[1])) { vm_runtime_error(v, "TypeError: write() wants text, not a %s (str(x), or an f-string)", kind_of(a[1])); return VM_ERROR; }
    g_write((int)val_int(field_of(a[0], "_fd")), AS_STR(a[1])->s, AS_STR(a[1])->len);
    *out = VAL_INT(AS_STR(a[1])->len);
    return VM_OK;
}

/* the rest of a read file: up to n letters, one line, or all of it */
static VmStatus f_take(VM *v, Value f, i64 n, bool line, Value *out)
{
    if (!file_ok(v, f, true)) return VM_ERROR;
    Str *d = AS_STR(field_of(f, "_data"));
    i64 pos = val_int(field_of(f, "_pos")), end = d->len;
    if (line) { i64 e = pos; while (e < end && d->s[e] != '\n') e++; end = e < end ? e + 1 : e; }
    else if (n >= 0 && pos + n < end) end = pos + n;
    if (pos > end) pos = end;
    *out = str_new(d->s + pos, (size_t)(end - pos));
    set_field(f, "_pos", VAL_INT(end));
    return VM_OK;
}
static VmStatus n_f_read(VM *v, Value *a, int n, Value *out) { return f_take(v, a[0], n > 1 ? val_int(a[1]) : -1, false, out); }
static VmStatus n_f_readline(VM *v, Value *a, int n, Value *out) { (void)n; return f_take(v, a[0], -1, true, out); }
static VmStatus n_f_readlines(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value l = list_new();
    for (;;) {
        Value line;
        if (f_take(v, a[0], -1, true, &line) != VM_OK) { val_release(l); return VM_ERROR; }
        if (!AS_STR(line)->len) { val_release(line); break; }
        list_push(AS_LIST(l), line);
    }
    *out = l;
    return VM_OK;
}
static VmStatus n_f_close(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    *out = VAL_NIL;
    if (!IS_INSTANCE(a[0])) return VM_OK;
    i64 fd = val_int(field_of(a[0], "_fd"));
    if (fd >= 0 && !val_truthy(field_of(a[0], "closed"))) g_close((int)fd);
    set_field(a[0], "closed", VAL_BOOL(1));
    return VM_OK;
}
static VmStatus n_f_flush(VM *v, Value *a, int n, Value *out) { (void)v; (void)a; (void)n; *out = VAL_NIL; return VM_OK; }
static VmStatus n_f_enter(VM *v, Value *a, int n, Value *out) { (void)v; (void)n; *out = val_retain(a[0]); return VM_OK; }
static VmStatus n_f_exit(VM *v, Value *a, int n, Value *out)
{
    n_f_close(v, a, n, out);
    *out = VAL_BOOL(0);
    return VM_OK;
}
static VmStatus n_f_repr(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    Buf b; buf_init(&b);
    buf_puts(&b, "<file ");
    val_repr(&b, field_of(a[0], "name"));
    buf_puts(&b, " mode=");
    val_repr(&b, field_of(a[0], "mode"));
    buf_putc(&b, '>');
    *out = str_new(b.p, b.len);
    buf_free(&b);
    return VM_OK;
}

static VmStatus n_divmod(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value q, r;
    Value x = a[0], y = a[1];
    bool ints = x.k == V_INT && y.k == V_INT;
    double dx = val_num(x), dy = val_num(y);
    if (dy == 0) { vm_runtime_error(v, "ZeroDivisionError: divmod by zero"); return VM_ERROR; }
    double fq = nom_floor(dx / dy);
    if (ints) { q = VAL_INT((i64)fq); r = VAL_INT(x.as.i - (i64)fq * y.as.i); }
    else { q = VAL_NUM(fq); r = VAL_NUM(dx - fq * dy); }
    Value t = list_new();
    list_push(AS_LIST(t), q);
    list_push(AS_LIST(t), r);
    AS_LIST(t)->tuple = 1;
    *out = t;
    return VM_OK;
}

static VmStatus n_pyp(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    if (a[0].k == V_INT && a[1].k == V_INT && a[1].as.i >= 0) {
        i64 r = 1;
        bool over = false;
        for (i64 i = 0; i < a[1].as.i && !over; i++) over = __builtin_mul_overflow(r, a[0].as.i, &r);
        *out = over ? VAL_NUM(nom_pow(val_num(a[0]), val_num(a[1]))) : VAL_INT(r);
        return VM_OK;
    }
    (void)v;
    *out = VAL_NUM(nom_pow(val_num(a[0]), val_num(a[1])));
    return VM_OK;
}

/* set(xs): them with no repeats ({1, 2} prints as it should; in, len, for,
 * | & - work) */
static VmStatus n_set(VM *v, Value *a, int n, Value *out)
{
    Value all = list_new();
    AS_LIST(all)->tuple = 2;
    if (n) {
        Value l;
        if (!items_of(v, a[0], &l)) { val_release(all); return VM_ERROR; }
        for (u32 i = 0; i < AS_LIST(l)->len; i++) {
            bool seen = false;
            for (u32 j = 0; j < AS_LIST(all)->len && !seen; j++) seen = val_equal(AS_LIST(all)->v[j], AS_LIST(l)->v[i]);
            if (!seen) list_push(AS_LIST(all), val_retain(AS_LIST(l)->v[i]));
        }
        val_release(l);
    }
    *out = all;
    return VM_OK;
}

/* random: a little generator of its own (xorshift), seeded from the clock
 * of the game the first time, or random.seed(n) */
static u64 g_rng;
static double rnd(VM *v)
{
    if (!g_rng) {
        double t = reading_num(v, "time");
        g_rng = 0x9E3779B97F4A7C15ull ^ (u64)(t * 1000.0) ^ (u64)sysc(SYS_getpid, 0, 0, 0) << 32;
        if (!g_rng) g_rng = 1;
    }
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (double)(g_rng >> 11) / 9007199254740992.0;
}
static VmStatus n_random(VM *v, Value *a, int n, Value *out) { (void)a; (void)n; *out = VAL_NUM(rnd(v)); return VM_OK; }
static VmStatus n_uniform(VM *v, Value *a, int n, Value *out)
{ (void)n; double lo = val_num(a[0]), hi = val_num(a[1]); *out = VAL_NUM(lo + (hi - lo) * rnd(v)); return VM_OK; }
static VmStatus n_randint(VM *v, Value *a, int n, Value *out)
{ (void)n; i64 lo = val_int(a[0]), hi = val_int(a[1]); *out = VAL_INT(lo + (i64)(rnd(v) * (double)(hi - lo + 1))); return VM_OK; }
static VmStatus n_choice(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value l;
    if (!items_of(v, a[0], &l)) return VM_ERROR;
    u32 len = AS_LIST(l)->len;
    if (!len) { val_release(l); vm_runtime_error(v, "choice() from nothing"); return VM_ERROR; }
    *out = val_retain(AS_LIST(l)->v[(u32)(rnd(v) * len) % len]);
    val_release(l);
    return VM_OK;
}
static VmStatus n_seed(VM *v, Value *a, int n, Value *out) { (void)v; (void)n; g_rng = (u64)val_int(a[0]) * 2654435761ull + 1; *out = VAL_NIL; return VM_OK; }

static VmStatus n_hypot(VM *v, Value *a, int n, Value *out)
{ (void)v; double s = 0; for (int i = 0; i < n; i++) s += val_num(a[i]) * val_num(a[i]); *out = VAL_NUM(__builtin_sqrt(s)); return VM_OK; }
static VmStatus n_log2(VM *v, Value *a, int n, Value *out)
{
    (void)v;
    double x = nom_math(MATH_LOG, val_num(a[0]), 0);
    if (n > 1) x /= nom_math(MATH_LOG, val_num(a[1]), 0);
    *out = VAL_NUM(x);
    return VM_OK;
}
MATH1(n_log10, MATH_LOG10)
static VmStatus n_now(VM *v, Value *a, int n, Value *out) { (void)a; (void)n; *out = VAL_NUM(reading_num(v, "time")); return VM_OK; }

/* import math: a dict of the module's things (math.sqrt is a call to
 * sqrt, math.pi a number) */
static void mod_put(Value d, const char *name, const char *native)
{
    int k = native_find(native, (int)nom_strlen(native));
    if (k >= 0) dict_set(AS_DICT(d), str_newz(name), func_obj_new(k, true));
}

static VmStatus n_module(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    const char *m = IS_STR(a[0]) ? AS_STR(a[0])->s : "";
    Value d = dict_new();
    if (!nom_strcmp(m, "math")) {
        static const char *SAME[] = { "sqrt", "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "exp",
                                      "floor", "ceil", "hypot", "log10", NULL };
        for (int i = 0; SAME[i]; i++) mod_put(d, SAME[i], SAME[i]);
        mod_put(d, "log", "_log");
        mod_put(d, "degrees", "deg");
        mod_put(d, "radians", "rad");
        mod_put(d, "fabs", "_fabs");
        mod_put(d, "pow", "_pow");
        static const char *MORE[][2] = { { "isclose", "_isclose" }, { "copysign", "_copysign" }, { "trunc", "_trunc" },
            { "isfinite", "_isfinite" }, { "isinf", "_isinf" }, { "isnan", "_isnan" }, { "factorial", "_factorial" },
            { "gcd", "_gcd" }, { "dist", "_dist" }, { "prod", "_prod" }, { "fsum", "_fsum" }, { "log2", "_log2" },
            { "sinh", "_sinh" }, { "cosh", "_cosh" }, { "tanh", "_tanh" }, { "fmod", "_fmod" }, { "modf", "_modf" },
            { "exp2", "_exp2" }, { "cbrt", "_cbrt" }, { "sqrt", "sqrt" } };
        for (u32 i = 0; i < sizeof MORE / sizeof MORE[0]; i++) mod_put(d, MORE[i][0], MORE[i][1]);
        dict_set(AS_DICT(d), str_newz("pi"), VAL_NUM(3.141592653589793));
        dict_set(AS_DICT(d), str_newz("e"), VAL_NUM(2.718281828459045));
        dict_set(AS_DICT(d), str_newz("tau"), VAL_NUM(6.283185307179586));
        dict_set(AS_DICT(d), str_newz("inf"), VAL_NUM(__builtin_inf()));
        dict_set(AS_DICT(d), str_newz("nan"), VAL_NUM(__builtin_nan("")));
    } else if (!nom_strcmp(m, "time")) {
        mod_put(d, "sleep", "sleep");
        mod_put(d, "time", "_now");
        mod_put(d, "monotonic", "_now");
    } else if (!nom_strcmp(m, "random")) {
        mod_put(d, "random", "_random");
        mod_put(d, "uniform", "_uniform");
        mod_put(d, "randint", "_randint");
        mod_put(d, "choice", "_choice");
        mod_put(d, "seed", "_seed");
    }
    *out = d;
    return VM_OK;
}

static VmStatus n_pow(VM *v, Value *a, int n, Value *out)
{ (void)v; (void)n; *out = VAL_NUM(nom_pow(val_num(a[0]), val_num(a[1]))); return VM_OK; }

/* the rest of math: what a flight's maths reaches for */
static bool nums(VM *v, Value *a, int n, const char *fn)
{
    for (int i = 0; i < n; i++)
        if (a[i].k != V_INT && a[i].k != V_NUM && a[i].k != V_BOOL) {
            vm_runtime_error(v, "TypeError: math.%s() wants numbers, not a %s", fn, kind_of(a[i]));
            return false;
        }
    return true;
}
static VmStatus n_fabs(VM *v, Value *a, int n, Value *out)
{ if (!nums(v, a, n, "fabs")) return VM_ERROR; double x = val_num(a[0]); *out = VAL_NUM(x < 0 || (x == 0 && 1.0 / x < 0) ? -x : x); return VM_OK; }
static VmStatus n_isclose(VM *v, Value *a, int n, Value *out)
{
    if (!nums(v, a, n, "isclose")) return VM_ERROR;
    Value rt = VAL_NUM(1e-9), at = VAL_NUM(0.0);
    kw_get(v, "rel_tol", &rt);
    kw_get(v, "abs_tol", &at);
    double x = val_num(a[0]), y = val_num(a[1]), d = x - y;
    if (d < 0) d = -d;
    double ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, big = ax > ay ? ax : ay;
    *out = VAL_BOOL(x == y || d <= val_num(rt) * big || d <= val_num(at));
    return VM_OK;
}
static VmStatus n_copysign(VM *v, Value *a, int n, Value *out)
{
    if (!nums(v, a, n, "copysign")) return VM_ERROR;
    double x = val_num(a[0]), y = val_num(a[1]);
    if (x < 0) x = -x;
    *out = VAL_NUM(y < 0 || (y == 0 && 1.0 / y < 0) ? -x : x);
    return VM_OK;
}
static VmStatus n_trunc(VM *v, Value *a, int n, Value *out)
{ if (!nums(v, a, n, "trunc")) return VM_ERROR; double x = val_num(a[0]); *out = VAL_INT((i64)x); return VM_OK; }
static VmStatus n_isfinite(VM *v, Value *a, int n, Value *out)
{ if (!nums(v, a, n, "isfinite")) return VM_ERROR; double x = val_num(a[0]); *out = VAL_BOOL(x - x == 0); return VM_OK; }
static VmStatus n_isinf(VM *v, Value *a, int n, Value *out)
{ if (!nums(v, a, n, "isinf")) return VM_ERROR; double x = val_num(a[0]); *out = VAL_BOOL(x == x && x - x != 0); return VM_OK; }
static VmStatus n_isnan(VM *v, Value *a, int n, Value *out)
{ if (!nums(v, a, n, "isnan")) return VM_ERROR; double x = val_num(a[0]); *out = VAL_BOOL(x != x); return VM_OK; }
static VmStatus n_factorial(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    if (a[0].k != V_INT || a[0].as.i < 0) { vm_runtime_error(v, "ValueError: factorial() wants a whole number, 0 or more"); return VM_ERROR; }
    i64 r = 1;
    double d = 1;
    bool over = false;
    for (i64 i = 2; i <= a[0].as.i; i++) { d *= (double)i; if (!over) over = __builtin_mul_overflow(r, i, &r); }
    *out = over ? VAL_NUM(d) : VAL_INT(r);
    return VM_OK;
}
static VmStatus n_gcd(VM *v, Value *a, int n, Value *out)
{
    i64 g = 0;
    for (int i = 0; i < n; i++) {
        if (a[i].k != V_INT && a[i].k != V_BOOL) { vm_runtime_error(v, "TypeError: gcd() wants whole numbers"); return VM_ERROR; }
        i64 x = val_int(a[i]);
        if (x < 0) x = -x;
        while (x) { i64 t = g % x; g = x; x = t; }
    }
    *out = VAL_INT(g);
    return VM_OK;
}
static VmStatus n_dist(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value p, q;
    if (!items_of(v, a[0], &p)) return VM_ERROR;
    if (!items_of(v, a[1], &q)) { val_release(p); return VM_ERROR; }
    if (AS_LIST(p)->len != AS_LIST(q)->len) { val_release(p); val_release(q); vm_runtime_error(v, "ValueError: dist() of two points with different numbers of coordinates"); return VM_ERROR; }
    double sq = 0;
    for (u32 i = 0; i < AS_LIST(p)->len; i++) { double d = val_num(AS_LIST(p)->v[i]) - val_num(AS_LIST(q)->v[i]); sq += d * d; }
    val_release(p);
    val_release(q);
    *out = VAL_NUM(__builtin_sqrt(sq));
    return VM_OK;
}
static VmStatus n_prod(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value l;
    if (!items_of(v, a[0], &l)) return VM_ERROR;
    Value acc = VAL_INT(1);
    for (u32 i = 0; i < AS_LIST(l)->len; i++) {
        Value x = AS_LIST(l)->v[i];
        i64 r;
        if (acc.k == V_INT && x.k == V_INT && !__builtin_mul_overflow(acc.as.i, x.as.i, &r)) acc = VAL_INT(r);
        else acc = VAL_NUM(val_num(acc) * val_num(x));
    }
    val_release(l);
    *out = acc;
    return VM_OK;
}
static VmStatus n_fsum(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    Value l;
    if (!items_of(v, a[0], &l)) return VM_ERROR;
    /* compensated (Neumaier): fsum([0.1] * 10) is 1.0 */
    double s = 0, c = 0;
    for (u32 i = 0; i < AS_LIST(l)->len; i++) {
        double x = val_num(AS_LIST(l)->v[i]), t = s + x;
        double as = s < 0 ? -s : s, ax = x < 0 ? -x : x;
        c += as >= ax ? (s - t) + x : (x - t) + s;
        s = t;
    }
    val_release(l);
    *out = VAL_NUM(s + c);
    return VM_OK;
}
static VmStatus n_log2b(VM *v, Value *a, int n, Value *out)
{ (void)n; (void)v; *out = VAL_NUM(nom_math(MATH_LOG, val_num(a[0]), 0) / 0.6931471805599453); return VM_OK; }
static VmStatus n_hyper(VM *v, Value *a, int n, Value *out, int which)
{
    (void)n;
    if (!nums(v, a, 1, which == 0 ? "sinh" : which == 1 ? "cosh" : "tanh")) return VM_ERROR;
    double e = nom_math(MATH_EXP, val_num(a[0]), 0), ie = 1.0 / e;
    *out = VAL_NUM(which == 0 ? (e - ie) / 2 : which == 1 ? (e + ie) / 2 : (e - ie) / (e + ie));
    return VM_OK;
}
static VmStatus n_sinh(VM *v, Value *a, int n, Value *out) { return n_hyper(v, a, n, out, 0); }
static VmStatus n_cosh(VM *v, Value *a, int n, Value *out) { return n_hyper(v, a, n, out, 1); }
static VmStatus n_tanh(VM *v, Value *a, int n, Value *out) { return n_hyper(v, a, n, out, 2); }
static VmStatus n_fmod(VM *v, Value *a, int n, Value *out)
{
    if (!nums(v, a, n, "fmod")) return VM_ERROR;
    double x = val_num(a[0]), y = val_num(a[1]);
    if (y == 0) { vm_runtime_error(v, "ValueError: math domain error"); return VM_ERROR; }
    double q = x / y;
    q = q < 0 ? -nom_floor(-q) : nom_floor(q);
    *out = VAL_NUM(x - q * y);
    return VM_OK;
}
static VmStatus n_modf(VM *v, Value *a, int n, Value *out)
{
    if (!nums(v, a, n, "modf")) return VM_ERROR;
    double x = val_num(a[0]), w = x < 0 ? -nom_floor(-x) : nom_floor(x);
    Value t = list_new();
    list_push(AS_LIST(t), VAL_NUM(x - w));
    list_push(AS_LIST(t), VAL_NUM(w));
    AS_LIST(t)->tuple = 1;
    *out = t;
    return VM_OK;
}
static VmStatus n_exp2(VM *v, Value *a, int n, Value *out)
{ if (!nums(v, a, n, "exp2")) return VM_ERROR; *out = VAL_NUM(nom_pow(2.0, val_num(a[0]))); return VM_OK; }
static VmStatus n_cbrt(VM *v, Value *a, int n, Value *out)
{
    if (!nums(v, a, n, "cbrt")) return VM_ERROR;
    double x = val_num(a[0]), r = nom_pow(x < 0 ? -x : x, 1.0 / 3.0);
    *out = VAL_NUM(x < 0 ? -r : r);
    return VM_OK;
}

/* "%d m at %.1f s" % (120, 3.25): %d %i %f %e %g %s %r %x %%, with flags,
 * a width and a precision */
bool host_percent(VM *v, Value fmt, Value args, Value *out)
{
    Str *f = AS_STR(fmt);
    Value one[1] = { args };
    Value *av = IS_LIST(args) ? AS_LIST(args)->v : one;
    u32 an = IS_LIST(args) ? AS_LIST(args)->len : 1, ai = 0;
    Buf b; buf_init(&b);
    for (u32 i = 0; i < f->len; i++) {
        char c = f->s[i];
        if (c != '%') { buf_putc(&b, c); continue; }
        if (i + 1 < f->len && f->s[i + 1] == '%') { buf_putc(&b, '%'); i++; continue; }
        char spec[24];
        int k = 0;
        i++;
        while (i < f->len && k < 20 && (f->s[i] == '-' || f->s[i] == '+' || f->s[i] == ' ' || f->s[i] == '0' ||
               f->s[i] == '.' || (f->s[i] >= '0' && f->s[i] <= '9'))) spec[k++] = f->s[i++];
        if (i >= f->len) { vm_runtime_error(v, "a %% with nothing after it"); buf_free(&b); return false; }
        char t = f->s[i];
        if (ai >= an) { vm_runtime_error(v, "not enough values for the %%s in the text"); buf_free(&b); return false; }
        Value x = av[ai++];
        /* as format(x, spec) would: "<" for "-", a type letter on the end */
        char fs[32];
        int q = 0;
        int sk = 0;
        if (k && spec[0] == '-') { fs[q++] = '<'; sk = 1; }
        for (; sk < k; sk++) fs[q++] = spec[sk];
        if (t == 'd' || t == 'i') { if (x.k == V_NUM) x = VAL_INT((i64)x.as.d); fs[q++] = 'd'; }
        else if (t == 'f' || t == 'F' || t == 'e' || t == 'g') fs[q++] = 'f';
        else if (t == 'x') fs[q++] = 'x';
        else if (t != 's' && t != 'r') { vm_runtime_error(v, "%%%c isn't a format here (%%d %%f %%s %%r)", t); buf_free(&b); return false; }
        fs[q] = 0;
        Value sv = str_newz(fs), r;
        Value pa[2] = { x, sv };
        if (t == 'r') { val_repr(&b, x); val_release(sv); continue; }
        bool dot = false;
        for (int z = 0; fs[z]; z++) dot |= fs[z] == '.';
        if (t == 'f' && !dot) { val_release(sv); fs[q - 1] = 0; nom_snprintf(fs + nom_strlen(fs), 8, ".6f"); sv = str_newz(fs); pa[1] = sv; }
        n_format(v, pa, 2, &r);
        buf_put(&b, AS_STR(r)->s, AS_STR(r)->len);
        val_release(r);
        val_release(sv);
    }
    if (ai < an && IS_LIST(args)) { vm_runtime_error(v, "more values than %%s in the text"); buf_free(&b); return false; }
    *out = str_new(b.p ? b.p : "", b.len);
    buf_free(&b);
    return true;
}

/* "{} and {:.2f}".format(a, b) -- {} in order, {0} by number, {name} by
 * keyword, each with an optional :spec */
static VmStatus str_format(VM *v, Str *f, Value *a, int n, Value *out)
{
    Buf b; buf_init(&b);
    int auto_i = 0;
    for (u32 i = 0; i < f->len; i++) {
        char c = f->s[i];
        if (c == '{' && i + 1 < f->len && f->s[i + 1] == '{') { buf_putc(&b, '{'); i++; continue; }
        if (c == '}' && i + 1 < f->len && f->s[i + 1] == '}') { buf_putc(&b, '}'); i++; continue; }
        if (c != '{') { buf_putc(&b, c); continue; }
        u32 j = i + 1;
        while (j < f->len && f->s[j] != '}') j++;
        if (j >= f->len) { vm_runtime_error(v, "a { with no } in the text"); buf_free(&b); return VM_ERROR; }
        u32 colon = i + 1;
        while (colon < j && f->s[colon] != ':') colon++;
        Value x = VAL_NIL;
        if (colon == i + 1) {
            if (auto_i >= n) { vm_runtime_error(v, "more {} than values given"); buf_free(&b); return VM_ERROR; }
            x = a[auto_i++];
        } else if (f->s[i + 1] >= '0' && f->s[i + 1] <= '9') {
            i64 k = nom_strtoll(f->s + i + 1, 0, 10);
            if (k < 0 || k >= n) { vm_runtime_error(v, "{%d} but only %d values", (int)k, n); buf_free(&b); return VM_ERROR; }
            x = a[k];
        } else {
            char nm[64];
            u32 l = colon - i - 1 < 63 ? colon - i - 1 : 63;
            nom_memcpy(nm, f->s + i + 1, l);
            nm[l] = 0;
            if (!kw_get(v, nm, &x)) { vm_runtime_error(v, "{%s} but no %s= given", nm, nm); buf_free(&b); return VM_ERROR; }
        }
        Value sv = str_new(colon < j ? f->s + colon + 1 : "", colon < j ? j - colon - 1 : 0), r;
        Value pa[2] = { x, sv };
        n_format(v, pa, 2, &r);
        buf_put(&b, AS_STR(r)->s, AS_STR(r)->len);
        val_release(r);
        val_release(sv);
        i = j;
    }
    *out = str_new(b.p ? b.p : "", b.len);
    buf_free(&b);
    return VM_OK;
}

/* the methods of a list, a dict, a string that aren't a native's name */
static VmStatus value_method(VM *v, Value obj, Str *name, Value *a, int n, Value *out, bool *found)
{
    const char *m = name->s;
    *found = true;
    *out = VAL_NIL;
    if (IS_TUPLE(obj) && nom_strcmp(m, "count") && nom_strcmp(m, "index")) { *found = false; return VM_OK; }
    if (IS_SET(obj)) {
        List *l = AS_LIST(obj);
        if ((!nom_strcmp(m, "add") || !nom_strcmp(m, "discard") || !nom_strcmp(m, "remove")) && n == 1) {
            for (u32 i = 0; i < l->len; i++)
                if (val_equal(l->v[i], a[0])) {
                    if (m[0] != 'a') { val_release(l->v[i]); for (u32 j = i; j + 1 < l->len; j++) l->v[j] = l->v[j + 1]; l->len--; }
                    return VM_OK;
                }
            if (m[0] == 'a') { list_push(l, val_retain(a[0])); return VM_OK; }
            if (m[0] == 'r') {
                Buf b; buf_init(&b); val_repr(&b, a[0]); buf_putc(&b, 0);
                vm_runtime_error(v, "KeyError: %s", b.p);
                buf_free(&b);
                return VM_ERROR;
            }
            return VM_OK;
        }
        if (!nom_strcmp(m, "pop") && n == 0) {
            if (!l->len) { vm_runtime_error(v, "KeyError: 'pop from an empty set'"); return VM_ERROR; }
            *out = l->v[0];
            for (u32 j = 0; j + 1 < l->len; j++) l->v[j] = l->v[j + 1];
            l->len--;
            return VM_OK;
        }
        if (!nom_strcmp(m, "clear") && n == 0) { for (u32 i = 0; i < l->len; i++) val_release(l->v[i]); l->len = 0; return VM_OK; }
        if (!nom_strcmp(m, "copy") && n == 0) { if (!items_of(v, obj, out)) return VM_ERROR; AS_LIST(*out)->tuple = 2; return VM_OK; }
        bool inplace = !nom_strcmp(m, "update") || !nom_strcmp(m, "intersection_update") || !nom_strcmp(m, "difference_update");
        int what = !nom_strcmp(m, "union") || !nom_strcmp(m, "update") ? 0 : !nom_strcmp(m, "intersection") || !nom_strcmp(m, "intersection_update") ? 1
                 : !nom_strcmp(m, "difference") || !nom_strcmp(m, "difference_update") ? 2 : !nom_strcmp(m, "symmetric_difference") ? 3
                 : !nom_strcmp(m, "issubset") ? 4 : !nom_strcmp(m, "issuperset") ? 5 : !nom_strcmp(m, "isdisjoint") ? 6 : -1;
        if (what >= 0) {
            /* s.union(a, b), s.issubset(t): against each one's items */
            Value r = list_new();
            AS_LIST(r)->tuple = 2;
            for (u32 i = 0; i < l->len; i++) list_push(AS_LIST(r), val_retain(l->v[i]));
            bool answer = true;
            for (int k = 0; k < n; k++) {
                Value o;
                if (!items_of(v, a[k], &o)) { val_release(r); return VM_ERROR; }
                List *ol = AS_LIST(o), *rl = AS_LIST(r);
                if (what == 0 || what == 3) {
                    for (u32 i = 0; i < ol->len; i++) {
                        bool have = false;
                        u32 at = 0;
                        for (u32 j = 0; j < rl->len && !have; j++) if (val_equal(rl->v[j], ol->v[i])) { have = true; at = j; }
                        if (!have) list_push(rl, val_retain(ol->v[i]));
                        else if (what == 3 && set_has(l, ol->v[i])) { val_release(rl->v[at]); for (u32 j = at; j + 1 < rl->len; j++) rl->v[j] = rl->v[j + 1]; rl->len--; }
                    }
                } else if (what == 1 || what == 2) {
                    u32 w = 0;
                    for (u32 i = 0; i < rl->len; i++) {
                        bool in = false;
                        for (u32 j = 0; j < ol->len && !in; j++) in = val_equal(rl->v[i], ol->v[j]);
                        if (in == (what == 1)) rl->v[w++] = rl->v[i]; else val_release(rl->v[i]);
                    }
                    rl->len = w;
                } else {
                    for (u32 i = 0; i < (what == 5 ? ol->len : l->len); i++) {
                        Value x = what == 5 ? ol->v[i] : l->v[i];
                        bool in = false;
                        List *in_l = what == 5 ? l : ol;
                        for (u32 j = 0; j < in_l->len && !in; j++) in = val_equal(x, in_l->v[j]);
                        if (what == 6 ? in : !in) answer = false;
                    }
                }
                val_release(o);
            }
            if (what >= 4) { val_release(r); *out = VAL_BOOL(answer); return VM_OK; }
            if (inplace) {
                for (u32 i = 0; i < l->len; i++) val_release(l->v[i]);
                l->len = 0;
                for (u32 i = 0; i < AS_LIST(r)->len; i++) list_push(l, val_retain(AS_LIST(r)->v[i]));
                val_release(r);
                return VM_OK;
            }
            *out = r;
            return VM_OK;
        }
        *found = false;
        return VM_OK;
    }
    if (IS_LIST(obj)) {
        List *l = AS_LIST(obj);
        if (!nom_strcmp(m, "insert") && n == 2) {
            i64 at = val_int(a[0]);
            if (at < 0) at += l->len;
            if (at < 0) at = 0;
            if (at > (i64)l->len) at = l->len;
            list_push(l, VAL_NIL);
            for (i64 i = (i64)l->len - 1; i > at; i--) l->v[i] = l->v[i - 1];
            l->v[at] = val_retain(a[1]);
            return VM_OK;
        }
        if ((!nom_strcmp(m, "remove") || !nom_strcmp(m, "index") || !nom_strcmp(m, "count")) && n == 1) {
            i64 cnt = 0;
            for (u32 i = 0; i < l->len; i++) {
                if (!val_equal(l->v[i], a[0])) continue;
                if (!nom_strcmp(m, "count")) { cnt++; continue; }
                if (!nom_strcmp(m, "index")) { *out = VAL_INT((i64)i); return VM_OK; }
                val_release(l->v[i]);
                for (u32 j = i; j + 1 < l->len; j++) l->v[j] = l->v[j + 1];
                l->len--;
                return VM_OK;
            }
            if (!nom_strcmp(m, "count")) { *out = VAL_INT(cnt); return VM_OK; }
            Buf b; buf_init(&b); val_repr(&b, a[0]); buf_putc(&b, 0);
            vm_runtime_error(v, "ValueError: %s isn't in the list", b.p);
            buf_free(&b);
            return VM_ERROR;
        }
        if (!nom_strcmp(m, "pop")) {
            if (!l->len) { vm_runtime_error(v, "pop from an empty list"); return VM_ERROR; }
            i64 at = n ? val_int(a[0]) : (i64)l->len - 1;
            if (at < 0) at += l->len;
            if (at < 0 || at >= (i64)l->len) { vm_runtime_error(v, "pop index out of range"); return VM_ERROR; }
            *out = l->v[at];
            for (u32 j = (u32)at; j + 1 < l->len; j++) l->v[j] = l->v[j + 1];
            l->len--;
            return VM_OK;
        }
        if (!nom_strcmp(m, "extend") && n == 1) {
            Value it;
            if (!items_of(v, a[0], &it)) return VM_ERROR;
            for (u32 i = 0; i < AS_LIST(it)->len; i++) list_push(l, val_retain(AS_LIST(it)->v[i]));
            val_release(it);
            return VM_OK;
        }
        if (!nom_strcmp(m, "sort") && n == 0) {
            Value key = VAL_NIL, rev = VAL_NIL;
            kw_get(v, "key", &key);
            kw_get(v, "reverse", &rev);
            return sort_list(v, l, key, val_truthy(rev)) ? VM_OK : VM_ERROR;
        }
        if (!nom_strcmp(m, "reverse") && n == 0) {
            for (u32 i = 0; i < l->len / 2; i++) { Value t = l->v[i]; l->v[i] = l->v[l->len - 1 - i]; l->v[l->len - 1 - i] = t; }
            return VM_OK;
        }
        if (!nom_strcmp(m, "copy") && n == 0) return items_of(v, obj, out) ? VM_OK : VM_ERROR;
        /* a set's (a list with no repeats, here) */
        if ((!nom_strcmp(m, "add") || !nom_strcmp(m, "discard")) && n == 1) {
            for (u32 i = 0; i < l->len; i++)
                if (val_equal(l->v[i], a[0])) {
                    if (m[0] == 'd') { val_release(l->v[i]); for (u32 j = i; j + 1 < l->len; j++) l->v[j] = l->v[j + 1]; l->len--; }
                    return VM_OK;
                }
            if (m[0] == 'a') list_push(l, val_retain(a[0]));
            return VM_OK;
        }
        if (!nom_strcmp(m, "clear") && n == 0) { for (u32 i = 0; i < l->len; i++) val_release(l->v[i]); l->len = 0; return VM_OK; }
    }
    if (IS_DICT(obj)) {
        Dict *d = AS_DICT(obj);
        if ((!nom_strcmp(m, "items") || !nom_strcmp(m, "values") || !nom_strcmp(m, "keys")) && n == 0) {
            Value r = list_new();
            for (u32 i = 0; i < d->len; i++) {
                if (m[0] == 'k') list_push(AS_LIST(r), val_retain(d->k[i]));
                else if (m[0] == 'v') list_push(AS_LIST(r), val_retain(d->v[i]));
                else { Value t = list_new(); list_push(AS_LIST(t), val_retain(d->k[i])); list_push(AS_LIST(t), val_retain(d->v[i])); AS_LIST(t)->tuple = 1; list_push(AS_LIST(r), t); }
            }
            *out = r;
            return VM_OK;
        }
        if (!nom_strcmp(m, "pop") && (n == 1 || n == 2)) {
            for (u32 i = 0; i < d->len; i++) {
                if (!val_equal(d->k[i], a[0])) continue;
                *out = d->v[i];
                val_release(d->k[i]);
                for (u32 j = i; j + 1 < d->len; j++) { d->k[j] = d->k[j + 1]; d->v[j] = d->v[j + 1]; }
                d->len--;
                return VM_OK;
            }
            if (n == 2) { *out = val_retain(a[1]); return VM_OK; }
            vm_runtime_error(v, "no such key to pop");
            return VM_ERROR;
        }
        if (!nom_strcmp(m, "setdefault") && n == 2) {
            Value got;
            if (dict_get(d, a[0], &got)) { *out = val_retain(got); return VM_OK; }
            dict_set(d, val_retain(a[0]), val_retain(a[1]));
            *out = val_retain(a[1]);
            return VM_OK;
        }
        if (!nom_strcmp(m, "copy") && n == 0) {
            Value r = dict_new();
            for (u32 i = 0; i < d->len; i++) dict_set(AS_DICT(r), val_retain(d->k[i]), val_retain(d->v[i]));
            *out = r;
            return VM_OK;
        }
        if (!nom_strcmp(m, "setdefault") && n == 1) {
            Value got;
            if (dict_get(d, a[0], &got)) { *out = val_retain(got); return VM_OK; }
            dict_set(d, val_retain(a[0]), VAL_NIL);
            return VM_OK;
        }
        if (!nom_strcmp(m, "clear") && n == 0) {
            for (u32 i = 0; i < d->len; i++) { val_release(d->k[i]); val_release(d->v[i]); }
            d->len = 0;
            return VM_OK;
        }
        if (!nom_strcmp(m, "popitem") && n == 0) {
            if (!d->len) { vm_runtime_error(v, "KeyError: 'popitem(): dictionary is empty'"); return VM_ERROR; }
            Value t = list_new();
            list_push(AS_LIST(t), d->k[d->len - 1]);
            list_push(AS_LIST(t), d->v[d->len - 1]);
            AS_LIST(t)->tuple = 1;
            d->len--;
            *out = t;
            return VM_OK;
        }
        if (!nom_strcmp(m, "update") && n <= 1) {
            /* d.update(other), d.update(a=1), d.update([("k", v)]) */
            if (n == 1 && !IS_DICT(a[0])) {
                Value l;
                if (!items_of(v, a[0], &l)) return VM_ERROR;
                for (u32 i = 0; i < AS_LIST(l)->len; i++) {
                    Value pr = AS_LIST(l)->v[i];
                    if (!IS_LIST(pr) || AS_LIST(pr)->len != 2) { val_release(l); vm_runtime_error(v, "TypeError: update() of a list wants pairs: [(key, value), ...]"); return VM_ERROR; }
                    dict_set(d, val_retain(AS_LIST(pr)->v[0]), val_retain(AS_LIST(pr)->v[1]));
                }
                val_release(l);
            } else if (n == 1)
                for (u32 i = 0; i < AS_DICT(a[0])->len; i++) dict_set(d, val_retain(AS_DICT(a[0])->k[i]), val_retain(AS_DICT(a[0])->v[i]));
            if (v->kw.k != V_NIL) {
                v->kw_used = true;
                for (u32 i = 0; i < AS_DICT(v->kw)->len; i++) dict_set(d, val_retain(AS_DICT(v->kw)->k[i]), val_retain(AS_DICT(v->kw)->v[i]));
            }
            return VM_OK;
        }
        if (!nom_strcmp(m, "get") && (n == 1 || n == 2)) {
            Value got;
            *out = dict_get(d, a[0], &got) ? val_retain(got) : (n == 2 ? val_retain(a[1]) : VAL_NIL);
            return VM_OK;
        }
        /* math.sqrt(16), a module's things: call what's under the key */
        Value fnv, key = str_new(name->s, name->len);
        bool has = dict_get(d, key, &fnv);
        val_release(key);
        if (has && fnv.k == V_OBJ && (fnv.as.o->kind == O_NATIVE || fnv.as.o->kind == O_FUNC))
            return vm_call(v, fnv, a, n, out) ? VM_OK : VM_ERROR;
    }
    if (obj.k == V_NUM && !nom_strcmp(m, "is_integer") && n == 0) { *out = VAL_BOOL(obj.as.d == nom_floor(obj.as.d)); return VM_OK; }
    if ((obj.k == V_INT || obj.k == V_BOOL) && !nom_strcmp(m, "bit_length") && n == 0) {
        i64 x = val_int(obj), k = 0;
        u64 u = x < 0 ? (u64)(-(x + 1)) + 1 : (u64)x;
        while (u) { k++; u >>= 1; }
        *out = VAL_INT(k);
        return VM_OK;
    }
    if (IS_STR(obj)) {
        Str *s = AS_STR(obj);
        if (!nom_strcmp(m, "format")) return str_format(v, s, a, n, out);
        if ((!nom_strcmp(m, "strip") || !nom_strcmp(m, "lstrip") || !nom_strcmp(m, "rstrip")) && n <= 1) {
            /* s.strip(), s.strip("xy"): those letters off the ends */
            const char *cs = n && IS_STR(a[0]) ? AS_STR(a[0])->s : " \t\n\r\f\v";
            u32 i = 0, j = s->len;
            #define STRIPPABLE(c) (n && IS_STR(a[0]) ? (nom_memchr_(cs, c)) : is_space(c))
            if (m[0] != 'r') while (i < j && STRIPPABLE(s->s[i])) i++;
            if (m[0] != 'l') while (j > i && STRIPPABLE(s->s[j - 1])) j--;
            #undef STRIPPABLE
            *out = str_new(s->s + i, j - i);
            return VM_OK;
        }
        if ((!nom_strcmp(m, "split") || !nom_strcmp(m, "rsplit")) && n <= 2) {
            /* s.split(sep, maxsplit), s.rsplit(...), sep None: white space */
            Value sepv = n > 0 ? a[0] : VAL_NIL, maxv = n > 1 ? a[1] : VAL_INT(-1);
            kw_get(v, "sep", &sepv);
            kw_get(v, "maxsplit", &maxv);
            i64 most = val_int(maxv);
            bool rev = m[0] == 'r';
            if (IS_STR(sepv) && !AS_STR(sepv)->len) { vm_runtime_error(v, "ValueError: empty separator"); return VM_ERROR; }
            Value l = list_new();
            List *o = AS_LIST(l);
            u32 len = s->len;
            if (!IS_STR(sepv)) {
                i64 cuts = 0;
                if (!rev) {
                    u32 i = 0;
                    while (i < len) {
                        while (i < len && is_space(s->s[i])) i++;
                        if (i >= len) break;
                        if (most >= 0 && cuts >= most) { u32 e = len; while (e > i && is_space(s->s[e - 1])) e--; list_push(o, str_new(s->s + i, e - i)); break; }
                        u32 st = i;
                        while (i < len && !is_space(s->s[i])) i++;
                        list_push(o, str_new(s->s + st, i - st));
                        cuts++;
                    }
                } else {
                    i64 j = len;
                    while (j > 0) {
                        while (j > 0 && is_space(s->s[j - 1])) j--;
                        if (j <= 0) break;
                        if (most >= 0 && cuts >= most) { u32 b0 = 0; while ((i64)b0 < j && is_space(s->s[b0])) b0++; list_push(o, str_new(s->s + b0, (size_t)(j - b0))); break; }
                        i64 e = j;
                        while (j > 0 && !is_space(s->s[j - 1])) j--;
                        list_push(o, str_new(s->s + j, (size_t)(e - j)));
                        cuts++;
                    }
                    for (u32 i = 0; i < o->len / 2; i++) { Value t = o->v[i]; o->v[i] = o->v[o->len - 1 - i]; o->v[o->len - 1 - i] = t; }
                }
            } else {
                Str *sp = AS_STR(sepv);
                i64 cuts = 0;
                if (!rev) {
                    u32 start = 0;
                    for (u32 i = 0; i + sp->len <= len && (most < 0 || cuts < most); ) {
                        if (!nom_memcmp(s->s + i, sp->s, sp->len)) { list_push(o, str_new(s->s + start, i - start)); i += sp->len; start = i; cuts++; }
                        else i++;
                    }
                    list_push(o, str_new(s->s + start, len - start));
                } else {
                    i64 end = len;
                    for (i64 i = (i64)len - sp->len; i >= 0 && (most < 0 || cuts < most); ) {
                        if (!nom_memcmp(s->s + i, sp->s, sp->len)) { list_push(o, str_new(s->s + i + sp->len, (size_t)(end - i - sp->len))); end = i; i -= sp->len; cuts++; }
                        else i--;
                    }
                    list_push(o, str_new(s->s, (size_t)end));
                    for (u32 i = 0; i < o->len / 2; i++) { Value t = o->v[i]; o->v[i] = o->v[o->len - 1 - i]; o->v[o->len - 1 - i] = t; }
                }
            }
            *out = l;
            return VM_OK;
        }
        if (!nom_strcmp(m, "splitlines") && n == 0) {
            Value l = list_new();
            u32 st = 0;
            for (u32 i = 0; i < s->len; i++)
                if (s->s[i] == '\n' || s->s[i] == '\r') {
                    list_push(AS_LIST(l), str_new(s->s + st, i - st));
                    if (s->s[i] == '\r' && i + 1 < s->len && s->s[i + 1] == '\n') i++;
                    st = i + 1;
                }
            if (st < s->len) list_push(AS_LIST(l), str_new(s->s + st, s->len - st));
            *out = l;
            return VM_OK;
        }
        if ((!nom_strcmp(m, "partition") || !nom_strcmp(m, "rpartition")) && n == 1 && IS_STR(a[0])) {
            Str *sp = AS_STR(a[0]);
            if (!sp->len) { vm_runtime_error(v, "ValueError: empty separator"); return VM_ERROR; }
            i64 at = -1;
            if (m[0] == 'p') { for (u32 i = 0; i + sp->len <= s->len && at < 0; i++) if (!nom_memcmp(s->s + i, sp->s, sp->len)) at = i; }
            else for (i64 i = (i64)s->len - sp->len; i >= 0 && at < 0; i--) if (!nom_memcmp(s->s + i, sp->s, sp->len)) at = i;
            Value t = list_new();
            List *o = AS_LIST(t);
            if (at < 0) {
                if (m[0] == 'p') { list_push(o, val_retain(obj)); list_push(o, str_newz("")); list_push(o, str_newz("")); }
                else { list_push(o, str_newz("")); list_push(o, str_newz("")); list_push(o, val_retain(obj)); }
            } else {
                list_push(o, str_new(s->s, (size_t)at));
                list_push(o, val_retain(a[0]));
                list_push(o, str_new(s->s + at + sp->len, (size_t)(s->len - at - sp->len)));
            }
            o->tuple = 1;
            *out = t;
            return VM_OK;
        }
        if ((!nom_strcmp(m, "find") || !nom_strcmp(m, "rfind") || !nom_strcmp(m, "index") || !nom_strcmp(m, "rindex")) && n >= 1 && n <= 3 && IS_STR(a[0])) {
            Str *f = AS_STR(a[0]);
            i64 lo = n > 1 ? val_int(a[1]) : 0, hi = n > 2 ? val_int(a[2]) : (i64)s->len;
            if (lo < 0) lo += s->len;
            if (hi < 0) hi += s->len;
            if (lo < 0) lo = 0;
            if (hi > (i64)s->len) hi = s->len;
            i64 at = -1;
            if (m[0] != 'r') { for (i64 i = lo; i + f->len <= hi && at < 0; i++) if (!nom_memcmp(s->s + i, f->s, f->len)) at = i; }
            else for (i64 i = hi - f->len; i >= lo && at < 0; i--) if (!nom_memcmp(s->s + i, f->s, f->len)) at = i;
            if (at < 0 && (m[0] == 'i' || m[1] == 'i')) { vm_runtime_error(v, "ValueError: substring not found"); return VM_ERROR; }
            *out = VAL_INT(at);
            return VM_OK;
        }
        if ((!nom_strcmp(m, "startswith") || !nom_strcmp(m, "endswith")) && n == 1 && IS_LIST(a[0])) {
            /* s.startswith(("a", "b")): any of them */
            bool any = false;
            for (u32 i = 0; i < AS_LIST(a[0])->len && !any; i++) {
                Value pa[2] = { obj, AS_LIST(a[0])->v[i] }, r;
                n_affix(v, pa, 2, &r, m[0] == 'e');
                any = val_truthy(r);
            }
            *out = VAL_BOOL(any);
            return VM_OK;
        }
        if ((!nom_strcmp(m, "removeprefix") || !nom_strcmp(m, "removesuffix")) && n == 1 && IS_STR(a[0])) {
            Str *f = AS_STR(a[0]);
            bool pre = m[6] == 'p';
            if (f->len <= s->len && !nom_memcmp(s->s + (pre ? 0 : s->len - f->len), f->s, f->len))
                *out = str_new(s->s + (pre ? f->len : 0), s->len - f->len);
            else *out = val_retain(obj);
            return VM_OK;
        }
        if (!nom_strcmp(m, "replace") && n == 3 && IS_STR(a[0]) && IS_STR(a[1])) {
            Str *f = AS_STR(a[0]), *t = AS_STR(a[1]);
            i64 most = val_int(a[2]), done = 0;
            Buf b; buf_init(&b);
            for (u32 i = 0; i < s->len; ) {
                if (f->len && (most < 0 || done < most) && i + f->len <= s->len && !nom_memcmp(s->s + i, f->s, f->len)) { buf_put(&b, t->s, t->len); i += f->len; done++; }
                else buf_putc(&b, s->s[i++]);
            }
            *out = str_new(b.p ? b.p : "", b.len);
            buf_free(&b);
            return VM_OK;
        }
        if ((!nom_strcmp(m, "isalpha") || !nom_strcmp(m, "isalnum") || !nom_strcmp(m, "isspace") || !nom_strcmp(m, "isupper") ||
             !nom_strcmp(m, "islower") || !nom_strcmp(m, "isnumeric") || !nom_strcmp(m, "isdecimal")) && n == 0) {
            bool ok = s->len > 0, cased = false;
            for (u32 i = 0; i < s->len; i++) {
                char c = s->s[i];
                bool up = c >= 'A' && c <= 'Z', lo = c >= 'a' && c <= 'z', dg = c >= '0' && c <= '9';
                cased |= up || lo;
                switch (m[2]) {
                case 'a': ok &= m[3] == 'p' ? up || lo : up || lo || dg; break;      /* isalpha, isalnum */
                case 's': ok &= is_space(c); break;
                case 'u': ok &= !lo; break;
                case 'l': ok &= !up; break;
                default:  ok &= dg; break;                                       /* isnumeric, isdecimal */
                }
            }
            if (m[2] == 'u' || m[2] == 'l') ok = ok && cased;
            *out = VAL_BOOL(ok);
            return VM_OK;
        }
        if ((!nom_strcmp(m, "swapcase") || !nom_strcmp(m, "casefold")) && n == 0) {
            Buf b; buf_init(&b);
            for (u32 i = 0; i < s->len; i++) {
                char c = s->s[i];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                else if (c >= 'a' && c <= 'z' && m[0] == 's') c = (char)(c - 32);
                buf_putc(&b, c);
            }
            *out = str_new(b.p ? b.p : "", b.len);
            buf_free(&b);
            return VM_OK;
        }
        if (!nom_strcmp(m, "count") && n == 1 && IS_STR(a[0])) {
            Str *f = AS_STR(a[0]);
            i64 c = 0;
            for (u32 i = 0; f->len && i + f->len <= s->len; ) { if (!nom_memcmp(s->s + i, f->s, f->len)) { c++; i += f->len; } else i++; }
            *out = VAL_INT(c);
            return VM_OK;
        }
        if (!nom_strcmp(m, "index") && n == 1 && IS_STR(a[0])) {
            Value r;
            Value pa[2] = { obj, a[0] };
            n_find(v, pa, 2, &r);
            if (r.as.i < 0) { vm_runtime_error(v, "substring not found"); return VM_ERROR; }
            *out = r;
            return VM_OK;
        }
        if (!nom_strcmp(m, "isdigit") && n == 0) {
            bool d = s->len > 0;
            for (u32 i = 0; i < s->len; i++) d &= s->s[i] >= '0' && s->s[i] <= '9';
            *out = VAL_BOOL(d);
            return VM_OK;
        }
        if ((!nom_strcmp(m, "rjust") || !nom_strcmp(m, "ljust") || !nom_strcmp(m, "center") || !nom_strcmp(m, "zfill")) && n >= 1) {
            i64 w = val_int(a[0]);
            char fill = m[0] == 'z' ? '0' : (n > 1 && IS_STR(a[1]) && AS_STR(a[1])->len) ? AS_STR(a[1])->s[0] : ' ';
            i64 pad = w - (i64)s->len;
            Buf b; buf_init(&b);
            i64 left = pad <= 0 ? 0 : (m[0] == 'r' || m[0] == 'z') ? pad : m[0] == 'c' ? pad / 2 : 0;
            i64 right = pad <= 0 ? 0 : pad - left;
            for (i64 i = 0; i < left; i++) buf_putc(&b, fill);
            buf_put(&b, s->s, s->len);
            for (i64 i = 0; i < right; i++) buf_putc(&b, fill);
            *out = str_new(b.p ? b.p : "", b.len);
            buf_free(&b);
            return VM_OK;
        }
        if (!nom_strcmp(m, "title") || !nom_strcmp(m, "capitalize")) {
            Buf b; buf_init(&b);
            bool start = true;
            for (u32 i = 0; i < s->len; i++) {
                char c = s->s[i];
                bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                if (letter && start && c >= 'a') c = (char)(c - 32);
                else if (letter && !start && c <= 'Z' && (m[0] == 't' || i > 0)) c = (char)(c + 32);
                buf_putc(&b, c);
                start = m[0] == 't' ? !letter : false;
            }
            *out = str_new(b.p ? b.p : "", b.len);
            buf_free(&b);
            return VM_OK;
        }
    }
    *found = false;
    return VM_OK;
}

/* countdown(5): T-5 ... T-1, a second each, then liftoff */
static VmStatus n_countdown(VM *v, Value *a, int n, Value *out)
{
    i64 k = n ? val_int(a[0]) : 5;
    for (i64 i = k; i > 0; i--) {
        char line[40];
        nom_snprintf(line, sizeof line, "say T-%d", (int)i);
        ask(v, line);
        g_puts(line + 4); g_puts("\n");
        Value one = VAL_INT(1);
        if (n_wait(v, &one, 1, out) != VM_OK) return VM_ERROR;
    }
    ask(v, "say Liftoff!");
    g_puts("Liftoff!\n");
    *out = VAL_NIL;
    return VM_OK;
}

static VmStatus n_echo(VM *v, Value *a, int n, Value *out);

static const Native NATIVES[] = {
    { "print", 0, 255, n_print },
    { "do",    1, 1, n_do    },
    { "api",   1, 1, n_do    },   /* the old name, still answered */
    { "ship",  0, 0, n_ship  },
    { "len",   1, 1, n_len   },
    { "str",   0, 1, n_str   },
    { "int",   0, 2, n_int   },
    { "split", 1, 2, n_split },
    { "find",  2, 2, n_find  },
    { "json",  1, 1, n_json  },
    { "lines", 1, 1, n_lines },
    { "append", 2, 2, n_append },
    { "keys",   1, 1, n_keys   },
    { "has",    2, 2, n_has    },
    { "join",   1, 2, n_join   },
    { "strip",  1, 1, n_strip  },
    { "replace",2, 3, n_replace},
    { "read",   1, 1, n_read   },
    { "write",  2, 2, n_write  },
    { "lower", 1, 1, n_lower },
    { "upper", 1, 1, n_upper },
    { "sub",   2, 3, n_sub   },
    { "exit",  0, 1, n_exit  },
    /* TUX ROCKET SCIENCE: numbers */
    { "float", 0, 1, n_float },
    { "abs",   1, 1, n_abs   },
    { "min",   1, 16, n_min   },
    { "max",   1, 16, n_max   },
    { "floor", 1, 1, n_floor },
    { "ceil",  1, 1, n_ceil  },
    { "round", 1, 2, n_round },
    { "sqrt",  1, 1, n_sqrt  },
    { "sin",   1, 1, n_sin   },
    { "cos",   1, 1, n_cos   },
    { "tan",   1, 1, n_tan   },
    { "asin",  1, 1, n_asin  },
    { "acos",  1, 1, n_acos  },
    { "atan",  1, 1, n_atan  },
    { "atan2", 2, 2, n_atan2 },
    { "exp",   1, 1, n_exp   },
    { "log",   1, 1, n_log   },
    { "rad",   1, 1, n_rad   },
    { "deg",   1, 1, n_deg   },
    /* the ship: readings and waiting */
    { "get",        1, 1, n_get        },
    { "wait",       1, 1, n_wait       },
    { "wait_until", 3, 3, n_wait_until },
    /* the ship's programs: each waits until it is done */
    { "launch",       0, 1, n_launch      },
    { "circularize",  0, 1, n_circularize },
    { "burn",         1, 2, n_burn_dv     },
    { "burn_until",   4, 4, n_burn_until  },
    { "until_orbit_crosses", 1, 1, n_until_crosses },
    { "transfer",     1, 2, n_transfer    },
    { "wait_for_window", 1, 1, n_window   },
    { "warp_to",      1, 1, n_warp_to     },
    { "capture",      0, 1, n_capture     },
    { "land",         0, 2, n_land        },
    { "hover",        1, 2, n_hover       },
    { "node",         0, 0, n_node        },
    { "drive_to",     1, 2, n_drive_to    },
    /* and the quick ones */
    { "throttle",  1, 1, n_throttle_pct },
    { "stage",     0, 0, n_stage     },
    { "sas",       1, 1, n_sas       },
    { "point",     2, 2, n_point     },
    { "chutes",    0, 0, n_chutes    },
    { "legs",      0, 1, n_legs      },
    { "mining",    0, 1, n_mining    },
    { "rcs",       1, 1, n_rcs       },
    { "say",       1, 1, n_say       },
    { "beep",      0, 1, n_beep      },
    { "abort",     0, 0, n_abort     },
    { "autostage", 1, 1, n_autostage },
    { "target",    1, 1, n_target    },
    { "countdown", 0, 1, n_countdown },
    /* the dot, and f-strings */
    { "_ship",     0, 0, n_the_ship  },
    { "format",    1, 2, n_format    },
    { "clamp",     3, 3, n_clamp     },
    { "startswith", 2, 2, n_startswith },
    { "endswith",  2, 2, n_endswith  },
    /* aiming, burning, the maths of an orbit */
    { "sleep",     1, 1, n_wait      },
    { "aim",       1, 1, n_aim       },
    { "aim_at",    2, 2, n_aim_at    },
    { "aim_along", 3, 3, n_aim_along },
    { "speed_needed", 1, 1, n_speed_needed },
    { "speed_at",  1, 1, n_speed_at  },
    { "burn_time", 1, 1, n_burn_time },
    { "time_to",   1, 1, n_time_to   },
    { "stopping_distance", 0, 0, n_stopping },
    { "when_to_leave", 1, 2, n_when_to_leave },
    { "what_if",   4, 4, n_what_if   },
    { "chart",     2, 2, n_chart     },
    { "wait_user_trigger", 0, 0, n_wait_space },
    { "wait_for_space", 0, 0, n_wait_space },
    { "_echo",     1, 1, n_echo      },
    /* and the rest of Python's builtins */
    { "range",     1, 3, n_range     },
    { "list",      0, 1, n_list      },
    { "tuple",     0, 1, n_tuple     },
    { "dict",      0, 1, n_dict      },
    { "sum",       1, 2, n_sum       },
    { "sorted",    1, 1, n_sorted    },
    { "reversed",  1, 1, n_reversed  },
    { "any",       1, 1, n_any       },
    { "all",       1, 1, n_all       },
    { "enumerate", 1, 2, n_enumerate },
    { "zip",       0, 16, n_zip      },
    { "map",       2, 16, n_map      },
    { "filter",    2, 2, n_filter    },
    { "bool",      0, 1, n_bool      },
    { "repr",      1, 1, n_repr      },
    { "dir",       0, 1, n_dir       },
    { "help",      0, 1, n_help      },
    { "type",      1, 1, n_type      },
    { "isinstance", 2, 2, n_isinstance },
    { "_exc_init", 1, 16, n_exc_init },
    { "_exc_str",  1, 1, n_exc_str  },
    { "_exc_repr", 1, 1, n_exc_repr },
    { "_super",    2, 2, n_super    },
    { "staticmethod", 1, 1, n_staticmethod },
    { "classmethod", 1, 1, n_classmethod },
    { "property",  1, 1, n_property },
    { "next",      1, 2, n_next     },
    { "iter",      1, 1, n_iter     },
    { "ord",       1, 1, n_ord      },
    { "chr",       1, 1, n_chr      },
    { "hex",       1, 1, n_hex      },
    { "bin",       1, 1, n_bin      },
    { "oct",       1, 1, n_oct      },
    { "hash",      1, 1, n_hash     },
    { "id",        1, 1, n_id       },
    { "callable",  1, 1, n_callable },
    { "getattr",   2, 3, n_getattr  },
    { "hasattr",   2, 2, n_hasattr  },
    { "setattr",   3, 3, n_setattr  },
    { "open",      1, 2, n_open     },
    { "frozenset", 0, 1, n_set      },
    { "_f_write",  2, 2, n_f_write  },
    { "_f_read",   1, 2, n_f_read   },
    { "_f_readline", 1, 1, n_f_readline },
    { "_f_readlines", 1, 1, n_f_readlines },
    { "_f_close",  1, 1, n_f_close  },
    { "_f_flush",  1, 1, n_f_flush  },
    { "_f_enter",  1, 1, n_f_enter  },
    { "_f_exit",   1, 4, n_f_exit   },
    { "_f_repr",   1, 1, n_f_repr   },
    { "_fabs",     1, 1, n_fabs     },
    { "_isclose",  2, 2, n_isclose  },
    { "_copysign", 2, 2, n_copysign },
    { "_trunc",    1, 1, n_trunc    },
    { "_isfinite", 1, 1, n_isfinite },
    { "_isinf",    1, 1, n_isinf    },
    { "_isnan",    1, 1, n_isnan    },
    { "_factorial", 1, 1, n_factorial },
    { "_gcd",      0, 16, n_gcd     },
    { "_dist",     2, 2, n_dist     },
    { "_prod",     1, 1, n_prod     },
    { "_fsum",     1, 1, n_fsum     },
    { "_log2",     1, 1, n_log2b    },
    { "_sinh",     1, 1, n_sinh     },
    { "_cosh",     1, 1, n_cosh     },
    { "_tanh",     1, 1, n_tanh     },
    { "_fmod",     2, 2, n_fmod     },
    { "_modf",     1, 1, n_modf     },
    { "_exp2",     1, 1, n_exp2     },
    { "_cbrt",     1, 1, n_cbrt     },
    { "divmod",    2, 2, n_divmod    },
    { "pow",       2, 2, n_pyp       },
    { "set",       0, 1, n_set       },
    { "hypot",     1, 8, n_hypot     },
    { "log10",     1, 1, n_log10     },
    { "_log",      1, 2, n_log2      },
    { "_pow",      2, 2, n_pow       },
    { "_now",      0, 0, n_now       },
    { "_random",   0, 0, n_random    },
    { "_uniform",  2, 2, n_uniform   },
    { "_randint",  2, 2, n_randint   },
    { "_choice",   1, 1, n_choice    },
    { "_seed",     1, 1, n_seed      },
    { "_module",   1, 1, n_module    },
};

const Native *native_table(int *count)
{
    *count = (int)(sizeof NATIVES / sizeof NATIVES[0]);
    return NATIVES;
}

int native_find(const char *name, int len)
{
    int n = (int)(sizeof NATIVES / sizeof NATIVES[0]);
    for (int i = 0; i < n; i++) {
        if ((int)nom_strlen(NATIVES[i].name) != len) continue;
        if (nom_strncmp(NATIVES[i].name, name, (size_t)len) == 0) return i;
    }
    return -1;
}


static void die(const char *what, const char *detail);
static void traceback(VM *vm, int pl);

/* TUX ROCKET SCIENCE: the constants, as lines ahead of every script (a
 * syntax error's line number is told back without them) */
static const char PRELUDE[] = "pi = 3.141592653589793\nkm = 1000\nship = _ship()\n";
#define PRELUDE_LINES 3

/* ============================================================ TUX ROCKET SCIENCE
 * THE DEBUGGER. A script running in the background says what it is doing
 * in /run/py/<pid>.vars -- the line it is on, and every variable it has,
 * as print would show it -- whenever it waits for the ship and every so
 * many instructions between: the Script Editor's watch, live. And it does
 * what /run/py/<pid>.ctl says, which the editor writes whole each time:
 *
 *     break 12 30        stop before these lines run
 *     mode run           or pause: done when it changes (it stops where it
 *                        is, a wait for the ship included)
 *     go 3               paused, a new number: go on (Continue)
 *     step 7             paused, a new number: run one line, stop again
 */
static VM *g_vm;
static int g_prelude_lines;
static char g_script[128] = "";
static char g_vars[48], g_ctl[48];
static int g_bp[64], g_nbp;
static bool g_paused;
static int g_step_seen = -1, g_step_from = -1, g_go_seen = -1, g_mode_seen = -1;
static int g_int_seen = -1;
static bool g_interrupt;
static char g_state[96] = "running";

static void dbg_publish(VM *v, const char *state)
{
    if (!g_bg || !g_vars[0] || !v) return;
    Buf b; buf_init(&b);
    buf_printf(&b, "script %s\nline %d\nstate %s\n", g_script, v->line - g_prelude_lines, state);
    Prog *pr = v->prog;
    for (int g = NBUILTIN_GLOBALS; g < pr->nglobals; g++) {
        if (!v->gdef[g]) continue;
        const char *nm = pr->global[g];
        if (!nom_strcmp(nm, "pi") || !nom_strcmp(nm, "km") || !nom_strcmp(nm, "ship") || nm[0] == '$') continue;
        Value x = v->global[g];
        if (x.k == V_OBJ && (x.as.o->kind == O_FUNC || x.as.o->kind == O_NATIVE || x.as.o->kind == O_CLOSURE || x.as.o->kind == O_CLASS)) continue;
        Buf r; buf_init(&r);
        val_repr(&r, x);
        buf_printf(&b, "%s\t", nm);
        buf_put(&b, r.p ? r.p : "", r.len > 160 ? 160 : r.len);
        if (r.len > 160) buf_puts(&b, "...");
        buf_putc(&b, '\n');
        buf_free(&r);
    }
    /* the function it is in, and its locals */
    for (int k = v->nframes - 1; k >= 1; k--) {
        struct Frame *fr = &v->frame[k];
        const char *ln = fr->fn->locals ? fr->fn->locals : "";
        int i = 0;
        while (*ln) {
            const char *e = ln;
            while (*e && *e != ',') e++;
            if (fr->base + i < v->sp && *ln != '$') {
                Buf r; buf_init(&r);
                val_repr(&r, v->stack[fr->base + i]);
                buf_printf(&b, "%s.", fr->fn->name);
                buf_put(&b, ln, (size_t)(e - ln));
                buf_putc(&b, '\t');
                buf_put(&b, r.p ? r.p : "", r.len > 160 ? 160 : r.len);
                buf_putc(&b, '\n');
                buf_free(&r);
            }
            i++;
            ln = *e ? e + 1 : e;
        }
        break;   /* (the innermost only) */
    }
    int fd = g_open(g_vars, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) { g_write(fd, b.p ? b.p : "", b.len); g_close(fd); }
    buf_free(&b);
}

static char g_ctlbuf[1024];
static void dbg_read_ctl(void)
{
    if (!g_bg || !g_ctl[0]) return;
    i64 n = g_slurp(g_ctl, g_ctlbuf, sizeof g_ctlbuf - 1);
    if (n < 0) return;
    g_ctlbuf[n] = 0;
    g_nbp = 0;
    for (const char *q = g_ctlbuf; *q; ) {
        if (!nom_strncmp(q, "break", 5)) {
            q += 5;
            while (*q && *q != '\n') {
                while (*q == ' ') q++;
                int ln = 0, any = 0;
                while (*q >= '0' && *q <= '9') { ln = ln * 10 + (*q++ - '0'); any = 1; }
                if (any && g_nbp < 64) g_bp[g_nbp++] = ln;
                if (!any && *q && *q != '\n') q++;
            }
        } else if (!nom_strncmp(q, "mode ", 5)) {
            int m = !nom_strncmp(q + 5, "pause", 5);
            if (m != g_mode_seen && g_mode_seen >= 0) {
                if (m && !g_paused && g_step_from < 0) { g_paused = true; nom_snprintf(g_state, sizeof g_state, "paused"); }
                if (!m && g_paused) { g_paused = false; g_step_from = -1; }
            }
            g_mode_seen = m;
        } else if (!nom_strncmp(q, "interrupt ", 10)) {
            /* Ctrl+C: what is running stops, with KeyboardInterrupt */
            int k = (int)nom_strtoll(q + 10, 0, 10);
            if (g_int_seen >= 0 && k != g_int_seen) { g_interrupt = true; g_paused = false; }
            g_int_seen = k;
        } else if (!nom_strncmp(q, "go ", 3)) {
            int k = (int)nom_strtoll(q + 3, 0, 10);
            if (g_go_seen >= 0 && k != g_go_seen && g_paused) { g_paused = false; g_step_from = -1; }
            g_go_seen = k;
        } else if (!nom_strncmp(q, "step ", 5)) {
            int k = (int)nom_strtoll(q + 5, 0, 10);
            if (g_step_seen >= 0 && k != g_step_seen && g_paused && g_vm) {
                g_paused = false;
                g_step_from = g_vm->line;       /* run until the line changes */
            }
            g_step_seen = k;
        }
        while (*q && *q != '\n') q++;
        if (*q) q++;
    }
}

static bool dbg_break_at(int line)
{
    for (int i = 0; i < g_nbp; i++)
        if (g_bp[i] == line) return true;
    return false;
}

/* paused: the script waits right here (a wait for the ship included) */
static void dbg_hold(VM *v)
{
    while (g_paused) {
        dbg_publish(v, g_state);
        if (sysc(SYS_yield, 0, 0, 0) != 1) { g_paused = false; break; }
        dbg_read_ctl();
    }
}

/* every time it gives the ship a frame */
static void dbg_waiting(VM *v)
{
    if (!g_bg || !g_vars[0]) return;
    dbg_read_ctl();
    dbg_hold(v);
    if (g_interrupt) {
        /* (inside a wait for the ship: it ends with the error at once) */
        g_interrupt = false;
        vm_runtime_error(v, "KeyboardInterrupt: stopped (Ctrl+C)");
        g_puts("KeyboardInterrupt: stopped (Ctrl+C)\n");
    }
    dbg_publish(v, "waiting for the ship");
}

/* runs the VM until it ends, with the debugger's stops: a line at a time
 * while there are breakpoints or a step, else in long runs */
static VmStatus dbg_run(VM *vm)
{
    VmStatus st;
    int last = vm->line, k = 0;
    for (;;) {
        if (++k % 8 == 1) dbg_read_ctl();
        dbg_hold(vm);
        if (g_interrupt) {
            g_interrupt = false;
            vm_runtime_error(vm, "KeyboardInterrupt: stopped (Ctrl+C)");
            return VM_ERROR;
        }
        if (g_nbp > 0 || g_step_from >= 0) {
            st = vm_run(vm, 1);
            if (st != VM_YIELD) return st;
            if (vm->line != last) {
                last = vm->line;
                int ln = vm->line - g_prelude_lines;
                if (g_step_from >= 0 && vm->line != g_step_from) {
                    g_step_from = -1;
                    g_paused = true;
                    nom_snprintf(g_state, sizeof g_state, "paused at line %d (a step)", ln);
                } else if (dbg_break_at(ln)) {
                    g_paused = true;
                    nom_snprintf(g_state, sizeof g_state, "paused at line %d (a breakpoint)", ln);
                }
            }
        } else {
            st = vm_run(vm, 1000);
            if (st != VM_YIELD) return st;
            last = vm->line;
            if (k % 4 == 0) dbg_publish(vm, "running");  /* (a few times a frame's CPU) */
        }
    }
}

static void dbg_begin(VM *vm, const char *name)
{
    g_vm = vm;
    if (!in_background()) return;
    nom_snprintf(g_script, sizeof g_script, "%s", name);
    int pid = (int)sysc(SYS_getpid, 0, 0, 0);
    g_mkdir("/run/py", 1);
    nom_snprintf(g_vars, sizeof g_vars, "/run/py/%d.vars", pid);
    nom_snprintf(g_ctl, sizeof g_ctl, "/run/py/%d.ctl", pid);
    dbg_read_ctl();
    dbg_publish(vm, "starting");
}

/* _echo(x): what the console prints for an expression on its own */
static VmStatus n_echo(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    *out = VAL_NIL;
    if (a[0].k == V_NIL) return VM_OK;
    Buf b; buf_init(&b);
    val_repr(&b, a[0]);
    buf_putc(&b, '\n');
    g_write(1, b.p, b.len);
    buf_free(&b);
    return VM_OK;
}

/* py -i: THE CONSOLE. Runs in the background for as long as the game does,
 * keeping its variables; takes what is written to /run/py/console.in (each
 * piece ended by a line holding only \x1e) as a script does, a frame at a
 * time, waits and all; prints what an expression on its own comes to. */
static char g_in[16384];
static void console(const char *prelude)
{
    char err[NOM_ERR_MAX] = "";
    Prog *prog = prog_compile(prelude, "console", err, sizeof err);
    if (!prog) die("console", err);
    nom_meter_begin(768u * 1024u);
    VM *vm = vm_new(prog, 0, 0);
    if (!in_background()) die("console", "run it in the background: ship run -i");
    while (vm_run(vm, 100000) == VM_YIELD) { }   /* (the constants) */
    dbg_begin(vm, "console");
    g_prelude_lines = 0;
    i64 seen = 0;
    g_puts("console ready: type python, it runs on the ship\n");
    for (;;) {
        dbg_publish(vm, "ready");
        if (sysc(SYS_yield, 0, 0, 0) != 1) break;
        dbg_read_ctl();
        i64 n = g_slurp("/run/py/console.in", g_in, sizeof g_in - 1);
        if (n < seen) seen = 0;           /* started again from empty */
        if (n <= seen) continue;
        g_in[n] = 0;
        /* each piece up to a line of \x1e */
        char *q = g_in + seen;
        for (;;) {
            char *end = q;
            while (*end && !(end[0] == '\x1e' && (end == g_in || end[-1] == '\n'))) end++;
            if (!*end) break;
            *end = 0;
            seen = (end - g_in) + 1;
            if (g_in[seen] == '\n') seen++;
            int fi = prog_compile_more(prog, q, true, err, sizeof err);
            if (fi < 0) {
                /* "[line 1]: name 'x'..." -> "error: name 'x'..."
                 * (a line number only when the piece had more than one) */
                const char *msg = err;
                int ln = 0;
                if (!nom_strncmp(err, "[line ", 6)) {
                    ln = (int)nom_strtoll(err + 6, 0, 10);
                    const char *c2 = err;
                    while (*c2 && !(c2[0] == ']' && c2[1] == ':')) c2++;
                    if (*c2) msg = c2 + 3;
                }
                bool many = false;
                for (const char *z = q; *z; z++) if (*z == '\n' && z[1]) many = true;
                char m2[NOM_ERR_MAX + 32];
                if (many && ln) nom_snprintf(m2, sizeof m2, "error: line %d: %s\n", ln, msg);
                else nom_snprintf(m2, sizeof m2, "error: %s\n", msg);
                g_puts(m2);
            } else {
                vm_start_func(vm, fi);
                VmStatus st = dbg_run(vm);
                if (st == VM_ERROR) { g_puts("error: "); g_puts(vm_err(vm)); g_puts("\n"); traceback(vm, 0); }
            }
            q = g_in + seen;
        }
    }
    g_exit(0);
}

/* what was calling what when it went wrong (the function it was in, and
 * the line each was called from), under the error, as Python's traceback:
 *     py: line 4: ZeroDivisionError: division by zero
 *       in rate(), called at line 9
 *       in report(), called at line 14 */
static void traceback(VM *vm, int pl)
{
    Buf b; buf_init(&b);
    int shown = 0;
    int k = vm->nframes - 1;
    while (k >= 1 && shown < 8) {
        struct Frame *fr = &vm->frame[k], *caller = &vm->frame[k - 1];
        int cl = caller->ip > 0 && caller->ip <= caller->fn->chunk.len ? caller->fn->chunk.line[caller->ip - 1] - pl : 0;
        /* the same call over and over (a function calling itself): once */
        int same = 0;
        while (k - same - 1 >= 1) {
            struct Frame *a = &vm->frame[k - same - 1], *ac = &vm->frame[k - same - 2];
            int al = ac->ip > 0 && ac->ip <= ac->fn->chunk.len ? ac->fn->chunk.line[ac->ip - 1] - pl : 0;
            if (a->fn != fr->fn || al != cl) break;
            same++;
        }
        if (same) buf_printf(&b, "  in %s(), called at line %d (%d times over)\n", fr->fn->name, cl, same + 1);
        else buf_printf(&b, "  in %s(), called at line %d\n", fr->fn->name, cl);
        k -= same + 1;
        shown++;
    }
    if (k >= 1) buf_printf(&b, "  ... and %d more calls\n", k);
    if (b.len) g_write(1, b.p, b.len);
    buf_free(&b);
}

/* ------------------------------------------------------------------ main */
static void die(const char *what, const char *detail)
{
    g_puts("py: ");
    g_puts(what);
    if (detail && detail[0]) { g_puts(": "); g_puts(detail); }
    g_puts("\n");
    g_exit(1);
}

void _start(void)
{
    if (g_getarg(argbuf, sizeof argbuf) <= 0) {
        g_puts("usage: py <script>          run a file\n"
               "       py -c '<source>'     run a line\n"
               "\n"
               "Python on this machine: what you'd write anywhere (classes,\n"
               "generators, try/finally, comprehensions, f-strings, with, ...),\n"
               "and the ship: `ship`, its parts by name, aim(), burn(), sleep().\n"
               "\n"
               "  THE SHIP'S PROGRAMS (each waits until it is done; run the\n"
               "  script with `ship run <script>` so the ship can move on):\n"
               "  launch(80000)  circularize()  burn(\"prograde\", 120)\n"
               "  burn_until(\"prograde\", \"apoapsis\", \">\", 2000000)\n"
               "  wait_for_window(\"krill\")  until_orbit_crosses(\"krill\")\n"
               "  transfer(\"krill\")  warp_to(\"krill\")  capture(30000)  land()\n"
               "  hover(50, 20)  node()  wait(5)  wait_until(\"altitude\", \"<\", 5000)\n"
               "  throttle(1) stage() sas(\"prograde\") point(45, 90) chutes() legs() mining()\n"
               "  get(\"apoapsis\")  countdown(5)  say(\"hi\")  abort()\n"
               "  maths: sqrt sin cos tan atan2 ... (radians), rad() deg(), pi,\n"
               "         abs min max round floor ceil float\n"
               "\n"
               "  do(command)   one command to the ship -- the same one a button sends\n"
               "  ship()        the whole ship, ready for json()\n"
               "  json(text)    an answer, as a dict you can index\n"
               "  lines(text)   an answer, as a list of lines\n"
               "  lists         append, join, keys, has, len\n"
               "  strings       split, find, sub, lower, upper, strip, replace, str, int\n"
               "  files         read(path), write(path, text)\n"
               "  print, exit\n"
               "\n"
               "  while True:\n"
               "      s = json(ship())\n"
               "      if s[\"weapon\"] == \"100\":\n"
               "          do(\"fire\")\n");
        g_exit(2);
    }

    const char *text = argbuf;
    if (argbuf[0] == '-' && argbuf[1] == 'i' && (argbuf[2] == 0 || argbuf[2] == ' '))
        console(PRELUDE);
    if (argbuf[0] == '-' && argbuf[1] == 'c') {
        /* A NEWLINE ON THE END, and it is not cosmetic: this is an
         * indentation-sensitive language, and its lexer ends a statement at a
         * NEWLINE token. `py -c 'print(1)'` compiled to nothing at all and
         * exited silently -- no output, no error -- until the line was
         * terminated the way a line in a file always is. */
        const char *from = argbuf + 2;
        while (*from == ' ') from++;
        /* AND ITS QUOTES TAKEN OFF. The shell hands the argument through with
         * them still on, so `py -c "print(9)"` arrived as source reading
         * "print(9)" -- a perfectly valid string literal, evaluated and
         * discarded, printing nothing and reporting no error. Twenty minutes
         * of a program that did exactly what it was told. */
        size_t flen = 0;
        while (from[flen]) flen++;
        if (flen >= 2 && (from[0] == '"' || from[0] == '\'') && from[flen - 1] == from[0]) {
            from++;
            flen -= 2;
        }
        size_t n = 0;
        while (n < flen && n < sizeof src - 2) { src[n] = from[n]; n++; }
        src[n++] = '\n';
        src[n] = 0;
        text = src;
    } else {
        i64 n = g_slurp(argbuf, src, sizeof src);
        if (n < 0) die("cannot read", argbuf);
        src[n] = 0;
        text = src;
    }

    /* TUX ROCKET SCIENCE: the constants, as one line ahead of the script
     * (a syntax error's line number is told back without it) */
    size_t plen = sizeof PRELUDE - 1, tlen = nom_strlen(src);
    if (tlen + plen < sizeof src) {
        nom_memmove(src + plen, src, tlen + 1);
        nom_memcpy(src, PRELUDE, plen);
    }
    text = src;

    char err[NOM_ERR_MAX] = "";
    Prog *prog = prog_compile(text, "script", err, sizeof err);
    if (!prog) {
        /* "[line 7]: why" is "line 4: why" of what was written */
        char fixed[NOM_ERR_MAX + 16];
        const char *at = err;
        int ln = 0;
        if (!nom_strncmp(at, "[line ", 6)) {
            at += 6;
            while (*at >= '0' && *at <= '9') ln = ln * 10 + (*at++ - '0');
            if (*at == ']') at++;
            if (*at == ':') at++;
            while (*at == ' ') at++;
        }
        nom_snprintf(fixed, sizeof fixed, "line %d: %s", ln - PRELUDE_LINES, at);
        die(fixed, "");
    }

    /* The meter: 768 KB of the megabyte the heap has. A script that runs away is
     * stopped by the VM rather than by the machine falling over, and the
     * message says which. */
    nom_meter_begin(768u * 1024u);
    VM *vm = vm_new(prog, 0, 0);
    if (!vm) die("out of memory", "");

    /* Instructions, not seconds: this machine has no clock a program can read
     * and that is deliberate (cpu.h). Ten million is a long script and a very
     * short wait. */
    /* TUX ROCKET SCIENCE: a flight script runs for the whole flight (an
     * autopilot, a watch kept on the apoapsis for an hour), so ten million
     * is a breath, not a limit. What bounds a script here is the machine
     * itself: it gets so many slices of the CPU a frame and no more. The
     * memory meter above still stops one that runs away with the heap. */
    g_prelude_lines = PRELUDE_LINES;
    dbg_begin(vm, argbuf[0] == '-' ? "py -c" : argbuf);
    VmStatus st = dbg_run(vm);
    if (st == VM_ERROR) {
        /* where it went wrong, in the script's own line numbers */
        char at[NOM_ERR_MAX + 32];
        nom_snprintf(at, sizeof at, "line %d: %s", vm->line - PRELUDE_LINES, vm_err(vm));
        nom_snprintf(g_state, sizeof g_state, "stopped: %s", at);
        dbg_publish(vm, g_state);
        g_puts("py: ");
        g_puts(at);
        g_puts("\n");
        traceback(vm, PRELUDE_LINES);
        g_exit(1);
    }
    dbg_publish(vm, "finished");

    int code = 0;
    vm_free(vm);
    prog_free(prog);
    g_exit(code);
}
