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
 */
#include "lang.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

void vm_runtime_error(VM *v, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v->err, sizeof v->err, fmt, ap);
    va_end(ap);
}

VM *vm_new(Prog *p, Vfs *fs, Sim *sim)
{
    VM *v = nom_alloc(sizeof(VM));
    v->prog = p;
    v->fs = fs;
    v->sim = sim;
    v->nframes = 1;
    v->frame[0].fn = &p->func[0];
    v->frame[0].ip = 0;
    v->frame[0].base = 0;
    v->sp = p->func[0].nlocals;   /* top-level locals are unused but reserved */
    v->kw = VAL_NIL;
    for (int i = 0; i < v->sp; i++) v->stack[i] = VAL_NIL;
    return v;
}

void vm_free(VM *v)
{
    if (!v) return;
    val_release(v->kw);
    for (int i = 0; i < v->sp; i++) val_release(v->stack[i]);
    for (int i = 0; i < NOM_GLOBALS_MAX; i++)
        if (v->gdef[i]) val_release(v->global[i]);
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
    v->done = false;
    v->err[0] = 0;
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
        vm_runtime_error(v, "value stack overflow (STACK_MAX=%d)", NOM_STACK_MAX);
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
    case V_NIL:  return "nil";
    case V_BOOL: return "bool";
    case V_INT:  return "int";
    case V_NUM:  return "num";
    case V_OBJ:
        switch (v.as.o->kind) {
        case O_STR:  return "str";
        case O_LIST: return "list";
        case O_DICT: return "dict";
        case O_HANDLE: return "ship's part";
        case O_RANGE: return "range";
        case O_CLOSURE: return "function";
        default:     return "function";
        }
    }
    return "?";
}

static bool is_number(Value v) { return v.k == V_INT || v.k == V_NUM || v.k == V_BOOL; }

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
    if (IS_LIST(a) && IS_LIST(b) && op == OP_ADD) {
        Value r = list_new();
        List *x = AS_LIST(a), *y = AS_LIST(b), *z = AS_LIST(r);
        for (uint32_t i = 0; i < x->len; i++) list_push(z, val_retain(x->v[i]));
        for (uint32_t i = 0; i < y->len; i++) list_push(z, val_retain(y->v[i]));
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
        vm_runtime_error(v, "TypeError: can't do that to a %s and a %s",
                         type_name(a), type_name(b));
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
            if (y == 0) { vm_runtime_error(v, "ZeroDivisionError: integer division by zero"); return false; }
            { int64_t q = x / y; if ((x % y != 0) && ((x < 0) != (y < 0))) q--; *out = VAL_INT(q); }
            return true;
        case OP_MOD:
            if (y == 0) { vm_runtime_error(v, "ZeroDivisionError: modulo by zero"); return false; }
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
        if (y == 0.0) { vm_runtime_error(v, "ZeroDivisionError: division by zero"); return false; }
        *out = VAL_NUM(x / y);
        return true;
    case OP_FDIV:
        if (y == 0.0) { vm_runtime_error(v, "ZeroDivisionError: integer division by zero"); return false; }
        *out = VAL_NUM(nom_floor(x / y));
        return true;
    case OP_MOD:
        if (y == 0.0) { vm_runtime_error(v, "ZeroDivisionError: modulo by zero"); return false; }
        { double r = x - y * nom_floor(x / y); *out = VAL_NUM(r); }
        return true;
    case OP_POW:  *out = VAL_NUM(nom_pow(x, y)); return true;
    default: break;
    }
    vm_runtime_error(v, "bad arithmetic opcode");
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
        if (key.k != V_INT) { vm_runtime_error(v, "list index must be an int, got %s", type_name(key)); return false; }
        List *l = AS_LIST(box);
        int64_t i = key.as.i;
        if (i < 0) i += l->len;
        if (i < 0 || i >= (int64_t)l->len) {
            vm_runtime_error(v, "IndexError: list index out of range (index %lld, the list has %lld)", (long long)key.as.i, (long long)l->len);
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
        if (i < 0 || i >= n) { vm_runtime_error(v, "range index %lld out of range", (long long)val_int(key)); return false; }
        *out = VAL_INT(r->start + i * r->step);
        return true;
    }
    if (IS_STR(box)) {
        if (key.k != V_INT) { vm_runtime_error(v, "string index must be an int"); return false; }
        Str *s = AS_STR(box);
        int64_t i = key.as.i;
        if (i < 0) i += s->len;
        if (i < 0 || i >= (int64_t)s->len) { vm_runtime_error(v, "string index out of range"); return false; }
        *out = str_new(s->s + i, 1);
        return true;
    }
    vm_runtime_error(v, "cannot index a %s", type_name(box));
    return false;
}

/* -------------------------------------------------------------- the loop */
#define READ_BYTE()  (chunk->code[f->ip++])
#define READ_U16()   (f->ip += 2, (int)((chunk->code[f->ip - 2] << 8) | chunk->code[f->ip - 1]))
#define READ_S16()   ((int16_t)READ_U16())

VmStatus vm_run(VM *v, int budget)
{
    if (v->done) return VM_OK;
    if (v->err[0]) return VM_ERROR;

    struct Frame *f = &v->frame[v->nframes - 1];
    Chunk *chunk = &f->fn->chunk;

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
            vm_runtime_error(v, "memory budget exhausted (%zu bytes allocated)",
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
        case OP_GETNATIVE: { int n = READ_BYTE(); if (!push(v, func_obj_new(n, true))) goto fail; break; }
        case OP_GETFUNC:   { int n = READ_BYTE(); if (!push(v, func_obj_new(n, false))) goto fail; break; }

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_FDIV: case OP_MOD: case OP_POW: {
            Value b = pop(v), a = pop(v), r;
            bool ok = arith(v, (OpCode)op, a, b, &r);
            val_release(a); val_release(b);
            if (!ok) goto fail;
            if (!push(v, r)) goto fail;
            break;
        }
        case OP_NEG: {
            Value a = pop(v);
            if (!is_number(a)) { vm_runtime_error(v, "cannot negate a %s", type_name(a)); val_release(a); goto fail; }
            Value r = (a.k == V_NUM) ? VAL_NUM(-a.as.d) : VAL_INT(-val_int(a));
            val_release(a);
            if (!push(v, r)) goto fail;
            break;
        }
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            Value b = pop(v), a = pop(v);
            bool r = false;
            bool ok = compare(v, (OpCode)op, a, b, &r);
            val_release(a); val_release(b);
            if (!ok) goto fail;
            if (!push(v, VAL_BOOL(r))) goto fail;
            break;
        }
        case OP_IN: {
            Value box = pop(v), needle = pop(v);
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
            } else {
                vm_runtime_error(v, "'in' needs a list, dict or string, got %s", type_name(box));
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
            Value val = pop(v), key = pop(v), box = pop(v);
            if (IS_LIST(box)) {
                if (key.k != V_INT) { vm_runtime_error(v, "list index must be an int"); goto setindex_fail; }
                List *l = AS_LIST(box);
                int64_t i = key.as.i;
                if (i < 0) i += l->len;
                if (i < 0 || i >= (int64_t)l->len) { vm_runtime_error(v, "list index out of range"); goto setindex_fail; }
                val_release(l->v[i]);
                l->v[i] = val;
                val_release(key); val_release(box);
            } else if (IS_DICT(box)) {
                dict_set(AS_DICT(box), key, val);   /* takes both */
                val_release(box);
            } else {
                vm_runtime_error(v, "cannot assign into a %s", type_name(box));
                goto setindex_fail;
            }
            break;
        setindex_fail:
            val_release(val); val_release(key); val_release(box);
            goto fail;
        }

        case OP_FORPREP: {
            Value seq = peek(v, 0);
            if (!IS_LIST(seq) && !IS_STR(seq) && !IS_DICT(seq) && !IS_RANGE(seq)) {
                vm_runtime_error(v, "cannot iterate over a %s", type_name(seq));
                goto fail;
            }
            if (!push(v, VAL_INT(0))) goto fail;
            break;
        }
        case OP_FORNEXT: {
            int off = READ_S16();
            Value seq = peek(v, 1);
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

        case OP_CALL: {
            int nargs = READ_BYTE();
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
            if (callee.k != V_OBJ ||
                (callee.as.o->kind != O_NATIVE && callee.as.o->kind != O_FUNC && callee.as.o->kind != O_CLOSURE)) {
                vm_runtime_error(v, "cannot call a %s", type_name(callee));
                goto fail;
            }
            int idx = func_obj_index(callee);

            if (callee.as.o->kind == O_NATIVE) {
                int ncount = 0;
                const Native *tbl = native_table(&ncount);
                const Native *nat = &tbl[idx];
                if (nargs < nat->minargs || nargs > nat->maxargs) {
                    vm_runtime_error(v, "%s() takes %d..%d arguments, got %d",
                                     nat->name, nat->minargs, nat->maxargs, nargs);
                    goto fail;
                }
                Value out = VAL_NIL;
                v->kw_used = false;
                VmStatus st = nat->fn(v, &v->stack[v->sp - nargs], nargs, &out);
                if (st == VM_OK && v->kw.k != V_NIL && !v->kw_used) {
                    vm_runtime_error(v, "%s() takes no keyword arguments", nat->name);
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
                if (st == VM_ERROR) goto fail;
                for (int i = 0; i < nargs; i++) val_release(v->stack[v->sp - 1 - i]);
                v->sp -= nargs;
                val_release(pop(v));            /* the callee */
                if (!push(v, out)) goto fail;
                break;
            }

            Func *fn = &v->prog->func[idx];
            /* TUX ROCKET SCIENCE: f(1, b=5): keywords to their parameters,
             * then the defaults for the ones left */
            if (v->kw.k != V_NIL || (nargs < fn->arity && fn->ndefaults > 0)) {
                Value slot[NOM_ARGS_MAX];
                bool have[NOM_ARGS_MAX];
                for (int i = 0; i < fn->arity; i++) { have[i] = i < nargs; slot[i] = i < nargs ? v->stack[v->sp - nargs + i] : VAL_NIL; }
                if (nargs > fn->arity) {
                    vm_runtime_error(v, "%s() takes %d arguments, got %d", fn->name, fn->arity, nargs);
                    goto fail;
                }
                if (v->kw.k != V_NIL) {
                    Dict *kd = AS_DICT(v->kw);
                    for (uint32_t k = 0; k < kd->len; k++) {
                        Str *kn = AS_STR(kd->k[k]);
                        int at = -1, i = 0;
                        for (const char *q = fn->locals ? fn->locals : ""; *q && i < fn->arity; i++) {
                            const char *e = q;
                            while (*e && *e != ',') e++;
                            if ((uint32_t)(e - q) == kn->len && !memcmp(q, kn->s, kn->len)) at = i;
                            q = *e ? e + 1 : e;
                        }
                        if (at < 0 || have[at]) {
                            vm_runtime_error(v, at < 0 ? "%s() has no parameter called %s" : "%s() got %s twice", fn->name, kn->s);
                            goto fail;
                        }
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
                        vm_runtime_error(v, "%s() is missing an argument (it takes %d)", fn->name, fn->arity);
                        goto fail;
                    }
                    slot[i] = val_retain(fn->defaults[d]);
                    have[i] = true;
                }
                v->sp -= nargs;            /* (their values are in slot now) */
                for (int i = 0; i < fn->arity; i++)
                    if (!push(v, slot[i])) goto fail;
                nargs = fn->arity;
            }
            if (nargs != fn->arity) {
                vm_runtime_error(v, "%s() takes %d arguments, got %d", fn->name, fn->arity, nargs);
                goto fail;
            }
            if (v->nframes >= NOM_FRAMES_MAX) {
                vm_runtime_error(v, "RecursionError: a function called itself (or others) %d deep, and that's the limit", NOM_FRAMES_MAX);
                goto fail;
            }
            /* args are already in place; reserve the rest of the locals */
            int base = v->sp - nargs;
            for (int i = nargs; i < fn->nlocals; i++)
                if (!push(v, VAL_NIL)) goto fail;
            if (callee.as.o->kind == O_CLOSURE) {
                /* the outer function's names it uses, into their locals */
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
            f = nf;
            chunk = &fn->chunk;
            break;
        }
        case OP_RET: {
            while (v->nhandler > 0 && v->handler[v->nhandler - 1].frame >= v->nframes - 1) v->nhandler--;
            Value r = pop(v);
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
            v->kw_used = false;
            VmStatus st = host_callmethod(v, obj, AS_STR(v->prog->consts[k]), &v->stack[v->sp - nargs], nargs, &r);
            if (st == VM_OK && v->kw.k != V_NIL && !v->kw_used) {
                vm_runtime_error(v, ".%s() takes no keyword arguments", AS_STR(v->prog->consts[k])->s);
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
        case OP_UNPACK: {
            /* a, b = seq: its n items, the first on top */
            int n = READ_BYTE();
            Value seq = pop(v);
            int64_t len = IS_LIST(seq) ? (int64_t)AS_LIST(seq)->len : IS_RANGE(seq) ? range_len(AS_RANGE(seq))
                        : IS_STR(seq) ? (int64_t)AS_STR(seq)->len : -1;
            if (len != n) {
                if (len < 0) vm_runtime_error(v, "can't unpack a %s into %d names", type_name(seq), n);
                else vm_runtime_error(v, "%d names but %lld values to unpack", n, (long long)len);
                val_release(seq);
                goto fail;
            }
            for (int i = n - 1; i >= 0; i--) {
                Value key = VAL_INT(i), e;
                if (!index_get(v, seq, key, &e)) { val_release(seq); goto fail; }
                if (!push(v, e)) { val_release(seq); goto fail; }
            }
            val_release(seq);
            break;
        }
        case OP_SLICE: {
            /* x[a:b:c] -- a new list (or string) */
            Value stp = pop(v), hi = pop(v), lo = pop(v), box = pop(v);
            int64_t n = IS_LIST(box) ? (int64_t)AS_LIST(box)->len : IS_STR(box) ? (int64_t)AS_STR(box)->len : -1;
            if (n < 0) {
                vm_runtime_error(v, "can't slice a %s", type_name(box));
                val_release(box);
                goto fail;
            }
            int64_t st = stp.k == V_NIL ? 1 : val_int(stp);
            if (st == 0) { vm_runtime_error(v, "a slice step can't be 0"); val_release(box); goto fail; }
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
            }
            val_release(box);
            if (!push(v, r)) goto fail;
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
            for (int i = 0; i < fn->ndefaults; i++) val_release(fn->defaults[i]);
            for (int i = 0; i < n; i++) fn->defaults[i] = v->stack[v->sp - n + i];
            v->sp -= n;
            fn->ndefaults = n;
            break;
        }
        case OP_TRY: {
            int off = READ_S16();
            if (v->nhandler >= 16) { vm_runtime_error(v, "try blocks nested too deep"); goto fail; }
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
            Buf t; buf_init(&t);
            val_tostr(&t, x);
            buf_putc(&t, 0);
            vm_runtime_error(v, "%s", t.p);
            buf_free(&t);
            val_release(x);
            goto fail;
        }
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
            /* except ValueError / (A, B): does the error (on top) say it's
             * one of these kinds? ("ValueError: ..."); leaves it there */
            int k = READ_U16();
            Str *kinds = AS_STR(v->prog->consts[k]);
            Str *msg = IS_STR(peek(v, 0)) ? AS_STR(peek(v, 0)) : NULL;
            bool hit = false;
            for (uint32_t i = 0; msg && i < kinds->len && !hit; ) {
                uint32_t j = i;
                while (j < kinds->len && kinds->s[j] != '|') j++;
                uint32_t n = j - i;
                hit = (n == 9 && !memcmp(kinds->s + i, "Exception", 9)) ||
                      (msg->len > n && !memcmp(msg->s, kinds->s + i, n) && msg->s[n] == ':');
                i = j + 1;
            }
            if (!push(v, VAL_BOOL(hit))) goto fail;
            break;
        }
        case OP_EXCTEXT: {
            /* except ... as e: e is what went wrong, without its kind */
            Value m = pop(v);
            Str *s = IS_STR(m) ? AS_STR(m) : NULL;
            uint32_t at = 0;
            if (s) {
                uint32_t i = 0;
                while (i < s->len && ((s->s[i] >= 'A' && s->s[i] <= 'Z') || (s->s[i] >= 'a' && s->s[i] <= 'z'))) i++;
                if (i > 5 && i + 1 < s->len && s->s[i] == ':' && s->s[i + 1] == ' ' &&
                    (!memcmp(s->s + i - 5, "Error", 5) || (i == 9 && !memcmp(s->s, "Exception", 9)))) at = i + 2;
            }
            Value e = s ? str_new(s->s + at, s->len - at) : val_retain(m);
            val_release(m);
            if (!push(v, e)) goto fail;
            break;
        }
        case OP_PUTN: {
            /* a dict comprehension's next key and value, into its dict d below */
            int d = READ_BYTE();
            Value val = pop(v), key = pop(v);
            Value dd = v->stack[v->sp - 1 - d];
            if (IS_DICT(dd)) dict_set(AS_DICT(dd), key, val); else { val_release(key); val_release(val); }
            break;
        }
        case OP_DUP2: {
            Value a = peek(v, 1), b = peek(v, 0);
            if (!push(v, val_retain(a))) goto fail;
            if (!push(v, val_retain(b))) goto fail;
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
     * began, the error's words pushed for `except ... as e`, and on from the
     * except block. With none, the script stops, as before. */
    if (v->nhandler > 0) {
        struct Handler h = v->handler[--v->nhandler];
        while (v->nframes - 1 > h.frame) {
            struct Frame *fr = &v->frame[v->nframes - 1];
            while (v->sp > fr->base) val_release(pop(v));
            val_release(pop(v));                /* the callee */
            v->nframes--;
        }
        while (v->sp > h.sp) val_release(pop(v));
        val_release(v->kw);
        v->kw = VAL_NIL;
        Value msg = str_newz(v->err);
        v->err[0] = 0;
        f = &v->frame[v->nframes - 1];
        chunk = &f->fn->chunk;
        f->ip = h.ip;
        if (!push(v, msg)) return VM_ERROR;
        if (budget > 0) goto again;
        return VM_YIELD;
    }
    return VM_ERROR;
}

/* TUX ROCKET SCIENCE: see lang.h */
bool vm_call(VM *v, Value fn, Value *args, int n, Value *out)
{
    if (fn.k != V_OBJ || (fn.as.o->kind != O_FUNC && fn.as.o->kind != O_NATIVE && fn.as.o->kind != O_CLOSURE)) {
        vm_runtime_error(v, "a %s can't be called", type_name(fn));
        return false;
    }
    int idx = func_obj_index(fn);
    if (fn.as.o->kind == O_NATIVE) {
        int cnt = 0;
        const Native *nat = &native_table(&cnt)[idx];
        if (n < nat->minargs || n > nat->maxargs) {
            vm_runtime_error(v, "%s() takes %d..%d arguments, got %d", nat->name, nat->minargs, nat->maxargs, n);
            return false;
        }
        *out = VAL_NIL;
        return nat->fn(v, args, n, out) == VM_OK;
    }
    Func *f = &v->prog->func[idx];
    if (n > f->arity || n < f->arity - f->ndefaults) {
        vm_runtime_error(v, "%s() takes %d arguments, got %d", f->name, f->arity, n);
        return false;
    }
    if (v->nframes >= NOM_FRAMES_MAX) { vm_runtime_error(v, "call depth exceeded"); return false; }
    /* the call, laid out as OP_CALL would, and run until it returns (with
     * the try handlers out of reach: they belong to the code around) */
    int sp0 = v->sp;
    if (!push(v, val_retain(fn))) return false;
    int base = v->sp;
    for (int i = 0; i < n; i++) if (!push(v, val_retain(args[i]))) return false;
    for (int i = n; i < f->arity; i++) if (!push(v, val_retain(f->defaults[i - (f->arity - f->ndefaults)]))) return false;
    for (int i = f->arity; i < f->nlocals; i++) if (!push(v, VAL_NIL)) return false;
    if (fn.as.o->kind == O_CLOSURE) {
        Closure *cl = (Closure *)fn.as.o;
        for (int i = 0; i < cl->n && i < f->ncap; i++) {
            val_release(v->stack[base + f->cap_slot[i]]);
            v->stack[base + f->cap_slot[i]] = val_retain(cl->cap[i]);
        }
    }
    struct Frame *nf = &v->frame[v->nframes++];
    nf->fn = f;
    nf->ip = 0;
    nf->base = base;
    int saved_stop = v->stop_frames, saved_h = v->nhandler;
    v->stop_frames = v->nframes - 1;
    v->nhandler = 0;
    VmStatus st;
    while ((st = vm_run(v, 1000000)) == VM_YIELD) { }
    v->stop_frames = saved_stop;
    v->nhandler = saved_h;
    if (st != VM_RETURNED) {
        /* (an error: what this call left on the stack goes) */
        while (v->nframes > 1 && v->frame[v->nframes - 1].base > sp0) v->nframes--;
        while (v->sp > sp0) val_release(pop(v));
        return false;
    }
    *out = pop(v);
    return true;
}
