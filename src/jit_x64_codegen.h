/* Private x64 emitter. Included after the runtime helpers in jit_x64.c. */
typedef struct {
    uint8_t bytes[524288];
    size_t count;
    int failed;
    uint16_t promoted;
} X64Emitter;

static void xe_byte(X64Emitter* e, unsigned b) {
    if (e->count >= sizeof(e->bytes)) { e->failed = 1; return; }
    e->bytes[e->count++] = (uint8_t)b;
}
static void xe_u32(X64Emitter* e, uint32_t v) {
    for (int i = 0; i < 4; i++) xe_byte(e, v >> (i * 8));
}
static void xe_u64(X64Emitter* e, uint64_t v) {
    for (int i = 0; i < 8; i++) xe_byte(e, (unsigned)(v >> (i * 8)));
}
static void xe_rex(X64Emitter* e, int w, int r, int b) {
    unsigned rex = 0x40u | (w ? 8u : 0u) | ((r >> 3) << 2) | (b >> 3);
    if (rex != 0x40) xe_byte(e, rex);
}
static void xe_mem(X64Emitter* e, int reg, int base, int off) {
    xe_byte(e, 0x80 | ((reg & 7) << 3) | (base & 7));
    if ((base & 7) == 4) xe_byte(e, 0x24);
    xe_u32(e, (uint32_t)off);
}
static void xe_mov(X64Emitter* e, int dst, int src) {
    xe_rex(e, 1, src, dst); xe_byte(e, 0x89);
    xe_byte(e, 0xc0 | ((src & 7) << 3) | (dst & 7));
}
static void xe_imm(X64Emitter* e, int dst, uint64_t v) {
    xe_rex(e, 1, 0, dst); xe_byte(e, 0xb8 + (dst & 7)); xe_u64(e, v);
}
static void xe_load(X64Emitter* e, int dst, int base, int off) {
    xe_rex(e, 1, dst, base); xe_byte(e, 0x8b); xe_mem(e, dst, base, off);
}
static void xe_store(X64Emitter* e, int src, int base, int off) {
    xe_rex(e, 1, src, base); xe_byte(e, 0x89); xe_mem(e, src, base, off);
}
static void xe_lea(X64Emitter* e, int dst, int base, int off) {
    xe_rex(e, 1, dst, base); xe_byte(e, 0x8d); xe_mem(e, dst, base, off);
}
static void xe_sse_mem(X64Emitter* e, int store, int reg, int base, int off) {
    xe_byte(e, 0xf2); xe_rex(e, 0, reg, base);
    xe_byte(e, 0x0f); xe_byte(e, store ? 0x11 : 0x10); xe_mem(e, reg, base, off);
}
static void xe_sse(X64Emitter* e, int opcode, int dst, int src) {
    xe_byte(e, 0xf2); xe_rex(e, 0, dst, src); xe_byte(e, 0x0f);
    xe_byte(e, opcode); xe_byte(e, 0xc0 | ((dst & 7) << 3) | (src & 7));
}
static void xe_ucomi(X64Emitter* e, int a, int b) {
    xe_byte(e, 0x66); xe_rex(e, 0, a, b); xe_byte(e, 0x0f);
    xe_byte(e, 0x2e); xe_byte(e, 0xc0 | ((a & 7) << 3) | (b & 7));
}
static void xe_double(X64Emitter* e, int reg, double value) {
    uint64_t bits; memcpy(&bits, &value, sizeof(bits)); xe_imm(e, 0, bits);
    xe_byte(e, 0x66); xe_rex(e, 1, reg, 0); xe_byte(e, 0x0f);
    xe_byte(e, 0x6e); xe_byte(e, 0xc0 | ((reg & 7) << 3));
}
static size_t xe_jump(X64Emitter* e, int cond) {
    if (cond < 0) xe_byte(e, 0xe9);
    else { xe_byte(e, 0x0f); xe_byte(e, 0x80 + cond); }
    size_t patch = e->count; xe_u32(e, 0); return patch;
}
static void xe_patch(X64Emitter* e, size_t patch, size_t target) {
    int64_t delta = (int64_t)target - (int64_t)(patch + 4);
    if (patch + 4 > e->count || delta < INT32_MIN || delta > INT32_MAX) { e->failed = 1; return; }
    int32_t rel = (int32_t)delta; memcpy(e->bytes + patch, &rel, 4);
}
static void xe_call(X64Emitter* e, void* target) {
    xe_imm(e, 0, (uint64_t)(uintptr_t)target); xe_byte(e, 0xff); xe_byte(e, 0xd0);
}
static void xe_push(X64Emitter* e, int reg) {
    if (reg >= 8) xe_byte(e, 0x41);
    xe_byte(e, 0x50 + (reg & 7));
}
static void xe_pop(X64Emitter* e, int reg) {
    if (reg >= 8) xe_byte(e, 0x41);
    xe_byte(e, 0x58 + (reg & 7));
}
static void xe_stack(X64Emitter* e, int bytes, int add) {
    xe_byte(e, 0x48); xe_byte(e, 0x81); xe_byte(e, add ? 0xc4 : 0xec); xe_u32(e, (uint32_t)bytes);
}
static int xe_publish(X64Emitter* e, void* ctx, void** fn, void** memory, size_t* size) {
    if (e->failed || e->count == 0) return 0;
    long page = sysconf(_SC_PAGESIZE); if (page <= 0) page = 4096;
    size_t bytes = (e->count + (size_t)page - 1) / (size_t)page * (size_t)page;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_JIT
    flags |= MAP_JIT;
#endif
    void* code = mmap(NULL, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (code == MAP_FAILED) return 0;
    memcpy(code, e->bytes, e->count);
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)code, (char*)code + e->count);
#endif
    if (mprotect(code, bytes, PROT_READ | PROT_EXEC) != 0) { munmap(code, bytes); return 0; }
    X64CodeBundle* b = calloc(1, sizeof(*b));
    if (!b) { munmap(code, bytes); return 0; }
    b->magic = X64_CODE_BUNDLE_MAGIC; b->exec_mem = code; b->exec_size = bytes; b->ctx = ctx;
    *fn = code; *memory = b; *size = sizeof(*b); return 1;
}

/* Find operand-stack depths over the CFG before assigning fixed XMM registers.
 * Branch joins must agree; a linear scan alone silently miscompiles merges. */
static int xe_slot_depths(const ToiJitSlotInstr* ops, int count, int* depths) {
    for (int i = 0; i <= count; i++) depths[i] = -1;
    depths[0] = 0;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < count; i++) {
            if (depths[i] < 0) continue;
            int sp = depths[i], need = 0, delta = 0;
            switch (ops[i].op) {
                case TOI_JIT_SLOT_PUSH_SLOT: case TOI_JIT_SLOT_PUSH_CONST: delta = 1; break;
                case TOI_JIT_SLOT_BIN_ADD: case TOI_JIT_SLOT_BIN_SUB: case TOI_JIT_SLOT_BIN_MUL:
                case TOI_JIT_SLOT_BIN_DIV: case TOI_JIT_SLOT_BIN_MOD: case TOI_JIT_SLOT_CMP_LT:
                case TOI_JIT_SLOT_CMP_GT: case TOI_JIT_SLOT_CMP_EQ: need = 2; delta = -1; break;
                case TOI_JIT_SLOT_POP: need = 1; delta = -1; break;
                case TOI_JIT_SLOT_STORE_SLOT: case TOI_JIT_SLOT_JUMP_IF_FALSE:
                case TOI_JIT_SLOT_JUMP_IF_TRUE: need = 1; break;
                case TOI_JIT_SLOT_JUMP: break;
                default: return 0;
            }
            if (sp < need || sp + delta > 14) return 0;
            sp += delta;
            int targets[2] = {i + 1, -1};
            if (ops[i].op == TOI_JIT_SLOT_JUMP) targets[0] = ops[i].arg;
            else if (ops[i].op == TOI_JIT_SLOT_JUMP_IF_FALSE || ops[i].op == TOI_JIT_SLOT_JUMP_IF_TRUE) targets[1] = ops[i].arg;
            for (int j = 0; j < 2; j++) {
                int t = targets[j]; if (t == -1) continue;
                if (t < 0 || t > count) return 0;
                if (depths[t] == -1) { depths[t] = sp; changed = 1; }
                else if (depths[t] != sp) return 0;
            }
        }
    }
    return depths[count] == 0;
}

/* SysV AMD64: r12=slots, r15=constants. XMM2..15 hold expression values.
 * Only fmod calls out; all live XMM registers are spilled across that ABI edge.
 * Keep fmod rather than div/trunc/mul: the latter changes large/NaN/Inf results. */
static int xe_compile_slots(const ToiJitSlotInstr* ops, int count, uint8_t var, uint8_t end,
                            int mode, void** fn, void** memory, size_t* size) {
    int depths[257];
    if (!ops || count <= 0 || count > 256 || !xe_slot_depths(ops, count, depths)) return 0;
    X64Emitter e = {0};
    xe_push(&e, 12); xe_push(&e, 13); xe_push(&e, 14); xe_push(&e, 15);
    enum { FRAME = 296, SLOTS = 224, CONSTANTS = 248 };
    xe_stack(&e, FRAME, 0);
    if (mode) {
        xe_mov(&e, 13, 7); xe_mov(&e, 14, 6);
        xe_sse_mem(&e, 0, 3, 7, 0); xe_sse_mem(&e, 1, 3, 4, SLOTS);
        xe_sse_mem(&e, 0, 3, 6, 0); xe_sse_mem(&e, 1, 3, 4, SLOTS + 8);
        xe_sse_mem(&e, 0, 3, 2, 0); xe_sse_mem(&e, 1, 3, 4, SLOTS + 16);
        xe_lea(&e, 12, 4, SLOTS);
        if (mode == 2) {
            for (int i = 0; i < 3; i++) xe_sse_mem(&e, 1, i, 4, CONSTANTS + i * 8);
            xe_lea(&e, 15, 4, CONSTANTS);
        } else xe_mov(&e, 15, 1);
    } else { xe_mov(&e, 12, 7); xe_mov(&e, 15, 6); }
    size_t loop = e.count;
    xe_sse_mem(&e, 0, 0, 12, var * 8); xe_double(&e, 1, 1);
    xe_sse(&e, 0x58, 0, 1); xe_sse_mem(&e, 1, 0, 12, var * 8);
    xe_sse_mem(&e, 0, 1, 12, end * 8); xe_ucomi(&e, 0, 1);
    size_t exit_nan = xe_jump(&e, 10), exit_gt = xe_jump(&e, 7);
    size_t labels[257], patches[256]; int targets[256], np = 0;
    for (int i = 0; i < count; i++) {
        labels[i] = e.count; if (depths[i] < 0) continue;
        int sp = depths[i], a = sp, b = sp + 1; /* binary lhs/rhs registers */
        switch (ops[i].op) {
            case TOI_JIT_SLOT_PUSH_SLOT: xe_sse_mem(&e, 0, sp + 2, 12, ops[i].arg * 8); break;
            case TOI_JIT_SLOT_PUSH_CONST: xe_sse_mem(&e, 0, sp + 2, 15, ops[i].arg * 8); break;
            case TOI_JIT_SLOT_STORE_SLOT: xe_sse_mem(&e, 1, sp + 1, 12, ops[i].arg * 8); break;
            case TOI_JIT_SLOT_POP: break;
            case TOI_JIT_SLOT_BIN_ADD: xe_sse(&e, 0x58, a, b); break;
            case TOI_JIT_SLOT_BIN_SUB: xe_sse(&e, 0x5c, a, b); break;
            case TOI_JIT_SLOT_BIN_MUL: xe_sse(&e, 0x59, a, b); break;
            case TOI_JIT_SLOT_BIN_DIV: xe_sse(&e, 0x5e, a, b); break;
            case TOI_JIT_SLOT_BIN_MOD:
                for (int r = 2; r < sp; r++) xe_sse_mem(&e, 1, r, 4, (r - 2) * 8);
                xe_sse(&e, 0x10, 0, a); xe_sse(&e, 0x10, 1, b);
                xe_call(&e, (void*)&fmod); xe_sse(&e, 0x10, a, 0);
                for (int r = 2; r < sp; r++) xe_sse_mem(&e, 0, r, 4, (r - 2) * 8);
                break;
            case TOI_JIT_SLOT_CMP_LT: case TOI_JIT_SLOT_CMP_GT: case TOI_JIT_SLOT_CMP_EQ: {
                xe_ucomi(&e, a, b);
                int cond = ops[i].op == TOI_JIT_SLOT_CMP_LT ? 2 : ops[i].op == TOI_JIT_SLOT_CMP_GT ? 7 : 4;
                xe_byte(&e, 0x0f); xe_byte(&e, 0x90 + cond); xe_byte(&e, 0xc0); /* setcc al */
                xe_byte(&e, 0x0f); xe_byte(&e, 0x9b); xe_byte(&e, 0xc2); /* setnp dl */
                xe_byte(&e, 0x20); xe_byte(&e, 0xd0); /* and al,dl */
                xe_byte(&e, 0x0f); xe_byte(&e, 0xb6); xe_byte(&e, 0xc0);
                xe_byte(&e, 0xf2); xe_rex(&e, 0, a, 0); xe_byte(&e, 0x0f); xe_byte(&e, 0x2a);
                xe_byte(&e, 0xc0 | ((a & 7) << 3)); break;
            }
            case TOI_JIT_SLOT_JUMP:
                patches[np] = xe_jump(&e, -1); targets[np++] = ops[i].arg; break;
            case TOI_JIT_SLOT_JUMP_IF_FALSE: case TOI_JIT_SLOT_JUMP_IF_TRUE: {
                xe_double(&e, 0, 0); xe_ucomi(&e, sp + 1, 0);
                if (ops[i].op == TOI_JIT_SLOT_JUMP_IF_FALSE) {
                    size_t unordered = xe_jump(&e, 10);
                    patches[np] = xe_jump(&e, 4); targets[np++] = ops[i].arg;
                    xe_patch(&e, unordered, e.count);
                } else {
                    patches[np] = xe_jump(&e, 10); targets[np++] = ops[i].arg;
                    patches[np] = xe_jump(&e, 5); targets[np++] = ops[i].arg;
                }
                break;
            }
            default: return 0;
        }
        if (np > 254) return 0;
    }
    labels[count] = e.count;
    size_t again = xe_jump(&e, -1); xe_patch(&e, again, loop);
    size_t done = e.count; xe_patch(&e, exit_nan, done); xe_patch(&e, exit_gt, done);
    for (int i = 0; i < np; i++) xe_patch(&e, patches[i], labels[targets[i]]);
    if (mode) {
        xe_sse_mem(&e, 0, 0, 12, 0); xe_sse_mem(&e, 1, 0, 13, 0);
        xe_sse_mem(&e, 0, 0, 12, 8); xe_sse_mem(&e, 1, 0, 14, 0);
    }
    xe_stack(&e, FRAME, 1); xe_pop(&e, 15); xe_pop(&e, 14); xe_pop(&e, 13); xe_pop(&e, 12); xe_byte(&e, 0xc3);
    return xe_publish(&e, NULL, fn, memory, size);
}

/* Direct trace bodies use fixed, CFG-verified Value stack locations. Arithmetic,
 * loads, stores and control flow are emitted; complex language operations use
 * the same runtime primitives as the interpreter bridge. A failed operation
 * resumes at that operation, never at a previously committed side effect. */
typedef struct {
    VM* vm;
    ObjClosure* closure;
    Value* slots;
    int slot_count;
    ToiJitNativeExecState* state;
    uint32_t remaining;
    Value stack[TOI_JIT_TRACE_STACK_CAP];
} XeTraceFrame;

#include "jit_native_lower.h"

typedef int (*XeCalleeFn)(VM*, ObjClosure*, const Value*, Value*);
typedef struct XeCalleeEntry {
    struct XeCalleeEntry* next;
    ObjClosure* closure;
    ToiJitNativeTraceFn fn;
    XeCalleeFn entry_fn;
    JitProgram* program;
    void* memory;
    size_t size;
    void* entry_memory;
    size_t entry_size;
    uint8_t ret_slot, var_slot, end_slot, slot_count, argc;
} XeCalleeEntry;

typedef struct XeTraceContext XeTraceContext;
typedef struct {
    ObjTable* table;
    ObjString* key;
    uint32_t shape;
    int slot;
} XeTableLane;
typedef struct {
    XeTraceContext* owner;
    XeCalleeEntry* calls[4];
    XeTableLane tables[4];
    uint32_t call_next, table_next;
    int fused_slot;
} XeSite;

struct XeTraceContext {
    ToiJitNativeTraceInstr ops[256];
    uint8_t var, end;
    int count, max_local;
    XeCalleeEntry* callees;
    ToiJitX64Stats stats;
    XeSite* sites[256];
    pthread_mutex_t compile_lock;
    unsigned entry_count;
    uint8_t numeric_local[256], numeric_upvalue[256], skip_store[256];
};

static void xe_trace_context_free(void* ptr) {
    XeTraceContext* ctx = ptr;
    XeCalleeEntry* entry = ctx->callees;
    while (entry) {
        XeCalleeEntry* next = entry->next;
        toi_jit_x64_free_code(entry->memory, entry->size);
        toi_jit_x64_free_code(entry->entry_memory, entry->entry_size);
        free(entry); entry = next;
    }
    for (int i = 0; i < ctx->count; i++) free(ctx->sites[i]);
    pthread_mutex_destroy(&ctx->compile_lock);
    free(ctx);
}

static void xe_trace_context_mark(void* ptr) {
    XeTraceContext* ctx = ptr;
    for (int i = 0; i < ctx->count; i++) mark_value(ctx->ops[i].constant);
    XeCalleeEntry* e = __atomic_load_n(&ctx->callees, __ATOMIC_ACQUIRE);
    for (; e; e = e->next) {
        mark_object((struct Obj*)e->closure);
        toi_jit_x64_mark_code(e->memory);
    }
}

static int xe_callee_execute(XeCalleeEntry* entry, VM* vm, Value callee, Value* args, int argc, Value* out) {
    if (!entry || !IS_CLOSURE(callee) || AS_CLOSURE(callee) != entry->closure || argc != entry->argc) return 0;
    ObjFunction* f = entry->closure->function;
    for (int i = 0; i < argc && i < f->param_types_count; i++) {
        if (!jit_value_matches_type(args[i], f->param_types[i])) return 0;
    }
    return entry->entry_fn && entry->entry_fn(vm, entry->closure, args, out);
}

#include "jit_x64_runtime.h"

static int xe_trace_entry(XeTraceFrame* f, const XeTraceContext* ctx) {
    if (!f->vm || !f->closure || !f->slots || !f->state || f->slot_count <= ctx->max_local) return 0;
    f->state->stack_count = 0;
    f->state->reason = TOI_JIT_NATIVE_DEOPT_NONE;
    f->remaining = (ctx->stats.fast_call_loops || ctx->stats.fast_string_loops) ? 4096 : 128;
    return 1;
}
static int xe_trace_resume(XeTraceFrame* f, const ToiJitNativeTraceInstr* op, int sp, int reason) {
    if (!f->state) return TOI_JIT_NATIVE_EXEC_DEOPT;
    f->state->resume_offset = op ? op->deopt_offset : TOI_JIT_NATIVE_RESUME_BACKEDGE;
    f->state->op = op ? op->op : 0;
    f->state->reason = (uint8_t)reason;
    f->state->stack_count = (uint16_t)sp;
    for (int i = 0; i < sp; i++) f->state->stack[i] = f->stack[i];
    return TOI_JIT_NATIVE_EXEC_CONTINUE;
}
static int xe_trace_upvalue(XeTraceFrame* f, const ToiJitNativeTraceInstr* in, int sp) {
    if (in->a >= f->closure->upvalue_count || !f->closure->upvalues[in->a] ||
        !f->closure->upvalues[in->a]->location) return 0;
    f->stack[sp] = *f->closure->upvalues[in->a]->location;
    return 1;
}
static int xe_trace_get(XeTraceFrame* f, const ToiJitNativeTraceInstr* in, int sp) {
    Value out;
    XeSite* site = (XeSite*)(uintptr_t)in->aux_data;
    Value table = f->stack[sp - 2], key = f->stack[sp - 1];
    if (site && IS_TABLE(table) && AS_TABLE(table)->no_metatable_fast && IS_STRING(key)) {
        if (!xe_table_lookup(site, AS_TABLE(table), AS_STRING(key), &out, 0)) out = NIL_VAL;
        f->stack[sp - 2] = out; return 1;
    }
    int op = in->op == TOI_JIT_NATIVE_OP_GET_TABLE_NUM ? TOI_JIT_VAL_GET_TABLE_NUM : TOI_JIT_VAL_GET_TABLE;
    if (!jit_value_get_table_num_fast(f->stack[sp - 2], f->stack[sp - 1], (uint8_t)op, &out)) return 0;
    f->stack[sp - 2] = out; return 1;
}
static int xe_trace_set(XeTraceFrame* f, const ToiJitNativeTraceInstr* in, int sp) {
    Value out;
    XeSite* site = (XeSite*)(uintptr_t)in->aux_data;
    Value table = f->stack[sp - 3], key = f->stack[sp - 2], value = f->stack[sp - 1];
    if (site && in->op == TOI_JIT_NATIVE_OP_SET_TABLE && IS_TABLE(table) &&
        AS_TABLE(table)->no_metatable_fast && IS_STRING(key)) {
        xe_table_store(site, AS_TABLE(table), AS_STRING(key), value);
        f->stack[sp - 3] = value; return 1;
    }
    int op = in->op == TOI_JIT_NATIVE_OP_SET_TABLE_NUM ? TOI_JIT_VAL_SET_TABLE_NUM : TOI_JIT_VAL_SET_TABLE;
    if (!jit_value_set_table_fast(f->stack[sp - 3], f->stack[sp - 2], f->stack[sp - 1], (uint8_t)op, &out)) return 0;
    f->stack[sp - 3] = out; return 1;
}
static int xe_trace_append(XeTraceFrame* f, const ToiJitNativeTraceInstr* in, int sp) {
    (void)in; Value out;
    if (!jit_value_append_fast(f->stack[sp - 2], f->stack[sp - 1], &out)) return 0;
    f->stack[sp - 2] = out; return 1;
}
static int xe_trace_call(XeTraceFrame* f, const ToiJitNativeTraceInstr* in, int sp) {
    int base = sp - 1 - in->a; Value out;
    XeSite* site = (XeSite*)(uintptr_t)in->aux_data;
    int ok;
    if (in->op == TOI_JIT_NATIVE_OP_CALL) {
        ok = site ? xe_site_call(site, f->vm, f->stack[base], &f->stack[base + 1], in->a, &out)
                  : jit_call_primitive(f->vm, f->stack[base], &f->stack[base + 1], in->a, &out);
    } else {
        ok = site ? xe_site_invoke(site, f->vm, f->stack[base], in->constant, &f->stack[base + 1], in->a, &out)
                  : jit_invoke_primitive(f->vm, f->stack[base], in->constant, &f->stack[base + 1], in->a, &out);
    }
    if (!ok) return 0;
    f->stack[base] = out; return 1;
}
/* This builtin cannot allocate or invoke user code. Read the live module
 * value on every draw so replacing math.random invalidates the fast path even
 * when the table shape stays unchanged. No VM argument/result staging. */
static int xe_is_random_invoke(const ToiJitNativeTraceInstr* in) {
    return in->op == TOI_JIT_NATIVE_OP_INVOKE && in->a == 0 &&
        IS_STRING(in->constant) && AS_STRING(in->constant)->length == 6 &&
        memcmp(AS_STRING(in->constant)->chars, "random", 6) == 0;
}
static int xe_trace_random(XeTraceFrame* f, const ToiJitNativeTraceInstr* in, int sp) {
    Value receiver = f->stack[sp - 1];
    if (!IS_TABLE(receiver)) return 0;
    ObjTable* table = AS_TABLE(receiver);
    if (!table->is_module || !table->no_metatable_fast) return 0;
    XeSite* site = (XeSite*)(uintptr_t)in->aux_data;
    ObjString* key = AS_STRING(in->constant);
    Value target;
    int slot = site ? xe_table_slot(site, table, key) : -1;
    if (slot >= 0) target = table->table.entries[slot].value;
    else {
        if (!table_get_with_slot(&table->table, key, &target, &slot)) return 0;
        if (site) xe_table_remember(site, table, key, slot);
    }
    if (!IS_NATIVE(target) || AS_NATIVE_OBJ(target)->fast_kind != NATIVE_FAST_MATH_RANDOM) return 0;
    f->stack[sp - 1] = NUMBER_VAL((double)rand() / (double)RAND_MAX);
    return 1;
}

static int xe_trace_length(XeTraceFrame* f, const ToiJitNativeTraceInstr* in, int sp) {
    (void)in;
    return jit_length_primitive(f->stack[sp - 1], &f->stack[sp - 1]);
}
static int xe_trace_slice(XeTraceFrame* f, const ToiJitNativeTraceInstr* in, int sp) {
    (void)in;
    Value out;
    if (!jit_slice_primitive(f->stack[sp - 4], f->stack[sp - 3], f->stack[sp - 2], f->stack[sp - 1], &out)) return 0;
    f->stack[sp - 4] = out;
    return 1;
}
static int xe_trace_string(XeTraceFrame* f, const ToiJitNativeTraceInstr* in, int sp) {
    int n = in->op - TOI_JIT_NATIVE_OP_BUILD_STRING2 + 2;
    XeSite* site = (XeSite*)(uintptr_t)in->aux_data;
    Value* out = site && site->fused_slot >= 0 ? &f->slots[site->fused_slot] : &f->stack[sp - n];
    return jit_build_string_primitive(f->vm, &f->stack[sp - n], n, out);
}
static int xe_trace_falsey(const Value* v) { return jit_value_falsey(*v); }
static void xe_trace_equal(Value* a, const Value* b) {
    int equal = 0;
    if (a->type == b->type) {
        if (IS_NIL(*a)) equal = 1;
        else if (IS_NUMBER(*a)) equal = AS_NUMBER(*a) == AS_NUMBER(*b);
        else if (IS_BOOL(*a)) equal = AS_BOOL(*a) == AS_BOOL(*b);
        else if (IS_STRING(*a) && IS_STRING(*b)) {
            ObjString* x = AS_STRING(*a); ObjString* y = AS_STRING(*b);
            equal = x == y || (x->length == y->length && memcmp(x->chars, y->chars, (size_t)x->length) == 0);
        } else equal = AS_OBJ(*a) == AS_OBJ(*b);
    }
    *a = BOOL_VAL(equal);
}

static int xe_trace_depths(const ToiJitNativeTraceInstr* ops, int count, int* depths) {
    for (int i = 0; i <= count; i++) depths[i] = -1;
    depths[0] = 0; int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < count; i++) {
            if (depths[i] < 0) continue;
            int sp = depths[i], need = 0, delta = 0;
            switch (ops[i].op) {
                case TOI_JIT_NATIVE_OP_PUSH_LOCAL: case TOI_JIT_NATIVE_OP_PUSH_UPVALUE:
                case TOI_JIT_NATIVE_OP_PUSH_CONST: case TOI_JIT_NATIVE_OP_PUSH_NIL:
                case TOI_JIT_NATIVE_OP_PUSH_TRUE: case TOI_JIT_NATIVE_OP_PUSH_FALSE: delta = 1; break;
                case TOI_JIT_NATIVE_OP_STORE_LOCAL: case TOI_JIT_NATIVE_OP_NOT: case TOI_JIT_NATIVE_OP_LENGTH:
                case TOI_JIT_NATIVE_OP_JUMP_IF_FALSE: case TOI_JIT_NATIVE_OP_JUMP_IF_TRUE: need = 1; break;
                case TOI_JIT_NATIVE_OP_POP: need = 1; delta = -1; break;
                case TOI_JIT_NATIVE_OP_BIN_ADD: case TOI_JIT_NATIVE_OP_BIN_SUB:
                case TOI_JIT_NATIVE_OP_BIN_MUL: case TOI_JIT_NATIVE_OP_BIN_DIV:
                case TOI_JIT_NATIVE_OP_BIN_MOD: case TOI_JIT_NATIVE_OP_CMP_EQ:
                case TOI_JIT_NATIVE_OP_CMP_LT: case TOI_JIT_NATIVE_OP_CMP_GT:
                case TOI_JIT_NATIVE_OP_APPEND: case TOI_JIT_NATIVE_OP_GET_TABLE:
                case TOI_JIT_NATIVE_OP_GET_TABLE_NUM: need = 2; delta = -1; break;
                case TOI_JIT_NATIVE_OP_SLICE: need = 4; delta = -3; break;
                case TOI_JIT_NATIVE_OP_SET_TABLE: case TOI_JIT_NATIVE_OP_SET_TABLE_NUM: need = 3; delta = -2; break;
                case TOI_JIT_NATIVE_OP_CALL: case TOI_JIT_NATIVE_OP_INVOKE: need = ops[i].a + 1; delta = -ops[i].a; break;
                case TOI_JIT_NATIVE_OP_BUILD_STRING2: case TOI_JIT_NATIVE_OP_BUILD_STRING3:
                case TOI_JIT_NATIVE_OP_BUILD_STRING4: case TOI_JIT_NATIVE_OP_BUILD_STRING5:
                    need = ops[i].op - TOI_JIT_NATIVE_OP_BUILD_STRING2 + 2; delta = 1 - need; break;
                case TOI_JIT_NATIVE_OP_ADJUST_STACK:
                    delta = ops[i].a - sp; break;
                case TOI_JIT_NATIVE_OP_NOP: case TOI_JIT_NATIVE_OP_JUMP: break;
                default: return 0;
            }
            if (sp < need || sp + delta >= TOI_JIT_TRACE_STACK_CAP) return 0;
            sp += delta; int targets[2] = {i + 1, -1};
            if (ops[i].op == TOI_JIT_NATIVE_OP_JUMP) targets[0] = ops[i].target;
            else if (ops[i].op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE || ops[i].op == TOI_JIT_NATIVE_OP_JUMP_IF_TRUE) targets[1] = ops[i].target;
            for (int j = 0; j < 2; j++) {
                int t = targets[j]; if (t == -1) continue;
                if (t < 0 || t > count) return 0;
                /* An explicit target zero is a numeric-for backedge, not a
                 * merge into the first instruction of the current iteration. */
                if (t == count || (t == 0 && (ops[i].op == TOI_JIT_NATIVE_OP_JUMP || j == 1))) {
                    continue;
                }
                if (depths[t] == -1) { depths[t] = sp; changed = 1; }
                else if (depths[t] != sp) return 0;
            }
        }
    }
    return 1;
}
static int xe_trace_stack(int index) { return (int)offsetof(XeTraceFrame, stack) + index * (int)sizeof(Value); }
static void xe_value_copy(X64Emitter* e, int dstbase, int dst, int srcbase, int src) {
    xe_load(e, 0, srcbase, src); xe_load(e, 2, srcbase, src + 8);
    xe_store(e, 0, dstbase, dst); xe_store(e, 2, dstbase, dst + 8);
}
static void xe_value_const(X64Emitter* e, int off, Value value) {
    uint64_t payload; memcpy(&payload, &value.as, 8);
    xe_imm(e, 0, value.type); xe_store(e, 0, 12, off);
    xe_imm(e, 0, payload); xe_store(e, 0, 12, off + 8);
}
static void xe_cmp_mem32(X64Emitter* e, int base, int off, int value) {
    xe_rex(e, 0, 7, base); xe_byte(e, 0x81); xe_mem(e, 7, base, off); xe_u32(e, (uint32_t)value);
}
static void xe_test_eax(X64Emitter* e) { xe_byte(e, 0x85); xe_byte(e, 0xc0); }

static void xe_trace_spill(X64Emitter* e, uint16_t mask) {
    for (int i = 0; i < 14; i++) {
        if (!(mask & (1u << i))) continue;
        xe_imm(e, 0, VAL_NUMBER); xe_store(e, 0, 12, xe_trace_stack(i));
        xe_sse_mem(e, 1, i + 2, 12, xe_trace_stack(i) + 8);
    }
}
static void xe_trace_flush(X64Emitter* e) {
    xe_trace_spill(e, e->promoted); e->promoted = 0;
}
typedef struct { size_t patch; int pc, sp, reason; uint16_t promoted; } XeTraceBail;
static void xe_trace_guard(X64Emitter* e, XeTraceBail* bails, int* n, int cond, int pc, int sp, int reason) {
    if (*n >= 1024) { e->failed = 1; return; }
    bails[*n] = (XeTraceBail){xe_jump(e, cond), pc, sp, reason, e->promoted}; (*n)++;
}
static void xe_trace_helper(X64Emitter* e, const ToiJitNativeTraceInstr* op, int sp, void* helper) {
    xe_trace_flush(e);
    xe_mov(e, 7, 12); xe_imm(e, 6, (uintptr_t)op); xe_imm(e, 2, (uint64_t)sp);
    xe_call(e, helper); xe_test_eax(e);
}

/* String lengths are direct loads; tables retain the general counting path. */
static void xe_trace_emit_length(X64Emitter* e, const ToiJitNativeTraceInstr* in,
                                  int pc, int sp, XeTraceBail* bails, int* nb) {
    xe_trace_flush(e);
    int off = xe_trace_stack(sp - 1);
    xe_cmp_mem32(e, 12, off, VAL_OBJ); size_t not_obj = xe_jump(e, 5);
    xe_load(e, 0, 12, off + 8);
    xe_cmp_mem32(e, 0, offsetof(struct Obj, type), OBJ_STRING); size_t not_string = xe_jump(e, 5);
    xe_byte(e, 0x8b); xe_mem(e, 0, 0, offsetof(ObjString, length));
    xe_byte(e, 0xf2); xe_byte(e, 0x0f); xe_byte(e, 0x2a); xe_byte(e, 0xc0);
    xe_sse_mem(e, 1, 0, 12, off + 8); xe_imm(e, 0, VAL_NUMBER); xe_store(e, 0, 12, off);
    size_t done = xe_jump(e, -1);
    xe_patch(e, not_obj, e->count); xe_patch(e, not_string, e->count);
    xe_trace_helper(e, in, sp, (void*)&xe_trace_length);
    xe_trace_guard(e, bails, nb, 4, pc, sp, TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
    xe_patch(e, done, e->count);
}

/* String-key IC hits execute entirely in generated code. Read the current
 * entry value and validate its key so replacement, rehash and pointer reuse
 * cannot turn a cached slot into a stale lookup. */
static void xe_trace_string_load(X64Emitter* e, const ToiJitNativeTraceInstr* in,
                                 int sp, size_t* done, int* nd) {
    XeSite* site = (XeSite*)(uintptr_t)in->aux_data;
    if (!site) return;
    int a = xe_trace_stack(sp - 2), b = xe_trace_stack(sp - 1);
    size_t invalid[5]; int ni = 0;
    xe_cmp_mem32(e, 12, a, VAL_OBJ); invalid[ni++] = xe_jump(e, 5);
    xe_cmp_mem32(e, 12, b, VAL_OBJ); invalid[ni++] = xe_jump(e, 5);
    xe_load(e, 0, 12, a + 8); xe_load(e, 2, 12, b + 8);
    xe_cmp_mem32(e, 0, offsetof(struct Obj, type), OBJ_TABLE); invalid[ni++] = xe_jump(e, 5);
    xe_cmp_mem32(e, 2, offsetof(struct Obj, type), OBJ_STRING); invalid[ni++] = xe_jump(e, 5);
    xe_byte(e, 0x80); xe_mem(e, 7, 0, offsetof(ObjTable, no_metatable_fast)); xe_byte(e, 0);
    invalid[ni++] = xe_jump(e, 4);
    for (int lane = 0; lane < 4; lane++) {
        size_t miss[6]; int nm = 0;
        xe_imm(e, 8, (uintptr_t)&site->tables[lane]);
        xe_load(e, 9, 8, offsetof(XeTableLane, table));
        xe_byte(e, 0x4c); xe_byte(e, 0x39); xe_byte(e, 0xc8); // cmp rax,r9
        miss[nm++] = xe_jump(e, 5);
        xe_load(e, 9, 8, offsetof(XeTableLane, key));
        xe_byte(e, 0x4c); xe_byte(e, 0x39); xe_byte(e, 0xca); // cmp rdx,r9
        miss[nm++] = xe_jump(e, 5);
        xe_rex(e, 0, 9, 8); xe_byte(e, 0x8b); xe_mem(e, 9, 8, offsetof(XeTableLane, shape));
        xe_rex(e, 0, 9, 0); xe_byte(e, 0x39); xe_mem(e, 9, 0, offsetof(ObjTable, table) + offsetof(Table, shape_version));
        miss[nm++] = xe_jump(e, 5);
        xe_rex(e, 0, 9, 8); xe_byte(e, 0x8b); xe_mem(e, 9, 8, offsetof(XeTableLane, slot));
        xe_rex(e, 0, 9, 0); xe_byte(e, 0x39); xe_mem(e, 9, 0, offsetof(ObjTable, table) + offsetof(Table, capacity));
        miss[nm++] = xe_jump(e, 6); // capacity <= unsigned slot (also rejects -1)
        xe_load(e, 1, 0, offsetof(ObjTable, table) + offsetof(Table, entries));
        xe_rex(e, 1, 9, 9); xe_byte(e, 0x6b); xe_byte(e, 0xc9); xe_byte(e, sizeof(Entry)); // imul r9,r9,24
        xe_rex(e, 1, 9, 1); xe_byte(e, 0x01); xe_byte(e, 0xc9); // add rcx,r9
        xe_rex(e, 1, 2, 1); xe_byte(e, 0x39); xe_mem(e, 2, 1, offsetof(Entry, key));
        miss[nm++] = xe_jump(e, 5);
        xe_value_copy(e, 12, a, 1, offsetof(Entry, value));
        xe_imm(e, 8, (uintptr_t)&site->owner->stats.native_table_ic_hits);
        xe_byte(e, 0xf0); xe_rex(e, 1, 0, 8); xe_byte(e, 0xff); xe_mem(e, 0, 8, 0);
        done[(*nd)++] = xe_jump(e, -1);
        for (int i = 0; i < nm; i++) xe_patch(e, miss[i], e->count);
    }
    for (int i = 0; i < ni; i++) xe_patch(e, invalid[i], e->count);
}

/* Inline the common dense-array case; negative, fractional, sparse and
 * metatable keys retain the complete runtime lookup semantics. */
static void xe_trace_array_load(X64Emitter* e, const ToiJitNativeTraceInstr* in,
                                int pc, int sp, XeTraceBail* bails, int* nb) {
    xe_trace_flush(e);
    size_t string_done[4]; int nsd = 0;
    xe_trace_string_load(e, in, sp, string_done, &nsd);
    int a = xe_trace_stack(sp - 2), b = xe_trace_stack(sp - 1);
    size_t slow[10]; int ns = 0;
    xe_cmp_mem32(e, 12, a, VAL_OBJ); slow[ns++] = xe_jump(e, 5);
    xe_cmp_mem32(e, 12, b, VAL_NUMBER); slow[ns++] = xe_jump(e, 5);
    xe_load(e, 0, 12, a + 8);
    xe_cmp_mem32(e, 0, offsetof(struct Obj, type), OBJ_TABLE); slow[ns++] = xe_jump(e, 5);
    xe_byte(e, 0x80); xe_mem(e, 7, 0, offsetof(ObjTable, no_metatable_fast)); xe_byte(e, 0);
    slow[ns++] = xe_jump(e, 4);
    xe_sse_mem(e, 0, 0, 12, b + 8);
    xe_byte(e, 0xf2); xe_byte(e, 0x48); xe_byte(e, 0x0f); xe_byte(e, 0x2c); xe_byte(e, 0xd0); // cvttsd2si rdx,xmm0
    xe_byte(e, 0xf2); xe_byte(e, 0x48); xe_byte(e, 0x0f); xe_byte(e, 0x2a); xe_byte(e, 0xca); // cvtsi2sd xmm1,rdx
    xe_ucomi(e, 0, 1); slow[ns++] = xe_jump(e, 10); slow[ns++] = xe_jump(e, 5);
    xe_byte(e, 0x48); xe_byte(e, 0x83); xe_byte(e, 0xfa); xe_byte(e, 1); // cmp rdx,1
    slow[ns++] = xe_jump(e, 2);
    xe_byte(e, 0x8b); xe_mem(e, 1, 0, offsetof(ObjTable, table) + offsetof(Table, array_capacity));
    xe_byte(e, 0x48); xe_byte(e, 0x39); xe_byte(e, 0xca); // cmp rdx,rcx
    slow[ns++] = xe_jump(e, 7);
    xe_load(e, 1, 0, offsetof(ObjTable, table) + offsetof(Table, array));
    xe_byte(e, 0x48); xe_byte(e, 0xff); xe_byte(e, 0xca); // dec rdx
    xe_byte(e, 0x48); xe_byte(e, 0xc1); xe_byte(e, 0xe2); xe_byte(e, 4); // shl rdx,4
    xe_byte(e, 0x48); xe_byte(e, 0x01); xe_byte(e, 0xd1); // add rcx,rdx
    xe_cmp_mem32(e, 1, 0, VAL_NIL); slow[ns++] = xe_jump(e, 4);
    xe_value_copy(e, 12, a, 1, 0);
    size_t done = xe_jump(e, -1);
    for (int i = 0; i < ns; i++) xe_patch(e, slow[i], e->count);
    xe_trace_helper(e, in, sp, (void*)&xe_trace_get);
    xe_trace_guard(e, bails, nb, 4, pc, sp, TOI_JIT_NATIVE_DEOPT_TABLE_GUARD);
    xe_patch(e, done, e->count);
    for (int i = 0; i < nsd; i++) xe_patch(e, string_done[i], e->count);
}

/* Declared argument types remain guards even for native callees. */
static void xe_trace_call_types(X64Emitter* e, ObjFunction* f, int argc, int base,
                                int pc, int sp, XeTraceBail* bails, int* nb) {
    // Hints are part of call semantics, even when the body is inlined. A
    // failed hint returns to the CALL bytecode so normal error handling runs.
    for (int i = 0; f->param_types && i < f->param_types_count && i < argc; i++) {
        int off = base + (i + 1) * 16;
        int hint = f->param_types[i];
        if (hint == TYPEHINT_ANY) continue;
        int tag = (hint == TYPEHINT_INT || hint == TYPEHINT_FLOAT) ? VAL_NUMBER :
                  hint == TYPEHINT_BOOL ? VAL_BOOL : VAL_OBJ;
        xe_cmp_mem32(e, 12, off, tag);
        xe_trace_guard(e, bails, nb, 5, pc, sp, TOI_JIT_NATIVE_DEOPT_CALL_TARGET);
        if (hint == TYPEHINT_INT) {
            xe_sse_mem(e, 0, 0, 12, off + 8);
            xe_byte(e, 0xf2); xe_byte(e, 0x48); xe_byte(e, 0x0f); xe_byte(e, 0x2c); xe_byte(e, 0xc0);
            xe_byte(e, 0xf2); xe_byte(e, 0x48); xe_byte(e, 0x0f); xe_byte(e, 0x2a); xe_byte(e, 0xc8);
            xe_ucomi(e, 0, 1);
            xe_trace_guard(e, bails, nb, 10, pc, sp, TOI_JIT_NATIVE_DEOPT_CALL_TARGET);
            xe_trace_guard(e, bails, nb, 5, pc, sp, TOI_JIT_NATIVE_DEOPT_CALL_TARGET);
        } else if (hint == TYPEHINT_STR || hint == TYPEHINT_TABLE) {
            xe_load(e, 0, 12, off + 8);
            xe_cmp_mem32(e, 0, offsetof(struct Obj, type), hint == TYPEHINT_STR ? OBJ_STRING : OBJ_TABLE);
            xe_trace_guard(e, bails, nb, 5, pc, sp, TOI_JIT_NATIVE_DEOPT_CALL_TARGET);
        }
    }
}
static int xe_call_types_supported(ObjFunction* f, int argc) {
    for (int i = 0; f->param_types && i < f->param_types_count && i < argc; i++)
        if (f->param_types[i] > TYPEHINT_TABLE) return 0;
    return 1;
}

/* Eagerly linked pure callees bypass C cache lookup. Identity/type failures
 * materialize the original call operands without executing the callee. */
static int xe_trace_linked_call(X64Emitter* e, const ToiJitNativeTraceInstr* in,
                                int pc, int sp, XeTraceBail* bails, int* nb) {
    if (in->op != TOI_JIT_NATIVE_OP_CALL || !IS_CLOSURE(in->constant)) return 0;
    XeSite* site = (XeSite*)(uintptr_t)in->aux_data;
    XeCalleeEntry* entry = site ? site->calls[0] : NULL;
    if (!entry || !entry->fn || !entry->entry_fn || entry->closure != AS_CLOSURE(in->constant) ||
        entry->argc != in->a || !xe_call_types_supported(entry->closure->function, in->a)) return 0;
    xe_trace_flush(e);
    int base = xe_trace_stack(sp - 1 - in->a);
    xe_cmp_mem32(e, 12, base, VAL_OBJ);
    xe_trace_guard(e, bails, nb, 5, pc, sp, TOI_JIT_NATIVE_DEOPT_CALL_TARGET);
    xe_load(e, 0, 12, base + 8); xe_imm(e, 2, (uintptr_t)entry->closure);
    xe_byte(e, 0x48); xe_byte(e, 0x39); xe_byte(e, 0xd0);
    xe_trace_guard(e, bails, nb, 5, pc, sp, TOI_JIT_NATIVE_DEOPT_CALL_TARGET);
    xe_trace_call_types(e, entry->closure->function, in->a, base, pc, sp, bails, nb);
    xe_load(e, 7, 12, offsetof(XeTraceFrame, vm));
    xe_imm(e, 6, (uintptr_t)entry->closure);
    xe_lea(e, 2, 12, base + 16); xe_lea(e, 1, 12, base);
    xe_call(e, (void*)entry->entry_fn); xe_test_eax(e);
    xe_trace_guard(e, bails, nb, 4, pc, sp, TOI_JIT_NATIVE_DEOPT_CALL_TARGET);
    xe_imm(e, 8, (uintptr_t)&site->owner->stats.native_chain_hits);
    xe_byte(e, 0xf0); xe_rex(e, 1, 0, 8); xe_byte(e, 0xff); xe_mem(e, 0, 8, 0);
    return 1;
}

/* Identity-guarded tiny callees are inlined into the loop. Keep a helper edge
 * for changing callees and argument types rather than assuming monomorphism. */
static int xe_trace_tiny_call(X64Emitter* e, const ToiJitNativeTraceInstr* in,
                             int pc, int sp, XeTraceBail* bails, int* nb) {
    if (in->op != TOI_JIT_NATIVE_OP_CALL || in->a != 2 || !IS_CLOSURE(in->constant)) return 0;
    ObjFunction* f = AS_CLOSURE(in->constant)->function;
    if (!f || f->arity != 2 || f->is_variadic || f->is_generator) return 0;
    int kind = f->tiny_inline_kind;
    int arith = kind >= TINY_INLINE_IADD2 && kind <= TINY_INLINE_DIV2;
    int ret = kind == TINY_INLINE_RET0_2 || kind == TINY_INLINE_RET1_2;
    int len = kind == TINY_INLINE_LEN_ADD2 || kind == TINY_INLINE_LEN_ARG0_2 || kind == TINY_INLINE_LEN_ARG1_2;
    if (!arith && !ret && !len) return 0;
    if (!xe_call_types_supported(f, 2)) return 0;
    xe_trace_flush(e);
    int base = xe_trace_stack(sp - 3), a = base + 16, b = base + 32;
    size_t slow[8]; int ns = 0;
    xe_cmp_mem32(e, 12, base, VAL_OBJ); slow[ns++] = xe_jump(e, 5);
    xe_load(e, 0, 12, base + 8); xe_imm(e, 2, (uintptr_t)AS_OBJ(in->constant));
    xe_byte(e, 0x48); xe_byte(e, 0x39); xe_byte(e, 0xd0); slow[ns++] = xe_jump(e, 5);
    xe_trace_call_types(e, f, 2, base, pc, sp, bails, nb);
    if (ret) {
        xe_value_copy(e, 12, base, 12, kind == TINY_INLINE_RET0_2 ? a : b);
    } else {
        if (arith || kind == TINY_INLINE_LEN_ADD2) {
            xe_cmp_mem32(e, 12, a, VAL_NUMBER); slow[ns++] = xe_jump(e, 5);
            xe_sse_mem(e, 0, 0, 12, a + 8);
        }
        if (arith) {
            xe_cmp_mem32(e, 12, b, VAL_NUMBER); slow[ns++] = xe_jump(e, 5);
            xe_sse_mem(e, 0, 1, 12, b + 8);
            int opcode = (kind == TINY_INLINE_IADD2 || kind == TINY_INLINE_FADD2 || kind == TINY_INLINE_ADD2) ? 0x58 :
                         (kind == TINY_INLINE_ISUB2 || kind == TINY_INLINE_FSUB2 || kind == TINY_INLINE_SUB2) ? 0x5c :
                         (kind == TINY_INLINE_IMUL2 || kind == TINY_INLINE_FMUL2 || kind == TINY_INLINE_MUL2) ? 0x59 : 0x5e;
            xe_sse(e, opcode, 0, 1);
        } else {
            int source = kind == TINY_INLINE_LEN_ARG0_2 ? a : b;
            xe_cmp_mem32(e, 12, source, VAL_OBJ); slow[ns++] = xe_jump(e, 5);
            xe_load(e, 0, 12, source + 8);
            xe_cmp_mem32(e, 0, offsetof(struct Obj, type), OBJ_STRING); slow[ns++] = xe_jump(e, 5);
            xe_byte(e, 0x8b); xe_mem(e, 2, 0, offsetof(ObjString, length));
            xe_byte(e, 0xf2); xe_byte(e, 0x0f); xe_byte(e, 0x2a); xe_byte(e, 0xca); // cvtsi2sd xmm1,edx
            if (kind == TINY_INLINE_LEN_ADD2) xe_sse(e, 0x58, 0, 1);
            else xe_sse(e, 0x10, 0, 1);
        }
        xe_sse_mem(e, 1, 0, 12, base + 8); xe_imm(e, 0, VAL_NUMBER); xe_store(e, 0, 12, base);
    }
    size_t done = xe_jump(e, -1);
    for (int i = 0; i < ns; i++) xe_patch(e, slow[i], e->count);
    xe_trace_helper(e, in, sp, (void*)&xe_trace_call);
    xe_trace_guard(e, bails, nb, 4, pc, sp, TOI_JIT_NATIVE_DEOPT_CALL_TARGET);
    xe_patch(e, done, e->count); return 1;
}

static int xe_compile_trace_guarded(const ToiJitNativeTraceInstr*, int, uint8_t, uint8_t,
                                    const ToiJitNativeGuard*, int, void**, void**, size_t*);
static int xe_compile_trace(const ToiJitNativeTraceInstr* ops, int count, uint8_t var, uint8_t end,
                            void** fn, void** memory, size_t* size) {
    return xe_compile_trace_guarded(ops, count, var, end, NULL, 0, fn, memory, size);
}

#include "jit_x64_callees.h"

static int xe_compile_trace_guarded(const ToiJitNativeTraceInstr* ops, int count, uint8_t var, uint8_t end,
                                    const ToiJitNativeGuard* guards, int guard_count,
                                    void** fn, void** memory, size_t* size) {
    if (!ops || count <= 0 || count > 256 || sizeof(Value) != 16) return 0;
    int depths[257]; if (!xe_trace_depths(ops, count, depths)) {
        if (getenv("TOI_JIT_DUMP_LOOPS")) {
            fprintf(stderr, "[x64-direct-reject] inconsistent stack\n");
            for (int i = 0; i < count; i++) fprintf(stderr, "  %d op=%u a=%u target=%u depth=%d off=%u\n", i, ops[i].op, ops[i].a, ops[i].target, depths[i], ops[i].deopt_offset);
        }
        return 0;
    }
    XeTraceContext* ctx = calloc(1, sizeof(*ctx)); if (!ctx) return 0;
    if (pthread_mutex_init(&ctx->compile_lock, NULL) != 0) { free(ctx); return 0; }
    memcpy(ctx->ops, ops, (size_t)count * sizeof(*ops));
    ctx->count = count; ctx->var = var; ctx->end = end; ctx->max_local = var > end ? var : end;
    ctx->numeric_local[var] = ctx->numeric_local[end] = 1;
    for (int i = 0; guards && i < guard_count; i++) {
        if (guards[i].kind == TOI_JIT_NATIVE_GUARD_LOCAL_IS_NUMBER) ctx->numeric_local[guards[i].slot_a] = 1;
        if (guards[i].kind == TOI_JIT_NATIVE_GUARD_UPVALUE_IS_NUMBER) ctx->numeric_upvalue[guards[i].slot_a] = 1;
    }
    for (int i = 0; i < count; i++) {
        if ((ops[i].op == TOI_JIT_NATIVE_OP_PUSH_LOCAL || ops[i].op == TOI_JIT_NATIVE_OP_STORE_LOCAL) && ops[i].a > ctx->max_local) ctx->max_local = ops[i].a;
        /* Without a bytecode resume location, partial deoptimization cannot be
         * made precise. Keep those synthetic traces on the existing bridge. */
        if (ops[i].deopt_offset >= TOI_JIT_NATIVE_RESUME_BACKEDGE) { xe_trace_context_free(ctx); return 0; }
    }
    xe_trace_link_callees(ctx);
    X64Emitter e = {0}; XeTraceBail bails[1024]; int nb = 0;
    xe_push(&e, 12); xe_push(&e, 13); xe_push(&e, 14); xe_push(&e, 15); xe_push(&e, 3);
    int frame = ((int)sizeof(XeTraceFrame) + 15) & ~15;
    xe_stack(&e, frame, 0); xe_mov(&e, 12, 4); xe_mov(&e, 13, 2);
    xe_store(&e, 7, 12, offsetof(XeTraceFrame, vm));
    xe_store(&e, 6, 12, offsetof(XeTraceFrame, closure));
    xe_store(&e, 2, 12, offsetof(XeTraceFrame, slots));
    /* ecx is the slot_count argument; zero its high half before saving. */
    xe_byte(&e, 0x89); xe_byte(&e, 0xc9);
    xe_store(&e, 1, 12, offsetof(XeTraceFrame, slot_count));
    xe_store(&e, 8, 12, offsetof(XeTraceFrame, state));
    xe_mov(&e, 7, 12); xe_imm(&e, 6, (uintptr_t)ctx); xe_call(&e, (void*)&xe_trace_entry);
    xe_test_eax(&e); size_t bad_entry = xe_jump(&e, 4);
    size_t loop = e.count;
    xe_cmp_mem32(&e, 13, var * 16, VAL_NUMBER);
    xe_trace_guard(&e, bails, &nb, 5, -1, 0, TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
    xe_cmp_mem32(&e, 13, end * 16, VAL_NUMBER);
    xe_trace_guard(&e, bails, &nb, 5, -1, 0, TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
    xe_sse_mem(&e, 0, 0, 13, var * 16 + 8); xe_double(&e, 1, 1);
    xe_sse(&e, 0x58, 0, 1); xe_sse_mem(&e, 1, 0, 13, var * 16 + 8);
    xe_sse_mem(&e, 0, 1, 13, end * 16 + 8); xe_ucomi(&e, 0, 1);
    size_t exited = xe_jump(&e, 7);
    size_t labels[257], branches[512]; int targets[512], np = 0;
    uint8_t joins[257] = {0};
    for (int i = 0; i < count; i++) {
        if (ops[i].op == TOI_JIT_NATIVE_OP_JUMP || ops[i].op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE || ops[i].op == TOI_JIT_NATIVE_OP_JUMP_IF_TRUE)
            joins[ops[i].target] = 1;
    }
    for (int pc = 0; pc < count; pc++) {
        // All incoming edges use the canonical stack at a control-flow join.
        if (joins[pc]) xe_trace_flush(&e);
        labels[pc] = e.count; if (depths[pc] < 0) continue;
        const ToiJitNativeTraceInstr* in = &ctx->ops[pc];
        int sp = depths[pc], a = xe_trace_stack(sp - 2), b = xe_trace_stack(sp - 1);
        void* helper = NULL; int reason = TOI_JIT_NATIVE_DEOPT_RUNTIME;
        switch (in->op) {
            case TOI_JIT_NATIVE_OP_NOP: break;
            case TOI_JIT_NATIVE_OP_POP:
                if (sp <= 14) e.promoted &= (uint16_t)~(1u << (sp - 1));
                break;
            case TOI_JIT_NATIVE_OP_ADJUST_STACK:
                // The IR explicitly discards its stack at the numeric backedge.
                // Materialize nil only when a later instruction can read it.
                if (pc + 1 < count) for (int j = sp; j < in->a; j++) xe_value_const(&e, xe_trace_stack(j), NIL_VAL);
                if (in->a < 14) e.promoted &= (uint16_t)((1u << in->a) - 1u);
                break;
            case TOI_JIT_NATIVE_OP_PUSH_LOCAL:
                if (sp < 14 && ctx->numeric_local[in->a]) {
                    xe_cmp_mem32(&e, 13, in->a * 16, VAL_NUMBER);
                    xe_trace_guard(&e, bails, &nb, 5, pc, sp, TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
                    xe_sse_mem(&e, 0, sp + 2, 13, in->a * 16 + 8);
                    e.promoted |= (uint16_t)(1u << sp); ctx->stats.promoted_values++;
                } else xe_value_copy(&e, 12, xe_trace_stack(sp), 13, in->a * 16);
                break;
            case TOI_JIT_NATIVE_OP_STORE_LOCAL:
                if (ctx->skip_store[pc]) break;
                if (sp <= 14 && (e.promoted & (1u << (sp - 1)))) {
                    xe_imm(&e, 0, VAL_NUMBER); xe_store(&e, 0, 13, in->a * 16);
                    xe_sse_mem(&e, 1, sp + 1, 13, in->a * 16 + 8);
                } else xe_value_copy(&e, 13, in->a * 16, 12, b);
                break;
            case TOI_JIT_NATIVE_OP_PUSH_CONST:
                if (sp < 14 && IS_NUMBER(in->constant)) {
                    xe_double(&e, sp + 2, AS_NUMBER(in->constant));
                    e.promoted |= (uint16_t)(1u << sp); ctx->stats.promoted_values++;
                } else xe_value_const(&e, xe_trace_stack(sp), in->constant);
                break;
            case TOI_JIT_NATIVE_OP_PUSH_NIL: xe_value_const(&e, xe_trace_stack(sp), NIL_VAL); break;
            case TOI_JIT_NATIVE_OP_PUSH_TRUE: case TOI_JIT_NATIVE_OP_PUSH_FALSE:
                xe_value_const(&e, xe_trace_stack(sp), BOOL_VAL(in->op == TOI_JIT_NATIVE_OP_PUSH_TRUE)); break;
            case TOI_JIT_NATIVE_OP_PUSH_UPVALUE:
                if (sp < 14 && ctx->numeric_upvalue[in->a]) {
                    xe_load(&e, 0, 12, offsetof(XeTraceFrame, closure));
                    xe_cmp_mem32(&e, 0, offsetof(ObjClosure, upvalue_count), in->a);
                    xe_trace_guard(&e, bails, &nb, 14, pc, sp, TOI_JIT_NATIVE_DEOPT_RUNTIME);
                    xe_load(&e, 0, 0, offsetof(ObjClosure, upvalues));
                    xe_byte(&e, 0x48); xe_byte(&e, 0x85); xe_byte(&e, 0xc0);
                    xe_trace_guard(&e, bails, &nb, 4, pc, sp, TOI_JIT_NATIVE_DEOPT_RUNTIME);
                    xe_load(&e, 0, 0, in->a * 8);
                    xe_byte(&e, 0x48); xe_byte(&e, 0x85); xe_byte(&e, 0xc0);
                    xe_trace_guard(&e, bails, &nb, 4, pc, sp, TOI_JIT_NATIVE_DEOPT_RUNTIME);
                    xe_load(&e, 0, 0, offsetof(ObjUpvalue, location));
                    xe_byte(&e, 0x48); xe_byte(&e, 0x85); xe_byte(&e, 0xc0);
                    xe_trace_guard(&e, bails, &nb, 4, pc, sp, TOI_JIT_NATIVE_DEOPT_RUNTIME);
                    xe_cmp_mem32(&e, 0, 0, VAL_NUMBER);
                    xe_trace_guard(&e, bails, &nb, 5, pc, sp, TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
                    xe_sse_mem(&e, 0, sp + 2, 0, 8);
                    e.promoted |= (uint16_t)(1u << sp); ctx->stats.promoted_values++;
                } else helper = (void*)&xe_trace_upvalue;
                break;
            case TOI_JIT_NATIVE_OP_BIN_ADD: case TOI_JIT_NATIVE_OP_BIN_SUB:
            case TOI_JIT_NATIVE_OP_BIN_MUL: case TOI_JIT_NATIVE_OP_BIN_DIV:
            case TOI_JIT_NATIVE_OP_BIN_MOD: case TOI_JIT_NATIVE_OP_CMP_LT: case TOI_JIT_NATIVE_OP_CMP_GT: {
                int left = sp <= 14 ? sp : 0, right = sp <= 14 ? sp + 1 : 1;
                if (sp > 14) xe_trace_flush(&e);
                for (int arg = sp - 2; arg < sp; arg++) {
                    if (arg < 14 && (e.promoted & (1u << arg))) continue;
                    xe_cmp_mem32(&e, 12, xe_trace_stack(arg), VAL_NUMBER);
                    xe_trace_guard(&e, bails, &nb, 5, pc, sp, TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
                    xe_sse_mem(&e, 0, sp <= 14 ? arg + 2 : arg - sp + 2, 12, xe_trace_stack(arg) + 8);
                    if (sp <= 14) e.promoted |= (uint16_t)(1u << arg);
                }
                if (in->op == TOI_JIT_NATIVE_OP_CMP_LT || in->op == TOI_JIT_NATIVE_OP_CMP_GT) {
                    xe_ucomi(&e, left, right);
                    xe_byte(&e, 0x0f); xe_byte(&e, in->op == TOI_JIT_NATIVE_OP_CMP_LT ? 0x92 : 0x97); xe_byte(&e, 0xc0);
                    xe_byte(&e, 0x0f); xe_byte(&e, 0x9b); xe_byte(&e, 0xc2);
                    xe_byte(&e, 0x20); xe_byte(&e, 0xd0);
                    xe_byte(&e, 0x0f); xe_byte(&e, 0xb6); xe_byte(&e, 0xc0);
                    xe_store(&e, 0, 12, a + 8); xe_imm(&e, 0, VAL_BOOL); xe_store(&e, 0, 12, a);
                    if (sp <= 14) e.promoted &= (uint16_t)~(3u << (sp - 2));
                } else {
                    if (in->op == TOI_JIT_NATIVE_OP_BIN_MOD) {
                        xe_trace_flush(&e);
                        xe_sse_mem(&e, 0, 0, 12, a + 8); xe_sse_mem(&e, 0, 1, 12, b + 8);
                        xe_call(&e, (void*)&fmod); xe_sse(&e, 0x10, left, 0);
                    } else xe_sse(&e, in->op == TOI_JIT_NATIVE_OP_BIN_ADD ? 0x58 :
                                     in->op == TOI_JIT_NATIVE_OP_BIN_SUB ? 0x5c :
                                     in->op == TOI_JIT_NATIVE_OP_BIN_MUL ? 0x59 : 0x5e, left, right);
                    if (sp <= 14) {
                        e.promoted &= (uint16_t)~(1u << (sp - 1));
                        e.promoted |= (uint16_t)(1u << (sp - 2));
                        ctx->stats.promoted_arithmetic++;
                    } else xe_sse_mem(&e, 1, left, 12, a + 8);
                }
                break;
            }
            case TOI_JIT_NATIVE_OP_LENGTH:
                xe_trace_emit_length(&e, in, pc, sp, bails, &nb); break;
            case TOI_JIT_NATIVE_OP_SLICE:
                helper = (void*)&xe_trace_slice; reason = TOI_JIT_NATIVE_DEOPT_TYPE_GUARD; break;
            case TOI_JIT_NATIVE_OP_CMP_EQ:
                xe_trace_flush(&e);
                xe_lea(&e, 7, 12, a); xe_lea(&e, 6, 12, b); xe_call(&e, (void*)&xe_trace_equal); break;
            case TOI_JIT_NATIVE_OP_NOT:
                xe_trace_flush(&e);
                xe_lea(&e, 7, 12, b); xe_call(&e, (void*)&xe_trace_falsey);
                xe_store(&e, 0, 12, b + 8); xe_imm(&e, 0, VAL_BOOL); xe_store(&e, 0, 12, b); break;
            case TOI_JIT_NATIVE_OP_GET_TABLE: case TOI_JIT_NATIVE_OP_GET_TABLE_NUM:
                xe_trace_array_load(&e, in, pc, sp, bails, &nb); break;
            case TOI_JIT_NATIVE_OP_SET_TABLE: case TOI_JIT_NATIVE_OP_SET_TABLE_NUM:
                helper = (void*)&xe_trace_set; reason = TOI_JIT_NATIVE_DEOPT_TABLE_GUARD; break;
            case TOI_JIT_NATIVE_OP_APPEND:
                helper = (void*)&xe_trace_append; reason = TOI_JIT_NATIVE_DEOPT_TABLE_GUARD; break;
            case TOI_JIT_NATIVE_OP_CALL: case TOI_JIT_NATIVE_OP_INVOKE:
                if (xe_is_random_invoke(in)) {
                    ctx->stats.random_calls++;
                    helper = (void*)&xe_trace_random;
                    reason = TOI_JIT_NATIVE_DEOPT_INVOKE_TARGET; break;
                }
                if (xe_trace_tiny_call(&e, in, pc, sp, bails, &nb)) {
                    ctx->stats.inlined_calls++; break;
                }
                if (xe_trace_linked_call(&e, in, pc, sp, bails, &nb)) {
                    ctx->stats.linked_calls++; break;
                }
                helper = (void*)&xe_trace_call;
                reason = in->op == TOI_JIT_NATIVE_OP_CALL ? TOI_JIT_NATIVE_DEOPT_CALL_TARGET : TOI_JIT_NATIVE_DEOPT_INVOKE_TARGET; break;
            case TOI_JIT_NATIVE_OP_BUILD_STRING2: case TOI_JIT_NATIVE_OP_BUILD_STRING3:
            case TOI_JIT_NATIVE_OP_BUILD_STRING4: case TOI_JIT_NATIVE_OP_BUILD_STRING5:
                helper = (void*)&xe_trace_string; break;
            case TOI_JIT_NATIVE_OP_JUMP: case TOI_JIT_NATIVE_OP_JUMP_IF_FALSE: case TOI_JIT_NATIVE_OP_JUMP_IF_TRUE:
                xe_trace_flush(&e);
                if (in->op != TOI_JIT_NATIVE_OP_JUMP) {
                    xe_lea(&e, 7, 12, b); xe_call(&e, (void*)&xe_trace_falsey); xe_test_eax(&e);
                }
                branches[np] = xe_jump(&e, in->op == TOI_JIT_NATIVE_OP_JUMP ? -1 :
                                           in->op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE ? 5 : 4);
                targets[np++] = in->target; break;
            default: e.failed = 1; break;
        }
        if (helper) {
            xe_trace_helper(&e, in, sp, helper);
            xe_trace_guard(&e, bails, &nb, 4, pc, sp, reason);
        }
    }
    e.promoted = 0; // Pending operands are discarded at the numeric backedge.
    labels[count] = e.count;
    /* Commit complete bodies at a bounded backedge. */
    xe_rex(&e, 0, 1, 12); xe_byte(&e, 0xff); xe_mem(&e, 1, 12, offsetof(XeTraceFrame, remaining));
    size_t again = xe_jump(&e, 5); xe_patch(&e, again, loop);
    xe_mov(&e, 7, 12); xe_imm(&e, 6, 0); xe_imm(&e, 2, 0); xe_imm(&e, 1, 0);
    xe_call(&e, (void*)&xe_trace_resume); size_t completed = xe_jump(&e, -1);
    size_t exit_label = e.count; xe_patch(&e, exited, exit_label);
    xe_imm(&e, 0, TOI_JIT_NATIVE_EXEC_EXIT); size_t exit_return = xe_jump(&e, -1);
    size_t invalid_label = e.count; xe_patch(&e, bad_entry, invalid_label);
    xe_imm(&e, 0, TOI_JIT_NATIVE_EXEC_DEOPT); size_t invalid_return = xe_jump(&e, -1);
    for (int i = 0; i < np; i++) {
        int target = targets[i];
        xe_patch(&e, branches[i], (target == 0 || target == count) ? labels[count] : labels[target]);
    }
    size_t bail_returns[1024];
    for (int i = 0; i < nb; i++) {
        xe_patch(&e, bails[i].patch, e.count);
        xe_trace_spill(&e, bails[i].promoted);
        xe_mov(&e, 7, 12); xe_imm(&e, 6, bails[i].pc < 0 ? 0 : (uintptr_t)&ctx->ops[bails[i].pc]);
        xe_imm(&e, 2, (uint64_t)bails[i].sp); xe_imm(&e, 1, (uint64_t)bails[i].reason);
        xe_call(&e, (void*)&xe_trace_resume); bail_returns[i] = xe_jump(&e, -1);
    }
    size_t epilogue = e.count;
    xe_patch(&e, completed, epilogue); xe_patch(&e, exit_return, epilogue); xe_patch(&e, invalid_return, epilogue);
    for (int i = 0; i < nb; i++) xe_patch(&e, bail_returns[i], epilogue);
    xe_stack(&e, frame, 1); xe_pop(&e, 3); xe_pop(&e, 15); xe_pop(&e, 14); xe_pop(&e, 13); xe_pop(&e, 12); xe_byte(&e, 0xc3);
    unsigned calls = 0; int pure_calls = 1;
    for (int i = 0; i < count; i++) {
        int op = ops[i].op;
        if (op == TOI_JIT_NATIVE_OP_CALL || op == TOI_JIT_NATIVE_OP_INVOKE) calls++;
        if ((op == TOI_JIT_NATIVE_OP_INVOKE && !xe_is_random_invoke(&ops[i])) || op == TOI_JIT_NATIVE_OP_APPEND ||
            op == TOI_JIT_NATIVE_OP_GET_TABLE || op == TOI_JIT_NATIVE_OP_GET_TABLE_NUM ||
            op == TOI_JIT_NATIVE_OP_SET_TABLE || op == TOI_JIT_NATIVE_OP_SET_TABLE_NUM ||
            (op >= TOI_JIT_NATIVE_OP_BUILD_STRING2 && op <= TOI_JIT_NATIVE_OP_BUILD_STRING5) ||
            op == TOI_JIT_NATIVE_OP_JUMP || op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE || op == TOI_JIT_NATIVE_OP_JUMP_IF_TRUE)
            pure_calls = 0;
    }
    ctx->stats.fast_call_loops = pure_calls && calls > 0 && calls == ctx->stats.inlined_calls + ctx->stats.linked_calls + ctx->stats.random_calls;
    // Primitive builders and string slices allocate but never dispatch user
    // code or run GC. Bound their work like the existing typed executor.
    int string_work = 0, safe_strings = 1;
    for (int i = 0; i < count; i++) {
        int op = ops[i].op;
        if (op == TOI_JIT_NATIVE_OP_SLICE ||
            (op >= TOI_JIT_NATIVE_OP_BUILD_STRING2 && op <= TOI_JIT_NATIVE_OP_BUILD_STRING5)) string_work++;
        if (op == TOI_JIT_NATIVE_OP_CALL || op == TOI_JIT_NATIVE_OP_INVOKE ||
            op == TOI_JIT_NATIVE_OP_APPEND || op == TOI_JIT_NATIVE_OP_SET_TABLE || op == TOI_JIT_NATIVE_OP_SET_TABLE_NUM ||
            op == TOI_JIT_NATIVE_OP_GET_TABLE || op == TOI_JIT_NATIVE_OP_GET_TABLE_NUM ||
            op == TOI_JIT_NATIVE_OP_JUMP || op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE || op == TOI_JIT_NATIVE_OP_JUMP_IF_TRUE)
            safe_strings = 0;
    }
    ctx->stats.fast_string_loops = safe_strings && string_work > 0;
    if (xe_publish(&e, ctx, fn, memory, size)) {
        ((X64CodeBundle*)*memory)->free_ctx = xe_trace_context_free;
        ((X64CodeBundle*)*memory)->stats = &ctx->stats;
        ((X64CodeBundle*)*memory)->mark_ctx = xe_trace_context_mark;
        return 1;
    }
    const char* debug = getenv("TOI_JIT_DEBUG_DIRECT_CORE");
    if (debug && debug[0] && strcmp(debug, "0") != 0)
        fprintf(stderr, "[x64-direct-reject] instructions=%d bytes=%zu failed=%d\n", count, e.count, e.failed);
    xe_trace_context_free(ctx); return 0;
}
