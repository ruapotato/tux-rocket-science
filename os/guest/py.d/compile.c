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
} Compiler;

typedef struct Loop {
    int  start;            /* continue target */
    int  stack;            /* TUX ROCKET SCIENCE: values a for loop keeps on the stack (break pops them) */
    int  exitpatch[32];
    int  nexit;
    struct Loop *prev;
} Loop;

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
    return c->nlocals++;
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
        if (s >= 0) { emit2(p, OP_GETLOCAL, (uint8_t)s); return; }
        /* TUX ROCKET SCIENCE: a name of the def this one is inside (a
         * lambda in sorted() using its function's target): captured, as it
         * is when this one is made, into a local of its own */
        Compiler *c = p->comp;
        if (outer_has(c, name, len) && c->ncap < 16 && len < NOM_NAME_MAX) {
            s = add_local(p, name, len);
            c->fn->cap_slot[c->fn->ncap++] = s;
            memcpy(c->cap[c->ncap], name, (size_t)len);
            c->cap[c->ncap++][len] = 0;
            emit2(p, OP_GETLOCAL, (uint8_t)s);
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
        int s = add_local(p, name, len);
        emit2(p, OP_SETLOCAL, (uint8_t)s);
        return;
    }
    int g = global_slot(p, name, len, true);
    emit(p, OP_SETGLOBAL);
    emit_u16(p, g);
}

/* ---------------------------------------------------------- expressions */
static void comprehension(P *p, TokType closer, int for_at);
static void or_expr(P *p);

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

static int arg_list(P *p, TokType closer)
{
    int n = 0, k = 0;
    if (!check(p, closer)) {
        /* sum(x * x for x in xs): one generator argument, as a list */
        int fa = find_ahead(p, T_FOR);
        if (fa >= 0 && closer == T_RPAREN) { comprehension(p, closer, fa); return 1; }
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
    }
    expect(p, closer, closer == T_RPAREN ? "')'" : "']'");
    if (k) emit2(p, OP_KWARGS, (uint8_t)k);
    return n;
}

/* [f(x) for x in xs if g(x)] (and the generator in sum(...)): a new list,
 * the loop, the filter, then the item -- which comes first in the text,
 * so the tokens are visited out of order (the item's last) */
static void comprehension(P *p, TokType closer, int for_at)
{
    int item = p->cur;
    p->cur = for_at + 1;
    Token names[4];
    int nn = 0;
    do {
        if (!check(p, T_IDENT)) { perr(p, "expected a name after 'for'"); return; }
        if (nn < 4) names[nn++] = *advance(p); else advance(p);
    } while (match(p, T_COMMA));
    expect(p, T_IN, "'in'");
    emit2(p, OP_LIST, 0);
    or_expr(p);
    emit(p, OP_FORPREP);
    int start = cur_chunk(p)->len;
    int exit_jump = emit_jump(p, OP_FORNEXT);
    if (nn > 1) emit2(p, OP_UNPACK, (uint8_t)nn);
    for (int i = 0; i < nn; i++) emit_name_store(p, names[i].start, names[i].len);
    int filt = -1;
    if (match(p, T_IF)) {
        or_expr(p);
        filt = emit_jump(p, OP_JMPF);
    }
    int end = p->cur;
    p->cur = item;
    expression(p);
    if (p->cur != for_at && !p->failed) perr(p, "expected 'for' in the list comprehension");
    emit2(p, OP_APPENDN, 2);
    if (filt >= 0) patch_jump(p, filt);
    emit_loop(p, start);
    patch_jump(p, exit_jump);
    emit(p, OP_POP);
    emit(p, OP_POP);
    p->cur = end;
    expect(p, closer, closer == T_RPAREN ? "')'" : "']'");
}

/* a def's (or a lambda's) value: the function, or with the outer names it
 * uses, a closure of them as they are now */
static void emit_function(P *p, int fi, Compiler *c)
{
    if (!c->ncap) { emit2(p, OP_GETFUNC, (uint8_t)fi); return; }
    for (int i = 0; i < c->ncap; i++) emit_name_load(p, c->cap[i], (int)strlen(c->cap[i]));
    emit(p, OP_CLOSURE);
    emit2(p, (uint8_t)fi, (uint8_t)c->ncap);
}

/* lambda x, y: x * y -- a function with one line, returned */
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
    p->comp = &c;
    if (!check(p, T_COLON)) {
        do {
            if (!check(p, T_IDENT)) { perr(p, "expected a parameter name"); break; }
            Token *pt = advance(p);
            add_local(p, pt->start, pt->len);
            fn->arity++;
        } while (match(p, T_COMMA) && !p->failed);
    }
    expect(p, T_COLON, "':' after lambda's parameters");
    expression(p);
    emit(p, OP_RET);
    Buf nb; buf_init(&nb);
    for (int i = 0; i < c.nlocals; i++) { if (i) buf_putc(&nb, ','); buf_puts(&nb, c.local[i]); }
    buf_putc(&nb, 0);
    fn->locals = nb.p;
    p->comp = c.enclosing;
    emit_function(p, fi, &c);
}

/* TUX ROCKET SCIENCE: f"{needed:.0f} m/s, {name}" -- each {...} is an
 * expression, compiled from its own tokens, and format(value, spec)ed;
 * {{ and }} are braces. The pieces are added up into one string. */
static void fstring(P *p, Token *t)
{
    const char *s = t->sval;
    int n = t->len, line = t->line;
    emit_const(p, str_new("", 0));
    int fmt = native_find("format", 6);
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
        /* the expression runs to the matching }, and a : outside any
         * brackets or quotes starts the format */
        int j = i + 1, depth = 0, colon = -1;
        char quote = 0;
        for (; j < n; j++) {
            char c = s[j];
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '"' || c == '\'') quote = c;
            else if (c == '(' || c == '[' || c == '{') depth++;
            else if ((c == ')' || c == ']' || c == '}') && depth) depth--;
            else if (c == '}') break;
            else if (c == ':' && !depth && colon < 0) colon = j;
        }
        if (j >= n) { perr(p, "f-string: a '{' with no '}'"); break; }
        int e1 = colon >= 0 ? colon : j;
        /* f"{x=}": "x=" and then x as repr shows it */
        bool self_doc = e1 - 1 > i && s[e1 - 1] == '=' && s[e1 - 2] != '=' && s[e1 - 2] != '!' && s[e1 - 2] != '<' && s[e1 - 2] != '>';
        if (self_doc) {
            emit_const(p, str_new(s + i + 1, (size_t)(e1 - i - 1)));
            emit(p, OP_ADD);
            e1--;
            if (colon < 0) fmt = native_find("repr", 4);
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
        if (fmt >= 0) emit2(p, OP_GETNATIVE, (uint8_t)fmt);
        P sub = *p;
        sub.tl = tl;
        sub.cur = 0;
        expression(&sub);
        if (!sub.failed && !check(&sub, T_NEWLINE) && !check(&sub, T_EOF))
            perr(&sub, "f-string: more than one expression in {...}");
        p->failed = sub.failed;
        lex_free(&tl);
        nom_free(src);
        if (p->failed) break;
        if (self_doc && colon < 0) {
            emit2(p, OP_CALL, 1);                 /* repr(x) */
            fmt = native_find("format", 6);
        } else {
            if (colon >= 0) emit_const(p, str_new(s + colon + 1, (size_t)(j - colon - 1)));
            else emit_const(p, str_new("", 0));
            if (fmt >= 0) emit2(p, OP_CALL, 2);
        }
        emit(p, OP_ADD);
        i = j + 1;
    }
    buf_free(&lit);
}

static void primary(P *p)
{
    Token *t = advance(p);
    switch (t->type) {
    case T_INT:   emit_const(p, VAL_INT(t->ival)); return;
    case T_NUM:   emit_const(p, VAL_NUM(t->dval)); return;
    case T_STR:   emit_const(p, str_new(t->sval, (size_t)t->len)); return;
    case T_FSTR:  fstring(p, t); return;
    case T_TRUE:  emit(p, OP_TRUE);  return;
    case T_FALSE: emit(p, OP_FALSE); return;
    case T_NIL:   emit(p, OP_NIL);   return;
    case T_IDENT: emit_name_load(p, t->start, t->len); return;
    case T_LAMBDA: lambda_expr(p); return;
    case T_LPAREN: {
        /* (a, b) is a tuple -- here, a list; () an empty one */
        if (match(p, T_RPAREN)) { emit2(p, OP_TUPLE, 0); return; }
        int fa = find_ahead(p, T_FOR);
        if (fa >= 0) { comprehension(p, T_RPAREN, fa); return; }
        expression(p);
        if (check(p, T_COMMA)) {
            int n = 1;
            while (match(p, T_COMMA) && !p->failed) {
                skip_seps(p);
                if (check(p, T_RPAREN)) break;
                expression(p);
                n++;
            }
            emit2(p, OP_TUPLE, (uint8_t)n);
        }
        expect(p, T_RPAREN, "')'");
        return;
    }
    case T_LBRACKET: {
        int fa = find_ahead(p, T_FOR);
        if (fa >= 0) { comprehension(p, T_RBRACKET, fa); return; }
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
        int fa = find_ahead(p, T_FOR);
        if (fa >= 0) {
            /* {k: v for k in s if c} */
            int item = p->cur;
            p->cur = fa + 1;
            Token names[4];
            int nn = 0;
            do {
                if (!check(p, T_IDENT)) { perr(p, "expected a name after 'for'"); return; }
                if (nn < 4) names[nn++] = *advance(p); else advance(p);
            } while (match(p, T_COMMA));
            expect(p, T_IN, "'in'");
            emit2(p, OP_DICT, 0);
            or_expr(p);
            emit(p, OP_FORPREP);
            int start = cur_chunk(p)->len;
            int exit_jump = emit_jump(p, OP_FORNEXT);
            if (nn > 1) emit2(p, OP_UNPACK, (uint8_t)nn);
            for (int i = 0; i < nn; i++) emit_name_store(p, names[i].start, names[i].len);
            int filt = -1;
            if (match(p, T_IF)) { or_expr(p); filt = emit_jump(p, OP_JMPF); }
            int end = p->cur;
            p->cur = item;
            expression(p);
            expect(p, T_COLON, "':' in a dict comprehension");
            expression(p);
            emit2(p, OP_PUTN, 2);
            if (filt >= 0) patch_jump(p, filt);
            emit_loop(p, start);
            patch_jump(p, exit_jump);
            emit(p, OP_POP);
            emit(p, OP_POP);
            p->cur = end;
            expect(p, T_RBRACE, "'}'");
            return;
        }
        int n = 0;
        if (!check(p, T_RBRACE)) {
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
        }
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
        if (match(p, T_LPAREN)) {
            int n = arg_list(p, T_RPAREN);
            emit2(p, OP_CALL, (uint8_t)n);
        } else if (match(p, T_LBRACKET)) {
            /* x[i], or a slice: x[a:b], x[:b], x[a:], x[::-1] */
            if (check(p, T_COLON)) emit(p, OP_NIL); else expression(p);
            if (match(p, T_COLON)) {
                if (check(p, T_RBRACKET) || check(p, T_COLON)) emit(p, OP_NIL); else expression(p);
                if (match(p, T_COLON) && !check(p, T_RBRACKET)) expression(p); else emit(p, OP_NIL);
                expect(p, T_RBRACKET, "']'");
                emit(p, OP_SLICE);
            } else {
                expect(p, T_RBRACKET, "']'");
                emit(p, OP_INDEX);
            }
        } else if (match(p, T_DOT)) {
            /* TUX ROCKET SCIENCE: engine.throttle, ship.orbit.top,
             * booster.decouple() */
            if (!check(p, T_IDENT)) { perr(p, "expected a name after '.'"); return; }
            Token *nt = advance(p);
            int k = add_const(p, str_new(nt->start, (size_t)nt->len));
            if (match(p, T_LPAREN)) {
                int n = arg_list(p, T_RPAREN);
                emit(p, OP_CALLMETHOD);
                emit_u16(p, k);
                emit(p, (uint8_t)n);
            } else {
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
    sum(p);
    int jumps[16], nj = 0;
    for (;;) {
        int op = cmp_op(p);
        if (op < 0) break;
        int right = p->cur;
        sum(p);
        if (op >= 1000) { emit(p, (uint8_t)(op - 1000)); emit(p, OP_NOT); }
        else emit(p, (uint8_t)op);
        if (p->failed) return;
        /* 0 < x < 5 is 0 < x and x < 5 (x worked out again: the same) */
        if (at_cmp(p) && nj < 16) {
            jumps[nj++] = emit_jump(p, OP_JMPF_KEEP);
            emit(p, OP_POP);
            int save = p->cur;
            p->cur = right;
            sum(p);
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
 * order (TUX ROCKET SCIENCE) */
static void expression(P *p)
{
    int ifat = -1, elseat = -1, depth = 0;
    for (int i = p->cur; i < p->tl.ntok; i++) {
        TokType t = p->tl.tok[i].type;
        if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) depth++;
        else if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) { if (!depth) break; depth--; }
        else if (!depth) {
            if (t == T_NEWLINE || t == T_EOF || t == T_COMMA || t == T_COLON || t == T_FOR || t == T_ASSIGN ||
                t == T_LAMBDA || t == T_INDENT || t == T_DEDENT) break;
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

/* a, b (a return, the right of an =): a list when there's a comma */
static void expr_list(P *p)
{
    expression(p);
    if (!check(p, T_COMMA)) return;
    int n = 1;
    while (match(p, T_COMMA) && !p->failed) {
        if (check(p, T_NEWLINE) || check(p, T_EOF)) break;
        expression(p);
        n++;
    }
    emit2(p, OP_TUPLE, (uint8_t)n);
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
    expect(p, T_COLON, "':' or '{' to open a block");
    if (p->failed) return;
    if (match(p, T_NEWLINE)) {
        if (!match(p, T_INDENT)) { perr(p, "expected an indented block"); return; }
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
    l->prev = p->loop;
    p->loop = l;
}

static void end_loop(P *p, Loop *l)
{
    for (int i = 0; i < l->nexit; i++) patch_jump(p, l->exitpatch[i]);
    p->loop = l->prev;
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

/* for NAME in EXPR: body
 *
 * Stack layout during the loop is [seq, index]; OP_FORNEXT pushes the next
 * element or jumps past the two cleanup POPs. `break` therefore jumps to the
 * cleanup, which is why it does not need to know the loop's stack depth. */
static void for_stmt(P *p)
{
    /* for x in s, and (TUX ROCKET SCIENCE) for i, x in enumerate(s) */
    Token names[4];
    int nn = 0;
    do {
        if (!check(p, T_IDENT)) { perr(p, "expected a loop variable name"); return; }
        if (nn < 4) names[nn++] = *advance(p); else { perr(p, "too many loop names"); return; }
    } while (match(p, T_COMMA));
    expect(p, T_IN, "'in'");
    expression(p);
    emit(p, OP_FORPREP);                 /* pushes the index 0 */

    int start = cur_chunk(p)->len;
    int exit_jump = emit_jump(p, OP_FORNEXT);
    if (nn > 1) emit2(p, OP_UNPACK, (uint8_t)nn);
    for (int i = 0; i < nn; i++) emit_name_store(p, names[i].start, names[i].len);

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

static void def_stmt(P *p)
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

    /* bind the name before compiling the body so recursion resolves (a def
     * inside a def is that one's local, and sees only its own names and the
     * globals) */
    bool top = p->comp->toplevel;
    int g = top ? global_slot(p, nameTok.start, nameTok.len, true) : -1;

    p->comp = &c;
    expect(p, T_LPAREN, "'(' after function name");
    int dflt[NOM_ARGS_MAX], ndflt = 0;   /* where each default's text starts */
    if (!check(p, T_RPAREN)) {
        do {
            if (!check(p, T_IDENT)) { perr(p, "expected a parameter name"); break; }
            Token *pt = advance(p);
            add_local(p, pt->start, pt->len);
            fn->arity++;
            if (fn->arity > NOM_ARGS_MAX) { perr(p, "too many parameters"); break; }
            if (match(p, T_ASSIGN)) {
                /* def f(a, b=10): b's default, worked out when the def runs */
                dflt[ndflt++] = p->cur;
                int depth = 0;
                while (!check(p, T_EOF) && !(depth == 0 && (check(p, T_COMMA) || check(p, T_RPAREN)))) {
                    TokType t = peek(p)->type;
                    if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) depth++;
                    if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) depth--;
                    advance(p);
                }
            } else if (ndflt) { perr(p, "a parameter without a default can't follow one with"); break; }
        } while (match(p, T_COMMA) && !p->failed);
    }
    expect(p, T_RPAREN, "')'");
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

    if (ndflt) {
        int after = p->cur;
        for (int i = 0; i < ndflt; i++) {
            p->cur = dflt[i];
            expression(p);
        }
        emit(p, OP_SETDEFAULTS);
        emit2(p, (uint8_t)fi, (uint8_t)ndflt);
        p->cur = after;
    }
    emit_function(p, fi, &c);
    if (top) {
        emit(p, OP_SETGLOBAL);
        emit_u16(p, g);
    } else {
        emit_name_store(p, nameTok.start, nameTok.len);
    }
}

/* Is the upcoming token run an assignment target? Looks ahead without
 * emitting, then rewinds. */
static bool looks_like_assignment(P *p)
{
    int save = p->cur;
    bool yes = false;
    /* a, b = ... */
    if (check(p, T_STAR) || (check(p, T_IDENT) && p->tl.tok[p->cur + 1].type == T_COMMA && p->tl.tok[p->cur + 2].type == T_STAR)) {
        perr(p, "'a, *rest = ...' isn't supported here: try a = l[0] and rest = l[1:]");
        return false;
    }
    if (check(p, T_IDENT) && p->tl.tok[p->cur + 1].type == T_COMMA) {
        int i = p->cur;
        while (p->tl.tok[i].type == T_IDENT && p->tl.tok[i + 1].type == T_COMMA) i += 2;
        if (p->tl.tok[i].type == T_IDENT && p->tl.tok[i + 1].type == T_ASSIGN) return true;
    }
    if (check(p, T_IDENT)) {
        p->cur++;
        int depth = 0;
        for (;;) {
            TokType t = peek(p)->type;
            if (t == T_LBRACKET || t == T_LPAREN) { depth++; p->cur++; continue; }
            if (depth == 0 && t == T_DOT && p->tl.tok[p->cur + 1].type == T_IDENT) { p->cur += 2; continue; }
            if (depth > 0) {
                if (t == T_RBRACKET || t == T_RPAREN) depth--;
                else if (t == T_NEWLINE || t == T_EOF) break;
                p->cur++;
                continue;
            }
            yes = (t == T_ASSIGN || t == T_PLUSEQ || t == T_MINUSEQ ||
                   t == T_STAREQ || t == T_SLASHEQ);
            break;
        }
    }
    p->cur = save;
    return yes;
}

static void assignment(P *p)
{
    Token nameTok = *advance(p);

    if (check(p, T_COMMA)) {
        /* a, b = 1, 2 / a, b = b, a / x, y = f(): the right worked out
         * whole, then unpacked into the names */
        Token names[16];
        int nn = 0;
        names[nn++] = nameTok;
        while (match(p, T_COMMA)) {
            if (nn < 16) names[nn++] = *advance(p); else { perr(p, "too many names"); return; }
        }
        expect(p, T_ASSIGN, "'='");
        expr_list(p);
        emit2(p, OP_UNPACK, (uint8_t)nn);
        for (int i = 0; i < nn; i++) emit_name_store(p, names[i].start, names[i].len);
        return;
    }

    if (check(p, T_LBRACKET) || check(p, T_DOT) || check(p, T_LPAREN)) {
        /* NAME[k].b[j] = value: everything up to the last [..] or .name is
         * read, the last one is written. TUX ROCKET SCIENCE: the .name ones
         * (engine.throttle = 100), and += on either. */
        emit_name_load(p, nameTok.start, nameTok.len);
        int attr = -1;          /* the last is .name (its constant), or [k] */
        for (;;) {
            if (match(p, T_DOT)) {
                Token *nt = advance(p);
                int k = add_const(p, str_new(nt->start, (size_t)nt->len));
                if (match(p, T_LPAREN)) {           /* ship.part("x").throttle = 1 */
                    int n = arg_list(p, T_RPAREN);
                    emit(p, OP_CALLMETHOD);
                    emit_u16(p, k);
                    emit(p, (uint8_t)n);
                    continue;
                }
                if (check(p, T_DOT) || check(p, T_LBRACKET)) { emit(p, OP_GETATTR); emit_u16(p, k); continue; }
                attr = k;
                break;
            }
            if (match(p, T_LPAREN)) {
                int n = arg_list(p, T_RPAREN);
                emit2(p, OP_CALL, (uint8_t)n);
                continue;
            }
            if (match(p, T_LBRACKET)) {
                expression(p);
                expect(p, T_RBRACKET, "']'");
                if (check(p, T_DOT) || check(p, T_LBRACKET)) { emit(p, OP_INDEX); continue; }
                break;
            }
            perr(p, "expected '=' after the name");
            return;
        }
        if (p->failed) return;
        TokType op = advance(p)->type;
        if (op != T_ASSIGN) {
            /* the current value, from a second copy of the box (and key) */
            if (attr >= 0) { emit(p, OP_DUP); emit(p, OP_GETATTR); emit_u16(p, attr); }
            else { emit(p, OP_DUP2); emit(p, OP_INDEX); }
        }
        expression(p);
        switch (op) {
        case T_ASSIGN:  break;
        case T_PLUSEQ:  emit(p, OP_ADD); break;
        case T_MINUSEQ: emit(p, OP_SUB); break;
        case T_STAREQ:  emit(p, OP_MUL); break;
        case T_SLASHEQ: emit(p, OP_DIV); break;
        default: perr(p, "bad assignment operator"); return;
        }
        if (attr >= 0) { emit(p, OP_SETATTR); emit_u16(p, attr); }
        else emit(p, OP_SETINDEX);
        return;
    }

    TokType op = advance(p)->type;
    if (op == T_ASSIGN && check(p, T_IDENT) && p->tl.tok[p->cur + 1].type == T_ASSIGN) {
        /* a = b = 3: the value once, to every name */
        Token more[8];
        int nm = 0;
        while (check(p, T_IDENT) && p->tl.tok[p->cur + 1].type == T_ASSIGN && nm < 8) {
            more[nm++] = *advance(p);
            advance(p);
        }
        expr_list(p);
        for (int i = 0; i < nm; i++) {
            emit(p, OP_DUP);
            emit_name_store(p, more[i].start, more[i].len);
        }
        emit_name_store(p, nameTok.start, nameTok.len);
        return;
    }
    if (op != T_ASSIGN) {
        emit_name_load(p, nameTok.start, nameTok.len);
        expression(p);
        switch (op) {
        case T_PLUSEQ:  emit(p, OP_ADD); break;
        case T_MINUSEQ: emit(p, OP_SUB); break;
        case T_STAREQ:  emit(p, OP_MUL); break;
        case T_SLASHEQ: emit(p, OP_DIV); break;
        default: perr(p, "bad assignment operator"); return;
        }
    } else {
        expr_list(p);
    }
    emit_name_store(p, nameTok.start, nameTok.len);
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

/* try: ... except [Kind [as e]]: ... -- any error is caught (the kind is
 * read and let be: there are no kinds of error here); e is its words */
static void try_stmt(P *p)
{
    int h = emit_jump(p, OP_TRY);
    p->in_try++;
    block(p);
    p->in_try--;
    emit(p, OP_ENDTRY);
    int ends[8], ne = 0;
    /* try ... else: when nothing went wrong */
    int save = p->cur;
    while (check(p, T_NEWLINE)) advance(p);
    if (!check(p, T_EXCEPT)) { perr(p, check(p, T_FINALLY) ? "'finally' isn't supported here: put the clean-up after the try" : "expected 'except' after the try block"); return; }
    p->cur = save;
    int over = emit_jump(p, OP_JMP);   /* past the excepts, or to the else */
    patch_jump(p, h);
    /* the error's words are on the stack; each except asks if they're its
     * kind ("KeyError: 'x'"), and the first that is takes them */
    bool caught_all = false;
    for (;;) {
        save = p->cur;
        while (check(p, T_NEWLINE)) advance(p);
        if (!match(p, T_EXCEPT)) { p->cur = save; break; }
        Buf kinds; buf_init(&kinds);
        if (match(p, T_LPAREN)) {
            while (!check(p, T_RPAREN) && !check(p, T_EOF)) {
                Token *t = advance(p);
                if (t->type == T_IDENT) { if (kinds.len) buf_putc(&kinds, '|'); buf_put(&kinds, t->start, (size_t)t->len); }
            }
            expect(p, T_RPAREN, "')'");
        } else if (check(p, T_IDENT)) {
            Token *t = advance(p);
            buf_put(&kinds, t->start, (size_t)t->len);
        }
        int next = -1;
        if (kinds.len && !(kinds.len == 13 && !memcmp(kinds.p, "BaseException", 13))) {
            emit(p, OP_EXCMATCH);
            emit_u16(p, add_const(p, str_new(kinds.p, kinds.len)));
            next = emit_jump(p, OP_JMPF);
        } else caught_all = true;
        buf_free(&kinds);
        if (match(p, T_AS)) { emit(p, OP_EXCTEXT); Token e = *advance(p); emit_name_store(p, e.start, e.len); }
        else emit(p, OP_POP);
        block(p);
        if (ne < 8) ends[ne++] = emit_jump(p, OP_JMP);
        if (next >= 0) patch_jump(p, next);
        if (caught_all) break;
    }
    if (!caught_all) emit(p, OP_RAISE);   /* none of its kinds: on up, as it was */
    patch_jump(p, over);
    save = p->cur;
    while (check(p, T_NEWLINE)) advance(p);
    if (match(p, T_ELSE)) block(p); else p->cur = save;
    save = p->cur;
    while (check(p, T_NEWLINE)) advance(p);
    if (check(p, T_FINALLY)) { perr(p, "'finally' isn't supported here: put the clean-up after the try"); return; }
    p->cur = save;
    for (int i = 0; i < ne; i++) patch_jump(p, ends[i]);
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
    if (match(p, T_PASS))  { return; }

    if (match(p, T_BREAK)) {
        if (!p->loop) { perr(p, "'break' outside a loop"); return; }
        if (p->loop->nexit < 32) p->loop->exitpatch[p->loop->nexit++] = emit_jump(p, OP_JMP);
        return;
    }
    if (match(p, T_CONTINUE)) {
        if (!p->loop) { perr(p, "'continue' outside a loop"); return; }
        emit_loop(p, p->loop->start);
        return;
    }
    if (match(p, T_RETURN)) {
        if (check(p, T_NEWLINE) || check(p, T_RBRACE) || check(p, T_EOF) || check(p, T_DEDENT)) emit(p, OP_NIL);
        else expr_list(p);
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
    if (match(p, T_IMPORT)) { import_stmt(p); return; }
    if (match(p, T_ASSERT)) {
        /* assert cond, "why": stops the script, saying why, if cond isn't so */
        expression(p);
        int ok = emit_jump(p, OP_JMPT_KEEP);
        emit(p, OP_POP);
        emit_const(p, str_newz("AssertionError: "));
        if (match(p, T_COMMA)) {
            int k = native_find("str", 3);
            emit2(p, OP_GETNATIVE, (uint8_t)k);
            expression(p);
            emit2(p, OP_CALL, 1);
        } else emit_const(p, str_newz("it wasn't so"));
        emit(p, OP_ADD);
        emit(p, OP_RAISE);
        patch_jump(p, ok);
        emit(p, OP_POP);
        return;
    }
    if (match(p, T_FROM)) { from_stmt(p); return; }
    if (match(p, T_TRY)) { try_stmt(p); return; }
    if (match(p, T_RAISE)) {
        if (check(p, T_NEWLINE) || check(p, T_EOF) || check(p, T_DEDENT)) emit_const(p, str_newz("an error"));
        else expression(p);
        emit(p, OP_RAISE);
        return;
    }
    if (check(p, T_EXCEPT) || check(p, T_FINALLY)) {
        perr(p, check(p, T_EXCEPT) ? "'except' without a 'try' above it" : "'finally' isn't supported here: put the clean-up after the try");
        return;
    }

    if (looks_like_assignment(p)) { assignment(p); return; }

    if (p->echo && p->comp->toplevel && !p->loop) {
        int k = native_find("_echo", 5);
        if (k >= 0) {
            emit2(p, OP_GETNATIVE, (uint8_t)k);
            expression(p);
            emit2(p, OP_CALL, 1);
            emit(p, OP_POP);
            return;
        }
    }
    expression(p);
    emit(p, OP_POP);
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
