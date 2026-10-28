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

/* lang.h — internals of the NomScript compiler and VM.
 *
 * Bytecode, not a tree-walk, because a blocking device read has to suspend a
 * script mid-program and resume it many ticks later, and because the per-tick
 * cost has to be countable in instructions. See D4.
 */
#ifndef NOM_LANG_H
#define NOM_LANG_H

#include "nom.h"

/* ------------------------------------------------------------------ lexer */
typedef enum {
    T_EOF, T_NEWLINE, T_INDENT, T_DEDENT,
    T_INT, T_NUM, T_STR, T_IDENT,
    /* keywords */
    T_IF, T_ELIF, T_ELSE, T_WHILE, T_FOR, T_IN, T_BREAK, T_CONTINUE,
    T_DEF, T_RETURN, T_AND, T_OR, T_NOT, T_TRUE, T_FALSE, T_NIL, T_PASS,
    /* TUX ROCKET SCIENCE: the rest of the Python a player reaches for */
    T_GLOBAL, T_IMPORT, T_FROM, T_TRY, T_EXCEPT, T_FINALLY, T_RAISE, T_LAMBDA, T_IS, T_AS, T_ASSERT, T_CLASS,
    /* punctuation */
    T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_LBRACE, T_RBRACE,
    T_COMMA, T_COLON, T_DOT, T_FSTR,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_DSLASH, T_PERCENT, T_DSTAR,
    T_ASSIGN, T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ
} TokType;

typedef struct {
    TokType type;
    const char *start;
    int         len;
    int         line;
    int64_t     ival;
    double      dval;
    char       *sval;      /* T_STR: owned, unescaped */
} Token;

typedef struct {
    Token *tok;
    int    ntok;
    int    cap;    /* TUX ROCKET SCIENCE: sized to the source, not TOK_MAX */
} TokenList;

bool lex_source(const char *src, TokenList *out, char *err, size_t errsz);
void lex_free(TokenList *tl);

/* ------------------------------------------------------------------ codes */
typedef enum {
    OP_CONST, OP_NIL, OP_TRUE, OP_FALSE,
    OP_POP, OP_DUP,
    OP_GETLOCAL, OP_SETLOCAL,
    OP_GETGLOBAL, OP_SETGLOBAL,
    OP_GETNATIVE, OP_GETFUNC,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_FDIV, OP_MOD, OP_POW, OP_NEG,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_IN,
    OP_NOT,
    OP_JMP, OP_JMPF, OP_JMPF_KEEP, OP_JMPT_KEEP,
    OP_CALL, OP_RET,
    OP_LIST, OP_DICT,
    OP_INDEX, OP_SETINDEX,
    OP_FORPREP, OP_FORNEXT,
    /* TUX ROCKET SCIENCE: a.b, a.b = v, a.b(...) (the ship and its parts
     * are things with names), and the [k] += v that needs two copies */
    OP_GETATTR, OP_SETATTR, OP_CALLMETHOD, OP_DUP2,
    /* and Python's: a, b = ...; x[1:3]; [f(x) for x in s]; f(a, key=b);
     * def f(a, b=1); try/except/raise */
    OP_UNPACK, OP_SLICE, OP_APPENDN, OP_KWARGS, OP_SETDEFAULTS,
    OP_TRY, OP_ENDTRY, OP_RAISE,
    OP_TUPLE, OP_CLOSURE, OP_EXCMATCH, OP_EXCTEXT, OP_PUTN, OP_CLASS,
    OP_HALT
} OpCode;

struct Chunk {
    uint8_t *code;
    int     *line;
    int      len, cap;
};

struct Func {
    char   name[NOM_NAME_MAX];
    int    arity;
    int    nlocals;
    Chunk  chunk;
    char  *locals;      /* TUX ROCKET SCIENCE: its locals' names, "a,b,c" (the debugger's watch) */
    int    ndefaults;   /* the last ndefaults parameters' defaults */
    int    ncap;        /* a closure's: which local each captured value goes in */
    int    cap_slot[16];
    Value  defaults[NOM_ARGS_MAX];
};

struct Prog {
    char   name[NOM_NAME_MAX];
    Value  consts[NOM_CONSTS_MAX];
    int    nconsts;
    char   global[NOM_GLOBALS_MAX][NOM_NAME_MAX];
    int    nglobals;
    Func   func[NOM_FUNCS_MAX];
    int    nfuncs;          /* func[0] is the top level */
};

/* --------------------------------------------------------------- natives */
typedef VmStatus (*NativeFn)(VM *v, Value *args, int n, Value *out);

typedef struct {
    const char *name;
    int         minargs, maxargs;
    NativeFn    fn;
} Native;

const Native *native_table(int *count);
int           native_find(const char *name, int len);

/* TUX ROCKET SCIENCE: what a.b means, for the host program (py.c) to say:
 * a dict's key, or a thing aboard the ship asked for one of its readings.
 * Each returns false with vm_runtime_error set. */
bool          host_getattr(VM *v, Value obj, Str *name, Value *out);
bool          host_setattr(VM *v, Value obj, Str *name, Value val);
VmStatus      host_callmethod(VM *v, Value obj, Str *name, Value *args, int n, Value *out);
/* "%d m at %.1f s" % (120, 3.25) */
bool          host_percent(VM *v, Value fmt, Value args, Value *out);

/* Objects for callables (defined in value.c). */
Value func_obj_new(int index, bool native);
int   func_obj_index(Value v);

/* ------------------------------------------------------------------- vm */
struct VM {
    Prog   *prog;
    Vfs    *fs;
    Sim    *sim;
    /* THE SHIP, for the natives a player's script uses. NULL is legal and
     * every ship native says so rather than dereferencing it. */
    Ship   *ship;
    /* The tick a script sees when there is no Sim driving it. sleep() and
     * tick() used to reach straight into sim->tick, which is what tied the
     * whole language to a simulation that is no longer the game. */
    uint64_t tick;
    /* Where print() goes when there is neither a console nor... see below.
     * A hook rather than a direct call to sim_log(), because a direct call is
     * a link-time dependency on the entire station simulation and that is
     * what kept the language out of the break-fix binary (and so out of the
     * gates). */
    void  (*log_hook)(struct VM *v, const char *text);

    /* Where print() goes when there is no Sim: the machine's console. This is
     * what makes a boot script's output the boot output. */
    Buf    *console;
    /* Run another script file, in the same machine, and return whether it
     * succeeded. Set by the boot runtime; NULL means exec() is unavailable.
     * This is how /sbin/init hands off to /etc/rc.boot for real. */
    bool  (*run_script)(struct VM *v, const char *path);
    void   *host;        /* the Machine, for run_script */
    /* Mounting and starting a service are things only a booting machine can
     * do. Hooks rather than #ifdefs, so natives.c stays the single sandbox. */
    bool  (*mount_hook)(struct VM *v, const char *what, const char *where,
                        char *err, size_t errsz);
    bool  (*svc_hook)(struct VM *v, const char *exec, char *err, size_t errsz);
    int     depth;       /* exec nesting, so a corrupted script cannot recurse */

    Value   stack[NOM_STACK_MAX];
    int     sp;

    struct Frame {
        Func  *fn;
        int    ip;
        int    base;        /* stack index of local slot 0 */
        Value  init_self;   /* TUX ROCKET SCIENCE: __init__'s instance, what the call gives back */
    } frame[NOM_FRAMES_MAX];
    int     nframes;

    Value   global[NOM_GLOBALS_MAX];
    bool    gdef[NOM_GLOBALS_MAX];

    /* TUX ROCKET SCIENCE: f(a, key=b): the keyword arguments of the call
     * being made (a native reads them and says it did); try's handlers */
    Value    kw;            /* a dict, or nil */
    bool     kw_used;
    struct Handler { int frame, sp, ip; } handler[16];
    int      nhandler;
    int      stop_frames;   /* vm_call: stop when the frames are back to this */

    uint64_t steps;
    uint64_t wake_tick;
    bool     sleeping;      /* sleep() is mid-flight; see natives.c */
    bool     watching;      /* watch() is armed on watch_path */
    char     watch_path[NOM_PATH_MAX];
    char     watch_last[64];
    char     blocked_on[NOM_PATH_MAX];
    char     err[NOM_ERR_MAX];
    int      line;
    bool     done;
};

void vm_runtime_error(VM *v, const char *fmt, ...);

/* TUX ROCKET SCIENCE: the REPL. More source compiled into the same program
 * (its globals, its functions), as one more top-level chunk: its function
 * index, or -1 with err set. echo: an expression on its own line prints
 * what it comes to (via the native _echo), as Python's prompt does. */
int  prog_compile_more(Prog *pr, const char *src, bool echo, char *err, size_t errsz);
/* run that chunk next, on the VM's globals as they are */
void vm_start_func(VM *v, int fi);
/* TUX ROCKET SCIENCE: calls a function value (a native, a def, a lambda)
 * from a native -- sorted's key=, map(): its result, or false with v->err */
bool vm_call(VM *v, Value fn, Value *args, int n, Value *out);

#endif /* NOM_LANG_H */
