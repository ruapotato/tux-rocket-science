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

/* lex.c — tokenizer for NomScript.
 *
 * Tracks indentation the way HAMSH_SPEC 5 describes: an INDENT when a logical
 * line's column exceeds the enclosing level, one DEDENT per level closed.
 * INDENT/DEDENT are suppressed inside ( ) and [ ] so expressions may wrap, and
 * a brace block simply ignores stray ones — which is what lets the colon form
 * and the brace form nest inside each other.
 */
#include "lang.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

typedef struct {
    const char *src, *p;
    int    line;
    int    depth;              /* bracket nesting: suppresses INDENT/DEDENT */
    int    open_line[32];      /* TUX ROCKET SCIENCE: where each open bracket is, for "never closed" */
    char   open_ch[32];
    int    nopen;
    int    indent[32], nindent;
    bool   at_line_start;
    uint32_t brace_dict;       /* TUX ROCKET SCIENCE: which open { are dict literals (bit per level) */
    int    nbrace;
    TokenList *out;
    char  *err;
    size_t errsz;
    bool   failed;
} Lexer;

static void lx_err(Lexer *lx, const char *fmt, ...)
{
    if (lx->failed) return;
    lx->failed = true;
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(lx->err, lx->errsz, "[line %d]: %s", lx->line, msg);
}

static Token *push(Lexer *lx, TokType t)
{
    TokenList *tl = lx->out;
    if (tl->ntok >= tl->cap) { lx_err(lx, "script too long (TOK_MAX)"); return NULL; }
    Token *tk = &tl->tok[tl->ntok++];
    memset(tk, 0, sizeof *tk);
    tk->type = t;
    tk->line = lx->line;
    return tk;
}

static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_digit(char c)       { return c >= '0' && c <= '9'; }
static bool is_ident(char c)       { return is_ident_start(c) || is_digit(c); }

static struct { const char *word; TokType t; } KEYWORDS[] = {
    { "if", T_IF }, { "elif", T_ELIF }, { "else", T_ELSE },
    { "while", T_WHILE }, { "for", T_FOR }, { "in", T_IN },
    { "break", T_BREAK }, { "continue", T_CONTINUE },
    { "def", T_DEF }, { "return", T_RETURN },
    { "and", T_AND }, { "or", T_OR }, { "not", T_NOT },
    { "true", T_TRUE }, { "false", T_FALSE }, { "nil", T_NIL },
    /* Python spellings accepted too — muscle memory is not a bug */
    { "True", T_TRUE }, { "False", T_FALSE }, { "None", T_NIL },
    { "pass", T_PASS },
    { "global", T_GLOBAL }, { "import", T_IMPORT }, { "from", T_FROM },
    { "try", T_TRY }, { "except", T_EXCEPT }, { "finally", T_FINALLY },
    { "raise", T_RAISE }, { "lambda", T_LAMBDA }, { "is", T_IS }, { "as", T_AS }, { "assert", T_ASSERT }, { "class", T_CLASS },
    { "del", T_DEL }, { "nonlocal", T_NONLOCAL }, { "yield", T_YIELD }, { "with", T_WITH },
    { NULL, T_EOF }
};

static TokType keyword_of(const char *s, int len)
{
    for (int i = 0; KEYWORDS[i].word; i++)
        if ((int)strlen(KEYWORDS[i].word) == len && memcmp(KEYWORDS[i].word, s, (size_t)len) == 0)
            return KEYWORDS[i].t;
    return T_IDENT;
}

/* Handle the start of a logical line: measure indentation and emit
 * INDENT/DEDENT. Returns false if the line turned out to be blank/comment. */
static bool handle_indent(Lexer *lx)
{
    int col = 0;
    const char *p = lx->p;
    for (;;) {
        if (*p == ' ') { col++; p++; }
        else if (*p == '\t') { col = (col / 4 + 1) * 4; p++; }
        else break;
    }
    if (*p == '\n' || *p == '\r' || *p == '#' || *p == 0) {
        lx->p = p;                 /* blank or comment-only: carries no indent */
        return false;
    }
    lx->p = p;

    int cur = lx->indent[lx->nindent - 1];
    if (col > cur) {
        if (lx->nindent >= 32) { lx_err(lx, "indentation too deep"); return true; }
        lx->indent[lx->nindent++] = col;
        push(lx, T_INDENT);
    } else {
        while (lx->nindent > 1 && col < lx->indent[lx->nindent - 1]) {
            lx->nindent--;
            push(lx, T_DEDENT);
        }
        if (col != lx->indent[lx->nindent - 1])
            lx_err(lx, "inconsistent indentation (column %d)", col);
    }
    return true;
}

/* a code point as UTF-8 (\u00e9 is two bytes) */
static void put_utf8(Buf *b, u32 c)
{
    if (c < 0x80) buf_putc(b, (char)c);
    else if (c < 0x800) { buf_putc(b, (char)(0xc0 | (c >> 6))); buf_putc(b, (char)(0x80 | (c & 0x3f))); }
    else if (c < 0x10000) { buf_putc(b, (char)(0xe0 | (c >> 12))); buf_putc(b, (char)(0x80 | ((c >> 6) & 0x3f))); buf_putc(b, (char)(0x80 | (c & 0x3f))); }
    else { buf_putc(b, (char)(0xf0 | (c >> 18))); buf_putc(b, (char)(0x80 | ((c >> 12) & 0x3f))); buf_putc(b, (char)(0x80 | ((c >> 6) & 0x3f))); buf_putc(b, (char)(0x80 | (c & 0x3f))); }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool g_raw;   /* r"...": a backslash is a backslash */

static void lex_string(Lexer *lx, char quote)
{
    Buf b;
    buf_init(&b);
    /* TUX ROCKET SCIENCE: '''...''' and """...""": they may span lines */
    bool triple = lx->p[0] == quote && lx->p[1] == quote;
    if (triple) lx->p += 2;
    while (*lx->p && (triple ? !(lx->p[0] == quote && lx->p[1] == quote && lx->p[2] == quote) : *lx->p != quote)) {
        char c = *lx->p++;
        if (c == '\n' && !triple) { lx_err(lx, "unterminated string (a ' or \" left open)"); buf_free(&b); return; }
        if (c == '\n') lx->line++;
        if (c == '\\' && g_raw) {
            /* (raw: kept, and the quote after it doesn't end the string) */
            buf_putc(&b, c);
            if (*lx->p) { if (*lx->p == '\n') lx->line++; buf_putc(&b, *lx->p++); }
            continue;
        }
        if (c == '\\') {
            char e = *lx->p++;
            switch (e) {
            case '\n': lx->line++; break;               /* a line carried on */
            case 'a':  buf_putc(&b, '\a'); break;
            case 'b':  buf_putc(&b, '\b'); break;
            case 'f':  buf_putc(&b, '\f'); break;
            case 'v':  buf_putc(&b, '\v'); break;
            case 'x': case 'u': case 'U': {
                int nd = e == 'x' ? 2 : e == 'u' ? 4 : 8;
                u32 cp = 0;
                for (int i = 0; i < nd; i++) {
                    int h = hexval(*lx->p);
                    if (h < 0) { lx_err(lx, "\\%c needs %d hex digits", e, nd); buf_free(&b); return; }
                    cp = cp * 16 + (u32)h;
                    lx->p++;
                }
                if (e == 'x') buf_putc(&b, (char)cp); else put_utf8(&b, cp);
                break;
            }
            case 'n':  buf_putc(&b, '\n'); break;
            case 't':  buf_putc(&b, '\t'); break;
            case 'r':  buf_putc(&b, '\r'); break;
            case '0':  buf_putc(&b, '\0'); break;
            case '\\': buf_putc(&b, '\\'); break;
            case '"':  buf_putc(&b, '"');  break;
            case '\'': buf_putc(&b, '\''); break;
            default:   buf_putc(&b, '\\'); buf_putc(&b, e); break;
            }
        } else {
            buf_putc(&b, c);
        }
    }
    if (*lx->p != quote) { lx_err(lx, "unterminated string (a ' or \" left open)"); buf_free(&b); return; }
    lx->p += triple ? 3 : 1;
    Token *tk = push(lx, T_STR);
    if (tk) {
        tk->sval = b.p ? b.p : nom_strdup("");
        tk->len  = (int)b.len;
        if (!b.p) buf_free(&b);
    } else {
        buf_free(&b);
    }
}

static void lex_number(Lexer *lx)
{
    const char *start = lx->p;
    /* TUX ROCKET SCIENCE: 0x1f, 0b101, 0o17 */
    if (lx->p[0] == '0' && (lx->p[1] == 'x' || lx->p[1] == 'X' || lx->p[1] == 'b' || lx->p[1] == 'B' || lx->p[1] == 'o' || lx->p[1] == 'O')) {
        int base = (lx->p[1] | 32) == 'x' ? 16 : (lx->p[1] | 32) == 'b' ? 2 : 8;
        lx->p += 2;
        int64_t v = 0;
        int nd = 0;
        for (;; lx->p++) {
            if (*lx->p == '_') continue;
            int h = hexval(*lx->p);
            if (h < 0 || h >= base) break;
            v = v * base + h;
            nd++;
        }
        if (!nd) { lx_err(lx, "a number in base %d needs digits after the 0%c", base, start[1]); return; }
        Token *tk = push(lx, T_INT);
        if (!tk) return;
        tk->ival = v;
        tk->start = start;
        tk->len = (int)(lx->p - start);
        return;
    }
    /* digits, and 1_000_000's underscores (dropped) */
    char tmp[64];
    int n = 0;
    bool isfloat = false;
#define TAKE_DIGITS() while (is_digit(*lx->p) || (*lx->p == '_' && is_digit(lx->p[1]))) { if (*lx->p != '_' && n < 62) tmp[n++] = *lx->p; lx->p++; }
    TAKE_DIGITS();
    /* 2.5, and 5. and .5 */
    if (*lx->p == '.' && !is_ident_start(lx->p[1]) && lx->p[1] != '.') {
        isfloat = true;
        tmp[n++] = '.';
        lx->p++;
        TAKE_DIGITS();
    }
    if (*lx->p == 'e' || *lx->p == 'E') {
        const char *save = lx->p;
        int sn = n;
        tmp[n++] = 'e';
        lx->p++;
        if (*lx->p == '+' || *lx->p == '-') tmp[n++] = *lx->p++;
        if (is_digit(*lx->p)) { isfloat = true; TAKE_DIGITS(); }
        else { lx->p = save; n = sn; }
    }
#undef TAKE_DIGITS
    if (n >= 62) { lx_err(lx, "number literal too long"); return; }
    tmp[n] = 0;
    Token *tk = push(lx, isfloat ? T_NUM : T_INT);
    if (!tk) return;
    if (isfloat) tk->dval = strtod(tmp, NULL);
    else {
        /* too big for 64 bits: a float, as the arithmetic does */
        int64_t v = 0;
        bool over = false;
        for (int i = 0; i < n; i++) over |= __builtin_mul_overflow(v, 10, &v) || __builtin_add_overflow(v, tmp[i] - '0', &v);
        if (over) { tk->type = T_NUM; tk->dval = strtod(tmp, NULL); }
        else tk->ival = v;
    }
    tk->start = start;
    tk->len = (int)(lx->p - start);
}

bool lex_source(const char *src, TokenList *out, char *err, size_t errsz)
{
    Lexer lx;
    memset(&lx, 0, sizeof lx);
    lx.src = lx.p = src;
    lx.line = 1;
    lx.indent[0] = 0;
    lx.nindent = 1;
    lx.at_line_start = true;
    lx.out = out;
    lx.err = err;
    lx.errsz = errsz;

    /* never more tokens than characters, and the closing DEDENTs, NEWLINE
     * and EOF on top: a short f-string expression doesn't take the heap */
    size_t slen = strlen(src);
    out->cap = slen + 40 < NOM_TOK_MAX ? (int)slen + 40 : NOM_TOK_MAX;
    out->tok = nom_alloc(sizeof(Token) * (size_t)out->cap);
    out->ntok = 0;

    while (*lx.p && !lx.failed) {
        if (lx.at_line_start && lx.depth == 0) {
            lx.at_line_start = false;
            if (!handle_indent(&lx)) {
                /* skip the blank/comment line entirely */
                while (*lx.p && *lx.p != '\n') lx.p++;
                if (*lx.p == '\n') { lx.p++; lx.line++; lx.at_line_start = true; }
                continue;
            }
        }

        char c = *lx.p;
        if (c == ' ' || c == '\t' || c == '\r') { lx.p++; continue; }
        if (c == '#') { while (*lx.p && *lx.p != '\n') lx.p++; continue; }
        if (c == '\\' && lx.p[1] == '\n') { lx.p += 2; lx.line++; continue; }  /* explicit continuation */
        if (c == '\n') {
            lx.p++;
            if (lx.depth == 0) {
                /* collapse runs of newlines into one separator (on the line
                 * it ends, so "expected ':'" names the line without one) */
                int n = out->ntok;
                if (n && out->tok[n - 1].type != T_NEWLINE && out->tok[n - 1].type != T_INDENT)
                    push(&lx, T_NEWLINE);
                lx.at_line_start = true;
            }
            lx.line++;
            continue;
        }

        if (is_digit(c) || (c == '.' && is_digit(lx.p[1]))) { lex_number(&lx); continue; }
        if (c == '"' || c == '\'') { lx.p++; g_raw = false; lex_string(&lx, c); continue; }
        /* TUX ROCKET SCIENCE: f"..." -- the {...} in it are filled in
         * (compile.c splits it); r"..." raw, b"..." (text here), rf"..." */
        {
            int k = 0;
            bool fs = false, raw = false;
            while (k < 2 && ((lx.p[k] | 32) == 'f' || (lx.p[k] | 32) == 'r' || (lx.p[k] | 32) == 'b')) {
                fs |= (lx.p[k] | 32) == 'f';
                raw |= (lx.p[k] | 32) == 'r';
                k++;
            }
            if (k && (lx.p[k] == '"' || lx.p[k] == '\'') && !(k == 2 && (lx.p[0] | 32) == (lx.p[1] | 32))) {
                char q = lx.p[k];
                lx.p += k + 1;
                int before = out->ntok;
                g_raw = raw;
                lex_string(&lx, q);
                g_raw = false;
                if (fs && out->ntok > before) out->tok[out->ntok - 1].type = T_FSTR;
                continue;
            }
        }
        if (is_ident_start(c)) {
            const char *start = lx.p;
            while (is_ident(*lx.p)) lx.p++;
            int len = (int)(lx.p - start);
            Token *tk = push(&lx, keyword_of(start, len));
            if (tk) { tk->start = start; tk->len = len; }
            continue;
        }

        lx.p++;
        TokType t = T_EOF;
        switch (c) {
        case '(': case '[':
            t = c == '(' ? T_LPAREN : T_LBRACKET;
            lx.depth++;
            if (lx.nopen < 32) { lx.open_line[lx.nopen] = lx.line; lx.open_ch[lx.nopen] = c; }
            lx.nopen++;
            break;
        case ')': case ']':
            t = c == ')' ? T_RPAREN : T_RBRACKET;
            if (lx.nopen > 0 && lx.nopen <= 32 && lx.open_ch[lx.nopen - 1] != (c == ')' ? '(' : '[')) {
                lx_err(&lx, "'%c' doesn't match the '%c' on line %d", c, lx.open_ch[lx.nopen - 1], lx.open_line[lx.nopen - 1]);
                break;
            }
            if (lx.nopen > 0) lx.nopen--;
            if (lx.depth) lx.depth--;
            break;
        case '{': {
            /* a dict literal (after =, (, [, a comma, an operator...) wraps
             * lines like ( does; a { opening a block does not */
            t = T_LBRACE;
            int n = out->ntok;
            TokType pv = n ? out->tok[n - 1].type : T_NEWLINE;
            bool dict = !(pv == T_IDENT || pv == T_INT || pv == T_NUM || pv == T_STR || pv == T_RPAREN ||
                          pv == T_RBRACKET || pv == T_RBRACE || pv == T_TRUE || pv == T_FALSE || pv == T_NIL || pv == T_ELSE);
            if (lx.nbrace < 32) {
                if (dict) { lx.brace_dict |= 1u << lx.nbrace; lx.depth++; }
                else lx.brace_dict &= ~(1u << lx.nbrace);
            }
            lx.nbrace++;
            break;
        }
        case '}':
            t = T_RBRACE;
            if (lx.nbrace > 0) {
                lx.nbrace--;
                if (lx.nbrace < 32 && (lx.brace_dict & (1u << lx.nbrace)) && lx.depth) lx.depth--;
            }
            break;
        case ';':
            /* two statements on one line */
            if (lx.depth == 0) {
                int n = out->ntok;
                if (n && out->tok[n - 1].type != T_NEWLINE) push(&lx, T_NEWLINE);
                continue;
            }
            lx_err(&lx, "unexpected ';'");
            break;
        case ',': t = T_COMMA;  break;
        case ':': t = (*lx.p == '=') ? (lx.p++, T_WALRUS) : T_COLON; break;
        case '.': t = T_DOT;    break;
        case '%': t = (*lx.p == '=') ? (lx.p++, T_PERCENTEQ) : T_PERCENT; break;
        case '+': t = (*lx.p == '=') ? (lx.p++, T_PLUSEQ)  : T_PLUS;  break;
        case '-':
            if (*lx.p == '=') { lx.p++; t = T_MINUSEQ; }
            else if (*lx.p == '>') { lx.p++; t = T_ARROW; }
            else t = T_MINUS;
            break;
        case '*':
            if (*lx.p == '*') { lx.p++; t = (*lx.p == '=') ? (lx.p++, T_DSTAREQ) : T_DSTAR; }
            else if (*lx.p == '=') { lx.p++; t = T_STAREQ; }
            else t = T_STAR;
            break;
        case '/':
            if (*lx.p == '/') { lx.p++; t = (*lx.p == '=') ? (lx.p++, T_DSLASHEQ) : T_DSLASH; }
            else if (*lx.p == '=') { lx.p++; t = T_SLASHEQ; }
            else t = T_SLASH;
            break;
        case '&': t = (*lx.p == '=') ? (lx.p++, T_AMPEQ) : T_AMP; break;
        case '|': t = (*lx.p == '=') ? (lx.p++, T_PIPEEQ) : T_PIPE; break;
        case '^': t = (*lx.p == '=') ? (lx.p++, T_CARETEQ) : T_CARET; break;
        case '~': t = T_TILDE; break;
        case '@': t = T_AT; break;
        case '=': t = (*lx.p == '=') ? (lx.p++, T_EQ) : T_ASSIGN; break;
        case '!':
            if (*lx.p == '=') { lx.p++; t = T_NE; }
            else { lx_err(&lx, "unexpected '!' (did you mean 'not'?)"); }
            break;
        case '<':
            if (*lx.p == '<') { lx.p++; t = (*lx.p == '=') ? (lx.p++, T_SHLEQ) : T_SHL; }
            else t = (*lx.p == '=') ? (lx.p++, T_LE) : T_LT;
            break;
        case '>':
            if (*lx.p == '>') { lx.p++; t = (*lx.p == '=') ? (lx.p++, T_SHREQ) : T_SHR; }
            else t = (*lx.p == '=') ? (lx.p++, T_GE) : T_GT;
            break;
        default:
            lx_err(&lx, "unexpected character '%c'", c);
            break;
        }
        if (lx.failed) break;
        Token *tk = push(&lx, t);
        if (tk) { tk->start = lx.p - 1; tk->len = 1; }
    }

    if (!lx.failed && lx.nopen > 0 && lx.nopen <= 32) {
        /* a bracket left open swallows every line after it: say which */
        lx.line = lx.open_line[lx.nopen - 1];
        lx_err(&lx, "this '%c' is never closed (a ')' or ']' missing?)", lx.open_ch[lx.nopen - 1]);
    }
    if (!lx.failed) {
        int n = out->ntok;
        if (n && out->tok[n - 1].type != T_NEWLINE) push(&lx, T_NEWLINE);
        while (lx.nindent > 1) { lx.nindent--; push(&lx, T_DEDENT); }
        push(&lx, T_EOF);
    }
    if (lx.failed) { lex_free(out); return false; }
    return true;
}

void lex_free(TokenList *tl)
{
    if (!tl->tok) return;
    for (int i = 0; i < tl->ntok; i++)
        if (tl->tok[i].type == T_STR || tl->tok[i].type == T_FSTR) nom_free(tl->tok[i].sval);
    nom_free(tl->tok);
    tl->tok = NULL;
    tl->ntok = 0;
}
