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

/* ------------------------------------------------------------- natives --
 *
 * The whole library, and it is short on purpose. Everything the game can do
 * is one call away through api(), so this does not need a function per verb
 * -- it needs the handful of things you cannot write in the language itself.
 */

/* print(...) -- to stdout, which on this machine is the terminal. */
static VmStatus n_print(VM *v, Value *a, int n, Value *out)
{
    (void)v;
    Buf b;
    buf_init(&b);
    for (int i = 0; i < n; i++) {
        if (i) buf_putc(&b, ' ');
        val_tostr(&b, a[i]);
    }
    buf_putc(&b, '\n');
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
static VmStatus n_len(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    if (IS_STR(a[0]))  { *out = VAL_INT(AS_STR(a[0])->len);  return VM_OK; }
    if (IS_LIST(a[0])) { *out = VAL_INT(AS_LIST(a[0])->len); return VM_OK; }
    if (IS_DICT(a[0])) { *out = VAL_INT(AS_DICT(a[0])->len); return VM_OK; }
    *out = VAL_INT(0);
    return VM_OK;
}

static VmStatus n_str(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    Buf b;
    buf_init(&b);
    val_tostr(&b, a[0]);
    *out = str_new(b.p ? b.p : "", b.len);
    buf_free(&b);
    return VM_OK;
}

static VmStatus n_int(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    if (IS_STR(a[0])) {
        const char *s = AS_STR(a[0])->s;
        i64 sign = 1, val = 0;
        while (*s == ' ') s++;
        if (*s == '-') { sign = -1; s++; }
        while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; }
        *out = VAL_INT(sign * val);
        return VM_OK;
    }
    *out = VAL_INT(val_int(a[0]));
    return VM_OK;
}

/* split(s, sep) -- a list of pieces. The workhorse of reading anything back
 * from the API, and the thing sh can only fake. */
static VmStatus n_split(VM *v, Value *a, int n, Value *out)
{
    (void)v;
    if (!IS_STR(a[0])) { *out = list_new(); return VM_OK; }
    const char *s = AS_STR(a[0])->s;
    u32 slen = AS_STR(a[0])->len;
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
    (void)v;
    const char *sep = (n > 1 && IS_STR(a[1])) ? AS_STR(a[1])->s : "";
    Buf b;
    buf_init(&b);
    if (IS_LIST(a[0])) {
        List *l = AS_LIST(a[0]);
        for (u32 i = 0; i < l->len; i++) {
            if (i) buf_puts(&b, sep);
            val_tostr(&b, l->v[i]);
        }
    }
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
 *     land()
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

static VmStatus n_float(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    if (IS_STR(a[0])) { *out = VAL_NUM(nom_strtod(AS_STR(a[0])->s, 0)); return VM_OK; }
    *out = VAL_NUM(val_num(a[0]));
    return VM_OK;
}

static VmStatus n_abs(VM *v, Value *a, int n, Value *out)
{
    (void)v; (void)n;
    if (a[0].k == V_INT) *out = VAL_INT(a[0].as.i < 0 ? -a[0].as.i : a[0].as.i);
    else { double x = val_num(a[0]); *out = VAL_NUM(x < 0 ? -x : x); }
    return VM_OK;
}

static VmStatus n_minmax(VM *v, Value *a, int n, Value *out, int max)
{
    (void)v;
    if (n == 1 && IS_LIST(a[0])) { List *l = AS_LIST(a[0]); a = l->v; n = (int)l->len; }
    int best = 0;
    for (int i = 1; i < n; i++) {
        double x = val_num(a[i]), b = val_num(a[best]);
        if (max ? x > b : x < b) best = i;
    }
    *out = n ? val_retain(a[best]) : VAL_NIL;
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
static VmStatus n_round(VM *v, Value *a, int n, Value *out)
{
    (void)v;
    double x = val_num(a[0]);
    if (n < 2) { *out = VAL_INT((i64)nom_floor(x + 0.5)); return VM_OK; }
    double p = 1;
    for (i64 i = 0; i < val_int(a[1]); i++) p *= 10;
    *out = VAL_NUM(nom_floor(x * p + 0.5) / p);
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
static VmStatus n_wait(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    double until = reading_num(v, "time") + arg_num(a[0]);
    char wake[64];
    nom_snprintf(wake, sizeof wake, "wake %d", (int)until + 1);
    while (reading_num(v, "time") < until) {
        ask(v, wake);           /* so a warp eases off before it */
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
QUICK(n_rcs, "rcs")
QUICK(n_say, "say")
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
    vm_runtime_error(v, "%s isn't aboard any more (it went with a stage, or broke off)",
                     AS_HANDLE(obj)->label[0] ? AS_HANDLE(obj)->label : "that part");
    return true;
}

bool host_getattr(VM *v, Value obj, Str *name, Value *out)
{
    if (IS_DICT(obj)) {
        Value key = str_new(name->s, name->len), got;
        bool ok = dict_get(AS_DICT(obj), key, &got);
        val_release(key);
        if (!ok) { vm_runtime_error(v, "no '%s' in it (it has: keys(it))", name->s); return false; }
        *out = val_retain(got);
        return true;
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
    vm_runtime_error(v, "a %s has no .%s", IS_STR(obj) ? "string" : IS_LIST(obj) ? "list" : obj.k == V_NIL ? "nothing (None)" : "number", name->s);
    return false;
}

bool host_setattr(VM *v, Value obj, Str *name, Value val)
{
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
        for (int i = 0; i < n; i++) { buf_putc(&b, ' '); val_tostr(&b, args[i]); }
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
    /* the rest: x.f(a) is f(x, a) */
    int k = native_find(name->s, (int)name->len);
    int count = 0;
    const Native *tbl = native_table(&count);
    if (k < 0 || n + 1 > NOM_ARGS_MAX || n + 1 < tbl[k].minargs || n + 1 > tbl[k].maxargs) {
        vm_runtime_error(v, "a %s has no .%s()", IS_STR(obj) ? "string" : IS_LIST(obj) ? "list" : IS_DICT(obj) ? "dict" : "number", name->s);
        return VM_ERROR;
    }
    Value all[NOM_ARGS_MAX];
    all[0] = obj;
    for (int i = 0; i < n; i++) all[i + 1] = args[i];
    return tbl[k].fn(v, all, n + 1, out);
}

/* format(x, ".1f") -- what f"{x:.1f}" does: [,][.places][f d % s], and a
 * width with < > ^ before it */
static void put_fixed(Buf *b, double x, int places, bool commas)
{
    if (x != x) { buf_puts(b, "nan"); return; }
    if (x < 0) { buf_putc(b, '-'); x = -x; }
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

static VmStatus n_format(VM *v, Value *a, int n, Value *out)
{
    (void)v;
    const char *s = (n > 1 && IS_STR(a[1])) ? AS_STR(a[1])->s : "";
    char align = 0, type = 0;
    int width = 0, places = -1;
    bool commas = false;
    if (s[0] && (s[1] == '<' || s[1] == '>' || s[1] == '^')) s++;      /* (a fill: spaces only) */
    if (*s == '<' || *s == '>' || *s == '^') align = *s++;
    while (*s >= '0' && *s <= '9') width = width * 10 + (*s++ - '0');
    if (*s == ',') { commas = true; s++; }
    if (*s == '.') { s++; places = 0; while (*s >= '0' && *s <= '9') places = places * 10 + (*s++ - '0'); }
    if (*s) type = *s;
    Buf b; buf_init(&b);
    bool num = a[0].k == V_INT || a[0].k == V_NUM;
    if (num && (type == 'f' || type == 'F' || type == '%' || type == 'd' || places >= 0 || commas)) {
        double x = val_num(a[0]);
        if (type == '%') x *= 100;
        int pl = type == 'd' ? 0 : places >= 0 ? places : (type == 'f' || type == '%') ? 6 : (a[0].k == V_INT ? 0 : 2);
        put_fixed(&b, x, pl, commas);
        if (type == '%') buf_putc(&b, '%');
    } else {
        val_tostr(&b, a[0]);
    }
    int pad = width - (int)b.len;
    Buf o; buf_init(&o);
    if (!align) align = num ? '>' : '<';
    int left = pad <= 0 ? 0 : align == '>' ? pad : align == '^' ? pad / 2 : 0;
    int right = pad <= 0 ? 0 : pad - left;
    for (int i = 0; i < left; i++) buf_putc(&o, ' ');
    if (b.len) buf_put(&o, b.p, b.len);
    for (int i = 0; i < right; i++) buf_putc(&o, ' ');
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
    { "print", 0, 8, n_print },
    { "do",    1, 1, n_do    },
    { "api",   1, 1, n_do    },   /* the old name, still answered */
    { "ship",  0, 0, n_ship  },
    { "len",   1, 1, n_len   },
    { "str",   1, 1, n_str   },
    { "int",   1, 1, n_int   },
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
    { "float", 1, 1, n_float },
    { "abs",   1, 1, n_abs   },
    { "min",   1, 8, n_min   },
    { "max",   1, 8, n_max   },
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
    { "land",         0, 0, n_land        },
    { "hover",        1, 2, n_hover       },
    { "node",         0, 0, n_node        },
    /* and the quick ones */
    { "throttle",  1, 1, n_throttle_pct },
    { "stage",     0, 0, n_stage     },
    { "sas",       1, 1, n_sas       },
    { "point",     2, 2, n_point     },
    { "chutes",    0, 0, n_chutes    },
    { "legs",      0, 1, n_legs      },
    { "rcs",       1, 1, n_rcs       },
    { "say",       1, 1, n_say       },
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
static char g_state[96] = "running";

static void dbg_publish(VM *v, const char *state)
{
    if (!g_bg || !g_vars[0] || !v) return;
    Buf b; buf_init(&b);
    buf_printf(&b, "script %s\nline %d\nstate %s\n", g_script, v->line - g_prelude_lines, state);
    Prog *pr = v->prog;
    for (int g = 0; g < pr->nglobals; g++) {
        if (!v->gdef[g]) continue;
        const char *nm = pr->global[g];
        if (!nom_strcmp(nm, "pi") || !nom_strcmp(nm, "km") || !nom_strcmp(nm, "ship")) continue;
        Value x = v->global[g];
        if (x.k == V_OBJ && (x.as.o->kind == O_FUNC || x.as.o->kind == O_NATIVE)) continue;
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
            if (fr->base + i < v->sp) {
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
    nom_meter_begin(512u * 1024u);
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
                /* "parse error [line 1]: undefined name 'x'" -> "error: undefined name 'x'"
                 * (a line number only when the piece had more than one) */
                const char *msg = err;
                int ln = 0;
                if (!nom_strncmp(err, "parse error [line ", 18)) {
                    ln = (int)nom_strtoll(err + 18, 0, 10);
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
                if (st == VM_ERROR) { g_puts("error: "); g_puts(vm_err(vm)); g_puts("\n"); }
            }
            q = g_in + seen;
        }
    }
    g_exit(0);
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
               "A Python subset on this machine: if/elif/else, while, for/in,\n"
               "def/return, numbers (2.5 is 2.5), strings, lists and dicts.\n"
               "\n"
               "  THE SHIP'S PROGRAMS (each waits until it is done; run the\n"
               "  script with `ship run <script>` so the ship can move on):\n"
               "  launch(80000)  circularize()  burn(\"prograde\", 120)\n"
               "  burn_until(\"prograde\", \"apoapsis\", \">\", 2000000)\n"
               "  wait_for_window(\"krill\")  until_orbit_crosses(\"krill\")\n"
               "  transfer(\"krill\")  warp_to(\"krill\")  capture(30000)  land()\n"
               "  hover(50, 20)  node()  wait(5)  wait_until(\"altitude\", \"<\", 5000)\n"
               "  throttle(1) stage() sas(\"prograde\") point(45, 90) chutes() legs()\n"
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
        /* "[line 7]" is line 6 of what was written */
        char fixed[NOM_ERR_MAX];
        const char *at = err;
        size_t k = 0;
        while (*at && k < sizeof fixed - 12) {
            if (!nom_strncmp(at, "[line ", 6)) {
                at += 6;
                int ln = 0;
                while (*at >= '0' && *at <= '9') ln = ln * 10 + (*at++ - '0');
                k += (size_t)nom_snprintf(fixed + k, sizeof fixed - k, "[line %d", ln - PRELUDE_LINES);
                continue;
            }
            fixed[k++] = *at++;
        }
        fixed[k] = 0;
        die("syntax", fixed);
    }

    /* The meter: 512 KB of the 640 the heap has. A script that runs away is
     * stopped by the VM rather than by the machine falling over, and the
     * message says which. */
    nom_meter_begin(512u * 1024u);
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
        die("error", at);
    }
    dbg_publish(vm, "finished");

    int code = 0;
    vm_free(vm);
    prog_free(prog);
    g_exit(code);
}
