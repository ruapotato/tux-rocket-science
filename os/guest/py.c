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
    static char said[2048];
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
static bool yield(VM *v)
{
    if (sysc(SYS_yield, 0, 0, 0) == 1) return true;
    vm_runtime_error(v, "this waits for the ship to move on, and typed at a terminal the ship "
                        "waits for the command: run the script in the background (ship run <script>)");
    return false;
}

/* until the game has moved on a frame (and done what was asked in this one) */
static bool next_frame(VM *v)
{
    double f0 = reading_num(v, "frame");
    for (;;) {
        if (!yield(v)) return false;
        if (reading_num(v, "frame") != f0) return true;
    }
}

/* wait(seconds): of game time */
static VmStatus n_wait(VM *v, Value *a, int n, Value *out)
{
    (void)n;
    double until = reading_num(v, "time") + arg_num(a[0]);
    while (reading_num(v, "time") < until)
        if (!yield(v)) return VM_ERROR;
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
PROGRAM(n_burn, "burn")
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
QUICK(n_throttle, "throttle")
QUICK(n_stage, "stage")
QUICK(n_sas, "sas")
QUICK(n_point, "point")
QUICK(n_chutes, "chutes")
QUICK(n_legs, "legs")
QUICK(n_rcs, "rcs")
QUICK(n_say, "say")
QUICK(n_abort, "abort")
QUICK(n_autostage, "autostage")
QUICK(n_target, "target")

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
    { "burn",         2, 2, n_burn        },
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
    { "throttle",  1, 1, n_throttle  },
    { "stage",     0, 0, n_stage     },
    { "sas",       1, 1, n_sas       },
    { "point",     2, 2, n_point     },
    { "chutes",    0, 0, n_chutes    },
    { "legs",      0, 0, n_legs      },
    { "rcs",       1, 1, n_rcs       },
    { "say",       1, 1, n_say       },
    { "abort",     0, 0, n_abort     },
    { "autostage", 1, 1, n_autostage },
    { "target",    1, 1, n_target    },
    { "countdown", 0, 1, n_countdown },
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
    static const char PRELUDE[] = "pi = 3.141592653589793\n";
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
                k += (size_t)nom_snprintf(fixed + k, sizeof fixed - k, "[line %d", ln - 1);
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
    VmStatus st;
    while ((st = vm_run(vm, 10000000)) == VM_YIELD) { }
    if (st == VM_ERROR)    die("error", vm_err(vm));

    int code = 0;
    vm_free(vm);
    prog_free(prog);
    g_exit(code);
}
