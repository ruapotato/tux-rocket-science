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

/* compile.c — NomScript parser and code generator, single pass.
 *
 * Statement dispatch is deterministic in the spirit of HAMSH_SPEC 2: a
 * statement is a control construct if it starts with a reserved word, an
 * assignment if it matches TARGET (= | += | ...) , and otherwise an
 * expression. There is no heuristic "is this code or a command" step.
 */
#include "lang.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

typedef struct Compiler {
    Func   *fn;
    char    local[NOM_LOCALS_MAX][NOM_NAME_MAX];
    int     nlocals;
    bool    toplevel;
    struct Compiler *enclosing;
    char    global[16][NOM_NAME_MAX];   /* TUX ROCKET SCIENCE: `global n` in a def */
    int     nglobal;
    char    cap[16][NOM_NAME_MAX];      /* the outer def's names this one uses (a closure) */
    int     ncap;
    /* its names a def (or lambda) inside it might use: they live in cells,
     * which the inner one shares (and nonlocal writes to) */
    char    cellname[48][NOM_NAME_MAX];
    int     ncellname;
    bool    is_cell[NOM_LOCALS_MAX];
    char    nonlocal[16][NOM_NAME_MAX];
    int     nnonlocal;
    int     try_base;      /* the try levels around it when it began (its return unwinds to here) */
    char    klass[NOM_NAME_MAX];        /* a method's class (for super()) */
} Compiler;

typedef struct Loop {
    int  start;            /* continue target */
    int  stack;            /* TUX ROCKET SCIENCE: values a for loop keeps on the stack (break pops them) */
    int  exitpatch[32];
    int  nexit;
    int  try_base;         /* the try levels around the loop (a break unwinds to here) */
    struct Loop *prev;
} Loop;

/* a try (or with) the code is inside: what leaving it early must do */
enum { TRY_EXCEPT, TRY_FINALLY, TRY_WITH };
typedef struct { int kind; int fin; char with_name[12]; } TryLevel;

typedef struct {
    TokenList tl;
    int       cur;
    Prog     *prog;
    Compiler *comp;
    Loop     *loop;
    char     *err;
    size_t    errsz;
    bool      failed;
    bool      echo;         /* TUX ROCKET SCIENCE: the REPL prints a lone expression */
    int       in_try;       /* inside a try: an unknown name is the except's to catch */
    TryLevel  trys[16];
    int       ntry;
    int       hidden;       /* $w1, $w2...: the with statements' managers */
    int       in_finally;   /* (no break/continue/return in a finally) */
    const char *klass;      /* the class whose body is being compiled */
} P;

static void expression(P *p);
static void statement(P *p);
static void block(P *p);

/* ------------------------------------------------------------- utilities */
static void perr(P *p, const char *fmt, ...)
{
    if (p->failed) return;
    p->failed = true;
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    int line = p->cur < p->tl.ntok ? p->tl.tok[p->cur].line : 0;
    snprintf(p->err, p->errsz, "[line %d]: %s", line, msg);
}

static Token *peek(P *p)  { return &p->tl.tok[p->cur]; }
static Token *prev(P *p)  { return &p->tl.tok[p->cur - 1]; }
static bool check(P *p, TokType t) { return peek(p)->type == t; }
static Token *advance(P *p)
{
    if (p->cur < p->tl.ntok - 1) p->cur++;
    return prev(p);
}
static bool match(P *p, TokType t) { if (check(p, t)) { advance(p); return true; } return false; }
static void expect(P *p, TokType t, const char *what)
{
    if (!match(p, t)) perr(p, "expected %s", what);
}
/* Newlines and stray INDENT/DEDENT are separators, never syntax. */
static void skip_seps(P *p)
{
    while (check(p, T_NEWLINE) || check(p, T_INDENT) || check(p, T_DEDENT)) advance(p);
}

/* ----------------------------------------------------------------- emit */
static Chunk *cur_chunk(P *p) { return &p->comp->fn->chunk; }

static void emit(P *p, uint8_t b)
{
    Chunk *c = cur_chunk(p);
    if (c->len >= NOM_CODE_MAX) { perr(p, "script too large (CODE_MAX)"); return; }
    if (c->len == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 128;
        c->code = nom_realloc(c->code, (size_t)c->cap);
        c->line = nom_realloc(c->line, (size_t)c->cap * sizeof(int));
    }
    c->line[c->len] = p->cur ? prev(p)->line : 1;
    c->code[c->len++] = b;
}

static void emit2(P *p, uint8_t a, uint8_t b) { emit(p, a); emit(p, b); }
static void emit_u16(P *p, int v) { emit(p, (uint8_t)((v >> 8) & 0xff)); emit(p, (uint8_t)(v & 0xff)); }

static int emit_jump(P *p, uint8_t op)
{
    emit(p, op);
    emit(p, 0xff);
    emit(p, 0xff);
    return cur_chunk(p)->len - 2;
}

static void patch_jump(P *p, int pos)
{
    Chunk *c = cur_chunk(p);
    int off = c->len - (pos + 2);
    if (off > 32767 || off < -32768) { perr(p, "jump too far"); return; }
    c->code[pos]     = (uint8_t)((off >> 8) & 0xff);
    c->code[pos + 1] = (uint8_t)(off & 0xff);
}

static void emit_loop(P *p, int start)
{
    emit(p, OP_JMP);
    int off = start - (cur_chunk(p)->len + 2);
    if (off < -32768) { perr(p, "loop too large"); return; }
    emit_u16(p, off & 0xffff);
}

static int add_const(P *p, Value v)
{
    Prog *pr = p->prog;
    for (int i = 0; i < pr->nconsts; i++)
        if (pr->consts[i].k == v.k && val_equal(pr->consts[i], v)) {
            val_release(v);
            return i;
        }
    if (pr->nconsts >= NOM_CONSTS_MAX) { perr(p, "too many constants"); val_release(v); return 0; }
    pr->consts[pr->nconsts] = v;
    return pr->nconsts++;
}

static void emit_const(P *p, Value v)
{
    int k = add_const(p, v);
    emit(p, OP_CONST);
    emit_u16(p, k);
}

/* ----------------------------------------------------------- name lookup */
static int local_slot(Compiler *c, const char *name, int len)
{
    for (int i = c->nlocals - 1; i >= 0; i--)
        if ((int)strlen(c->local[i]) == len && memcmp(c->local[i], name, (size_t)len) == 0)
            return i;
    return -1;
}

static int add_local(P *p, const char *name, int len)
{
    Compiler *c = p->comp;
    int slot = local_slot(c, name, len);
    if (slot >= 0) return slot;
    if (c->nlocals >= NOM_LOCALS_MAX) { perr(p, "too many local names"); return 0; }
    if (len >= NOM_NAME_MAX) { perr(p, "name too long"); return 0; }
    memcpy(c->local[c->nlocals], name, (size_t)len);
    c->local[c->nlocals][len] = 0;
    if (c->nlocals + 1 > c->fn->nlocals) c->fn->nlocals = c->nlocals + 1;
    /* a name a def inside this one uses: a cell, made at the call */
    for (int i = 0; i < c->ncellname; i++)
        if ((int)strlen(c->cellname[i]) == len && !memcmp(c->cellname[i], name, (size_t)len) && c->fn->ncells < 32) {
            c->is_cell[c->nlocals] = true;
            c->fn->cell_slot[c->fn->ncells++] = (uint8_t)c->nlocals;
            break;
        }
    return c->nlocals++;
}

/* a name of a def this one is inside, as a local of this one that holds
 * the same cell: its slot */
static int capture(P *p, const char *name, int len)
{
    Compiler *c = p->comp;
    if (c->ncap >= 16 || c->nlocals >= NOM_LOCALS_MAX || len >= NOM_NAME_MAX) { perr(p, "a def uses too many names of the def it's in"); return 0; }
    int s = c->nlocals++;
    memcpy(c->local[s], name, (size_t)len);
    c->local[s][len] = 0;
    if (c->nlocals > c->fn->nlocals) c->fn->nlocals = c->nlocals;
    c->is_cell[s] = true;
    c->fn->cap_slot[c->fn->ncap++] = s;
    memcpy(c->cap[c->ncap], name, (size_t)len);
    c->cap[c->ncap++][len] = 0;
    return s;
}

static bool is_nonlocal(Compiler *c, const char *name, int len)
{
    for (int i = 0; i < c->nnonlocal; i++)
        if ((int)strlen(c->nonlocal[i]) == len && !memcmp(c->nonlocal[i], name, (size_t)len)) return true;
    return false;
}

static int global_slot(P *p, const char *name, int len, bool create)
{
    Prog *pr = p->prog;
    for (int i = 0; i < pr->nglobals; i++)
        if ((int)strlen(pr->global[i]) == len && memcmp(pr->global[i], name, (size_t)len) == 0)
            return i;
    if (!create) return -1;
    if (pr->nglobals >= NOM_GLOBALS_MAX) { perr(p, "too many global names"); return 0; }
    if (len >= NOM_NAME_MAX) { perr(p, "name too long"); return 0; }
    memcpy(pr->global[pr->nglobals], name, (size_t)len);
    pr->global[pr->nglobals][len] = 0;
    return pr->nglobals++;
}

/* Resolution order: local, then an already-declared global, then a native.
 * An unresolvable name is a compile error rather than a silent nil — the
 * never-a-wrong-answer rule (HAMSH_SPEC 16a). */
static bool declared_global(Compiler *c, const char *name, int len)
{
    for (int i = 0; i < c->nglobal; i++)
        if ((int)strlen(c->global[i]) == len && memcmp(c->global[i], name, (size_t)len) == 0) return true;
    return false;
}

/* TUX ROCKET SCIENCE: how far apart two names are (edits to make one the
 * other), for "did you mean" */
static int edits(const char *a, int al, const char *b, int bl)
{
    if (al > 40 || bl > 40) return 99;
    int d[41][41];
    for (int i = 0; i <= al; i++) d[i][0] = i;
    for (int j = 0; j <= bl; j++) d[0][j] = j;
    for (int i = 1; i <= al; i++)
        for (int j = 1; j <= bl; j++) {
            int c = (a[i - 1] | 32) == (b[j - 1] | 32) ? 0 : 1;
            int m = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < m) m = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + c < m) m = d[i - 1][j - 1] + c;
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && d[i - 2][j - 2] + 1 < m) m = d[i - 2][j - 2] + 1;
            d[i][j] = m;
        }
    return d[al][bl];
}

/* the known name nearest to a mistyped one ("" if none is near) */
static const char *nearest_name(P *p, const char *name, int len)
{
    static char best[NOM_NAME_MAX];
    int bd = len <= 3 ? 1 : len <= 6 ? 2 : 3;
    best[0] = 0;
    int cnt = 0;
    const Native *tbl = native_table(&cnt);
    for (int i = 0; i < cnt; i++) {
        if (tbl[i].name[0] == '_') continue;
        int e = edits(name, len, tbl[i].name, (int)strlen(tbl[i].name));
        if (e <= bd && e > 0) { bd = e - 1; snprintf(best, sizeof best, "%s", tbl[i].name); }
    }
    for (int i = 0; i < p->prog->nglobals; i++) {
        int e = edits(name, len, p->prog->global[i], (int)strlen(p->prog->global[i]));
        if (e <= bd && e > 0) { bd = e - 1; snprintf(best, sizeof best, "%s", p->prog->global[i]); }
    }
    for (int i = 0; i < p->comp->nlocals; i++) {
        int e = edits(name, len, p->comp->local[i], (int)strlen(p->comp->local[i]));
        if (e <= bd && e > 0) { bd = e - 1; snprintf(best, sizeof best, "%s", p->comp->local[i]); }
    }
    static const char *KW[] = { "True", "False", "None", "while", "for", "return", "import", "lambda", NULL };
    for (int i = 0; KW[i]; i++) {
        int e = edits(name, len, KW[i], (int)strlen(KW[i]));
        if (e <= bd && e > 0) { bd = e - 1; snprintf(best, sizeof best, "%s", KW[i]); }
    }
    return best;
}

/* a name some def around this one (not the top) has as a local */
static bool outer_has(Compiler *c, const char *name, int len)
{
    for (Compiler *e = c->enclosing; e && !e->toplevel; e = e->enclosing)
        if (local_slot(e, name, len) >= 0) return true;
    return false;
}

static void emit_name_load(P *p, const char *name, int len)
{
    if (!p->comp->toplevel && !declared_global(p->comp, name, len)) {
        int s = local_slot(p->comp, name, len);
        if (s >= 0) { emit2(p, p->comp->is_cell[s] ? OP_GETCELL : OP_GETLOCAL, (uint8_t)s); return; }
        /* TUX ROCKET SCIENCE: a name of the def this one is inside (a
         * lambda in sorted() using its function's target): the same cell,
         * so a change either side is seen by both */
        if (outer_has(p->comp, name, len)) {
            s = capture(p, name, len);
            emit2(p, OP_GETCELL, (uint8_t)s);
            return;
        }
    }
    int g = global_slot(p, name, len, false);
    if (g >= 0) { emit(p, OP_GETGLOBAL); emit_u16(p, g); return; }
    int n = native_find(name, len);
    if (n >= 0) { emit2(p, OP_GETNATIVE, (uint8_t)n); return; }
    /* TUX ROCKET SCIENCE: in a def, a global the script sets further down
     * is found when it runs, as Python does (a name never set says so then);
     * at the top, a name nothing has set yet is a mistake now */
    if (!p->comp->toplevel || p->in_try) {
        g = global_slot(p, name, len, true);
        emit(p, OP_GETGLOBAL);
        emit_u16(p, g);
        return;
    }
    const char *near = nearest_name(p, name, len);
    if (near[0]) perr(p, "name '%.*s' is not defined -- did you mean '%s'?", len, name, near);
    else perr(p, "name '%.*s' is not defined (set it before this line?)", len, name);
}

static void emit_name_store(P *p, const char *name, int len)
{
    if (!p->comp->toplevel && !declared_global(p->comp, name, len)) {
        int s = local_slot(p->comp, name, len);
        if (s < 0 && is_nonlocal(p->comp, name, len)) {
            if (!outer_has(p->comp, name, len)) { perr(p, "nonlocal %.*s: no def around this one has a %.*s", len, name, len, name); return; }
            s = capture(p, name, len);
        }
        if (s < 0) s = add_local(p, name, len);
        emit2(p, p->comp->is_cell[s] ? OP_SETCELL : OP_SETLOCAL, (uint8_t)s);
        return;
    }
    int g = global_slot(p, name, len, true);
    emit(p, OP_SETGLOBAL);
    emit_u16(p, g);
}

/* ---------------------------------------------------------- expressions */
static void comprehension(P *p, TokType closer, int for_at, int kind);
static void or_expr(P *p);
static void bitor_expr(P *p);
enum { ST_STORE, ST_DEL };
static void store_target(P *p, int s, int e, int mode);
enum { COMP_LIST, COMP_SET = 2, COMP_DICT = 3 };

/* TUX ROCKET SCIENCE: where `tok` is, at this bracket depth, before the
 * bracket that closes it or the end of the line: its index, or -1 */
static int find_ahead(P *p, TokType tok)
{
    int depth = 0;
    for (int i = p->cur; i < p->tl.ntok; i++) {
        TokType t = p->tl.tok[i].type;
        if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) depth++;
        else if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) { if (!depth) return -1; depth--; }
        else if (!depth && t == tok) return i;
        else if (t == T_NEWLINE || t == T_EOF) return -1;
    }
    return -1;
}

/* the index of the bracket closing the one at i */
static int close_of(P *p, int i)
{
    int depth = 0;
    for (; i < p->tl.ntok; i++) {
        TokType t = p->tl.tok[i].type;
        if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) depth++;
        else if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) { if (--depth <= 0) return i; }
        else if (t == T_EOF) return i;
    }
    return i;
}

/* the index just past a block that a ':' at `colon` opens: an indented one,
 * or the rest of the line */
static int block_end(P *p, int colon)
{
    int i = colon + 1;
    if (p->tl.tok[i].type == T_NEWLINE && p->tl.tok[i + 1].type == T_INDENT) {
        int d = 0;
        for (i = i + 1; i < p->tl.ntok; i++) {
            TokType t = p->tl.tok[i].type;
            if (t == T_INDENT) d++;
            else if (t == T_DEDENT && --d == 0) return i + 1;
            else if (t == T_EOF) return i;
        }
        return i;
    }
    while (i < p->tl.ntok && p->tl.tok[i].type != T_NEWLINE && p->tl.tok[i].type != T_EOF) i++;
    return i;
}

/* the first token from i at depth 0 that is `tok`, before the line ends
 * (or -1) */
static int find_from(P *p, int i, TokType tok)
{
    int save = p->cur;
    p->cur = i;
    int r = find_ahead(p, tok);
    p->cur = save;
    return r;
}

/* a def's (or a lambda's) body, tokens from..to: if a def or lambda is in
 * it, the names it might use of this one's are noted, to be cells */
static void prescan_cells(P *p, Compiler *c, int from, int to)
{
    int first = -1;
    for (int i = from; i < to && i < p->tl.ntok; i++)
        if (p->tl.tok[i].type == T_DEF || p->tl.tok[i].type == T_LAMBDA) { first = i; break; }
    if (first < 0) return;
    for (int i = first; i < to && i < p->tl.ntok; i++) {
        Token *t = &p->tl.tok[i];
        if (t->type != T_IDENT || t->len >= NOM_NAME_MAX || c->ncellname >= 48) continue;
        bool have = false;
        for (int k = 0; k < c->ncellname && !have; k++)
            have = (int)strlen(c->cellname[k]) == t->len && !memcmp(c->cellname[k], t->start, (size_t)t->len);
        if (have) continue;
        memcpy(c->cellname[c->ncellname], t->start, (size_t)t->len);
        c->cellname[c->ncellname++][t->len] = 0;
    }
}

/* does the call whose '(' is at lp have a *x or **x among its arguments? */
static bool call_has_star(P *p, int lp)
{
    int close = close_of(p, lp), d = 0;
    for (int i = lp + 1; i < close; i++) {
        TokType t = p->tl.tok[i].type;
        if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) d++;
        else if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) d--;
        else if (!d && (t == T_STAR || t == T_DSTAR) && (i == lp + 1 || p->tl.tok[i - 1].type == T_COMMA)) return true;
    }
    return false;
}

/* f(a, b, key=c): the plain arguments, counted (the return), and the named
 * ones for OP_KWARGS; with a *xs or **opts among them, [list, dict] for
 * OP_CALLEX instead (*ex). The '(' is behind. */
static int arg_list(P *p, TokType closer, bool *ex)
{
    int n = 0, k = 0;
    *ex = false;
    if (match(p, closer)) return 0;
    /* sum(x * x for x in xs): one generator argument, as a list */
    int fa = find_ahead(p, T_FOR);
    if (fa >= 0 && closer == T_RPAREN) { comprehension(p, closer, fa, COMP_LIST); return 1; }
    if (call_has_star(p, p->cur - 1)) {
        *ex = true;
        emit2(p, OP_LIST, 0);
        bool kwpart = false;
        do {
            if (check(p, closer)) break;
            if (match(p, T_DSTAR)) {
                if (!kwpart) { emit2(p, OP_DICT, 0); kwpart = true; }
                expression(p);
                emit(p, OP_UPDATE);
            } else if (check(p, T_IDENT) && p->tl.tok[p->cur + 1].type == T_ASSIGN) {
                if (!kwpart) { emit2(p, OP_DICT, 0); kwpart = true; }
                Token *nt = advance(p);
                advance(p);
                emit_const(p, str_new(nt->start, (size_t)nt->len));
                expression(p);
                emit2(p, OP_PUTN, 0);
            } else {
                if (kwpart) { perr(p, "a plain argument (or *list) can't come after a name=value one"); return 0; }
                if (match(p, T_STAR)) { expression(p); emit(p, OP_EXTEND); }
                else { expression(p); emit2(p, OP_APPENDN, 0); }
            }
        } while (match(p, T_COMMA) && !p->failed);
        if (!kwpart) emit(p, OP_NIL);
        expect(p, closer, "')'");
        return 0;
    }
    do {
        skip_seps(p);
        if (check(p, closer)) break;            /* f(a, b,) */
        if (check(p, T_IDENT) && p->tl.tok[p->cur + 1].type == T_ASSIGN) {
            /* f(a, key=len): a name, then its value, for OP_KWARGS */
            Token *nt = advance(p);
            advance(p);
            emit_const(p, str_new(nt->start, (size_t)nt->len));
            expression(p);
            k++;
        } else {
            if (k) { perr(p, "a plain argument can't come after a name=value one"); return n; }
            expression(p);
            n++;
        }
        if (n + k > NOM_ARGS_MAX) { perr(p, "too many arguments (max %d)", NOM_ARGS_MAX); return n; }
        skip_seps(p);
    } while (match(p, T_COMMA) && !p->failed);
    expect(p, closer, closer == T_RPAREN ? "')'" : "']'");
    if (k) emit2(p, OP_KWARGS, (uint8_t)k);
    return n;
}

/* the call whose '(' is next: its arguments, and the call */
static void call_args(P *p)
{
    advance(p);
    bool ex;
    int n = arg_list(p, T_RPAREN, &ex);
    if (ex) emit(p, OP_CALLEX); else emit2(p, OP_CALL, (uint8_t)n);
}

/* obj.name(...) (the dot and name behind, the '(' next) */
static void method_call(P *p, int k)
{
    if (call_has_star(p, p->cur)) {
        emit(p, OP_GETATTR);
        emit_u16(p, k);
        call_args(p);
        return;
    }
    advance(p);
    bool ex;
    int n = arg_list(p, T_RPAREN, &ex);
    emit(p, OP_CALLMETHOD);
    emit_u16(p, k);
    emit(p, (uint8_t)n);
}

/* [f(x) for x in xs if g(x)] (and {k: v for...}, {x for...}, the generator
 * in sum(...)): a new list, then each `for` a loop and each `if` a test,
 * then the item -- which comes first in the text, so the tokens are visited
 * out of order (the item's last) */
static void comp_clause(P *p, int at, int item, int close, int loops, int kind)
{
    if (p->failed) return;
    if (at >= close) {
        int save = p->cur;
        p->cur = item;
        expression(p);
        if (kind == COMP_DICT) {
            expect(p, T_COLON, "':' in a dict comprehension");
            expression(p);
            emit2(p, OP_PUTN, (uint8_t)(2 * loops));
        } else emit2(p, OP_APPENDN, (uint8_t)(2 * loops));
        if (!p->failed && p->tl.tok[p->cur].type != T_FOR) perr(p, "expected 'for' in the comprehension");
        p->cur = save;
        return;
    }
    p->cur = at;
    if (match(p, T_FOR)) {
        int ts = p->cur;
        int in = find_ahead(p, T_IN);
        if (in < 0 || in >= close) { perr(p, "expected 'in' after 'for'"); return; }
        p->cur = in + 1;
        or_expr(p);
        int next = p->cur;
        emit(p, OP_FORPREP);
        int start = cur_chunk(p)->len;
        int exit_jump = emit_jump(p, OP_FORNEXT);
        store_target(p, ts, in, ST_STORE);
        comp_clause(p, next, item, close, loops + 1, kind);
        emit_loop(p, start);
        patch_jump(p, exit_jump);
        emit(p, OP_POP);
        emit(p, OP_POP);
    } else if (match(p, T_IF)) {
        or_expr(p);
        int next = p->cur;
        int j = emit_jump(p, OP_JMPF);
        comp_clause(p, next, item, close, loops, kind);
        patch_jump(p, j);
    } else perr(p, "expected 'for' or 'if' in the comprehension");
}

static void comprehension(P *p, TokType closer, int for_at, int kind)
{
    int item = p->cur;
    int close = close_of(p, item - 1);
    emit2(p, kind == COMP_DICT ? OP_DICT : OP_LIST, 0);
    comp_clause(p, for_at, item, close, 0, kind);
    if (kind == COMP_SET) emit(p, OP_TOSET);
    p->cur = close;
    expect(p, closer, closer == T_RPAREN ? "')'" : closer == T_RBRACE ? "'}'" : "']'");
}

/* load the cell a name's in (to hand to a def made here) */
static void emit_cell_ref(P *p, const char *name, int len)
{
    int s = local_slot(p->comp, name, len);
    if (s < 0 && !p->comp->toplevel && outer_has(p->comp, name, len)) s = capture(p, name, len);
    if (s >= 0) { emit2(p, OP_GETLOCAL, (uint8_t)s); return; }
    emit_name_load(p, name, len);
}

/* a def's (or a lambda's) value: the function, or with the outer names it
 * uses, a closure of their cells */
static void emit_function(P *p, int fi, Compiler *c)
{
    if (!c->ncap) { emit2(p, OP_GETFUNC, (uint8_t)fi); return; }
    for (int i = 0; i < c->ncap; i++) emit_cell_ref(p, c->cap[i], (int)strlen(c->cap[i]));
    emit(p, OP_CLOSURE);
    emit2(p, (uint8_t)fi, (uint8_t)c->ncap);
}

/* skips an annotation (a: float, -> int): to a ',' '=' or the closer */
static void skip_annotation(P *p, TokType closer)
{
    int depth = 0;
    while (!check(p, T_EOF) && !check(p, T_NEWLINE)) {
        TokType t = peek(p)->type;
        if (!depth && (t == T_COMMA || t == T_ASSIGN || t == closer)) break;
        if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) depth++;
        if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) { if (!depth) break; depth--; }
        advance(p);
    }
}

/* the parameters of a def (to its ')') or a lambda (to its ':'): a, b=1,
 * *rest, key=2, **more (annotations skipped). Where each default's text
 * starts is noted in dflt (-1: a keyword-only one with none), to be worked
 * out afterwards, in the scope around. */
static void params(P *p, Func *fn, TokType closer, int *dflt, int *nd)
{
    int nkw = 0;
    bool after_star = false;
    *nd = 0;
    fn->star = fn->dstar = -1;
    if (!check(p, closer)) {
        do {
            if (check(p, closer)) break;
            if (fn->dstar >= 0) { perr(p, "**%s must be the last parameter", p->comp->local[fn->dstar]); return; }
            if (match(p, T_DSTAR)) {
                if (!check(p, T_IDENT)) { perr(p, "expected a name after **"); return; }
                Token *pt = advance(p);
                fn->dstar = add_local(p, pt->start, pt->len);
                if (closer == T_RPAREN && match(p, T_COLON)) skip_annotation(p, closer);
                continue;
            }
            if (match(p, T_STAR)) {
                if (after_star) { perr(p, "only one * among the parameters"); return; }
                after_star = true;
                if (check(p, T_COMMA)) continue;        /* (a bare *: the ones after are keyword-only) */
                if (!check(p, T_IDENT)) { perr(p, "expected a name after *"); return; }
                Token *pt = advance(p);
                fn->star = add_local(p, pt->start, pt->len);
                if (closer == T_RPAREN && match(p, T_COLON)) skip_annotation(p, closer);
                continue;
            }
            if (!check(p, T_IDENT)) { perr(p, "expected a parameter name"); return; }
            Token *pt = advance(p);
            if (local_slot(p->comp, pt->start, pt->len) >= 0) { perr(p, "two parameters called %.*s", pt->len, pt->start); return; }
            add_local(p, pt->start, pt->len);
            if (closer == T_RPAREN && match(p, T_COLON)) skip_annotation(p, closer);
            if (*nd >= NOM_ARGS_MAX || fn->arity + nkw >= NOM_ARGS_MAX) { perr(p, "too many parameters (max %d)", NOM_ARGS_MAX); return; }
            if (match(p, T_ASSIGN)) {
                /* def f(a, b=10): b's default, worked out when the def runs */
                dflt[(*nd)++] = p->cur;
                if (!after_star) fn->ndefaults++;
                int depth = 0;
                while (!check(p, T_EOF) && !check(p, T_NEWLINE) && !(depth == 0 && (check(p, T_COMMA) || check(p, closer)))) {
                    TokType t = peek(p)->type;
                    if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) depth++;
                    if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) depth--;
                    advance(p);
                }
            } else if (after_star) {
                fn->kwreq |= 1u << nkw;
                dflt[(*nd)++] = -1;
            } else if (fn->ndefaults) { perr(p, "a parameter without a default can't follow one with"); return; }
            if (after_star) nkw++; else fn->arity++;
        } while (match(p, T_COMMA) && !p->failed);
    }
    fn->nkwonly = nkw;
}

/* the defaults noted by params(), worked out now (in the scope around the
 * def) and given to it */
static void emit_defaults(P *p, int fi, int *dflt, int nd)
{
    if (!nd) return;
    int after = p->cur;
    for (int i = 0; i < nd; i++) {
        if (dflt[i] < 0) { emit(p, OP_NIL); continue; }
        p->cur = dflt[i];
        expression(p);
    }
    emit(p, OP_SETDEFAULTS);
    emit2(p, (uint8_t)fi, (uint8_t)nd);
    p->cur = after;
}

/* lambda x, y=2: x * y -- a function with one line, returned */
static void lambda_expr(P *p)
{
    if (p->prog->nfuncs >= NOM_FUNCS_MAX) { perr(p, "too many functions"); return; }
    int fi = p->prog->nfuncs++;
    Func *fn = &p->prog->func[fi];
    memset(fn, 0, sizeof *fn);
    snprintf(fn->name, NOM_NAME_MAX, "%s", "<lambda>");
    Compiler c;
    memset(&c, 0, sizeof c);
    c.fn = fn;
    c.toplevel = false;
    c.enclosing = p->comp;
    c.try_base = p->ntry;
    /* its body, for the cells a lambda in it would share */
    {
        int colon = find_ahead(p, T_COLON);
        if (colon >= 0) {
            int e = colon + 1, depth = 0;
            for (; e < p->tl.ntok; e++) {
                TokType t = p->tl.tok[e].type;
                if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) depth++;
                else if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) { if (!depth) break; depth--; }
                else if (!depth && (t == T_COMMA || t == T_NEWLINE || t == T_EOF || t == T_FOR)) break;
            }
            prescan_cells(p, &c, colon, e);
        }
    }
    p->comp = &c;
    int dflt[NOM_ARGS_MAX], nd = 0;
    params(p, fn, T_COLON, dflt, &nd);
    expect(p, T_COLON, "':' after lambda's parameters");
    expression(p);
    emit(p, OP_RET);
    Buf nb; buf_init(&nb);
    for (int i = 0; i < c.nlocals; i++) { if (i) buf_putc(&nb, ','); buf_puts(&nb, c.local[i]); }
    buf_putc(&nb, 0);
    fn->locals = nb.p;
    p->comp = c.enclosing;
    emit_defaults(p, fi, dflt, nd);
    emit_function(p, fi, &c);
}

/* TUX ROCKET SCIENCE: f"{needed:.0f} m/s, {name!r}" -- each {...} is an
 * expression, compiled from its own tokens, and format(value, spec)ed (a
 * spec may have {...} in it too: {x:{width}.2f}); !r and !s are repr() and
 * str() first; {{ and }} are braces. The pieces are added up into one
 * string. */
static void fstring_text(P *p, const char *s, int n, int line)
{
    emit_const(p, str_new("", 0));
    Buf lit;
    buf_init(&lit);
    for (int i = 0; i <= n && !p->failed; ) {
        if (i < n && s[i] == '{' && i + 1 < n && s[i + 1] == '{') { buf_putc(&lit, '{'); i += 2; continue; }
        if (i < n && s[i] == '}' && i + 1 < n && s[i + 1] == '}') { buf_putc(&lit, '}'); i += 2; continue; }
        if (i < n && s[i] != '{') { buf_putc(&lit, s[i]); i++; continue; }
        if (lit.len) {
            emit_const(p, str_new(lit.p, lit.len));
            emit(p, OP_ADD);
            buf_clear(&lit);
        }
        if (i >= n) break;
        /* the expression runs to the matching }; a ! (not !=) or a : outside
         * any brackets or quotes ends it */
        int j = i + 1, depth = 0, colon = -1, bang = -1;
        char quote = 0;
        for (; j < n; j++) {
            char c = s[j];
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '"' || c == '\'') quote = c;
            else if (c == '(' || c == '[' || c == '{') depth++;
            else if ((c == ')' || c == ']' || c == '}') && depth) depth--;
            else if (c == '}') break;
            else if (c == '!' && !depth && colon < 0 && bang < 0 && j + 1 < n && s[j + 1] != '=') bang = j;
            else if (c == ':' && !depth && colon < 0) colon = j;
        }
        if (j >= n) { perr(p, "f-string: a '{' with no '}'"); break; }
        int e1 = bang >= 0 ? bang : colon >= 0 ? colon : j;
        char conv = bang >= 0 ? s[bang + 1] : 0;
        if (bang >= 0 && !((conv == 'r' || conv == 's' || conv == 'a') && (bang + 2 == j || bang + 2 == colon))) {
            perr(p, "f-string: after ! goes r or s ({x!r} is repr(x))");
            break;
        }
        /* f"{x=}": "x=" and then x as repr shows it */
        bool self_doc = e1 - 1 > i && s[e1 - 1] == '=' && s[e1 - 2] != '=' && s[e1 - 2] != '!' && s[e1 - 2] != '<' && s[e1 - 2] != '>';
        if (self_doc) {
            emit_const(p, str_new(s + i + 1, (size_t)(e1 - i - 1)));
            emit(p, OP_ADD);
            e1--;
            if (!conv && colon < 0) conv = 'r';
        }
        char *src = nom_alloc((size_t)(e1 - i) + 2);
        memcpy(src, s + i + 1, (size_t)(e1 - i - 1));
        src[e1 - i - 1] = '\n';
        src[e1 - i] = 0;
        TokenList tl;
        char lerr[NOM_ERR_MAX];
        if (!lex_source(src, &tl, lerr, sizeof lerr)) {
            perr(p, "f-string: %s", lerr);
            nom_free(src);
            break;
        }
        for (int k = 0; k < tl.ntok; k++) tl.tok[k].line = line;
        emit2(p, OP_GETNATIVE, (uint8_t)native_find("format", 6));
        if (conv) emit2(p, OP_GETNATIVE, (uint8_t)(conv == 's' ? native_find("str", 3) : native_find("repr", 4)));
        P sub = *p;
        sub.tl = tl;
        sub.cur = 0;
        if (check(&sub, T_NEWLINE)) perr(&sub, "f-string: an empty {}");
        else expression(&sub);
        if (!sub.failed && !check(&sub, T_NEWLINE) && !check(&sub, T_EOF))
            perr(&sub, "f-string: more than one expression in {...}");
        p->failed = sub.failed;
        lex_free(&tl);
        nom_free(src);
        if (p->failed) break;
        if (conv) emit2(p, OP_CALL, 1);
        if (colon >= 0) {
            bool nested = false;
            for (int k = colon + 1; k < j; k++) nested |= s[k] == '{';
            if (nested) fstring_text(p, s + colon + 1, j - colon - 1, line);
            else emit_const(p, str_new(s + colon + 1, (size_t)(j - colon - 1)));
        } else emit_const(p, str_new("", 0));
        emit2(p, OP_CALL, 2);
        emit(p, OP_ADD);
        i = j + 1;
    }
    buf_free(&lit);
}

/* x[...] (the '[' behind): a key, or a slice's three (and true) */
static bool subscript(P *p)
{
    if (check(p, T_COLON)) emit(p, OP_NIL);
    else {
        expression(p);
        if (check(p, T_COMMA)) {
            /* d[1, 2]: the key is a tuple */
            int n = 1;
            while (match(p, T_COMMA) && !check(p, T_RBRACKET)) { expression(p); n++; }
            emit2(p, OP_TUPLE, (uint8_t)n);
            return false;
        }
    }
    if (!match(p, T_COLON)) return false;
    if (check(p, T_RBRACKET) || check(p, T_COLON)) emit(p, OP_NIL); else expression(p);
    if (match(p, T_COLON) && !check(p, T_RBRACKET)) expression(p); else emit(p, OP_NIL);
    return true;
}

static void primary(P *p)
{
    Token *t = advance(p);
    switch (t->type) {
    case T_INT:   emit_const(p, VAL_INT(t->ival)); return;
    case T_NUM:   emit_const(p, VAL_NUM(t->dval)); return;
    case T_STR: case T_FSTR: {
        /* "a" "b" is "ab" (a long message over lines), f-strings among them */
        Buf all; buf_init(&all);
        bool any = false;
        for (;;) {
            if (t->type == T_STR) buf_put(&all, t->sval, (size_t)t->len);
            else {
                if (all.len || !any) { emit_const(p, str_new(all.p ? all.p : "", all.len)); if (any) emit(p, OP_ADD); any = true; buf_clear(&all); }
                fstring_text(p, t->sval, t->len, t->line);
                emit(p, OP_ADD);
            }
            if (!check(p, T_STR) && !check(p, T_FSTR)) break;
            t = advance(p);
        }
        if (all.len || !any) { emit_const(p, str_new(all.p ? all.p : "", all.len)); if (any) emit(p, OP_ADD); }
        buf_free(&all);
        return;
    }
    case T_TRUE:  emit(p, OP_TRUE);  return;
    case T_FALSE: emit(p, OP_FALSE); return;
    case T_NIL:   emit(p, OP_NIL);   return;
    case T_IDENT:
        if (t->len == 5 && !memcmp(t->start, "super", 5) && check(p, T_LPAREN) && p->tl.tok[p->cur + 1].type == T_RPAREN) {
            /* super(): the class this method is in, and its self */
            Compiler *m = p->comp;
            while (m && !m->klass[0]) m = m->enclosing;
            if (!m || !m->nlocals) { perr(p, "super() works inside a class's method"); return; }
            advance(p);
            advance(p);
            emit2(p, OP_GETNATIVE, (uint8_t)native_find("_super", 6));
            emit_name_load(p, m->klass, (int)strlen(m->klass));
            emit_name_load(p, m->local[0], (int)strlen(m->local[0]));
            emit2(p, OP_CALL, 2);
            return;
        }
        emit_name_load(p, t->start, t->len);
        return;
    case T_LAMBDA: lambda_expr(p); return;
    case T_YIELD: perr(p, "'yield' as a value isn't supported: put it on a line of its own"); return;
    case T_LPAREN: {
        /* (a, b) is a tuple -- here, a list; () an empty one */
        if (match(p, T_RPAREN)) { emit2(p, OP_TUPLE, 0); return; }
        int fa = find_ahead(p, T_FOR);
        if (fa >= 0) { comprehension(p, T_RPAREN, fa, COMP_LIST); return; }
        skip_seps(p);
        expression(p);
        skip_seps(p);
        if (check(p, T_COMMA)) {
            int n = 1;
            while (match(p, T_COMMA) && !p->failed) {
                skip_seps(p);
                if (check(p, T_RPAREN)) break;
                expression(p);
                n++;
                if (n > 255) { perr(p, "tuple too long"); return; }
            }
            emit2(p, OP_TUPLE, (uint8_t)n);
        }
        expect(p, T_RPAREN, "')'");
        return;
    }
    case T_LBRACKET: {
        int fa = find_ahead(p, T_FOR);
        if (fa >= 0) { comprehension(p, T_RBRACKET, fa, COMP_LIST); return; }
        /* [*a, *b]: built a piece at a time */
        bool stars = false;
        {
            int close = close_of(p, p->cur - 1), d = 0;
            for (int i = p->cur; i < close; i++) {
                TokType tt = p->tl.tok[i].type;
                if (tt == T_LPAREN || tt == T_LBRACKET || tt == T_LBRACE) d++;
                else if (tt == T_RPAREN || tt == T_RBRACKET || tt == T_RBRACE) d--;
                else if (!d && tt == T_STAR && (i == p->cur || p->tl.tok[i - 1].type == T_COMMA)) stars = true;
            }
        }
        if (stars) {
            emit2(p, OP_LIST, 0);
            do {
                skip_seps(p);
                if (check(p, T_RBRACKET)) break;
                if (match(p, T_STAR)) { expression(p); emit(p, OP_EXTEND); }
                else { expression(p); emit2(p, OP_APPENDN, 0); }
                skip_seps(p);
            } while (match(p, T_COMMA) && !p->failed);
            expect(p, T_RBRACKET, "']'");
            return;
        }
        int n = 0;
        if (!check(p, T_RBRACKET)) {
            do {
                skip_seps(p);
                if (check(p, T_RBRACKET)) break;
                expression(p);
                n++;
                if (n > 255) { perr(p, "list literal too long"); return; }
                skip_seps(p);
            } while (match(p, T_COMMA) && !p->failed);
        }
        expect(p, T_RBRACKET, "']'");
        emit2(p, OP_LIST, (uint8_t)n);
        return;
    }
    case T_LBRACE: {
        skip_seps(p);
        if (match(p, T_RBRACE)) { emit2(p, OP_DICT, 0); return; }
        int fa = find_ahead(p, T_FOR);
        int colon = find_ahead(p, T_COLON);
        if (fa >= 0) {
            /* {k: v for k in s if c} / {x for x in s} */
            comprehension(p, T_RBRACE, fa, colon >= 0 && colon < fa ? COMP_DICT : COMP_SET);
            return;
        }
        bool is_dict = check(p, T_DSTAR) || colon >= 0;
        if (!is_dict) {
            /* {a, b}: a set */
            int n = 0;
            do {
                skip_seps(p);
                if (check(p, T_RBRACE)) break;
                expression(p);
                n++;
                if (n > 255) { perr(p, "set literal too long"); return; }
                skip_seps(p);
            } while (match(p, T_COMMA) && !p->failed);
            expect(p, T_RBRACE, "'}'");
            emit2(p, OP_SET, (uint8_t)n);
            return;
        }
        if (find_ahead(p, T_DSTAR) >= 0) {
            /* {**a, "k": v}: a piece at a time */
            emit2(p, OP_DICT, 0);
            do {
                skip_seps(p);
                if (check(p, T_RBRACE)) break;
                if (match(p, T_DSTAR)) { expression(p); emit(p, OP_UPDATE); }
                else {
                    expression(p);
                    expect(p, T_COLON, "':' in dict literal");
                    expression(p);
                    emit2(p, OP_PUTN, 0);
                }
                skip_seps(p);
            } while (match(p, T_COMMA) && !p->failed);
            expect(p, T_RBRACE, "'}'");
            return;
        }
        int n = 0;
        do {
            skip_seps(p);
            if (check(p, T_RBRACE)) break;
            expression(p);
            expect(p, T_COLON, "':' in dict literal");
            expression(p);
            n++;
            if (n > 255) { perr(p, "dict literal too long"); return; }
            skip_seps(p);
        } while (match(p, T_COMMA) && !p->failed);
        expect(p, T_RBRACE, "'}'");
        emit2(p, OP_DICT, (uint8_t)n);
        return;
    }
    default:
        perr(p, "expected an expression");
        return;
    }
}

static void postfix(P *p)
{
    primary(p);
    for (;;) {
        if (check(p, T_LPAREN)) {
            call_args(p);
        } else if (match(p, T_LBRACKET)) {
            /* x[i], or a slice: x[a:b], x[:b], x[a:], x[::-1] */
            bool slice = subscript(p);
            expect(p, T_RBRACKET, "']'");
            emit(p, slice ? OP_SLICE : OP_INDEX);
        } else if (match(p, T_DOT)) {
            /* TUX ROCKET SCIENCE: engine.throttle, ship.orbit.top,
             * booster.decouple() */
            if (!check(p, T_IDENT)) { perr(p, "expected a name after '.'"); return; }
            Token *nt = advance(p);
            int k = add_const(p, str_new(nt->start, (size_t)nt->len));
            if (check(p, T_LPAREN)) method_call(p, k);
            else {
                emit(p, OP_GETATTR);
                emit_u16(p, k);
            }
        } else {
            return;
        }
        if (p->failed) return;
    }
}

static void unary(P *p);

static void power(P *p)
{
    postfix(p);
    if (match(p, T_DSTAR)) { unary(p); emit(p, OP_POW); }   /* right associative */
}

static void unary(P *p)
{
    if (match(p, T_MINUS)) { unary(p); emit(p, OP_NEG); return; }
    if (match(p, T_PLUS))  { unary(p); return; }
    if (match(p, T_TILDE)) { unary(p); emit(p, OP_INV); return; }
    power(p);
}

static void term(P *p)
{
    unary(p);
    for (;;) {
        if      (match(p, T_STAR))    { unary(p); emit(p, OP_MUL); }
        else if (match(p, T_SLASH))   { unary(p); emit(p, OP_DIV); }
        else if (match(p, T_DSLASH))  { unary(p); emit(p, OP_FDIV); }
        else if (match(p, T_PERCENT)) { unary(p); emit(p, OP_MOD); }
        else return;
        if (p->failed) return;
    }
}

static void sum(P *p)
{
    term(p);
    for (;;) {
        if      (match(p, T_PLUS))  { term(p); emit(p, OP_ADD); }
        else if (match(p, T_MINUS)) { term(p); emit(p, OP_SUB); }
        else return;
        if (p->failed) return;
    }
}

/* the bits: << >>, &, ^, | (each looser than the one before) */
static void shift_expr(P *p)
{
    sum(p);
    for (;;) {
        if      (match(p, T_SHL)) { sum(p); emit(p, OP_SHL); }
        else if (match(p, T_SHR)) { sum(p); emit(p, OP_SHR); }
        else return;
        if (p->failed) return;
    }
}
static void band_expr(P *p) { shift_expr(p); while (match(p, T_AMP) && !p->failed) { shift_expr(p); emit(p, OP_BAND); } }
static void bxor_expr(P *p) { band_expr(p); while (match(p, T_CARET) && !p->failed) { band_expr(p); emit(p, OP_BXOR); } }
static void bitor_expr(P *p) { bxor_expr(p); while (match(p, T_PIPE) && !p->failed) { bxor_expr(p); emit(p, OP_BOR); } }

/* the comparison at the cursor (consumed), or -1 */
static int cmp_op(P *p)
{
    if (match(p, T_EQ)) return OP_EQ;
    if (match(p, T_NE)) return OP_NE;
    if (match(p, T_LT)) return OP_LT;
    if (match(p, T_LE)) return OP_LE;
    if (match(p, T_GT)) return OP_GT;
    if (match(p, T_GE)) return OP_GE;
    if (match(p, T_IN)) return OP_IN;
    if (check(p, T_NOT) && p->tl.tok[p->cur + 1].type == T_IN) { advance(p); advance(p); return 1000 + OP_IN; }
    /* x is None, x is not None: equality (there are no shared objects to tell apart) */
    if (match(p, T_IS)) return match(p, T_NOT) ? OP_NE : OP_EQ;
    return -1;
}

static bool at_cmp(P *p)
{
    TokType t = peek(p)->type;
    return t == T_EQ || t == T_NE || t == T_LT || t == T_LE || t == T_GT || t == T_GE || t == T_IN || t == T_IS ||
           (t == T_NOT && p->tl.tok[p->cur + 1].type == T_IN);
}

static void comparison(P *p)
{
    bitor_expr(p);
    int jumps[16], nj = 0;
    for (;;) {
        int op = cmp_op(p);
        if (op < 0) break;
        int right = p->cur;
        bitor_expr(p);
        if (op >= 1000) { emit(p, (uint8_t)(op - 1000)); emit(p, OP_NOT); }
        else emit(p, (uint8_t)op);
        if (p->failed) return;
        /* 0 < x < 5 is 0 < x and x < 5 (x worked out again: the same) */
        if (at_cmp(p) && nj < 16) {
            jumps[nj++] = emit_jump(p, OP_JMPF_KEEP);
            emit(p, OP_POP);
            int save = p->cur;
            p->cur = right;
            bitor_expr(p);
            p->cur = save;
        }
    }
    for (int i = 0; i < nj; i++) patch_jump(p, jumps[i]);
}

static void not_expr(P *p)
{
    if (match(p, T_NOT)) { not_expr(p); emit(p, OP_NOT); return; }
    comparison(p);
}

static void and_expr(P *p)
{
    not_expr(p);
    while (match(p, T_AND)) {
        int j = emit_jump(p, OP_JMPF_KEEP);
        emit(p, OP_POP);
        not_expr(p);
        patch_jump(p, j);
        if (p->failed) return;
    }
}

static void or_expr(P *p)
{
    and_expr(p);
    while (match(p, T_OR)) {
        int j = emit_jump(p, OP_JMPT_KEEP);
        emit(p, OP_POP);
        and_expr(p);
        patch_jump(p, j);
        if (p->failed) return;
    }
}

/* a if c else b: c is worked out first, so the tokens are visited out of
 * order (TUX ROCKET SCIENCE); and n := value, which keeps it too */
static void expression(P *p)
{
    if (check(p, T_IDENT) && p->tl.tok[p->cur + 1].type == T_WALRUS) {
        Token nt = *advance(p);
        advance(p);
        expression(p);
        emit(p, OP_DUP);
        emit_name_store(p, nt.start, nt.len);
        return;
    }
    int ifat = -1, elseat = -1, depth = 0;
    for (int i = p->cur; i < p->tl.ntok; i++) {
        TokType t = p->tl.tok[i].type;
        if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) depth++;
        else if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) { if (!depth) break; depth--; }
        else if (!depth) {
            if (t == T_NEWLINE || t == T_EOF || t == T_COMMA || t == T_COLON || t == T_FOR || t == T_ASSIGN ||
                t == T_LAMBDA || t == T_INDENT || t == T_DEDENT || t == T_AS || t == T_WALRUS) break;
            if (t == T_IF && ifat < 0) ifat = i;
            else if (t == T_ELSE && ifat >= 0) { elseat = i; break; }
        }
    }
    if (ifat < 0 || elseat < 0) { or_expr(p); return; }
    int start = p->cur;
    p->cur = ifat + 1;
    or_expr(p);
    if (p->cur != elseat && !p->failed) { perr(p, "expected 'else' in 'a if c else b'"); return; }
    int jf = emit_jump(p, OP_JMPF);
    p->cur = start;
    or_expr(p);
    if (p->cur != ifat && !p->failed) { perr(p, "expected 'if' in 'a if c else b'"); return; }
    int je = emit_jump(p, OP_JMP);
    patch_jump(p, jf);
    p->cur = elseat + 1;
    expression(p);
    patch_jump(p, je);
}

/* a, b (a return, the right of an =): a tuple when there's a comma */
static void expr_list(P *p)
{
    expression(p);
    if (!check(p, T_COMMA)) return;
    int n = 1;
    while (match(p, T_COMMA) && !p->failed) {
        if (check(p, T_NEWLINE) || check(p, T_EOF) || check(p, T_DEDENT)) break;
        expression(p);
        n++;
        if (n > 255) { perr(p, "too many values"); return; }
    }
    emit2(p, OP_TUPLE, (uint8_t)n);
}

/* ------------------------------------------------------------ targets */

/* the items of a, (b, c), *d -- s..e split at the commas outside brackets,
 * the *one noted -- each stored in turn, after the value is unpacked */
static void tuple_target(P *p, int s, int e, int mode)
{
    Token *tk = p->tl.tok;
    int st[32], en[32], n = 0, star = -1, d = 0, b = s;
    for (int i = s; i <= e; i++) {
        TokType t = i < e ? tk[i].type : T_COMMA;
        if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) d++;
        else if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) d--;
        else if (t == T_COMMA && !d) {
            if (i > b) {
                if (n >= 32) { perr(p, "too many names to unpack into"); return; }
                st[n] = b;
                en[n] = i;
                if (tk[b].type == T_STAR) {
                    if (star >= 0) { perr(p, "only one *name when unpacking"); return; }
                    if (mode == ST_DEL) { perr(p, "can't del a *name"); return; }
                    star = n;
                    st[n]++;
                }
                n++;
            }
            b = i + 1;
        }
    }
    if (mode == ST_DEL) {
        for (int i = 0; i < n; i++) store_target(p, st[i], en[i], ST_DEL);
        return;
    }
    if (star >= 0) {
        emit(p, OP_UNPACKEX);
        emit2(p, (uint8_t)star, (uint8_t)(n - star - 1));
    } else emit2(p, OP_UNPACK, (uint8_t)n);
    for (int i = 0; i < n; i++) store_target(p, st[i], en[i], mode);
}

/* the value on top of the stack into the target written in tokens s..e: a
 * name, a.b, a[k], a[i:j], or several of them (a, (b, *c)) unpacked; or
 * (ST_DEL) the target taken away */
static void store_target(P *p, int s, int e, int mode)
{
    if (p->failed) return;
    Token *tk = p->tl.tok;
    if (s >= e) { perr(p, "expected a name to assign to"); return; }
    /* (a, b) and [a, b]: what's in them */
    if ((tk[s].type == T_LPAREN || tk[s].type == T_LBRACKET) && close_of(p, s) == e - 1) {
        if (e - 1 == s + 1) { perr(p, "can't assign to an empty ()"); return; }
        bool comma = false;
        int d = 0;
        for (int i = s + 1; i < e - 1; i++) {
            TokType t = tk[i].type;
            if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) d++;
            else if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) d--;
            else if (t == T_COMMA && !d) comma = true;
        }
        if (comma || tk[s].type == T_LBRACKET || tk[s + 1].type == T_STAR) tuple_target(p, s + 1, e - 1, mode);
        else store_target(p, s + 1, e - 1, mode);
        return;
    }
    {
        int d = 0;
        for (int i = s; i < e; i++) {
            TokType t = tk[i].type;
            if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) d++;
            else if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) d--;
            else if (t == T_COMMA && !d) { tuple_target(p, s, e, mode); return; }
        }
    }
    if (tk[s].type == T_STAR) { perr(p, "a *name goes among others: a, *rest = ..."); return; }
    if (tk[s].type != T_IDENT) {
        perr(p, tk[s].type == T_INT || tk[s].type == T_NUM || tk[s].type == T_STR ? "can't assign to a value (is it == you meant?)"
                                                                                : "can't assign to that (only to names, a.b and a[k])");
        return;
    }
    if (e - s == 1) {
        if (mode == ST_STORE) { emit_name_store(p, tk[s].start, tk[s].len); return; }
        /* del x */
        if (!p->comp->toplevel && !declared_global(p->comp, tk[s].start, tk[s].len)) {
            int sl = local_slot(p->comp, tk[s].start, tk[s].len);
            if (sl < 0) { perr(p, "del %.*s: there's no %.*s here", tk[s].len, tk[s].start, tk[s].len, tk[s].start); return; }
            emit(p, OP_NIL);
            emit2(p, p->comp->is_cell[sl] ? OP_SETCELL : OP_SETLOCAL, (uint8_t)sl);
            return;
        }
        int g = global_slot(p, tk[s].start, tk[s].len, false);
        if (g < 0) { perr(p, "del %.*s: there's no %.*s", tk[s].len, tk[s].start, tk[s].len, tk[s].start); return; }
        emit(p, OP_DELNAME);
        emit_u16(p, g);
        return;
    }
    int save = p->cur;
    p->cur = s + 1;
    emit_name_load(p, tk[s].start, tk[s].len);
    for (;;) {
        if (p->failed) break;
        if (p->cur >= e) { perr(p, "can't assign to a call"); break; }
        if (match(p, T_DOT)) {
            if (!check(p, T_IDENT)) { perr(p, "expected a name after '.'"); break; }
            Token *nt = advance(p);
            int k = add_const(p, str_new(nt->start, (size_t)nt->len));
            if (p->cur == e) {
                if (mode == ST_DEL) { emit(p, OP_DELATTR); emit_u16(p, k); }
                else { emit2(p, OP_ROT, 1); emit(p, OP_SETATTR); emit_u16(p, k); }
                break;
            }
            if (check(p, T_LPAREN)) { method_call(p, k); continue; }
            emit(p, OP_GETATTR);
            emit_u16(p, k);
            continue;
        }
        if (check(p, T_LPAREN)) { call_args(p); continue; }
        if (check(p, T_LBRACKET)) {
            int rb = close_of(p, p->cur);
            advance(p);
            bool slice = subscript(p);
            expect(p, T_RBRACKET, "']'");
            if (rb + 1 == e) {
                if (mode == ST_DEL) {
                    if (slice) { emit2(p, OP_LIST, 0); emit(p, OP_SETSLICE); }
                    else emit(p, OP_DELINDEX);
                } else if (slice) { emit2(p, OP_ROT, 4); emit(p, OP_SETSLICE); }
                else { emit2(p, OP_ROT, 2); emit(p, OP_SETINDEX); }
                break;
            }
            emit(p, slice ? OP_SLICE : OP_INDEX);
            continue;
        }
        perr(p, "can't assign to that (only to names, a.b and a[k])");
        break;
    }
    p->cur = save;
}

/* ------------------------------------------------------------ statements */

/* A block is either `{ ... }` or `: NEWLINE INDENT ... DEDENT` or an inline
 * `: statement`. Chosen per block, freely mixable — HAMSH_SPEC 5. */
static void block(P *p)
{
    if (match(p, T_LBRACE)) {
        skip_seps(p);
        while (!check(p, T_RBRACE) && !check(p, T_EOF) && !p->failed) {
            statement(p);
            skip_seps(p);
        }
        expect(p, T_RBRACE, "'}'");
        return;
    }
    if (!check(p, T_COLON)) {
        if (check(p, T_ASSIGN)) perr(p, "'=' sets a name: to compare, it's '==' (if x == 3:)");
        else if (check(p, T_NEWLINE) || check(p, T_EOF)) perr(p, "expected ':' at the end of the line (an if, for, while, def or class line ends with one)");
        else perr(p, "expected ':' to open the block");
        return;
    }
    advance(p);
    if (match(p, T_NEWLINE)) {
        if (!match(p, T_INDENT)) { perr(p, "expected an indented block (the lines after a ':' go further in)"); return; }
        while (!check(p, T_DEDENT) && !check(p, T_EOF) && !p->failed) {
            statement(p);
            while (check(p, T_NEWLINE)) advance(p);
        }
        expect(p, T_DEDENT, "end of indented block");
    } else {
        statement(p);      /* inline single-statement form: `if x: echo big` */
    }
}

static void begin_loop(P *p, Loop *l, int start)
{
    l->start = start;
    l->nexit = 0;
    l->try_base = p->ntry;
    l->prev = p->loop;
    p->loop = l;
}

static void end_loop(P *p, Loop *l)
{
    for (int i = 0; i < l->nexit; i++) patch_jump(p, l->exitpatch[i]);
    p->loop = l->prev;
}

/* a with's manager.__exit__(None, None, None), its answer dropped */
static void emit_with_exit(P *p, const char *name)
{
    emit_name_load(p, name, (int)strlen(name));
    emit(p, OP_NIL);
    emit(p, OP_NIL);
    emit(p, OP_NIL);
    emit(p, OP_CALLMETHOD);
    emit_u16(p, add_const(p, str_newz("__exit__")));
    emit(p, 3);
    emit(p, OP_POP);
}

/* leaving the try levels down to `downto` early (a return, a break): each
 * one's handler goes, a finally's block runs (compiled again here), a with
 * is told it's over */
static void emit_unwind(P *p, int downto)
{
    for (int i = p->ntry - 1; i >= downto && !p->failed; i--) {
        emit(p, OP_ENDTRY);
        TryLevel t = p->trys[i];
        if (t.kind == TRY_FINALLY) {
            TryLevel keep[16];
            int sn = p->ntry, save = p->cur;
            memcpy(keep, p->trys, sizeof keep);
            p->ntry = i;
            p->in_finally++;
            p->cur = t.fin;
            block(p);
            p->in_finally--;
            memcpy(p->trys, keep, sizeof keep);
            p->ntry = sn;
            p->cur = save;
        } else if (t.kind == TRY_WITH) {
            emit_with_exit(p, t.with_name);
        }
    }
}

static void if_stmt(P *p)
{
    expression(p);
    int else_jump = emit_jump(p, OP_JMPF);
    block(p);
    int end_jumps[32], nend = 0;
    for (;;) {
        /* Look past newlines for a continuation keyword, but never past a
         * DEDENT: that one belongs to an enclosing block. */
        int save = p->cur;
        while (check(p, T_NEWLINE)) advance(p);
        if (check(p, T_ELIF)) {
            advance(p);
            if (nend < 32) end_jumps[nend++] = emit_jump(p, OP_JMP);
            patch_jump(p, else_jump);
            expression(p);
            else_jump = emit_jump(p, OP_JMPF);
            block(p);
            continue;
        }
        if (check(p, T_ELSE)) {
            advance(p);
            if (check(p, T_IF)) { perr(p, "'else if' is spelled 'elif' in Python"); return; }
            if (nend < 32) end_jumps[nend++] = emit_jump(p, OP_JMP);
            patch_jump(p, else_jump);
            else_jump = -1;
            block(p);
            break;
        }
        p->cur = save;
        break;
    }
    if (else_jump >= 0) patch_jump(p, else_jump);
    for (int i = 0; i < nend; i++) patch_jump(p, end_jumps[i]);
}

static void while_stmt(P *p)
{
    int start = cur_chunk(p)->len;
    expression(p);
    int exit_jump = emit_jump(p, OP_JMPF);
    Loop l;
    begin_loop(p, &l, start);
    block(p);
    emit_loop(p, start);
    patch_jump(p, exit_jump);
    /* while ... else: runs when the loop ends without a break */
    int save = p->cur;
    while (check(p, T_NEWLINE)) advance(p);
    if (match(p, T_ELSE)) { p->loop = l.prev; block(p); p->loop = &l; } else p->cur = save;
    end_loop(p, &l);
}

/* for TARGETS in EXPR: body
 *
 * Stack layout during the loop is [seq, index]; OP_FORNEXT pushes the next
 * element or jumps past the two cleanup POPs. `break` therefore jumps to the
 * cleanup, which is why it does not need to know the loop's stack depth.
 * TUX ROCKET SCIENCE: the targets are anything an = takes (for i, (k, v) in
 * enumerate(d.items())), stored each time round. */
static void for_stmt(P *p)
{
    int ts = p->cur;
    int in = find_ahead(p, T_IN);
    if (in < 0) { perr(p, "expected 'in' in the for"); return; }
    p->cur = in + 1;
    expr_list(p);
    emit(p, OP_FORPREP);                 /* pushes the index 0 */

    int start = cur_chunk(p)->len;
    int exit_jump = emit_jump(p, OP_FORNEXT);
    store_target(p, ts, in, ST_STORE);

    Loop l;
    begin_loop(p, &l, start);
    block(p);
    emit_loop(p, start);
    patch_jump(p, exit_jump);
    int save = p->cur;
    while (check(p, T_NEWLINE)) advance(p);
    if (match(p, T_ELSE)) {
        /* for ... else: runs when the loop ends without a break */
        emit(p, OP_POP);
        emit(p, OP_POP);
        p->loop = l.prev;
        block(p);
        p->loop = &l;
        int jend = emit_jump(p, OP_JMP);
        end_loop(p, &l);                 /* (a break comes here) */
        emit(p, OP_POP);
        emit(p, OP_POP);
        patch_jump(p, jend);
        return;
    }
    p->cur = save;
    end_loop(p, &l);
    emit(p, OP_POP);                     /* index */
    emit(p, OP_POP);                     /* seq */
}

static void def_body(P *p, bool keep);
static void def_stmt(P *p) { def_body(p, false); }

/* @decorator lines, then the def they wrap: its value is decorator(def) */
static int decorators(P *p)
{
    int nd = 0;
    do {
        expression(p);
        nd++;
        while (check(p, T_NEWLINE)) advance(p);
    } while (match(p, T_AT) && !p->failed);
    if (!check(p, T_DEF)) perr(p, check(p, T_CLASS) ? "@decorators on a class aren't supported" : "expected a def after the @decorator");
    return nd;
}

/* class PID(Base):  defs and name = value, a class's own names */
static void class_stmt(P *p)
{
    if (!p->comp->toplevel) { perr(p, "a class goes at the top of the script, not inside a def"); return; }
    if (!check(p, T_IDENT)) { perr(p, "expected a class name"); return; }
    Token nameTok = *advance(p);
    int hasbase = 0;
    if (match(p, T_LPAREN)) {
        if (!check(p, T_RPAREN)) {
            int bs = p->cur;
            bool object = check(p, T_IDENT) && peek(p)->len == 6 && !memcmp(peek(p)->start, "object", 6) && p->tl.tok[p->cur + 1].type == T_RPAREN;
            if (object) advance(p);
            else { expression(p); hasbase = 1; }
            if (check(p, T_COMMA)) { p->cur = bs; perr(p, "a class builds on one class here (no multiple bases)"); return; }
        }
        expect(p, T_RPAREN, "')'");
    }
    emit(p, OP_CLASS);
    emit_u16(p, add_const(p, str_new(nameTok.start, (size_t)nameTok.len)));
    emit(p, (uint8_t)hasbase);
    expect(p, T_COLON, "':' after the class name");
    if (!match(p, T_NEWLINE) || !match(p, T_INDENT)) { perr(p, "expected the class's defs, indented below it"); return; }
    char kname[NOM_NAME_MAX];
    snprintf(kname, sizeof kname, "%.*s", nameTok.len, nameTok.start);
    const char *was = p->klass;
    p->klass = kname;
    while (!check(p, T_DEDENT) && !check(p, T_EOF) && !p->failed) {
        while (check(p, T_NEWLINE)) advance(p);
        if (check(p, T_DEDENT) || check(p, T_EOF)) break;
        if (match(p, T_PASS)) continue;
        if (check(p, T_STR)) { advance(p); continue; }          /* a docstring */
        if (check(p, T_DEF) || check(p, T_AT)) {
            emit(p, OP_DUP);
            int nd = 0;
            if (match(p, T_AT)) nd = decorators(p);
            if (!match(p, T_DEF)) { perr(p, "expected a def"); break; }
            if (!check(p, T_IDENT)) { perr(p, "expected a method name"); break; }
            Token mt = *peek(p);
            def_body(p, true);
            for (int i = 0; i < nd; i++) emit2(p, OP_CALL, 1);
            emit_const(p, str_new(mt.start, (size_t)mt.len));
            emit2(p, OP_ROT, 1);
            emit(p, OP_SETINDEX);
            continue;
        }
        if (check(p, T_IDENT) && (p->tl.tok[p->cur + 1].type == T_ASSIGN || p->tl.tok[p->cur + 1].type == T_COLON)) {
            /* count = 0 (and count: int = 0, or just count: int) */
            Token at = *advance(p);
            if (match(p, T_COLON)) {
                skip_annotation(p, T_NEWLINE);
                if (!match(p, T_ASSIGN)) continue;
            } else advance(p);
            emit(p, OP_DUP);
            emit_const(p, str_new(at.start, (size_t)at.len));
            expression(p);
            emit(p, OP_SETINDEX);
            continue;
        }
        perr(p, "in a class: methods (def) and names (x = 1) only");
        break;
    }
    p->klass = was;
    expect(p, T_DEDENT, "the end of the class");
    emit_name_store(p, nameTok.start, nameTok.len);
}

/* def NAME(params): body -- and with keep, its value left on the stack
 * (a method, a decorated def) rather than given its name */
static void def_body(P *p, bool keep)
{
    if (!check(p, T_IDENT)) { perr(p, "expected a function name"); return; }
    Token nameTok = *advance(p);
    if (p->prog->nfuncs >= NOM_FUNCS_MAX) { perr(p, "too many functions"); return; }

    int fi = p->prog->nfuncs++;
    Func *fn = &p->prog->func[fi];
    memset(fn, 0, sizeof *fn);
    int nlen = nameTok.len < NOM_NAME_MAX - 1 ? nameTok.len : NOM_NAME_MAX - 1;
    memcpy(fn->name, nameTok.start, (size_t)nlen);

    Compiler c;
    memset(&c, 0, sizeof c);
    c.fn = fn;
    c.toplevel = false;
    c.enclosing = p->comp;
    c.try_base = p->ntry;
    if (p->klass && p->comp->toplevel) { snprintf(c.klass, sizeof c.klass, "%s", p->klass); fn->is_method = true; }

    /* bind the name before compiling the body so recursion resolves (a def
     * inside a def is that one's local, and sees only its own names and the
     * globals) */
    bool top = p->comp->toplevel;
    int g = top && !keep ? global_slot(p, nameTok.start, nameTok.len, true) : -1;

    /* the body: what of it a def inside it uses lives in cells */
    if (check(p, T_LPAREN)) {
        int rp = close_of(p, p->cur);
        int colon = find_from(p, rp + 1, T_COLON);
        if (colon >= 0) prescan_cells(p, &c, colon, block_end(p, colon));
    }

    p->comp = &c;
    expect(p, T_LPAREN, "'(' after function name");
    int dflt[NOM_ARGS_MAX], nd = 0;
    params(p, fn, T_RPAREN, dflt, &nd);
    expect(p, T_RPAREN, "')'");
    if (match(p, T_ARROW)) skip_annotation(p, T_COLON);
    block(p);
    emit(p, OP_NIL);
    emit(p, OP_RET);
    p->comp = c.enclosing;
    /* the names of its locals, for the debugger's watch */
    {
        Buf nb; buf_init(&nb);
        for (int i = 0; i < c.nlocals; i++) { if (i) buf_putc(&nb, ','); buf_puts(&nb, c.local[i]); }
        buf_putc(&nb, 0);
        fn->locals = nb.p;
    }
    emit_defaults(p, fi, dflt, nd);
    emit_function(p, fi, &c);
    if (keep) return;                   /* (a method: the class keeps it) */
    if (top) {
        emit(p, OP_SETGLOBAL);
        emit_u16(p, g);
    } else {
        emit_name_store(p, nameTok.start, nameTok.len);
    }
}

static int aug_op(TokType t)
{
    switch (t) {
    case T_PLUSEQ: return OP_ADD;    case T_MINUSEQ: return OP_SUB;
    case T_STAREQ: return OP_MUL;    case T_SLASHEQ: return OP_DIV;
    case T_DSLASHEQ: return OP_FDIV; case T_PERCENTEQ: return OP_MOD;
    case T_DSTAREQ: return OP_POW;   case T_AMPEQ: return OP_BAND;
    case T_PIPEEQ: return OP_BOR;    case T_CARETEQ: return OP_BXOR;
    case T_SHLEQ: return OP_SHL;     case T_SHREQ: return OP_SHR;
    default: return -1;
    }
}

/* where this statement's =s are (outside brackets, before a lambda),
 * whether it's an x op= y, and an annotation's colon (x: float = 1) */
static int assign_scan(P *p, int *eqs, int max, int *aug, int *colon)
{
    int n = 0, depth = 0;
    *aug = *colon = -1;
    for (int i = p->cur; i < p->tl.ntok; i++) {
        TokType t = p->tl.tok[i].type;
        if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) depth++;
        else if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) { if (!depth) break; depth--; }
        else if (!depth) {
            if (t == T_NEWLINE || t == T_EOF || t == T_DEDENT || t == T_INDENT || t == T_LAMBDA) break;
            if (t == T_ASSIGN) { if (n < max) eqs[n++] = i; }
            else if (aug_op(t) >= 0) { if (!n && *aug < 0) *aug = i; break; }
            else if (t == T_COLON && !n && *colon < 0) *colon = i;
        }
    }
    return n;
}

/* the end of a simple statement, or a mistake */
static void end_of_line(P *p)
{
    if (check(p, T_NEWLINE) || check(p, T_EOF) || check(p, T_DEDENT) || check(p, T_RBRACE)) return;
    Token *pv = prev(p);
    TokType t = peek(p)->type;
    if (pv->type == T_IDENT && (t == T_STR || t == T_FSTR || t == T_INT || t == T_NUM || t == T_IDENT))
        perr(p, "a call needs brackets: %.*s(...)", pv->len, pv->start);
    else if (t == T_IDENT || t == T_INT || t == T_NUM || t == T_STR)
        perr(p, "two things side by side with nothing between (a ',' or an operator missing?)");
    else
        perr(p, "expected the end of the line here");
}

/* x op= value: the target worked out once, its value, the op, stored */
static void aug_assign(P *p, int at)
{
    int op = aug_op(p->tl.tok[at].type);
    int s = p->cur;
    Token *tk = p->tl.tok;
    if (tk[s].type != T_IDENT) { perr(p, "can't do op= to that (only to names, a.b and a[k])"); return; }
    if (at == s + 1) {
        Token nm = tk[s];
        p->cur = at + 1;
        emit_name_load(p, nm.start, nm.len);
        expression(p);
        emit(p, (uint8_t)op);
        emit_name_store(p, nm.start, nm.len);
        end_of_line(p);
        return;
    }
    /* NAME[k].b[j] op= value: everything up to the last [..] or .name is
     * read, the last one read again and written */
    p->cur = s + 1;
    emit_name_load(p, tk[s].start, tk[s].len);
    int attr = -1;
    bool slice = false;
    for (;;) {
        if (p->failed) return;
        if (match(p, T_DOT)) {
            if (!check(p, T_IDENT)) { perr(p, "expected a name after '.'"); return; }
            Token *nt = advance(p);
            int k = add_const(p, str_new(nt->start, (size_t)nt->len));
            if (p->cur == at) { attr = k; break; }
            if (check(p, T_LPAREN)) { method_call(p, k); continue; }
            emit(p, OP_GETATTR);
            emit_u16(p, k);
            continue;
        }
        if (check(p, T_LPAREN)) { call_args(p); continue; }
        if (check(p, T_LBRACKET)) {
            int rb = close_of(p, p->cur);
            advance(p);
            bool sl = subscript(p);
            expect(p, T_RBRACKET, "']'");
            if (rb + 1 == at) { slice = sl; break; }
            emit(p, sl ? OP_SLICE : OP_INDEX);
            continue;
        }
        perr(p, "expected op= after the name");
        return;
    }
    if (slice) { perr(p, "op= on a slice isn't supported: x[a:b] = x[a:b] + ..."); return; }
    if (p->cur != at) { perr(p, "expected op= after the name"); return; }
    p->cur = at + 1;
    if (attr >= 0) { emit(p, OP_DUP); emit(p, OP_GETATTR); emit_u16(p, attr); }
    else { emit(p, OP_DUP2); emit(p, OP_INDEX); }
    expression(p);
    emit(p, (uint8_t)op);
    if (attr >= 0) { emit(p, OP_SETATTR); emit_u16(p, attr); }
    else emit(p, OP_SETINDEX);
    end_of_line(p);
}

/* a = b = value, a, b = value, x.y[k] = value: the value worked out first,
 * then given to each target in turn (their text visited out of order) */
static void plain_assign(P *p, int *eqs, int neq)
{
    int s0 = p->cur;
    p->cur = eqs[neq - 1] + 1;
    if (check(p, T_YIELD)) { perr(p, "x = yield isn't supported: yield on a line of its own"); return; }
    if (check(p, T_NEWLINE) || check(p, T_EOF)) { perr(p, "expected a value after '='"); return; }
    expr_list(p);
    int end = p->cur;
    end_of_line(p);
    for (int i = 0; i < neq && !p->failed; i++) {
        int ts = i == 0 ? s0 : eqs[i - 1] + 1;
        if (i < neq - 1) emit(p, OP_DUP);
        store_target(p, ts, eqs[i], ST_STORE);
    }
    p->cur = end;
}

/* import math [as m] -- the ship's modules: math, time, random */
static bool module_ok(P *p, Token *t)
{
    static const char *MODS[] = { "math", "time", "random", NULL };
    for (int i = 0; MODS[i]; i++)
        if ((int)strlen(MODS[i]) == t->len && !memcmp(MODS[i], t->start, (size_t)t->len)) return true;
    perr(p, "no module called '%.*s' here (the ship has math, time and random)", t->len, t->start);
    return false;
}

static void emit_module(P *p, Token *m)
{
    int k = native_find("_module", 7);
    emit2(p, OP_GETNATIVE, (uint8_t)k);
    emit_const(p, str_new(m->start, (size_t)m->len));
    emit2(p, OP_CALL, 1);
}

static void import_stmt(P *p)
{
    do {
        if (!check(p, T_IDENT)) { perr(p, "expected a module name"); return; }
        Token m = *advance(p);
        if (!module_ok(p, &m)) return;
        Token as = m;
        if (match(p, T_AS)) as = *advance(p);
        emit_module(p, &m);
        emit_name_store(p, as.start, as.len);
    } while (match(p, T_COMMA));
}

/* from math import sqrt, pi as PI */
static void from_stmt(P *p)
{
    if (!check(p, T_IDENT)) { perr(p, "expected a module name"); return; }
    Token m = *advance(p);
    if (!module_ok(p, &m)) return;
    expect(p, T_IMPORT, "'import'");
    if (match(p, T_STAR)) { perr(p, "'import *' isn't supported: name what you want (from math import sqrt, pi)"); return; }
    bool paren = match(p, T_LPAREN);
    do {
        if (!check(p, T_IDENT)) { perr(p, "expected a name to import"); return; }
        Token nm = *advance(p);
        Token as = nm;
        if (match(p, T_AS)) as = *advance(p);
        emit_module(p, &m);
        emit_const(p, str_new(nm.start, (size_t)nm.len));
        emit(p, OP_INDEX);
        emit_name_store(p, as.start, as.len);
    } while (match(p, T_COMMA));
    if (paren) expect(p, T_RPAREN, "')'");
}

/* try: ... except Kind [as e]: ... else: ... finally: ...
 *
 * A finally's block is compiled twice here (nothing went wrong / something
 * did, and then the error goes on up), and once more wherever the try is
 * left early -- each return or break in it unwinds (emit_unwind). An except
 * with no kind is `except Exception` (Ctrl+C, a KeyboardInterrupt, still
 * stops the script). */
static void try_stmt(P *p)
{
    if (!check(p, T_COLON)) { perr(p, "expected ':' after try"); return; }
    /* where its clauses are */
    int i = block_end(p, p->cur), fin = -1;
    bool has_exc = false, has_else = false;
    for (;;) {
        while (p->tl.tok[i].type == T_NEWLINE) i++;
        TokType t = p->tl.tok[i].type;
        if (t == T_EXCEPT) {
            int c2 = find_from(p, i, T_COLON);
            if (c2 < 0) { perr(p, "expected ':' after except"); return; }
            has_exc = true;
            i = block_end(p, c2);
            continue;
        }
        if (t == T_ELSE && has_exc && !has_else) { has_else = true; i = block_end(p, i + 1); continue; }
        if (t == T_FINALLY) fin = i + 1;
        break;
    }
    if (!has_exc && fin < 0) { perr(p, "expected 'except' or 'finally' after the try block"); return; }
    if (p->ntry + 2 > 16) { perr(p, "try blocks nested too deep"); return; }
    int F = -1, H = -1;
    if (fin >= 0) { p->trys[p->ntry++] = (TryLevel){ TRY_FINALLY, fin, "" }; F = emit_jump(p, OP_TRY); }
    if (has_exc) { p->trys[p->ntry++] = (TryLevel){ TRY_EXCEPT, 0, "" }; H = emit_jump(p, OP_TRY); }
    p->in_try++;
    block(p);
    p->in_try--;
    if (has_exc) {
        p->ntry--;
        emit(p, OP_ENDTRY);
        int over = emit_jump(p, OP_JMP);   /* past the excepts, to the else */
        patch_jump(p, H);
        /* the error is on the stack; each except asks if it's its kind, and
         * the first that is takes it */
        int ends[16], ne = 0;
        for (;;) {
            int save = p->cur;
            while (check(p, T_NEWLINE)) advance(p);
            if (!match(p, T_EXCEPT)) { p->cur = save; break; }
            bool bare = check(p, T_COLON);
            if (bare) emit_name_load(p, "Exception", 9);
            else expression(p);           /* the kind, or a tuple of them */
            emit(p, OP_EXCMATCH);
            int next = emit_jump(p, OP_JMPF);
            if (match(p, T_AS)) {
                if (!check(p, T_IDENT)) { perr(p, "expected a name after 'as'"); return; }
                Token e = *advance(p);
                emit_name_store(p, e.start, e.len);
            } else emit(p, OP_POP);
            block(p);
            if (ne < 16) ends[ne++] = emit_jump(p, OP_JMP);
            patch_jump(p, next);
            if (bare) break;
        }
        emit(p, OP_RAISE);                /* none of its kinds: on up, as it was */
        patch_jump(p, over);
        int save = p->cur;
        while (check(p, T_NEWLINE)) advance(p);
        if (match(p, T_ELSE)) block(p); else p->cur = save;
        for (int k = 0; k < ne; k++) patch_jump(p, ends[k]);
    }
    if (fin >= 0) {
        p->ntry--;
        emit(p, OP_ENDTRY);
        while (check(p, T_NEWLINE)) advance(p);
        if (!match(p, T_FINALLY)) { perr(p, "expected 'finally'"); return; }
        int at = p->cur;
        p->in_finally++;
        block(p);                         /* when nothing went wrong */
        int end = emit_jump(p, OP_JMP);
        patch_jump(p, F);                 /* when something did: [the error] */
        int after = p->cur;
        p->cur = at;
        block(p);
        p->in_finally--;
        p->cur = after;
        emit(p, OP_RAISE);
        patch_jump(p, end);
    }
}

/* with open(path) as f: ... -- f is what its __enter__() gives, and its
 * __exit__ is called however the block is left (with the error, if one is
 * why; an __exit__ that answers True keeps it) */
static void with_stmt(P *p)
{
    expression(p);
    char nm[12];
    snprintf(nm, sizeof nm, "$w%d", ++p->hidden);
    emit(p, OP_DUP);
    emit_name_store(p, nm, (int)strlen(nm));
    emit(p, OP_CALLMETHOD);
    emit_u16(p, add_const(p, str_newz("__enter__")));
    emit(p, 0);
    if (match(p, T_AS)) {
        int ts = p->cur, te = ts, depth = 0;
        for (; te < p->tl.ntok; te++) {
            TokType t = p->tl.tok[te].type;
            if (t == T_LPAREN || t == T_LBRACKET) depth++;
            else if (t == T_RPAREN || t == T_RBRACKET) depth--;
            else if (!depth && (t == T_COMMA || t == T_COLON || t == T_NEWLINE || t == T_EOF)) break;
        }
        store_target(p, ts, te, ST_STORE);
        p->cur = te;
    } else emit(p, OP_POP);
    if (p->ntry >= 16) { perr(p, "with and try blocks nested too deep"); return; }
    TryLevel t = { TRY_WITH, 0, "" };
    snprintf(t.with_name, sizeof t.with_name, "%s", nm);
    p->trys[p->ntry++] = t;
    int T = emit_jump(p, OP_TRY);
    if (match(p, T_COMMA)) with_stmt(p); else block(p);
    p->ntry--;
    emit(p, OP_ENDTRY);
    emit_with_exit(p, nm);
    int end = emit_jump(p, OP_JMP);
    patch_jump(p, T);                    /* [the error] */
    emit_name_load(p, nm, (int)strlen(nm));
    emit(p, OP_WITHEXIT);
    patch_jump(p, end);
}

static void statement(P *p)
{
    /* Callers own separator handling; consuming a DEDENT here would steal the
     * one that terminates an enclosing indented block. */
    while (check(p, T_NEWLINE)) advance(p);
    if (check(p, T_EOF) || check(p, T_RBRACE) || check(p, T_DEDENT)) return;
    if (check(p, T_INDENT)) { perr(p, "unexpected indent: this line is further in than the one above it"); return; }

    if (match(p, T_IF))    { if_stmt(p);    return; }
    if (match(p, T_WHILE)) { while_stmt(p); return; }
    if (match(p, T_FOR))   { for_stmt(p);   return; }
    if (match(p, T_DEF))   { def_stmt(p);   return; }
    if (match(p, T_CLASS)) { class_stmt(p); return; }
    if (match(p, T_PASS))  { return; }
    if (match(p, T_AT)) {
        int nd = decorators(p);
        if (p->failed) return;
        advance(p);                       /* the def */
        Token nm = *peek(p);
        def_body(p, true);
        for (int i = 0; i < nd; i++) emit2(p, OP_CALL, 1);
        emit_name_store(p, nm.start, nm.len);
        return;
    }

    if (match(p, T_BREAK)) {
        if (!p->loop) { perr(p, "'break' outside a loop"); return; }
        if (p->in_finally) { perr(p, "'break' in a finally isn't supported"); return; }
        emit_unwind(p, p->loop->try_base);
        if (p->loop->nexit < 32) p->loop->exitpatch[p->loop->nexit++] = emit_jump(p, OP_JMP);
        else perr(p, "too many breaks in one loop");
        return;
    }
    if (match(p, T_CONTINUE)) {
        if (!p->loop) { perr(p, "'continue' outside a loop"); return; }
        if (p->in_finally) { perr(p, "'continue' in a finally isn't supported"); return; }
        emit_unwind(p, p->loop->try_base);
        emit_loop(p, p->loop->start);
        return;
    }
    if (match(p, T_RETURN)) {
        if (p->in_finally) { perr(p, "'return' in a finally isn't supported"); return; }
        if (p->comp->toplevel) { perr(p, "'return' goes inside a def (to stop the script, exit())"); return; }
        if (check(p, T_NEWLINE) || check(p, T_RBRACE) || check(p, T_EOF) || check(p, T_DEDENT)) emit(p, OP_NIL);
        else expr_list(p);
        emit_unwind(p, p->comp->try_base);
        emit(p, OP_RET);
        return;
    }
    /* TUX ROCKET SCIENCE: the rest of the statements a Python hand writes */
    if (match(p, T_GLOBAL)) {
        do {
            if (!check(p, T_IDENT)) { perr(p, "expected a name after 'global'"); return; }
            Token *nt = advance(p);
            if (p->comp->toplevel) continue;            /* (already global there) */
            if (p->comp->nglobal >= 16) { perr(p, "too many globals in one def"); return; }
            int len = nt->len < NOM_NAME_MAX - 1 ? nt->len : NOM_NAME_MAX - 1;
            memcpy(p->comp->global[p->comp->nglobal], nt->start, (size_t)len);
            p->comp->global[p->comp->nglobal++][len] = 0;
            global_slot(p, nt->start, nt->len, true);
        } while (match(p, T_COMMA));
        return;
    }
    if (match(p, T_NONLOCAL)) {
        if (p->comp->toplevel) { perr(p, "'nonlocal' goes inside a def inside a def (at the top, it's global)"); return; }
        do {
            if (!check(p, T_IDENT)) { perr(p, "expected a name after 'nonlocal'"); return; }
            Token *nt = advance(p);
            if (p->comp->nnonlocal >= 16 || nt->len >= NOM_NAME_MAX) { perr(p, "too many nonlocal names"); return; }
            if (!outer_has(p->comp, nt->start, nt->len)) { perr(p, "nonlocal %.*s: no def around this one has a %.*s", nt->len, nt->start, nt->len, nt->start); return; }
            memcpy(p->comp->nonlocal[p->comp->nnonlocal], nt->start, (size_t)nt->len);
            p->comp->nonlocal[p->comp->nnonlocal++][nt->len] = 0;
        } while (match(p, T_COMMA));
        return;
    }
    if (match(p, T_IMPORT)) { import_stmt(p); return; }
    if (match(p, T_ASSERT)) {
        /* assert cond, "why": stops the script, saying why, if cond isn't so */
        expression(p);
        int ok = emit_jump(p, OP_JMPT_KEEP);
        emit(p, OP_POP);
        emit_name_load(p, "AssertionError", 14);
        if (match(p, T_COMMA)) { expression(p); emit2(p, OP_CALL, 1); }
        else emit2(p, OP_CALL, 0);
        emit(p, OP_RAISE);
        patch_jump(p, ok);
        emit(p, OP_POP);
        return;
    }
    if (match(p, T_FROM)) { from_stmt(p); return; }
    if (match(p, T_TRY)) { try_stmt(p); return; }
    if (match(p, T_WITH)) { with_stmt(p); return; }
    if (match(p, T_RAISE)) {
        if (check(p, T_NEWLINE) || check(p, T_EOF) || check(p, T_DEDENT)) { emit(p, OP_RERAISE); return; }
        expression(p);
        if (match(p, T_FROM)) { expression(p); emit(p, OP_POP); }   /* (raise X from e: the X) */
        emit(p, OP_RAISE);
        return;
    }
    if (match(p, T_DEL)) {
        int s = p->cur, e = s, depth = 0;
        for (; e < p->tl.ntok; e++) {
            TokType t = p->tl.tok[e].type;
            if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) depth++;
            else if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) depth--;
            else if (!depth && (t == T_NEWLINE || t == T_EOF || t == T_DEDENT)) break;
        }
        store_target(p, s, e, ST_DEL);
        p->cur = e;
        return;
    }
    if (match(p, T_YIELD)) {
        if (p->comp->toplevel) { perr(p, "'yield' goes inside a def (it makes that def a generator)"); return; }
        p->comp->fn->is_gen = true;
        if (match(p, T_FROM)) {
            /* yield from xs: each of them */
            expression(p);
            emit(p, OP_FORPREP);
            int start = cur_chunk(p)->len;
            int ex = emit_jump(p, OP_FORNEXT);
            emit(p, OP_YIELD);
            emit_loop(p, start);
            patch_jump(p, ex);
            emit(p, OP_POP);
            emit(p, OP_POP);
            return;
        }
        if (check(p, T_NEWLINE) || check(p, T_EOF) || check(p, T_DEDENT)) emit(p, OP_NIL);
        else expr_list(p);
        emit(p, OP_YIELD);
        return;
    }
    if (check(p, T_EXCEPT) || check(p, T_FINALLY)) {
        perr(p, check(p, T_EXCEPT) ? "'except' without a 'try' above it" : "'finally' without a 'try' above it");
        return;
    }

    if (check(p, T_IDENT) && p->tl.tok[p->cur + 1].type == p->tl.tok[p->cur + 2].type &&
        (p->tl.tok[p->cur + 1].type == T_PLUS || p->tl.tok[p->cur + 1].type == T_MINUS) &&
        (p->tl.tok[p->cur + 3].type == T_NEWLINE || p->tl.tok[p->cur + 3].type == T_EOF)) {
        Token *nt = peek(p);
        perr(p, "Python has no %s: write %.*s %s= 1", p->tl.tok[p->cur + 1].type == T_PLUS ? "++" : "--", nt->len, nt->start,
             p->tl.tok[p->cur + 1].type == T_PLUS ? "+" : "-");
        return;
    }
    {
        int eqs[8], aug, colon;
        int neq = assign_scan(p, eqs, 8, &aug, &colon);
        if (aug >= 0) { aug_assign(p, aug); return; }
        if (colon >= 0) {
            /* x: float = 1.0 (the annotation skipped), or just x: float */
            int s = p->cur;
            if (neq) {
                p->cur = eqs[0] + 1;
                expr_list(p);
                int end = p->cur;
                store_target(p, s, colon, ST_STORE);
                p->cur = end;
            } else {
                while (!check(p, T_NEWLINE) && !check(p, T_EOF) && !check(p, T_DEDENT)) advance(p);
            }
            return;
        }
        if (neq) { plain_assign(p, eqs, neq); return; }
    }

    if (p->echo && p->comp->toplevel && !p->loop) {
        int k = native_find("_echo", 5);
        if (k >= 0) {
            emit2(p, OP_GETNATIVE, (uint8_t)k);
            expression(p);
            emit2(p, OP_CALL, 1);
            emit(p, OP_POP);
            end_of_line(p);
            return;
        }
    }
    expression(p);
    emit(p, OP_POP);
    end_of_line(p);
}

/* -------------------------------------------------------------- entry pt */
Prog *prog_compile(const char *src, const char *name, char *err, size_t errsz)
{
    P p;
    memset(&p, 0, sizeof p);
    p.err = err;
    p.errsz = errsz;
    if (err) err[0] = 0;

    if (!lex_source(src, &p.tl, err, errsz)) return NULL;

    Prog *pr = nom_alloc(sizeof(Prog));
    snprintf(pr->name, sizeof pr->name, "%s", name ? name : "script");
    pr->nfuncs = 1;
    snprintf(pr->func[0].name, NOM_NAME_MAX, "%s", "<main>");
    p.prog = pr;

    Compiler c;
    memset(&c, 0, sizeof c);
    c.fn = &pr->func[0];
    c.toplevel = true;
    p.comp = &c;
    /* the exceptions (and __name__): the first globals, set by vm_new */
    for (int i = 0; i < NEXC; i++) global_slot(&p, EXC_NAME[i], (int)strlen(EXC_NAME[i]), true);
    global_slot(&p, "__name__", 8, true);

    skip_seps(&p);
    while (!check(&p, T_EOF) && !p.failed) {
        statement(&p);
        skip_seps(&p);
    }
    emit(&p, OP_HALT);

    lex_free(&p.tl);
    if (p.failed) { prog_free(pr); return NULL; }
    return pr;
}

int prog_compile_more(Prog *pr, const char *src, bool echo, char *err, size_t errsz)
{
    P p;
    memset(&p, 0, sizeof p);
    p.err = err;
    p.errsz = errsz;
    p.echo = echo;
    if (err) err[0] = 0;
    if (pr->nfuncs >= NOM_FUNCS_MAX) {
        snprintf(err, errsz, "the console is full (%d chunks): start it again", NOM_FUNCS_MAX);
        return -1;
    }
    if (!lex_source(src, &p.tl, err, errsz)) return -1;
    int fi = pr->nfuncs++;
    Func *fn = &pr->func[fi];
    memset(fn, 0, sizeof *fn);
    snprintf(fn->name, NOM_NAME_MAX, "%s", "<console>");
    p.prog = pr;
    Compiler c;
    memset(&c, 0, sizeof c);
    c.fn = fn;
    c.toplevel = true;
    p.comp = &c;
    skip_seps(&p);
    while (!check(&p, T_EOF) && !p.failed) {
        statement(&p);
        skip_seps(&p);
    }
    emit(&p, OP_HALT);
    lex_free(&p.tl);
    if (p.failed) return -1;     /* (the chunk stays, unrun: names it bound stay unassigned) */
    return fi;
}

void prog_free(Prog *p)
{
    if (!p) return;
    for (int i = 0; i < p->nfuncs; i++) {
        nom_free(p->func[i].chunk.code);
        nom_free(p->func[i].chunk.line);
        nom_free(p->func[i].locals);
    }
    for (int i = 0; i < p->nconsts; i++) val_release(p->consts[i]);
    nom_free(p);
}
