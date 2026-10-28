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

/* vm.c — the NomScript bytecode interpreter.
 *
 * Two properties matter more than speed here:
 *
 *  1. `vm_run(v, budget)` executes at most `budget` instructions and returns
 *     VM_YIELD. That is the per-tick CPU budget: a script cannot hang the
 *     simulation and cannot win by burning more CPU than its opponent.
 *
 *  2. A native call that would block does NOT consume its operands. It rewinds
 *     `ip` to the OP_CALL and returns VM_BLOCKED, so resuming re-executes the
 *     identical call. Retry is idempotent by construction. See D4.
 *
 * TUX ROCKET SCIENCE: and the rest of Python's machinery on top -- errors
 * that are objects (classes a script can build on and catch by kind),
 * generators (a frame put by between values), cells (a name a def and the
 * def inside it share), bound methods, and a class's __add__, __str__,
 * __call__... used where Python uses them.
 */
#include "lang.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

const char *const EXC_NAME[NEXC] = {
    "BaseException", "Exception", "ArithmeticError", "ZeroDivisionError", "OverflowError",
    "LookupError", "KeyError", "IndexError", "ValueError", "TypeError", "RuntimeError",
    "RecursionError", "NotImplementedError", "NameError", "AttributeError", "AssertionError",
    "MemoryError", "StopIteration", "OSError", "FileNotFoundError", "KeyboardInterrupt",
    "UnboundLocalError",
};
const signed char EXC_BASE[NEXC] = { -1, 0, 1, 2, 2, 1, 5, 5, 1, 1, 1, 10, 10, 1, 1, 1, 1, 1, 1, 18, 0, 13 };

void vm_runtime_error(VM *v, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v->err, sizeof v->err, fmt, ap);
    va_end(ap);
}

static VM *g_text_vm;
static bool inst_text(Buf *b, Value x, bool repr);
static int g_text_depth;

VM *vm_new(Prog *p, Vfs *fs, Sim *sim)
{
    VM *v = nom_alloc(sizeof(VM));
    memset(v, 0, sizeof *v);
    v->prog = p;
    v->fs = fs;
    v->sim = sim;
    v->nframes = 1;
    v->frame[0].fn = &p->func[0];
    v->frame[0].ip = 0;
    v->frame[0].base = 0;
    v->sp = p->func[0].nlocals;   /* top-level locals are unused but reserved */
    v->kw = VAL_NIL;
    v->exc = v->handling = VAL_NIL;
    for (int i = 0; i < v->sp; i++) v->stack[i] = VAL_NIL;
    g_text_vm = v;
    g_inst_text = inst_text;
    /* the exceptions: classes, each built on its kind (KeyError on
     * LookupError on Exception...), BaseException's methods natives */
    for (int i = 0; i < NEXC && i < p->nglobals; i++) {
        Value base = EXC_BASE[i] >= 0 ? v->global[EXC_BASE[i]] : VAL_NIL;
        Value c = class_new(EXC_NAME[i], base);
        if (i == 0) {
            static const char *M[][2] = { { "__init__", "_exc_init" }, { "__str__", "_exc_str" }, { "__repr__", "_exc_repr" } };
            for (int k = 0; k < 3; k++) {
                int n = native_find(M[k][1], (int)strlen(M[k][1]));
                if (n >= 0) dict_set(AS_DICT(((Klass *)c.as.o)->dict), str_newz(M[k][0]), func_obj_new(n, true));
            }
        }
        v->global[i] = c;
        v->gdef[i] = true;
    }
    if (p->nglobals > NEXC) { v->global[NEXC] = str_newz("__main__"); v->gdef[NEXC] = true; }
    return v;
}

void vm_free(VM *v)
{
    if (!v) return;
    val_release(v->kw);
    val_release(v->exc);
    val_release(v->handling);
    for (int i = 0; i < v->sp; i++) val_release(v->stack[i]);
    for (int i = 0; i < NOM_GLOBALS_MAX; i++)
        if (v->gdef[i]) val_release(v->global[i]);
    if (g_text_vm == v) g_text_vm = NULL;
    nom_free(v);
}

void vm_set_ship(VM *v, Ship *ship) { v->ship = ship; }

void vm_start_func(VM *v, int fi)
{
    while (v->sp > 0) val_release(v->stack[--v->sp]);
    v->nframes = 1;
    v->frame[0].fn = &v->prog->func[fi];
    v->frame[0].ip = 0;
    v->frame[0].base = 0;
    v->frame[0].gen = NULL;
    v->nhandler = 0;
    v->done = false;
    v->err[0] = 0;
    val_release(v->exc);
    v->exc = VAL_NIL;
}

const char *vm_err(VM *v)        { return v->err; }
uint64_t    vm_steps(VM *v)      { return v->steps; }
uint64_t    vm_wake_tick(VM *v)  { return v->wake_tick; }
const char *vm_blocked_on(VM *v) { return v->blocked_on; }
int         vm_line(VM *v)       { return v->line; }

/* ------------------------------------------------------------ stack ops */
static bool push(VM *v, Value val)
{
    if (v->sp >= NOM_STACK_MAX) {
        vm_runtime_error(v, "MemoryError: value stack overflow (STACK_MAX=%d)", NOM_STACK_MAX);
        val_release(val);
        return false;
    }
    v->stack[v->sp++] = val;
    return true;
}

static Value pop(VM *v)  { return v->stack[--v->sp]; }
static Value peek(VM *v, int d) { return v->stack[v->sp - 1 - d]; }

/* --------------------------------------------------------------- helpers */
static const char *type_name(Value v)
{
    switch (v.k) {
    case V_NIL:  return "None";
    case V_BOOL: return "bool";
    case V_INT:  return "int";
    case V_NUM:  return "float";
    case V_OBJ:
        switch (v.as.o->kind) {
        case O_STR:  return "str";
        case O_LIST: return AS_LIST(v)->tuple == 1 ? "tuple" : AS_LIST(v)->tuple == 2 ? "set" : "list";
        case O_DICT: return "dict";
        case O_HANDLE: return "ship's part";
        case O_RANGE: return "range";
        case O_CLASS: return "class";
        case O_INSTANCE: return ((Klass *)((Instance *)v.as.o)->cls.as.o)->name;
        case O_GEN: return "generator";
        case O_BOUND: return "method";
        default:     return "function";
        }
    }
    return "?";
}

static bool is_number(Value v) { return v.k == V_INT || v.k == V_NUM || v.k == V_BOOL; }
static bool is_callable(Value v)
{
    return v.k == V_OBJ && (v.as.o->kind == O_NATIVE || v.as.o->kind == O_FUNC || v.as.o->kind == O_CLOSURE ||
                            v.as.o->kind == O_BOUND || v.as.o->kind == O_CLASS);
}

bool vm_method(Value x, const char *name, Value *out)
{
    if (!IS_INSTANCE(x)) return false;
    return class_find(((Instance *)x.as.o)->cls, name, strlen(name), out);
}

bool vm_isinstance(Value x, Value cls)
{
    if (!IS_INSTANCE(x) || !IS_CLASS(cls)) return false;
    for (Value c = ((Instance *)x.as.o)->cls; IS_CLASS(c); c = ((Klass *)c.as.o)->base)
        if (c.as.o == cls.as.o) return true;
    return false;
}

Value vm_exc_class(VM *v, const char *name)
{
    for (int i = 0; i < NEXC; i++)
        if (!strcmp(EXC_NAME[i], name) && v->gdef[i] && IS_CLASS(v->global[i])) return v->global[i];
    return VAL_NIL;
}

static bool is_exception(VM *v, Value x) { return v->gdef[0] && vm_isinstance(x, v->global[0]); }

/* str(x) of an instance, by its __str__ (or __repr__), for print and
 * f-strings: true with the words put in b, false to let it print as
 * <Name object>. What went wrong in the script before is kept. */
static bool inst_text(Buf *b, Value x, bool repr)
{
    VM *v = g_text_vm;
    Value fn;
    if (!v || g_text_depth > 8) return false;
    if (!(repr ? vm_method(x, "__repr__", &fn) : (vm_method(x, "__str__", &fn) || vm_method(x, "__repr__", &fn)))) return false;
    char saved[NOM_ERR_MAX];
    memcpy(saved, v->err, sizeof saved);
    Value se = v->exc, kw = v->kw;
    v->exc = VAL_NIL;
    v->kw = VAL_NIL;
    v->err[0] = 0;
    Value out = VAL_NIL;
    g_text_depth++;
    bool ok = vm_call(v, fn, &x, 1, &out) && IS_STR(out);
    g_text_depth--;
    if (ok) buf_put(b, AS_STR(out)->s, AS_STR(out)->len);
    val_release(out);
    val_release(v->exc);
    v->exc = se;
    v->kw = kw;
    memcpy(v->err, saved, sizeof saved);
    return ok;
}

/* an exception of the builtin kind called `kind`, its words `msg` */
static Value make_exc(VM *v, const char *kind, const char *msg, size_t len)
{
    Value cls = vm_exc_class(v, kind);
    if (cls.k == V_NIL) cls = vm_exc_class(v, "Exception");
    Value inst = instance_new(cls);
    Value args = list_new();
    list_push(AS_LIST(args), str_new(msg, len));
    AS_LIST(args)->tuple = 1;
    Dict *fd = AS_DICT(((Instance *)inst.as.o)->fields);
    dict_set(fd, str_newz("args"), args);
    dict_set(fd, str_newz("_msg"), str_new(msg, len));
    return inst;
}

/* what went wrong (v->err, "KeyError: 'x'"), as the exception it is */
static Value exc_from_err(VM *v)
{
    const char *e = v->err;
    int k = 0;
    while ((e[k] >= 'A' && e[k] <= 'Z') || (e[k] >= 'a' && e[k] <= 'z')) k++;
    if (k && e[k] == ':' && e[k + 1] == ' ') {
        char kind[40];
        if (k < (int)sizeof kind) {
            memcpy(kind, e, (size_t)k);
            kind[k] = 0;
            if (vm_exc_class(v, kind).k != V_NIL) return make_exc(v, kind, e + k + 2, strlen(e + k + 2));
        }
    }
    return make_exc(v, "Exception", e, strlen(e));
}

bool vm_new_instance(VM *v, Value cls, Value *args, int n, Value *out)
{
    Value inst = instance_new(cls), init;
    if (class_find(cls, "__init__", 8, &init)) {
        Value all[NOM_ARGS_MAX + 1];
        if (n >= NOM_ARGS_MAX) { vm_runtime_error(v, "TypeError: too many arguments"); val_release(inst); return false; }
        all[0] = inst;
        for (int i = 0; i < n; i++) all[i + 1] = args[i];
        Value r = VAL_NIL;
        if (!vm_call(v, init, all, n + 1, &r)) { val_release(inst); return false; }
        val_release(r);
    } else if (n) {
        vm_runtime_error(v, "TypeError: %s() takes no arguments (it has no __init__)", ((Klass *)cls.as.o)->name);
        val_release(inst);
        return false;
    }
    *out = inst;
    return true;
}

void vm_raise(VM *v, Value x)
{
    Value inst = VAL_NIL;
    if (IS_CLASS(x)) { if (!vm_new_instance(v, x, NULL, 0, &inst)) return; }
    else if (IS_INSTANCE(x)) inst = val_retain(x);
    else if (IS_STR(x)) inst = make_exc(v, "Exception", AS_STR(x)->s, AS_STR(x)->len);
    else { vm_runtime_error(v, "TypeError: raise needs an error: raise ValueError(\"why\")"); return; }
    if (!is_exception(v, inst)) {
        vm_runtime_error(v, "TypeError: a %s can't be raised: build the class on Exception (class %s(Exception):)", type_name(inst), type_name(inst));
        val_release(inst);
        return;
    }
    Buf b; buf_init(&b);
    val_tostr(&b, inst);
    const char *nm = ((Klass *)((Instance *)inst.as.o)->cls.as.o)->name;
    if (b.len) vm_runtime_error(v, "%s: %.*s", nm, (int)(b.len < 200 ? b.len : 200), b.p);
    else vm_runtime_error(v, "%s", nm);
    buf_free(&b);
    val_release(v->exc);
    v->exc = inst;
}

/* any sequence's items, in a new list: list, tuple, set, range, string
 * (its letters), dict (its keys), a generator (run to its end), an
 * instance with __iter__ */
bool vm_items(VM *v, Value s, Value *out)
{
    Value l = list_new();
    List *o = AS_LIST(l);
    if (IS_LIST(s)) for (uint32_t i = 0; i < AS_LIST(s)->len; i++) list_push(o, val_retain(AS_LIST(s)->v[i]));
    else if (IS_RANGE(s)) {
        Range *r = AS_RANGE(s);
        int64_t n = range_len(r);
        if (n > 1000000) { val_release(l); vm_runtime_error(v, "MemoryError: a list of %lld things won't fit", (long long)n); return false; }
        for (int64_t i = 0; i < n; i++) list_push(o, VAL_INT(r->start + i * r->step));
    }
    else if (IS_STR(s)) for (uint32_t i = 0; i < AS_STR(s)->len; i++) list_push(o, str_new(AS_STR(s)->s + i, 1));
    else if (IS_DICT(s)) for (uint32_t i = 0; i < AS_DICT(s)->len; i++) list_push(o, val_retain(AS_DICT(s)->k[i]));
    else if (IS_GEN(s)) {
        for (;;) {
            Value x;
            bool got;
            if (!gen_next(v, s, &x, &got)) { val_release(l); return false; }
            if (!got) break;
            list_push(o, x);
            if (o->len > 1000000) { val_release(l); vm_runtime_error(v, "MemoryError: a generator that doesn't end"); return false; }
        }
    } else {
        Value it;
        if (vm_method(s, "__iter__", &it)) {
            Value r;
            val_release(l);
            if (!vm_call(v, it, &s, 1, &r)) return false;
            bool ok = vm_items(v, r, out);
            val_release(r);
            return ok;
        }
        val_release(l);
        vm_runtime_error(v, "TypeError: a %s isn't something to go through (a list, a string, a range...)", type_name(s));
        return false;
    }
    *out = l;
    return true;
}

/* ------------------------------------------------------------ sets */
static bool set_has(List *l, Value x)
{
    for (uint32_t i = 0; i < l->len; i++) if (val_equal(l->v[i], x)) return true;
    return false;
}

/* a | b, a & b, a - b, a ^ b of two sets */
static Value set_op(OpCode op, List *a, List *b)
{
    Value r = list_new();
    List *o = AS_LIST(r);
    o->tuple = 2;
    for (uint32_t i = 0; i < a->len; i++) {
        bool inb = set_has(b, a->v[i]);
        if ((op == OP_BOR) || (op == OP_BAND && inb) || ((op == OP_SUB || op == OP_BXOR) && !inb)) list_push(o, val_retain(a->v[i]));
    }
    if (op == OP_BOR || op == OP_BXOR)
        for (uint32_t i = 0; i < b->len; i++)
            if (!set_has(a, b->v[i])) list_push(o, val_retain(b->v[i]));
    return r;
}

/* a class's __add__ (or the other's __radd__) and the rest: 1 done (*out),
 * 0 it has none, -1 it went wrong */
static int dunder_binop(VM *v, OpCode op, Value a, Value b, Value *out)
{
    static const char *NAMES[][2] = {
        [OP_ADD] = { "__add__", "__radd__" }, [OP_SUB] = { "__sub__", "__rsub__" }, [OP_MUL] = { "__mul__", "__rmul__" },
        [OP_DIV] = { "__truediv__", "__rtruediv__" }, [OP_FDIV] = { "__floordiv__", "__rfloordiv__" }, [OP_MOD] = { "__mod__", "__rmod__" },
        [OP_POW] = { "__pow__", "__rpow__" }, [OP_EQ] = { "__eq__", "__eq__" }, [OP_NE] = { "__ne__", "__ne__" },
        [OP_LT] = { "__lt__", "__gt__" }, [OP_LE] = { "__le__", "__ge__" }, [OP_GT] = { "__gt__", "__lt__" }, [OP_GE] = { "__ge__", "__le__" },
        [OP_BAND] = { "__and__", "__rand__" }, [OP_BOR] = { "__or__", "__ror__" }, [OP_BXOR] = { "__xor__", "__rxor__" },
        [OP_SHL] = { "__lshift__", "__rlshift__" }, [OP_SHR] = { "__rshift__", "__rrshift__" },
    };
    if (op >= (int)(sizeof NAMES / sizeof NAMES[0]) || !NAMES[op][0]) return 0;
    Value fn, args[2];
    if (vm_method(a, NAMES[op][0], &fn)) { args[0] = a; args[1] = b; }
    else if (vm_method(b, NAMES[op][1], &fn)) { args[0] = b; args[1] = a; }
    else if (op == OP_NE) {
        /* != is not ==, when only __eq__ is said */
        Value r;
        int d = dunder_binop(v, OP_EQ, a, b, &r);
        if (d <= 0) return d;
        *out = VAL_BOOL(!val_truthy(r));
        val_release(r);
        return 1;
    } else return 0;
    return vm_call(v, fn, args, 2, out) ? 1 : -1;
}

/* Arithmetic keeps ints as ints (so tick counters stay exact) and promotes to
 * double only when an operand already is one. `/` is always true division. */
static bool arith(VM *v, OpCode op, Value a, Value b, Value *out)
{
    if (IS_STR(a) && IS_STR(b) && op == OP_ADD) {
        Str *x = AS_STR(a), *y = AS_STR(b);
        Buf t; buf_init(&t);
        buf_put(&t, x->s, x->len);
        buf_put(&t, y->s, y->len);
        *out = str_new(t.p ? t.p : "", t.len);
        buf_free(&t);
        return true;
    }
    if (IS_SET(a) && IS_SET(b) && op == OP_SUB) { *out = set_op(op, AS_LIST(a), AS_LIST(b)); return true; }
    if (IS_LIST(a) && IS_LIST(b) && op == OP_ADD) {
        if (AS_LIST(a)->tuple != AS_LIST(b)->tuple || AS_LIST(a)->tuple == 2) {
            vm_runtime_error(v, "TypeError: can't add a %s and a %s (list(x) makes either a list)", type_name(a), type_name(b));
            return false;
        }
        Value r = list_new();
        List *x = AS_LIST(a), *y = AS_LIST(b), *z = AS_LIST(r);
        for (uint32_t i = 0; i < x->len; i++) list_push(z, val_retain(x->v[i]));
        for (uint32_t i = 0; i < y->len; i++) list_push(z, val_retain(y->v[i]));
        z->tuple = x->tuple;
        *out = r;
        return true;
    }
    if (IS_STR(a) && op == OP_MOD)         /* TUX ROCKET SCIENCE: "%.1f km" % x */
        return host_percent(v, a, b, out);
    if (op == OP_MUL && ((IS_LIST(a) && b.k == V_INT) || (a.k == V_INT && IS_LIST(b)))) {
        /* [0] * 5 */
        List *x = AS_LIST(IS_LIST(a) ? a : b);
        int64_t n = IS_LIST(a) ? b.as.i : a.as.i;
        if (n * (int64_t)x->len > 1000000) { vm_runtime_error(v, "MemoryError: a list that long won't fit"); return false; }
        Value r = list_new();
        for (int64_t k = 0; k < n; k++)
            for (uint32_t i = 0; i < x->len; i++) list_push(AS_LIST(r), val_retain(x->v[i]));
        AS_LIST(r)->tuple = x->tuple == 1 ? 1 : 0;
        *out = r;
        return true;
    }
    if (a.k == V_INT && IS_STR(b) && op == OP_MUL) { Value t = a; a = b; b = t; }
    if (IS_STR(a) && b.k == V_INT && op == OP_MUL) {
        Str *x = AS_STR(a);
        int64_t n = b.as.i;
        Buf t; buf_init(&t);
        for (int64_t i = 0; i < n && t.len < 65536; i++) buf_put(&t, x->s, x->len);
        *out = str_new(t.p ? t.p : "", t.len);
        buf_free(&t);
        return true;
    }
    if (op == OP_ADD && (IS_STR(a) || IS_STR(b)) && (is_number(a) || is_number(b))) {
        vm_runtime_error(v, "TypeError: can't add a number and text -- str(x) makes x text, or use an f-string: f\"{x} m\"");
        return false;
    }
    if (!is_number(a) || !is_number(b)) {
        static const char *SYM[] = { [OP_ADD] = "+", [OP_SUB] = "-", [OP_MUL] = "*", [OP_DIV] = "/", [OP_FDIV] = "//", [OP_MOD] = "%", [OP_POW] = "**" };
        if (a.k == V_NIL || b.k == V_NIL)
            vm_runtime_error(v, "TypeError: can't do %s with None (a value that isn't set -- a function that returned nothing?)", op <= OP_POW ? SYM[op] : "that");
        else
            vm_runtime_error(v, "TypeError: can't do %s to a %s and a %s", op <= OP_POW ? SYM[op] : "that", type_name(a), type_name(b));
        return false;
    }

    bool ints = (a.k != V_NUM) && (b.k != V_NUM);
    if (ints) {
        int64_t x = val_int(a), y = val_int(b);
        int64_t r;
        /* (too big for 64 bits: a float, rather than a number gone wrong) */
        switch (op) {
        case OP_ADD: *out = __builtin_add_overflow(x, y, &r) ? VAL_NUM((double)x + (double)y) : VAL_INT(r); return true;
        case OP_SUB: *out = __builtin_sub_overflow(x, y, &r) ? VAL_NUM((double)x - (double)y) : VAL_INT(r); return true;
        case OP_MUL: *out = __builtin_mul_overflow(x, y, &r) ? VAL_NUM((double)x * (double)y) : VAL_INT(r); return true;
        case OP_DIV:
            if (y == 0) { vm_runtime_error(v, "ZeroDivisionError: division by zero"); return false; }
            *out = VAL_NUM((double)x / (double)y);   /* Python-3 true division */
            return true;
        case OP_FDIV:
            if (y == 0) { vm_runtime_error(v, "ZeroDivisionError: integer division or modulo by zero"); return false; }
            { int64_t q = x / y; if ((x % y != 0) && ((x < 0) != (y < 0))) q--; *out = VAL_INT(q); }
            return true;
        case OP_MOD:
            if (y == 0) { vm_runtime_error(v, "ZeroDivisionError: integer modulo by zero"); return false; }
            { int64_t r = x % y; if (r != 0 && ((r < 0) != (y < 0))) r += y; *out = VAL_INT(r); }
            return true;
        case OP_POW:
            if (y >= 0 && y < 64) {
                int64_t pr = 1;
                bool over = false;
                for (int64_t i = 0; i < y && !over; i++) over = __builtin_mul_overflow(pr, x, &pr);
                *out = over ? VAL_NUM(nom_pow((double)x, (double)y)) : VAL_INT(pr);
            } else {
                *out = VAL_NUM(nom_pow((double)x, (double)y));
            }
            return true;
        default: break;
        }
    }

    double x = val_num(a), y = val_num(b);
    switch (op) {
    case OP_ADD:  *out = VAL_NUM(x + y); return true;
    case OP_SUB:  *out = VAL_NUM(x - y); return true;
    case OP_MUL:  *out = VAL_NUM(x * y); return true;
    case OP_DIV:
        if (y == 0.0) { vm_runtime_error(v, "ZeroDivisionError: float division by zero"); return false; }
        *out = VAL_NUM(x / y);
        return true;
    case OP_FDIV:
        if (y == 0.0) { vm_runtime_error(v, "ZeroDivisionError: float floor division by zero"); return false; }
        *out = VAL_NUM(nom_floor(x / y));
        return true;
    case OP_MOD:
        if (y == 0.0) { vm_runtime_error(v, "ZeroDivisionError: float modulo"); return false; }
        { double r = x - y * nom_floor(x / y); *out = VAL_NUM(r); }
        return true;
    case OP_POW:
        if (x < 0 && y != nom_floor(y)) { vm_runtime_error(v, "ValueError: a negative number to a fractional power"); return false; }
        *out = VAL_NUM(nom_pow(x, y));
        return true;
    default: break;
    }
    vm_runtime_error(v, "bad arithmetic opcode");
    return false;
}

/* & | ^ << >>: of whole numbers (and of True and False), or of two sets */
static bool bitop(VM *v, OpCode op, Value a, Value b, Value *out)
{
    if (IS_SET(a) && IS_SET(b) && op != OP_SHL && op != OP_SHR) { *out = set_op(op, AS_LIST(a), AS_LIST(b)); return true; }
    if ((a.k != V_INT && a.k != V_BOOL) || (b.k != V_INT && b.k != V_BOOL)) {
        vm_runtime_error(v, "TypeError: & | ^ << >> are for whole numbers (or sets), not a %s and a %s", type_name(a), type_name(b));
        return false;
    }
    int64_t x = val_int(a), y = val_int(b);
    switch (op) {
    case OP_BAND: *out = a.k == V_BOOL && b.k == V_BOOL ? VAL_BOOL(x & y) : VAL_INT(x & y); return true;
    case OP_BOR:  *out = a.k == V_BOOL && b.k == V_BOOL ? VAL_BOOL(x | y) : VAL_INT(x | y); return true;
    case OP_BXOR: *out = a.k == V_BOOL && b.k == V_BOOL ? VAL_BOOL(x ^ y) : VAL_INT(x ^ y); return true;
    case OP_SHL:
        if (y < 0) { vm_runtime_error(v, "ValueError: negative shift count"); return false; }
        *out = y >= 63 || (x && (x << y) >> y != x) ? VAL_NUM((double)x * nom_pow(2, (double)y)) : VAL_INT(x << y);
        return true;
    case OP_SHR:
        if (y < 0) { vm_runtime_error(v, "ValueError: negative shift count"); return false; }
        *out = VAL_INT(y >= 63 ? (x < 0 ? -1 : 0) : x >> y);
        return true;
    default: break;
    }
    return false;
}

static bool compare(VM *v, OpCode op, Value a, Value b, bool *out)
{
    if (op == OP_EQ) { *out = val_equal(a, b); return true; }
    if (op == OP_NE) { *out = !val_equal(a, b); return true; }
    if (IS_STR(a) && IS_STR(b)) {
        Str *x = AS_STR(a), *y = AS_STR(b);
        uint32_t n = x->len < y->len ? x->len : y->len;
        int c = memcmp(x->s, y->s, n);
        if (c == 0) c = (x->len < y->len) ? -1 : (x->len > y->len) ? 1 : 0;
        switch (op) {
        case OP_LT: *out = c <  0; return true;
        case OP_LE: *out = c <= 0; return true;
        case OP_GT: *out = c >  0; return true;
        case OP_GE: *out = c >= 0; return true;
        default: break;
        }
    }
    if (IS_SET(a) && IS_SET(b)) {
        /* a <= b: a is a subset of b */
        List *x = AS_LIST(a), *y = AS_LIST(b);
        bool sub = true, sup = true;
        for (uint32_t i = 0; i < x->len && sub; i++) sub = set_has(y, x->v[i]);
        for (uint32_t i = 0; i < y->len && sup; i++) sup = set_has(x, y->v[i]);
        *out = op == OP_LE ? sub : op == OP_GE ? sup : op == OP_LT ? sub && x->len < y->len : sup && x->len > y->len;
        return true;
    }
    if (IS_LIST(a) && IS_LIST(b)) {
        /* [1, 2] < [1, 3]: item by item */
        List *x = AS_LIST(a), *y = AS_LIST(b);
        for (uint32_t i = 0; i < x->len && i < y->len; i++) {
            if (val_equal(x->v[i], y->v[i])) continue;
            return compare(v, op, x->v[i], y->v[i], out);
        }
        Value lx = VAL_INT(x->len), ly = VAL_INT(y->len);
        return compare(v, op, lx, ly, out);
    }
    if (!is_number(a) || !is_number(b)) {
        if ((IS_STR(a) && is_number(b)) || (is_number(a) && IS_STR(b)))
            vm_runtime_error(v, "TypeError: can't compare a number and text -- float(x) makes text a number");
        else if (a.k == V_NIL || b.k == V_NIL)
            vm_runtime_error(v, "TypeError: can't put None in order with a %s (a value that isn't set yet?)", type_name(a.k == V_NIL ? b : a));
        else
            vm_runtime_error(v, "TypeError: can't put a %s and a %s in order", type_name(a), type_name(b));
        return false;
    }
    double x = val_num(a), y = val_num(b);
    switch (op) {
    case OP_LT: *out = x <  y; return true;
    case OP_LE: *out = x <= y; return true;
    case OP_GT: *out = x >  y; return true;
    case OP_GE: *out = x >= y; return true;
    default: break;
    }
    return false;
}

static bool index_get(VM *v, Value box, Value key, Value *out)
{
    if (IS_LIST(box)) {
        if (key.k != V_INT && key.k != V_BOOL) { vm_runtime_error(v, "TypeError: a %s index must be a whole number, not a %s", type_name(box), type_name(key)); return false; }
        List *l = AS_LIST(box);
        int64_t i = val_int(key);
        if (i < 0) i += l->len;
        if (i < 0 || i >= (int64_t)l->len) {
            vm_runtime_error(v, "IndexError: %s index out of range (index %lld, the %s has %lld)", type_name(box), (long long)val_int(key),
                             type_name(box), (long long)l->len);
            return false;
        }
        *out = val_retain(l->v[i]);
        return true;
    }
    if (IS_DICT(box)) {
        Value got;
        if (!dict_get(AS_DICT(box), key, &got)) {
            Buf b; buf_init(&b);
            val_repr(&b, key);
            vm_runtime_error(v, "KeyError: %s", b.p ? b.p : "?");
            buf_free(&b);
            return false;
        }
        *out = val_retain(got);
        return true;
    }
    if (IS_RANGE(box)) {
        Range *r = AS_RANGE(box);
        int64_t n = range_len(r), i = val_int(key);
        if (i < 0) i += n;
        if (i < 0 || i >= n) { vm_runtime_error(v, "IndexError: range object index out of range"); return false; }
        *out = VAL_INT(r->start + i * r->step);
        return true;
    }
    if (IS_STR(box)) {
        if (key.k != V_INT) { vm_runtime_error(v, "TypeError: string indices must be whole numbers"); return false; }
        Str *s = AS_STR(box);
        int64_t i = key.as.i;
        if (i < 0) i += s->len;
        if (i < 0 || i >= (int64_t)s->len) { vm_runtime_error(v, "IndexError: string index out of range"); return false; }
        *out = str_new(s->s + i, 1);
        return true;
    }
    Value fn;
    if (vm_method(box, "__getitem__", &fn)) {
        Value args[2] = { box, key };
        return vm_call(v, fn, args, 2, out);
    }
    vm_runtime_error(v, "TypeError: a %s can't be indexed with [ ]", type_name(box));
    return false;
}

/* -------------------------------------------------------------- the loop */
#define READ_BYTE()  (chunk->code[f->ip++])
#define READ_U16()   (f->ip += 2, (int)((chunk->code[f->ip - 2] << 8) | chunk->code[f->ip - 1]))
#define READ_S16()   ((int16_t)READ_U16())

/* the position of parameter `name` among fn's from..to, or -1 */
static int param_at(Func *fn, Str *name, int from, int to)
{
    int i = 0;
    for (const char *q = fn->locals ? fn->locals : ""; *q && i < to; i++) {
        const char *e = q;
        while (*e && *e != ',') e++;
        if (i >= from && (uint32_t)(e - q) == name->len && !memcmp(q, name->s, name->len)) return i;
        q = *e ? e + 1 : e;
    }
    return -1;
}

/* TUX ROCKET SCIENCE: into a def (or a lambda, a closure, a method,
 * __init__): the stack is [callee, args...]; the arguments bound to the
 * parameters (keywords by name, *rest and **more gathered, the defaults
 * for the ones left), locals reserved, cells made, the frame pushed.
 * init_self: the instance __init__ is making (what the call gives back),
 * or nil. */
static bool enter_func(VM *v, Value callee, int nargs, Value init_self)
{
    int idx = func_obj_index(callee);
    Func *fn = &v->prog->func[idx];
    int npar = fn->arity + (fn->star >= 0) + fn->nkwonly + (fn->dstar >= 0);
    if (v->kw.k != V_NIL || nargs != fn->arity || npar != fn->arity) {
        Value slot[NOM_ARGS_MAX + 4];
        bool have[NOM_ARGS_MAX + 4];
        Value extra = VAL_NIL;
        if (npar > NOM_ARGS_MAX + 2) { vm_runtime_error(v, "TypeError: %s() has too many parameters", fn->name); return false; }
        for (int i = 0; i < npar; i++) { have[i] = false; slot[i] = VAL_NIL; }
        Value *args = &v->stack[v->sp - nargs];
        int kw0 = fn->star >= 0 ? fn->star + 1 : fn->arity;
        if (nargs > fn->arity && fn->star < 0) {
            if (fn->is_method && fn->arity == 0)
                vm_runtime_error(v, "TypeError: %s() has no self: a method's first parameter is self (def %s(self):)", fn->name, fn->name);
            else if (fn->is_method)
                vm_runtime_error(v, "TypeError: %s() takes %d argument%s besides self, but %d were given", fn->name, fn->arity - 1, fn->arity == 2 ? "" : "s", nargs - 1);
            else
                vm_runtime_error(v, "TypeError: %s() takes %d argument%s, but %d were given", fn->name, fn->arity, fn->arity == 1 ? "" : "s", nargs);
            return false;
        }
        for (int i = 0; i < nargs && i < fn->arity; i++) { slot[i] = args[i]; have[i] = true; }
        if (fn->star >= 0) {
            Value t = list_new();
            for (int i = fn->arity; i < nargs; i++) list_push(AS_LIST(t), args[i]);
            AS_LIST(t)->tuple = 1;
            slot[fn->star] = t;
            have[fn->star] = true;
        }
        v->sp -= nargs;            /* (their values are in slot now) */
        if (fn->dstar >= 0) extra = dict_new();
        if (v->kw.k != V_NIL) {
            Dict *kd = AS_DICT(v->kw);
            for (uint32_t k = 0; k < kd->len; k++) {
                Str *kn = AS_STR(kd->k[k]);
                int at = param_at(fn, kn, 0, fn->arity);
                if (at < 0) at = param_at(fn, kn, kw0, kw0 + fn->nkwonly);
                if (at < 0) {
                    if (extra.k != V_NIL) { dict_set(AS_DICT(extra), val_retain(kd->k[k]), val_retain(kd->v[k])); continue; }
                    vm_runtime_error(v, "TypeError: %s() got an unexpected keyword argument '%s'", fn->name, kn->s);
                    goto bind_fail;
                }
                if (have[at]) { vm_runtime_error(v, "TypeError: %s() got two values for '%s'", fn->name, kn->s); goto bind_fail; }
                slot[at] = val_retain(kd->v[k]);
                have[at] = true;
            }
            val_release(v->kw);
            v->kw = VAL_NIL;
        }
        for (int i = 0; i < fn->arity; i++) {
            if (have[i]) continue;
            int d = i - (fn->arity - fn->ndefaults);
            if (d < 0) {
                /* which one: its name */
                const char *q = fn->locals ? fn->locals : "";
                for (int k = 0; k < i && *q; k++) { while (*q && *q != ',') q++; if (*q) q++; }
                int len = 0;
                while (q[len] && q[len] != ',') len++;
                vm_runtime_error(v, "TypeError: %s() is missing its '%.*s' (it takes %d argument%s, got %d)", fn->name, len, q,
                                 fn->arity - (fn->is_method ? 1 : 0), fn->arity - (fn->is_method ? 1 : 0) == 1 ? "" : "s", nargs - (fn->is_method ? 1 : 0));
                goto bind_fail;
            }
            slot[i] = val_retain(fn->defaults[d]);
            have[i] = true;
        }
        for (int j = 0; j < fn->nkwonly; j++) {
            if (have[kw0 + j]) continue;
            if (fn->kwreq & (1u << j)) { vm_runtime_error(v, "TypeError: %s() needs a keyword argument it wasn't given", fn->name); goto bind_fail; }
            slot[kw0 + j] = val_retain(fn->defaults[fn->ndefaults + j]);
            have[kw0 + j] = true;
        }
        if (fn->dstar >= 0) { slot[fn->dstar] = extra; extra = VAL_NIL; }
        for (int i = 0; i < npar; i++)
            if (!push(v, slot[i])) { for (int k = i + 1; k < npar; k++) val_release(slot[k]); return false; }
        nargs = npar;
        goto bound;
    bind_fail:
        for (int i = 0; i < npar; i++) if (have[i]) val_release(slot[i]);
        val_release(extra);
        return false;
    }
bound:
    if (v->nframes >= NOM_FRAMES_MAX) {
        vm_runtime_error(v, "RecursionError: a function called itself (or others) %d deep, and that's the limit", NOM_FRAMES_MAX);
        return false;
    }
    /* args are already in place; reserve the rest of the locals */
    int base = v->sp - nargs;
    for (int i = nargs; i < fn->nlocals; i++)
        if (!push(v, VAL_NIL)) return false;
    /* the names it shares with a def inside it, in cells */
    for (int i = 0; i < fn->ncells; i++) {
        int s = fn->cell_slot[i];
        v->stack[base + s] = cell_new(v->stack[base + s], s < npar);
    }
    if (callee.as.o->kind == O_CLOSURE) {
        /* the outer function's cells it uses, into their locals */
        Closure *cl = (Closure *)callee.as.o;
        for (int i = 0; i < cl->n && i < fn->ncap; i++) {
            val_release(v->stack[base + fn->cap_slot[i]]);
            v->stack[base + fn->cap_slot[i]] = val_retain(cl->cap[i]);
        }
    }
    struct Frame *nf = &v->frame[v->nframes++];
    nf->fn = fn;
    nf->ip = 0;
    nf->base = base;
    nf->init_self = init_self;
    nf->gen = NULL;
    return true;
}

/* the frame just entered is a generator's: put it by, as a generator
 * object (on the stack where the call's value goes) */
static bool make_gen(VM *v)
{
    struct Frame *fr = &v->frame[v->nframes - 1];
    Value gv = gen_new((int)(fr->fn - v->prog->func));
    Gen *g = (Gen *)gv.as.o;
    g->n = g->cap = v->sp - fr->base;
    g->vals = nom_alloc(sizeof(Value) * (size_t)(g->cap ? g->cap : 1));
    for (int i = 0; i < g->n; i++) g->vals[i] = v->stack[fr->base + i];
    v->sp = fr->base;
    val_release(pop(v));                    /* the callee */
    v->nframes--;
    return push(v, gv);
}

bool gen_next(VM *v, Value gv, Value *out, bool *got)
{
    Gen *g = (Gen *)gv.as.o;
    *got = false;
    if (g->state == 2) return true;
    if (g->state == 1) { vm_runtime_error(v, "ValueError: generator already executing"); return false; }
    if (v->cdepth >= 48) { vm_runtime_error(v, "RecursionError: generators inside generators too deep"); return false; }
    if (v->nframes >= NOM_FRAMES_MAX) { vm_runtime_error(v, "RecursionError: generators inside generators too deep"); return false; }
    if (v->sp + g->n + 64 >= NOM_STACK_MAX) { vm_runtime_error(v, "MemoryError: value stack overflow (STACK_MAX=%d)", NOM_STACK_MAX); return false; }
    int sp0 = v->sp;
    v->stack[v->sp++] = val_retain(gv);    /* (where the callee would be) */
    int base = v->sp;
    for (int i = 0; i < g->n; i++) v->stack[v->sp++] = g->vals[i];
    g->n = 0;
    struct Frame *nf = &v->frame[v->nframes++];
    nf->fn = &v->prog->func[g->fn];
    nf->ip = g->ip;
    nf->base = base;
    nf->init_self = VAL_NIL;
    nf->gen = (Obj *)g;
    /* the try blocks around stay out of reach (they belong to the code
     * that asked); the generator's own come back */
    struct Handler saved[32];
    int nsaved = v->nhandler, saved_stop = v->stop_frames;
    memcpy(saved, v->handler, sizeof saved);
    v->nhandler = 0;
    for (int i = 0; i < g->nh; i++)
        v->handler[v->nhandler++] = (struct Handler){ v->nframes - 1, base + g->hsp[i], g->hip[i] };
    v->stop_frames = v->nframes - 1;
    g->state = 1;
    VmStatus st;
    v->cdepth++;
    while ((st = vm_run(v, 1000000)) == VM_YIELD) { }
    v->cdepth--;
    v->stop_frames = saved_stop;
    memcpy(v->handler, saved, sizeof saved);
    v->nhandler = nsaved;
    if (st != VM_RETURNED) {
        g->state = 2;
        while (v->nframes > 1 && v->frame[v->nframes - 1].base > sp0) v->nframes--;
        while (v->sp > sp0) val_release(pop(v));
        if (st == VM_OK && !v->err[0]) vm_runtime_error(v, "RuntimeError: the script ended inside a generator");
        return false;
    }
    Value r = pop(v);
    if (g->state == 2) { val_release(r); return true; }    /* it returned: the end */
    *out = r;
    *got = true;
    return true;
}

VmStatus vm_run(VM *v, int budget)
{
    if (v->done) return VM_OK;
    if (v->err[0]) return VM_ERROR;

    struct Frame *f = &v->frame[v->nframes - 1];
    Chunk *chunk = &f->fn->chunk;
    int calln = 0;

    again:
    while (budget-- > 0) {
        if (f->ip < 0 || f->ip >= chunk->len) {
            vm_runtime_error(v, "instruction pointer ran off the end of %s", f->fn->name);
            goto fail;
        }
        v->line = chunk->line[f->ip];
        /* THE MEMORY BOUND, CHECKED WHERE TIME IS CHECKED.
         *
         * The instruction budget above stops a script that spins. It does not
         * stop one that allocates: `while true: l = append(l, l)` runs four
         * instructions per iteration and eats the machine. So the allocation
         * meter is read once per instruction, which caps the overshoot past
         * the limit at whatever ONE opcode can allocate -- and every opcode
         * that allocates is itself bounded (a string repeat stops at 64 KiB,
         * a list literal at its own length).
         *
         * It is a runtime error and not a yield, because a script that has
         * hit the wall must not be resumable into hitting it again. */
        if (nom_meter_over()) {
            vm_runtime_error(v, "MemoryError: memory budget exhausted (%zu bytes allocated)",
                             nom_meter_used());
            goto fail;
        }
        v->steps++;
        int call_site = f->ip;
        uint8_t op = READ_BYTE();

        switch (op) {
        case OP_CONST: { int k = READ_U16(); if (!push(v, val_retain(v->prog->consts[k]))) goto fail; break; }
        case OP_NIL:   if (!push(v, VAL_NIL)) goto fail; break;
        case OP_TRUE:  if (!push(v, VAL_BOOL(true))) goto fail; break;
        case OP_FALSE: if (!push(v, VAL_BOOL(false))) goto fail; break;
        case OP_POP:   val_release(pop(v)); break;
        case OP_DUP:   if (!push(v, val_retain(peek(v, 0)))) goto fail; break;

        case OP_GETLOCAL: {
            int s = READ_BYTE();
            if (!push(v, val_retain(v->stack[f->base + s]))) goto fail;
            break;
        }
        case OP_SETLOCAL: {
            int s = READ_BYTE();
            val_release(v->stack[f->base + s]);
            v->stack[f->base + s] = pop(v);
            break;
        }
        case OP_GETCELL: {
            int s = READ_BYTE();
            Value c = v->stack[f->base + s];
            if (c.k == V_OBJ && c.as.o->kind == O_CELL) {
                if (!((Cell *)c.as.o)->set) {
                    vm_runtime_error(v, "NameError: a name used before it was set (in %s)", f->fn->name);
                    goto fail;
                }
                if (!push(v, val_retain(((Cell *)c.as.o)->v))) goto fail;
            } else if (!push(v, val_retain(c))) goto fail;
            break;
        }
        case OP_SETCELL: {
            int s = READ_BYTE();
            Value x = pop(v), c = v->stack[f->base + s];
            if (c.k == V_OBJ && c.as.o->kind == O_CELL) {
                val_release(((Cell *)c.as.o)->v);
                ((Cell *)c.as.o)->v = x;
                ((Cell *)c.as.o)->set = true;
            } else {
                val_release(c);
                v->stack[f->base + s] = cell_new(x, true);
            }
            break;
        }
        case OP_GETGLOBAL: {
            int g = READ_U16();
            if (!v->gdef[g]) {
                vm_runtime_error(v, "NameError: name '%s' is not defined (or not set yet)", v->prog->global[g]);
                goto fail;
            }
            if (!push(v, val_retain(v->global[g]))) goto fail;
            break;
        }
        case OP_SETGLOBAL: {
            int g = READ_U16();
            if (v->gdef[g]) val_release(v->global[g]);
            v->global[g] = pop(v);
            v->gdef[g] = true;
            break;
        }
        case OP_DELNAME: {
            int g = READ_U16();
            if (!v->gdef[g]) { vm_runtime_error(v, "NameError: name '%s' is not defined", v->prog->global[g]); goto fail; }
            val_release(v->global[g]);
            v->global[g] = VAL_NIL;
            v->gdef[g] = false;
            break;
        }
        case OP_GETNATIVE: { int n = READ_BYTE(); if (!push(v, func_obj_new(n, true))) goto fail; break; }
        case OP_GETFUNC:   { int n = READ_BYTE(); if (!push(v, func_obj_new(n, false))) goto fail; break; }

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_FDIV: case OP_MOD: case OP_POW:
        case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: {
            Value b = pop(v), a = pop(v), r = VAL_NIL;
            bool ok;
            int d = (IS_INSTANCE(a) || IS_INSTANCE(b)) ? dunder_binop(v, (OpCode)op, a, b, &r) : 0;
            if (d) ok = d > 0;
            else if (op >= OP_BAND) ok = bitop(v, (OpCode)op, a, b, &r);
            else ok = arith(v, (OpCode)op, a, b, &r);
            val_release(a); val_release(b);
            if (!ok) goto fail;
            if (!push(v, r)) goto fail;
            break;
        }
        case OP_NEG: {
            Value a = pop(v), fn;
            if (vm_method(a, "__neg__", &fn)) {
                Value r;
                bool ok = vm_call(v, fn, &a, 1, &r);
                val_release(a);
                if (!ok || !push(v, r)) goto fail;
                break;
            }
            if (!is_number(a)) { vm_runtime_error(v, "TypeError: can't make a %s negative", type_name(a)); val_release(a); goto fail; }
            Value r = (a.k == V_NUM) ? VAL_NUM(-a.as.d) : VAL_INT(-val_int(a));
            val_release(a);
            if (!push(v, r)) goto fail;
            break;
        }
        case OP_INV: {
            Value a = pop(v);
            if (a.k != V_INT && a.k != V_BOOL) { vm_runtime_error(v, "TypeError: ~ is for whole numbers, not a %s", type_name(a)); val_release(a); goto fail; }
            if (!push(v, VAL_INT(~val_int(a)))) goto fail;
            break;
        }
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            Value b = pop(v), a = pop(v);
            if (IS_INSTANCE(a) || IS_INSTANCE(b)) {
                Value r;
                int d = dunder_binop(v, (OpCode)op, a, b, &r);
                if (d) {
                    val_release(a); val_release(b);
                    if (d < 0 || !push(v, r)) goto fail;
                    break;
                }
            }
            bool r = false;
            bool ok = compare(v, (OpCode)op, a, b, &r);
            val_release(a); val_release(b);
            if (!ok) goto fail;
            if (!push(v, VAL_BOOL(r))) goto fail;
            break;
        }
        case OP_IN: {
            Value box = pop(v), needle = pop(v), fn;
            bool found = false;
            if (IS_LIST(box)) {
                List *l = AS_LIST(box);
                for (uint32_t i = 0; i < l->len && !found; i++) found = val_equal(l->v[i], needle);
            } else if (IS_DICT(box)) {
                Value dummy;
                found = dict_get(AS_DICT(box), needle, &dummy);
            } else if (IS_RANGE(box)) {
                Range *r = AS_RANGE(box);
                int64_t x = val_int(needle), n = range_len(r);
                found = (needle.k == V_INT || (needle.k == V_NUM && (double)x == needle.as.d)) && n > 0 &&
                        (x - r->start) % r->step == 0 && (x - r->start) / r->step >= 0 && (x - r->start) / r->step < n;
            } else if (IS_STR(box) && IS_STR(needle)) {
                Str *h = AS_STR(box), *n = AS_STR(needle);
                if (n->len == 0) found = true;
                else for (uint32_t i = 0; i + n->len <= h->len && !found; i++)
                    found = memcmp(h->s + i, n->s, n->len) == 0;
            } else if (vm_method(box, "__contains__", &fn)) {
                Value args[2] = { box, needle }, r;
                bool ok = vm_call(v, fn, args, 2, &r);
                val_release(box); val_release(needle);
                if (!ok) goto fail;
                found = val_truthy(r);
                val_release(r);
                if (!push(v, VAL_BOOL(found))) goto fail;
                break;
            } else if (IS_GEN(box)) {
                Value l;
                if (!vm_items(v, box, &l)) { val_release(box); val_release(needle); goto fail; }
                for (uint32_t i = 0; i < AS_LIST(l)->len && !found; i++) found = val_equal(AS_LIST(l)->v[i], needle);
                val_release(l);
            } else {
                vm_runtime_error(v, IS_STR(box) ? "TypeError: 'in' a string needs a string to look for, not a %s"
                                                : "TypeError: 'in' needs a list, dict or string, got a %s", type_name(IS_STR(box) ? needle : box));
                val_release(box); val_release(needle);
                goto fail;
            }
            val_release(box); val_release(needle);
            if (!push(v, VAL_BOOL(found))) goto fail;
            break;
        }
        case OP_NOT: { Value a = pop(v); bool t = val_truthy(a); val_release(a); if (!push(v, VAL_BOOL(!t))) goto fail; break; }

        case OP_JMP:  { int off = READ_S16(); f->ip += off; break; }
        case OP_JMPF: { int off = READ_S16(); Value c = pop(v); if (!val_truthy(c)) f->ip += off; val_release(c); break; }
        case OP_JMPF_KEEP: { int off = READ_S16(); if (!val_truthy(peek(v, 0))) f->ip += off; break; }
        case OP_JMPT_KEEP: { int off = READ_S16(); if ( val_truthy(peek(v, 0))) f->ip += off; break; }

        case OP_LIST: {
            int n = READ_BYTE();
            Value l = list_new();
            for (int i = 0; i < n; i++) list_push(AS_LIST(l), v->stack[v->sp - n + i]);
            v->sp -= n;
            if (!push(v, l)) goto fail;
            break;
        }
        case OP_SET: {
            /* {a, b, a}: a set (the repeats dropped) */
            int n = READ_BYTE();
            Value l = list_new();
            AS_LIST(l)->tuple = 2;
            for (int i = 0; i < n; i++) {
                Value x = v->stack[v->sp - n + i];
                if (set_has(AS_LIST(l), x)) val_release(x); else list_push(AS_LIST(l), x);
            }
            v->sp -= n;
            if (!push(v, l)) goto fail;
            break;
        }
        case OP_TOSET: {
            /* a set comprehension's list, as the set */
            Value l = peek(v, 0);
            List *o = AS_LIST(l);
            uint32_t k = 0;
            for (uint32_t i = 0; i < o->len; i++) {
                bool seen = false;
                for (uint32_t j = 0; j < k && !seen; j++) seen = val_equal(o->v[j], o->v[i]);
                if (seen) val_release(o->v[i]); else o->v[k++] = o->v[i];
            }
            o->len = k;
            o->tuple = 2;
            break;
        }
        case OP_EXTEND: {
            /* [*a, ...], f(*a): a's items onto the list below */
            Value x = pop(v), it;
            bool ok = vm_items(v, x, &it);
            val_release(x);
            if (!ok) goto fail;
            List *l = AS_LIST(peek(v, 0));
            for (uint32_t i = 0; i < AS_LIST(it)->len; i++) list_push(l, val_retain(AS_LIST(it)->v[i]));
            val_release(it);
            break;
        }
        case OP_UPDATE: {
            /* {**a, ...}, f(**a): a's keys and values into the dict below */
            Value x = pop(v);
            if (!IS_DICT(x)) { vm_runtime_error(v, "TypeError: ** needs a dict, not a %s", type_name(x)); val_release(x); goto fail; }
            Dict *d = AS_DICT(peek(v, 0));
            for (uint32_t i = 0; i < AS_DICT(x)->len; i++) dict_set(d, val_retain(AS_DICT(x)->k[i]), val_retain(AS_DICT(x)->v[i]));
            val_release(x);
            break;
        }
        case OP_DICT: {
            int n = READ_BYTE();
            Value d = dict_new();
            for (int i = 0; i < n; i++) {
                Value k = v->stack[v->sp - 2 * n + 2 * i];
                Value val = v->stack[v->sp - 2 * n + 2 * i + 1];
                dict_set(AS_DICT(d), k, val);
            }
            v->sp -= 2 * n;
            if (!push(v, d)) goto fail;
            break;
        }
        case OP_INDEX: {
            Value key = pop(v), box = pop(v), r;
            bool ok = index_get(v, box, key, &r);
            val_release(key); val_release(box);
            if (!ok) goto fail;
            if (!push(v, r)) goto fail;
            break;
        }
        case OP_SETINDEX: {
            Value val = pop(v), key = pop(v), box = pop(v), fn;
            if (IS_LIST(box) && AS_LIST(box)->tuple) {
                vm_runtime_error(v, AS_LIST(box)->tuple == 1 ? "TypeError: a tuple can't be changed (make it a list: list(t))" : "TypeError: a set has no [ ] (s.add(x), s.discard(x))");
                goto setindex_fail;
            }
            if (IS_LIST(box)) {
                if (key.k != V_INT) { vm_runtime_error(v, "TypeError: list indices must be whole numbers, not a %s", type_name(key)); goto setindex_fail; }
                List *l = AS_LIST(box);
                int64_t i = key.as.i;
                if (i < 0) i += l->len;
                if (i < 0 || i >= (int64_t)l->len) { vm_runtime_error(v, "IndexError: list assignment index out of range (index %lld, the list has %lld)", (long long)key.as.i, (long long)l->len); goto setindex_fail; }
                val_release(l->v[i]);
                l->v[i] = val;
                val_release(key); val_release(box);
            } else if (IS_DICT(box)) {
                dict_set(AS_DICT(box), key, val);   /* takes both */
                val_release(box);
            } else if (IS_CLASS(box)) {
                dict_set(AS_DICT(((Klass *)box.as.o)->dict), key, val);   /* (a class body's names) */
                val_release(box);
            } else if (vm_method(box, "__setitem__", &fn)) {
                Value args[3] = { box, key, val }, r;
                bool ok = vm_call(v, fn, args, 3, &r);
                val_release(box); val_release(key); val_release(val);
                if (!ok) goto fail;
                val_release(r);
            } else {
                vm_runtime_error(v, IS_STR(box) ? "TypeError: a string can't be changed (make a new one: s[:i] + x + s[i+1:])" : "TypeError: can't assign into a %s with [ ]", type_name(box));
                goto setindex_fail;
            }
            break;
        setindex_fail:
            val_release(val); val_release(key); val_release(box);
            goto fail;
        }
        case OP_DELINDEX: {
            Value key = pop(v), box = pop(v);
            bool ok = true;
            if (IS_LIST(box) && !AS_LIST(box)->tuple && (key.k == V_INT || key.k == V_BOOL)) {
                List *l = AS_LIST(box);
                int64_t i = val_int(key);
                if (i < 0) i += l->len;
                if (i < 0 || i >= (int64_t)l->len) { vm_runtime_error(v, "IndexError: list assignment index out of range"); ok = false; }
                else {
                    val_release(l->v[i]);
                    for (uint32_t j = (uint32_t)i; j + 1 < l->len; j++) l->v[j] = l->v[j + 1];
                    l->len--;
                }
            } else if (IS_DICT(box)) {
                Dict *d = AS_DICT(box);
                uint32_t i = 0;
                while (i < d->len && !val_equal(d->k[i], key)) i++;
                if (i == d->len) {
                    Buf b; buf_init(&b);
                    val_repr(&b, key);
                    vm_runtime_error(v, "KeyError: %s", b.p ? b.p : "?");
                    buf_free(&b);
                    ok = false;
                } else {
                    val_release(d->k[i]);
                    val_release(d->v[i]);
                    for (uint32_t j = i; j + 1 < d->len; j++) { d->k[j] = d->k[j + 1]; d->v[j] = d->v[j + 1]; }
                    d->len--;
                }
            } else { vm_runtime_error(v, "TypeError: can't del from a %s with [ ]", type_name(box)); ok = false; }
            val_release(key); val_release(box);
            if (!ok) goto fail;
            break;
        }
        case OP_DELATTR: {
            int k = READ_U16();
            Value obj = pop(v);
            bool ok = false;
            if (IS_INSTANCE(obj)) {
                Dict *d = AS_DICT(((Instance *)obj.as.o)->fields);
                for (uint32_t i = 0; i < d->len && !ok; i++)
                    if (val_equal(d->k[i], v->prog->consts[k])) {
                        val_release(d->k[i]);
                        val_release(d->v[i]);
                        for (uint32_t j = i; j + 1 < d->len; j++) { d->k[j] = d->k[j + 1]; d->v[j] = d->v[j + 1]; }
                        d->len--;
                        ok = true;
                    }
            }
            if (!ok) vm_runtime_error(v, "AttributeError: no .%s to del", AS_STR(v->prog->consts[k])->s);
            val_release(obj);
            if (!ok) goto fail;
            break;
        }

        case OP_FORPREP: {
            Value seq = peek(v, 0), it;
            if (vm_method(seq, "__iter__", &it)) {
                /* a class with __iter__: what it gives is gone through */
                Value r;
                if (!vm_call(v, it, &seq, 1, &r)) goto fail;
                val_release(seq);
                v->stack[v->sp - 1] = r;
                seq = r;
            }
            if (!IS_LIST(seq) && !IS_STR(seq) && !IS_DICT(seq) && !IS_RANGE(seq) && !IS_GEN(seq)) {
                vm_runtime_error(v, seq.k == V_INT || seq.k == V_NUM ? "TypeError: can't go through a number (range(n) counts to it)"
                                                                     : "TypeError: can't go through a %s with for", type_name(seq));
                goto fail;
            }
            if (!push(v, VAL_INT(0))) goto fail;
            break;
        }
        case OP_FORNEXT: {
            int off = READ_S16();
            Value seq = peek(v, 1);
            if (IS_GEN(seq)) {
                Value x;
                bool got;
                if (!gen_next(v, seq, &x, &got)) goto fail;
                if (!got) { f->ip += off; break; }
                if (!push(v, x)) goto fail;
                break;
            }
            int64_t i = peek(v, 0).as.i;
            int64_t n = IS_LIST(seq) ? AS_LIST(seq)->len
                      : IS_DICT(seq) ? AS_DICT(seq)->len
                      : IS_RANGE(seq) ? range_len(AS_RANGE(seq))
                      : AS_STR(seq)->len;
            if (i >= n) { f->ip += off; break; }
            v->stack[v->sp - 1] = VAL_INT(i + 1);
            Value elem = IS_RANGE(seq) ? VAL_INT(AS_RANGE(seq)->start + i * AS_RANGE(seq)->step)
                       : IS_LIST(seq) ? val_retain(AS_LIST(seq)->v[i])
                       : IS_DICT(seq) ? val_retain(AS_DICT(seq)->k[i])
                       : str_new(AS_STR(seq)->s + i, 1);
            if (!push(v, elem)) goto fail;
            break;
        }

        case OP_CALLEX: {
            /* f(*a, **k): [callee, list, dict or None] spread into a call */
            Value kd = pop(v), al = pop(v);
            List *l = AS_LIST(al);
            if (v->sp + (int)l->len + 8 >= NOM_STACK_MAX) { vm_runtime_error(v, "MemoryError: too many arguments"); val_release(kd); val_release(al); goto fail; }
            for (uint32_t i = 0; i < l->len; i++) v->stack[v->sp++] = val_retain(l->v[i]);
            calln = (int)l->len;
            val_release(al);
            if (IS_DICT(kd) && AS_DICT(kd)->len) { val_release(v->kw); v->kw = kd; }
            else val_release(kd);
            goto do_call;
        }
        case OP_CALL:
            calln = READ_BYTE();
        do_call: {
            int nargs = calln;
            Value callee = v->stack[v->sp - nargs - 1];
            /* TUX ROCKET SCIENCE: ship() is the ship, and still the old call */
            if (callee.k == V_OBJ && callee.as.o->kind == O_HANDLE) {
                Value r = VAL_NIL;
                VmStatus st = host_callmethod(v, callee, NULL, &v->stack[v->sp - nargs], nargs, &r);
                if (st == VM_BLOCKED || st == VM_SLEEP) { f->ip = call_site; return st; }
                if (st == VM_ERROR) goto fail;
                for (int i = 0; i < nargs; i++) val_release(v->stack[v->sp - 1 - i]);
                v->sp -= nargs;
                val_release(pop(v));
                if (!push(v, r)) goto fail;
                break;
            }
            Value callf;
            bool inst_call = IS_INSTANCE(callee) && vm_method(callee, "__call__", &callf);
            if ((IS_BOUND(callee) && ((Bound *)callee.as.o)->fn.k != V_NIL) || inst_call) {
                /* obj.f(...) as a value, or an instance's __call__: the
                 * function, with obj first */
                Value self = val_retain(inst_call ? callee : ((Bound *)callee.as.o)->self);
                Value fn = val_retain(inst_call ? callf : ((Bound *)callee.as.o)->fn);
                if (!push(v, VAL_NIL)) { val_release(self); val_release(fn); goto fail; }
                for (int i = 0; i < nargs; i++) v->stack[v->sp - 1 - i] = v->stack[v->sp - 2 - i];
                v->stack[v->sp - nargs - 1] = self;
                val_release(v->stack[v->sp - nargs - 2]);
                v->stack[v->sp - nargs - 2] = fn;
                calln = nargs + 1;
                goto do_call;
            }
            if (IS_BOUND(callee)) {
                /* a list's, a string's, a part's own method, as a value */
                Bound *bd = (Bound *)callee.as.o;
                Value r = VAL_NIL;
                v->kw_used = false;
                VmStatus st = host_callmethod(v, bd->self, AS_STR(bd->name), &v->stack[v->sp - nargs], nargs, &r);
                if (st == VM_OK && v->kw.k != V_NIL && !v->kw_used) { vm_runtime_error(v, "TypeError: .%s() takes no keyword arguments", AS_STR(bd->name)->s); st = VM_ERROR; }
                val_release(v->kw);
                v->kw = VAL_NIL;
                if (st == VM_BLOCKED || st == VM_SLEEP) { f->ip = call_site; return st; }
                if (st == VM_ERROR) goto fail;
                for (int i = 0; i < nargs; i++) val_release(v->stack[v->sp - 1 - i]);
                v->sp -= nargs;
                val_release(pop(v));
                if (!push(v, r)) goto fail;
                break;
            }
            if (IS_CLASS(callee)) {
                /* PID(1, 2): a new instance, and its __init__ run on it */
                Value inst = instance_new(callee), init;
                if (!class_find(callee, "__init__", 8, &init)) {
                    if (nargs || v->kw.k != V_NIL) {
                        vm_runtime_error(v, "TypeError: %s() takes no arguments (it has no __init__)", ((Klass *)callee.as.o)->name);
                        val_release(inst);
                        goto fail;
                    }
                    val_release(pop(v));
                    if (!push(v, inst)) goto fail;
                    break;
                }
                if (init.k == V_OBJ && init.as.o->kind == O_NATIVE) {
                    /* (an exception's: BaseException.__init__) */
                    Value all[NOM_ARGS_MAX + 1], r;
                    if (nargs >= NOM_ARGS_MAX) { vm_runtime_error(v, "TypeError: too many arguments"); val_release(inst); goto fail; }
                    all[0] = inst;
                    for (int i = 0; i < nargs; i++) all[i + 1] = v->stack[v->sp - nargs + i];
                    bool ok = vm_call(v, init, all, nargs + 1, &r);
                    val_release(v->kw);
                    v->kw = VAL_NIL;
                    if (!ok) { val_release(inst); goto fail; }
                    val_release(r);
                    for (int i = 0; i < nargs; i++) val_release(v->stack[v->sp - 1 - i]);
                    v->sp -= nargs;
                    val_release(pop(v));
                    if (!push(v, inst)) goto fail;
                    break;
                }
                /* [class, args...] -> [__init__, instance, args...] */
                if (!push(v, VAL_NIL)) { val_release(inst); goto fail; }
                for (int i = 0; i < nargs; i++) v->stack[v->sp - 1 - i] = v->stack[v->sp - 2 - i];
                v->stack[v->sp - nargs - 1] = val_retain(inst);
                val_release(v->stack[v->sp - nargs - 2]);
                v->stack[v->sp - nargs - 2] = val_retain(init);
                if (!enter_func(v, init, nargs + 1, inst)) goto fail;
                f = &v->frame[v->nframes - 1];
                chunk = &f->fn->chunk;
                break;
            }
            if (callee.k != V_OBJ ||
                (callee.as.o->kind != O_NATIVE && callee.as.o->kind != O_FUNC && callee.as.o->kind != O_CLOSURE)) {
                if (callee.k == V_NIL) vm_runtime_error(v, "TypeError: None can't be called (a name set to None, or a function that returned nothing?)");
                else if (is_number(callee)) vm_runtime_error(v, "TypeError: a number can't be called (a name that's a number, used as a function? or a missing * : 2(x) is 2 * (x))");
                else vm_runtime_error(v, "TypeError: a %s can't be called (there's no () on it)", type_name(callee));
                goto fail;
            }
            int idx = func_obj_index(callee);

            if (callee.as.o->kind == O_NATIVE) {
                int ncount = 0;
                const Native *tbl = native_table(&ncount);
                const Native *nat = &tbl[idx];
                if (nargs < nat->minargs || nargs > nat->maxargs) {
                    if (nat->minargs == nat->maxargs)
                        vm_runtime_error(v, "TypeError: %s() takes %d argument%s, got %d", nat->name, nat->minargs, nat->minargs == 1 ? "" : "s", nargs);
                    else
                        vm_runtime_error(v, "TypeError: %s() takes %d to %d arguments, got %d", nat->name, nat->minargs, nat->maxargs, nargs);
                    goto fail;
                }
                Value out = VAL_NIL;
                v->kw_used = false;
                VmStatus st = nat->fn(v, &v->stack[v->sp - nargs], nargs, &out);
                if (st == VM_OK && v->kw.k != V_NIL && !v->kw_used) {
                    vm_runtime_error(v, "TypeError: %s() takes no keyword arguments", nat->name);
                    st = VM_ERROR;
                }
                val_release(v->kw);
                v->kw = VAL_NIL;
                if (st == VM_BLOCKED || st == VM_SLEEP) {
                    /* Leave the operands exactly as they are and re-run this
                     * very instruction when we resume. */
                    f->ip = call_site;
                    return st;
                }
                if (st == VM_ERROR) { val_release(out); goto fail; }
                for (int i = 0; i < nargs; i++) val_release(v->stack[v->sp - 1 - i]);
                v->sp -= nargs;
                val_release(pop(v));            /* the callee */
                if (!push(v, out)) goto fail;
                break;
            }

            if (!enter_func(v, callee, nargs, VAL_NIL)) goto fail;
            if (v->prog->func[idx].is_gen) { if (!make_gen(v)) goto fail; break; }
            f = &v->frame[v->nframes - 1];
            chunk = &f->fn->chunk;
            break;
        }
        case OP_RET: {
            while (v->nhandler > 0 && v->handler[v->nhandler - 1].frame >= v->nframes - 1) v->nhandler--;
            Value r = pop(v);
            if (f->init_self.k != V_NIL) { val_release(r); r = f->init_self; f->init_self = VAL_NIL; }
            if (f->gen) { ((Gen *)f->gen)->state = 2; f->gen = NULL; }
            int base = f->base;
            while (v->sp > base) val_release(pop(v));
            val_release(pop(v));                /* the callee object */
            v->nframes--;
            if (v->nframes == 0) { v->done = true; val_release(r); return VM_OK; }
            if (v->stop_frames && v->nframes == v->stop_frames) {
                if (!push(v, r)) goto fail;
                return VM_RETURNED;
            }
            f = &v->frame[v->nframes - 1];
            chunk = &f->fn->chunk;
            if (!push(v, r)) goto fail;
            break;
        }
        case OP_YIELD: {
            /* a generator gives a value: its frame put by, the value to
             * whoever asked (gen_next) */
            Value y = pop(v);
            Gen *g = (Gen *)f->gen;
            if (!g) { val_release(y); vm_runtime_error(v, "RuntimeError: yield outside a generator"); goto fail; }
            int j = v->nhandler;
            while (j > 0 && v->handler[j - 1].frame == v->nframes - 1) j--;
            if (v->nhandler - j > 8) { val_release(y); vm_runtime_error(v, "RuntimeError: a yield inside too many try blocks"); goto fail; }
            g->nh = 0;
            for (int k = j; k < v->nhandler; k++) {
                g->hsp[g->nh] = v->handler[k].sp - f->base;
                g->hip[g->nh++] = v->handler[k].ip;
            }
            v->nhandler = j;
            int n = v->sp - f->base;
            if (n > g->cap) { g->cap = n; g->vals = nom_realloc(g->vals, sizeof(Value) * (size_t)n); }
            for (int i = 0; i < n; i++) g->vals[i] = v->stack[f->base + i];
            g->n = n;
            g->ip = f->ip;
            v->sp = f->base;
            f->gen = NULL;
            val_release(pop(v));                /* (the generator, where the callee was) */
            v->nframes--;
            g->state = 0;
            if (!push(v, y)) goto fail;
            if (v->stop_frames && v->nframes == v->stop_frames) return VM_RETURNED;
            f = &v->frame[v->nframes - 1];
            chunk = &f->fn->chunk;
            break;
        }
        /* TUX ROCKET SCIENCE: the dot */
        case OP_GETATTR: {
            int k = READ_U16();
            Value obj = pop(v), r = VAL_NIL;
            bool ok = host_getattr(v, obj, AS_STR(v->prog->consts[k]), &r);
            val_release(obj);
            if (!ok) goto fail;
            if (!push(v, r)) goto fail;
            break;
        }
        case OP_SETATTR: {
            int k = READ_U16();
            Value val = pop(v), obj = pop(v);
            bool ok = host_setattr(v, obj, AS_STR(v->prog->consts[k]), val);
            val_release(val); val_release(obj);
            if (!ok) goto fail;
            break;
        }
        case OP_CALLMETHOD: {
            int k = READ_U16();
            int nargs = READ_BYTE();
            Value obj = v->stack[v->sp - nargs - 1], r = VAL_NIL;
            if (IS_INSTANCE(obj) || IS_CLASS(obj) || (obj.k == V_OBJ && obj.as.o->kind == O_SUPER)) {
                /* pid.update(err): the class's def, with the instance first
                 * (a function kept in self.x is called as it is); super().f():
                 * the base class's, with self */
                Str *nm = AS_STR(v->prog->consts[k]);
                Value fnv = VAL_NIL, self = obj, got;
                bool found, with_self;
                if (obj.as.o->kind == O_SUPER) {
                    Bound *sp = (Bound *)obj.as.o;
                    found = class_find(sp->fn, nm->s, nm->len, &fnv);
                    self = sp->self;
                    with_self = true;
                } else if (IS_INSTANCE(obj) && dict_get(AS_DICT(((Instance *)obj.as.o)->fields), v->prog->consts[k], &got)) {
                    fnv = got;
                    found = true;
                    with_self = false;
                } else {
                    found = class_find(IS_INSTANCE(obj) ? ((Instance *)obj.as.o)->cls : obj, nm->s, nm->len, &fnv);
                    with_self = IS_INSTANCE(obj);
                }
                if (found && IS_BOUND(fnv) && ((Bound *)fnv.as.o)->self.k == V_NIL && IS_STR(((Bound *)fnv.as.o)->name)) {
                    /* @staticmethod: no self; @classmethod: the class */
                    const char *mk = AS_STR(((Bound *)fnv.as.o)->name)->s;
                    if (!strcmp(mk, "$static")) { fnv = ((Bound *)fnv.as.o)->fn; with_self = false; }
                    else if (!strcmp(mk, "$class")) {
                        fnv = ((Bound *)fnv.as.o)->fn;
                        self = IS_INSTANCE(self) ? ((Instance *)self.as.o)->cls : self;
                        with_self = true;
                    }
                }
                if (!found && obj.as.o->kind == O_SUPER && !strcmp(nm->s, "__init__")) {
                    /* super().__init__() of a class built on nothing: object's, which does nothing */
                    for (int i = 0; i <= nargs; i++) val_release(pop(v));
                    val_release(v->kw);
                    v->kw = VAL_NIL;
                    if (!push(v, VAL_NIL)) goto fail;
                    break;
                }
                if (!found || !is_callable(fnv)) {
                    const char *cn = IS_CLASS(obj) ? ((Klass *)obj.as.o)->name : obj.as.o->kind == O_SUPER ? "super()" : type_name(obj);
                    if (found) vm_runtime_error(v, "TypeError: .%s of a %s isn't something it can do (no ())", nm->s, cn);
                    else vm_runtime_error(v, "AttributeError: a %s has no .%s()", cn, nm->s);
                    goto fail;
                }
                Value fn = val_retain(fnv);
                if (with_self) {
                    /* [obj, args...] -> [fn, self, args...] */
                    Value sv = val_retain(self);
                    if (!push(v, VAL_NIL)) { val_release(fn); val_release(sv); goto fail; }
                    for (int i = 0; i < nargs; i++) v->stack[v->sp - 1 - i] = v->stack[v->sp - 2 - i];
                    v->stack[v->sp - nargs - 1] = sv;
                    val_release(v->stack[v->sp - nargs - 2]);
                    v->stack[v->sp - nargs - 2] = fn;
                    calln = nargs + 1;
                } else {
                    /* [obj, args...] -> [fn, args...] */
                    val_release(v->stack[v->sp - nargs - 1]);
                    v->stack[v->sp - nargs - 1] = fn;
                    calln = nargs;
                }
                goto do_call;
            }
            v->kw_used = false;
            VmStatus st = host_callmethod(v, obj, AS_STR(v->prog->consts[k]), &v->stack[v->sp - nargs], nargs, &r);
            if (st == VM_OK && v->kw.k != V_NIL && !v->kw_used) {
                vm_runtime_error(v, "TypeError: .%s() takes no keyword arguments", AS_STR(v->prog->consts[k])->s);
                st = VM_ERROR;
            }
            val_release(v->kw);
            v->kw = VAL_NIL;
            if (st == VM_BLOCKED || st == VM_SLEEP) { f->ip = call_site; return st; }
            if (st == VM_ERROR) goto fail;
            for (int i = 0; i < nargs; i++) val_release(v->stack[v->sp - 1 - i]);
            v->sp -= nargs;
            val_release(pop(v));                /* the thing */
            if (!push(v, r)) goto fail;
            break;
        }
        case OP_UNPACK: case OP_UNPACKEX: {
            /* a, b = seq: its n items, the first on top; a, *b, c = seq:
             * the ones between, as a list */
            int before = READ_BYTE(), after = op == OP_UNPACKEX ? READ_BYTE() : 0;
            int n = before;
            Value seq = pop(v);
            if (!IS_LIST(seq) || AS_LIST(seq)->tuple == 2) {
                Value l;
                if (!vm_items(v, seq, &l)) {
                    val_release(seq);
                    if (!strncmp(v->err, "TypeError: a ", 13))
                        vm_runtime_error(v, "TypeError: can't unpack a %s into names", type_name(seq));
                    goto fail;
                }
                val_release(seq);
                seq = l;
            }
            List *l = AS_LIST(seq);
            int64_t len = (int64_t)l->len;
            if (op == OP_UNPACK ? len != n : len < before + after) {
                if (op == OP_UNPACKEX) vm_runtime_error(v, "ValueError: not enough values to unpack (expected at least %d, got %lld)", before + after, (long long)len);
                else if (len > n) vm_runtime_error(v, "ValueError: too many values to unpack (expected %d, got %lld)", n, (long long)len);
                else vm_runtime_error(v, "ValueError: not enough values to unpack (expected %d, got %lld)", n, (long long)len);
                val_release(seq);
                goto fail;
            }
            if (v->sp + len + 2 >= NOM_STACK_MAX) { vm_runtime_error(v, "MemoryError: too many to unpack"); val_release(seq); goto fail; }
            if (op == OP_UNPACKEX) {
                for (int i = after - 1; i >= 0; i--) v->stack[v->sp++] = val_retain(l->v[len - after + i]);
                Value mid = list_new();
                for (int64_t i = before; i < len - after; i++) list_push(AS_LIST(mid), val_retain(l->v[i]));
                v->stack[v->sp++] = mid;
            }
            for (int i = n - 1; i >= 0; i--) v->stack[v->sp++] = val_retain(l->v[i]);
            val_release(seq);
            break;
        }
        case OP_SLICE: {
            /* x[a:b:c] -- a new list (or string) */
            Value stp = pop(v), hi = pop(v), lo = pop(v), box = pop(v);
            int64_t n = IS_LIST(box) ? (int64_t)AS_LIST(box)->len : IS_STR(box) ? (int64_t)AS_STR(box)->len : -1;
            if (n < 0 && IS_RANGE(box)) {
                /* range(10)[::3]: as a list of them */
                Value l;
                if (!vm_items(v, box, &l)) { val_release(box); goto fail; }
                val_release(box);
                box = l;
                n = AS_LIST(box)->len;
            }
            if (n < 0 || IS_SET(box)) {
                vm_runtime_error(v, "TypeError: can't slice a %s", type_name(box));
                val_release(box);
                goto fail;
            }
            if ((lo.k != V_NIL && !is_number(lo)) || (hi.k != V_NIL && !is_number(hi)) || (stp.k != V_NIL && !is_number(stp))) {
                vm_runtime_error(v, "TypeError: slice positions must be whole numbers (or left out: x[2:])");
                val_release(box); val_release(lo); val_release(hi); val_release(stp);
                goto fail;
            }
            int64_t st = stp.k == V_NIL ? 1 : val_int(stp);
            if (st == 0) { vm_runtime_error(v, "ValueError: a slice step can't be 0"); val_release(box); goto fail; }
            int64_t a = lo.k == V_NIL ? (st > 0 ? 0 : n - 1) : val_int(lo);
            int64_t b = hi.k == V_NIL ? (st > 0 ? n : -n - 1) : val_int(hi);
            if (a < 0) a += n;
            if (b < 0) b += n;
            if (st > 0) { if (a < 0) a = 0; if (b > n) b = n; }
            else { if (a >= n) a = n - 1; if (b < -1) b = -1; }
            Value r;
            if (IS_STR(box)) {
                Buf t; buf_init(&t);
                for (int64_t i = a; st > 0 ? i < b : i > b; i += st) buf_putc(&t, AS_STR(box)->s[i]);
                r = str_new(t.p ? t.p : "", t.len);
                buf_free(&t);
            } else {
                r = list_new();
                for (int64_t i = a; st > 0 ? i < b : i > b; i += st) list_push(AS_LIST(r), val_retain(AS_LIST(box)->v[i]));
                AS_LIST(r)->tuple = AS_LIST(box)->tuple;
            }
            val_release(box);
            if (!push(v, r)) goto fail;
            break;
        }
        case OP_SETSLICE: {
            /* x[a:b] = items (and del x[a:b]): the slice replaced */
            Value val = pop(v), stp = pop(v), hi = pop(v), lo = pop(v), box = pop(v), it;
            if (!IS_LIST(box) || AS_LIST(box)->tuple || stp.k != V_NIL) {
                vm_runtime_error(v, stp.k != V_NIL ? "TypeError: assigning to a slice with a step isn't supported" : "TypeError: can't assign to a slice of a %s", type_name(box));
                val_release(val); val_release(stp); val_release(box);
                goto fail;
            }
            if (!vm_items(v, val, &it)) { val_release(val); val_release(box); goto fail; }
            val_release(val);
            List *l = AS_LIST(box), *ni = AS_LIST(it);
            int64_t n = l->len;
            int64_t a = lo.k == V_NIL ? 0 : val_int(lo), b = hi.k == V_NIL ? n : val_int(hi);
            if (a < 0) a += n;
            if (b < 0) b += n;
            if (a < 0) a = 0;
            if (a > n) a = n;
            if (b < a) b = a;
            if (b > n) b = n;
            Value *nv = nom_alloc(sizeof(Value) * (size_t)(n - (b - a) + ni->len + 1));
            int64_t k = 0;
            for (int64_t i = 0; i < a; i++) nv[k++] = l->v[i];
            for (uint32_t i = 0; i < ni->len; i++) nv[k++] = val_retain(ni->v[i]);
            for (int64_t i = a; i < b; i++) val_release(l->v[i]);
            for (int64_t i = b; i < n; i++) nv[k++] = l->v[i];
            nom_free(l->v);
            l->v = nv;
            l->len = l->cap = (uint32_t)k;
            if (!l->cap) { l->cap = 1; }
            val_release(it);
            val_release(box);
            break;
        }
        case OP_APPENDN: {
            /* a comprehension's next item, onto its list d below the top */
            int d = READ_BYTE();
            Value x = pop(v);
            Value l = v->stack[v->sp - 1 - d];
            if (IS_LIST(l)) list_push(AS_LIST(l), x); else val_release(x);
            break;
        }
        case OP_KWARGS: {
            /* the next call's keyword arguments: k (name, value) pairs */
            int k = READ_BYTE();
            Value d = dict_new();
            for (int i = 0; i < k; i++)
                dict_set(AS_DICT(d), v->stack[v->sp - 2 * k + 2 * i], v->stack[v->sp - 2 * k + 2 * i + 1]);
            v->sp -= 2 * k;
            val_release(v->kw);
            v->kw = d;
            break;
        }
        case OP_SETDEFAULTS: {
            int fi = READ_BYTE(), n = READ_BYTE();
            Func *fn = &v->prog->func[fi];
            for (int i = 0; i < n; i++) { val_release(fn->defaults[i]); fn->defaults[i] = v->stack[v->sp - n + i]; }
            v->sp -= n;
            break;
        }
        case OP_TRY: {
            int off = READ_S16();
            if (v->nhandler >= 32) { vm_runtime_error(v, "RecursionError: try blocks nested too deep"); goto fail; }
            v->handler[v->nhandler].frame = v->nframes - 1;
            v->handler[v->nhandler].sp = v->sp;
            v->handler[v->nhandler].ip = f->ip + off;
            v->nhandler++;
            break;
        }
        case OP_ENDTRY:
            if (v->nhandler > 0) v->nhandler--;
            break;
        case OP_RAISE: {
            Value x = pop(v);
            vm_raise(v, x);
            val_release(x);
            goto fail;
        }
        case OP_RERAISE:
            /* a bare raise: the error the except has, on up again */
            if (v->handling.k == V_NIL) { vm_runtime_error(v, "RuntimeError: no error to raise again (a bare raise goes in an except)"); goto fail; }
            vm_raise(v, v->handling);
            goto fail;
        case OP_TUPLE: {
            int n = READ_BYTE();
            Value l = list_new();
            for (int i = 0; i < n; i++) list_push(AS_LIST(l), v->stack[v->sp - n + i]);
            AS_LIST(l)->tuple = 1;
            v->sp -= n;
            if (!push(v, l)) goto fail;
            break;
        }
        case OP_CLOSURE: {
            int fi = READ_BYTE(), n = READ_BYTE();
            Value c = closure_new(fi, &v->stack[v->sp - n], n);
            v->sp -= n;
            if (!push(v, c)) goto fail;
            break;
        }
        case OP_EXCMATCH: {
            /* except ValueError / (A, B): is the error (below) one of these
             * kinds, or of a kind built on one? (it stays there) */
            Value kinds = pop(v), e = peek(v, 0);
            bool hit = false, ok = true;
            if (IS_LIST(kinds)) {
                for (uint32_t i = 0; i < AS_LIST(kinds)->len; i++) {
                    Value k = AS_LIST(kinds)->v[i];
                    if (!IS_CLASS(k)) ok = false;
                    else hit |= vm_isinstance(e, k);
                }
            } else if (IS_CLASS(kinds)) hit = vm_isinstance(e, kinds);
            else ok = false;
            val_release(kinds);
            if (!ok) { vm_runtime_error(v, "TypeError: except wants a kind of error (ValueError, or a class built on Exception)"); goto fail; }
            if (!push(v, VAL_BOOL(hit))) goto fail;
            break;
        }
        case OP_EXCTEXT:
            break;
        case OP_WITHEXIT: {
            /* the with block went wrong: [error, manager] -- its __exit__
             * told, and unless it answers True, the error goes on up */
            Value mgr = pop(v), e = pop(v), fn, r = VAL_NIL;
            bool ok = true;
            if (vm_method(mgr, "__exit__", &fn)) {
                Value args[4] = { mgr, IS_INSTANCE(e) ? ((Instance *)e.as.o)->cls : VAL_NIL, e, VAL_NIL };
                ok = vm_call(v, fn, args, 4, &r);
            }
            val_release(mgr);
            if (!ok) { val_release(e); goto fail; }
            bool keep = val_truthy(r);
            val_release(r);
            if (keep) { val_release(e); break; }
            vm_raise(v, e);
            val_release(e);
            goto fail;
        }
        case OP_PUTN: {
            /* a dict comprehension's next key and value, into its dict d below */
            int d = READ_BYTE();
            Value val = pop(v), key = pop(v);
            Value dd = v->stack[v->sp - 1 - d];
            if (IS_DICT(dd)) dict_set(AS_DICT(dd), key, val); else { val_release(key); val_release(val); }
            break;
        }
        case OP_CLASS: {
            /* class Name(Base): a new class, the base's names first */
            int k = READ_U16();
            int hasbase = READ_BYTE();
            Value base = hasbase ? pop(v) : VAL_NIL;
            if (hasbase && !IS_CLASS(base)) {
                vm_runtime_error(v, "TypeError: a class can only build on a class (not a %s)", type_name(base));
                val_release(base);
                goto fail;
            }
            Value c = class_new(AS_STR(v->prog->consts[k])->s, base);
            val_release(base);
            if (!push(v, c)) goto fail;
            break;
        }
        case OP_DUP2: {
            Value a = peek(v, 1), b = peek(v, 0);
            if (!push(v, val_retain(a))) goto fail;
            if (!push(v, val_retain(b))) goto fail;
            break;
        }
        case OP_ROT: {
            /* the value n down comes to the top */
            int n = READ_BYTE();
            Value x = v->stack[v->sp - 1 - n];
            for (int i = v->sp - 1 - n; i < v->sp - 1; i++) v->stack[i] = v->stack[i + 1];
            v->stack[v->sp - 1] = x;
            break;
        }

        case OP_HALT:
            v->done = true;
            return VM_OK;

        default:
            vm_runtime_error(v, "bad opcode %d", op);
            goto fail;
        }
    }
    return VM_YIELD;

fail:
    /* TUX ROCKET SCIENCE: try/except. The innermost handler takes it: the
     * frames above its own are dropped, the stack cut back to where the try
     * began, the error (an exception object) pushed for the except, and on
     * from there. With none, the script stops, as before. */
    if (v->nhandler > 0) {
        struct Handler h = v->handler[--v->nhandler];
        while (v->nframes - 1 > h.frame) {
            struct Frame *fr = &v->frame[v->nframes - 1];
            val_release(fr->init_self);
            fr->init_self = VAL_NIL;
            if (fr->gen) { ((Gen *)fr->gen)->state = 2; fr->gen = NULL; }
            while (v->sp > fr->base) val_release(pop(v));
            val_release(pop(v));                /* the callee */
            v->nframes--;
        }
        while (v->sp > h.sp) val_release(pop(v));
        val_release(v->kw);
        v->kw = VAL_NIL;
        Value e = v->exc.k != V_NIL ? v->exc : exc_from_err(v);
        v->exc = VAL_NIL;
        v->err[0] = 0;
        val_release(v->handling);
        v->handling = val_retain(e);
        f = &v->frame[v->nframes - 1];
        chunk = &f->fn->chunk;
        f->ip = h.ip;
        if (!push(v, e)) return VM_ERROR;
        if (budget > 0) goto again;
        return VM_YIELD;
    }
    return VM_ERROR;
}

/* TUX ROCKET SCIENCE: see lang.h */
bool vm_call(VM *v, Value fn, Value *args, int n, Value *out)
{
    *out = VAL_NIL;
    if (IS_BOUND(fn)) {
        Bound *b = (Bound *)fn.as.o;
        if (b->fn.k == V_NIL) return host_callmethod(v, b->self, AS_STR(b->name), args, n, out) == VM_OK;
        Value all[NOM_ARGS_MAX + 1];
        if (n >= NOM_ARGS_MAX) { vm_runtime_error(v, "TypeError: too many arguments"); return false; }
        all[0] = b->self;
        for (int i = 0; i < n; i++) all[i + 1] = args[i];
        return vm_call(v, b->fn, all, n + 1, out);
    }
    if (IS_CLASS(fn)) return vm_new_instance(v, fn, args, n, out);
    Value cf;
    if (IS_INSTANCE(fn) && vm_method(fn, "__call__", &cf)) {
        Value all[NOM_ARGS_MAX + 1];
        if (n >= NOM_ARGS_MAX) { vm_runtime_error(v, "TypeError: too many arguments"); return false; }
        all[0] = fn;
        for (int i = 0; i < n; i++) all[i + 1] = args[i];
        return vm_call(v, cf, all, n + 1, out);
    }
    if (fn.k != V_OBJ || (fn.as.o->kind != O_FUNC && fn.as.o->kind != O_NATIVE && fn.as.o->kind != O_CLOSURE)) {
        vm_runtime_error(v, "TypeError: a %s can't be called", type_name(fn));
        return false;
    }
    int idx = func_obj_index(fn);
    /* (the keyword arguments of the call that asked are its own) */
    Value skw = v->kw;
    bool sused = v->kw_used;
    v->kw = VAL_NIL;
    if (fn.as.o->kind == O_NATIVE) {
        int cnt = 0;
        const Native *nat = &native_table(&cnt)[idx];
        bool ok;
        if (n < nat->minargs || n > nat->maxargs) {
            vm_runtime_error(v, "TypeError: %s() takes %d to %d arguments, got %d", nat->name, nat->minargs, nat->maxargs, n);
            ok = false;
        } else ok = nat->fn(v, args, n, out) == VM_OK;
        val_release(v->kw);
        v->kw = skw;
        v->kw_used = sused;
        return ok;
    }
    Func *f = &v->prog->func[idx];
    /* the call, laid out as OP_CALL would, and run until it returns (with
     * the try handlers out of reach: they belong to the code around) */
    int sp0 = v->sp;
    bool ok = push(v, val_retain(fn));
    for (int i = 0; i < n && ok; i++) ok = push(v, val_retain(args[i]));
    ok = ok && enter_func(v, fn, n, VAL_NIL);
    v->kw = skw;
    v->kw_used = sused;
    if (!ok) { while (v->sp > sp0) val_release(pop(v)); return false; }
    if (f->is_gen) {
        if (!make_gen(v)) { while (v->sp > sp0) val_release(pop(v)); return false; }
        *out = pop(v);
        return true;
    }
    if (v->cdepth >= 48) {
        while (v->nframes > 1 && v->frame[v->nframes - 1].base > sp0) v->nframes--;
        while (v->sp > sp0) val_release(pop(v));
        vm_runtime_error(v, "RecursionError: calls inside calls too deep (a __str__ or key= function calling itself?)");
        return false;
    }
    int saved_stop = v->stop_frames, saved_h = v->nhandler;
    v->stop_frames = v->nframes - 1;
    v->nhandler = 0;
    VmStatus st;
    v->cdepth++;
    while ((st = vm_run(v, 1000000)) == VM_YIELD) { }
    v->cdepth--;
    v->stop_frames = saved_stop;
    v->nhandler = saved_h;
    if (st != VM_RETURNED) {
        /* (an error: what this call left on the stack goes) */
        while (v->nframes > 1 && v->frame[v->nframes - 1].base > sp0) {
            struct Frame *fr = &v->frame[v->nframes - 1];
            if (fr->gen) { ((Gen *)fr->gen)->state = 2; fr->gen = NULL; }
            val_release(fr->init_self);
            fr->init_self = VAL_NIL;
            v->nframes--;
        }
        while (v->sp > sp0) val_release(pop(v));
        if (st == VM_OK && !v->err[0]) vm_runtime_error(v, "RuntimeError: the script ended inside a call");
        return false;
    }
    *out = pop(v);
    return true;
}
