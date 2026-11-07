#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
#if !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS 0x1000
#endif

#include "jit_call_runtime.h"
#include "jit_native_trace.h"
#include "jit_x64.h"
#include "table.h"
#include "vm.h"

#define TOI_JIT_TRACE_STACK_CAP 256
#define X64_CODE_BUNDLE_MAGIC 0x584a4954u

typedef struct {
    uint8_t op;
    uint8_t arg;
} X64NumInstr;

typedef struct {
    uint32_t magic;
    void* exec_mem;
    size_t exec_size;
    void* ctx;
} X64CodeBundle;

typedef struct {
    int instr_count;
    ToiJitNumInstr instrs[128];
} X64NumericLoopCtx;

typedef struct {
    int instr_count;
    uint8_t var_slot;
    uint8_t end_slot;
    ToiJitSlotInstr instrs[256];
} X64SlotLoopCtx;

typedef struct {
    const ToiJitNativeTraceInstr* instrs;
    uint64_t packed_meta;
    uint32_t max_iters;
} X64NativeTraceCtx;

static int jit_value_falsey(Value v) {
    // Must match the interpreter (vm/ops_control.c is_falsey_local): nil, false,
    // 0, "" and empty tables are falsey. Machine-code branches depend on this.
    if (IS_NIL(v)) return 1;
    if (IS_BOOL(v)) return AS_BOOL(v) == 0;
    if (IS_NUMBER(v)) return AS_NUMBER(v) == 0;
    if (IS_STRING(v)) return AS_STRING(v)->length == 0;
    if (IS_TABLE(v)) {
        ObjTable* t = AS_TABLE(v);
        if (t->table.count > 0) return 0;
        for (int i = 0; i < t->table.array_capacity; i++) {
            if (!IS_NIL(t->table.array[i])) return 0;
        }
        return 1;
    }
    return 0;
}

static int x64_emit_u8(uint8_t* code, size_t cap, size_t* pos, uint8_t byte) {
    if (code == NULL || pos == NULL || *pos >= cap) return 0;
    code[(*pos)++] = byte;
    return 1;
}

static int x64_emit_u64(uint8_t* code, size_t cap, size_t* pos, uint64_t imm) {
    if (code == NULL || pos == NULL || *pos + 8u > cap) return 0;
    memcpy(code + *pos, &imm, 8u);
    *pos += 8u;
    return 1;
}

static int x64_emit_movabs_rax(uint8_t* code, size_t cap, size_t* pos, uint64_t imm) {
    return x64_emit_u8(code, cap, pos, 0x48) &&
           x64_emit_u8(code, cap, pos, 0xB8) &&
           x64_emit_u64(code, cap, pos, imm);
}

static int x64_emit_movabs_rdi(uint8_t* code, size_t cap, size_t* pos, uint64_t imm) {
    return x64_emit_u8(code, cap, pos, 0x48) &&
           x64_emit_u8(code, cap, pos, 0xBF) &&
           x64_emit_u64(code, cap, pos, imm);
}

static int x64_emit_jmp_rax(uint8_t* code, size_t cap, size_t* pos) {
    return x64_emit_u8(code, cap, pos, 0xFF) &&
           x64_emit_u8(code, cap, pos, 0xE0);
}

static int x64_emit_tailcall_same_sig(
    void* fn_ptr,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL || fn_ptr == NULL) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: invalid thunk outputs");
        return 0;
    }

    uint8_t code[16];
    size_t pos = 0;
    if (!x64_emit_movabs_rax(code, sizeof(code), &pos, (uint64_t)(uintptr_t)fn_ptr) ||
        !x64_emit_jmp_rax(code, sizeof(code), &pos)) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: thunk encode failed");
        return 0;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t map_size = ((pos + (size_t)page_size - 1u) / (size_t)page_size) * (size_t)page_size;
    void* mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: mmap failed");
        return 0;
    }
    memcpy(mem, code, pos);
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)mem, (char*)mem + pos);
#endif
    if (mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, map_size);
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: mprotect rx failed");
        return 0;
    }

    X64CodeBundle* bundle = (X64CodeBundle*)calloc(1, sizeof(X64CodeBundle));
    if (bundle == NULL) {
        munmap(mem, map_size);
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: bundle alloc failed");
        return 0;
    }
    bundle->magic = X64_CODE_BUNDLE_MAGIC;
    bundle->exec_mem = mem;
    bundle->exec_size = map_size;
    bundle->ctx = NULL;

    *out_fn = mem;
    *out_code_mem = bundle;
    *out_code_size = sizeof(X64CodeBundle);
    return 1;
}

static int x64_emit_tailcall_numeric_ctx(
    const X64NumericLoopCtx* ctx,
    void* fn_ptr,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (ctx == NULL || out_fn == NULL || out_code_mem == NULL || out_code_size == NULL || fn_ptr == NULL) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: invalid numeric thunk");
        return 0;
    }

    uint8_t code[64];
    size_t pos = 0;
    if (!x64_emit_u8(code, sizeof(code), &pos, 0x49) || !x64_emit_u8(code, sizeof(code), &pos, 0x89) || !x64_emit_u8(code, sizeof(code), &pos, 0xC8) || /* mov r8, rcx */
        !x64_emit_u8(code, sizeof(code), &pos, 0x48) || !x64_emit_u8(code, sizeof(code), &pos, 0x89) || !x64_emit_u8(code, sizeof(code), &pos, 0xD1) || /* mov rcx, rdx */
        !x64_emit_u8(code, sizeof(code), &pos, 0x48) || !x64_emit_u8(code, sizeof(code), &pos, 0x89) || !x64_emit_u8(code, sizeof(code), &pos, 0xF2) || /* mov rdx, rsi */
        !x64_emit_u8(code, sizeof(code), &pos, 0x48) || !x64_emit_u8(code, sizeof(code), &pos, 0x89) || !x64_emit_u8(code, sizeof(code), &pos, 0xFE) || /* mov rsi, rdi */
        !x64_emit_movabs_rdi(code, sizeof(code), &pos, (uint64_t)(uintptr_t)ctx) ||
        !x64_emit_movabs_rax(code, sizeof(code), &pos, (uint64_t)(uintptr_t)fn_ptr) ||
        !x64_emit_jmp_rax(code, sizeof(code), &pos)) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: numeric thunk encode failed");
        return 0;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t map_size = ((pos + (size_t)page_size - 1u) / (size_t)page_size) * (size_t)page_size;
    void* mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: mmap failed");
        return 0;
    }
    memcpy(mem, code, pos);
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)mem, (char*)mem + pos);
#endif
    if (mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, map_size);
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: mprotect rx failed");
        return 0;
    }

    X64CodeBundle* bundle = (X64CodeBundle*)calloc(1, sizeof(X64CodeBundle));
    if (bundle == NULL) {
        munmap(mem, map_size);
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: bundle alloc failed");
        return 0;
    }
    bundle->magic = X64_CODE_BUNDLE_MAGIC;
    bundle->exec_mem = mem;
    bundle->exec_size = map_size;
    bundle->ctx = (void*)ctx;

    *out_fn = mem;
    *out_code_mem = bundle;
    *out_code_size = sizeof(X64CodeBundle);
    return 1;
}

static int x64_emit_tailcall_slot_ctx(
    const X64SlotLoopCtx* ctx,
    void* fn_ptr,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (ctx == NULL || out_fn == NULL || out_code_mem == NULL || out_code_size == NULL || fn_ptr == NULL) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: invalid slot thunk");
        return 0;
    }

    uint8_t code[48];
    size_t pos = 0;
    if (!x64_emit_u8(code, sizeof(code), &pos, 0x48) || !x64_emit_u8(code, sizeof(code), &pos, 0x89) || !x64_emit_u8(code, sizeof(code), &pos, 0xF2) || /* mov rdx, rsi */
        !x64_emit_u8(code, sizeof(code), &pos, 0x48) || !x64_emit_u8(code, sizeof(code), &pos, 0x89) || !x64_emit_u8(code, sizeof(code), &pos, 0xFE) || /* mov rsi, rdi */
        !x64_emit_movabs_rdi(code, sizeof(code), &pos, (uint64_t)(uintptr_t)ctx) ||
        !x64_emit_movabs_rax(code, sizeof(code), &pos, (uint64_t)(uintptr_t)fn_ptr) ||
        !x64_emit_jmp_rax(code, sizeof(code), &pos)) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: slot thunk encode failed");
        return 0;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t map_size = ((pos + (size_t)page_size - 1u) / (size_t)page_size) * (size_t)page_size;
    void* mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: mmap failed");
        return 0;
    }
    memcpy(mem, code, pos);
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)mem, (char*)mem + pos);
#endif
    if (mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, map_size);
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: mprotect rx failed");
        return 0;
    }

    X64CodeBundle* bundle = (X64CodeBundle*)calloc(1, sizeof(X64CodeBundle));
    if (bundle == NULL) {
        munmap(mem, map_size);
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: bundle alloc failed");
        return 0;
    }
    bundle->magic = X64_CODE_BUNDLE_MAGIC;
    bundle->exec_mem = mem;
    bundle->exec_size = map_size;
    bundle->ctx = (void*)ctx;

    *out_fn = mem;
    *out_code_mem = bundle;
    *out_code_size = sizeof(X64CodeBundle);
    return 1;
}

static int x64_emit_tailcall_native_trace_ctx(
    const X64NativeTraceCtx* ctx,
    void* fn_ptr,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (ctx == NULL || out_fn == NULL || out_code_mem == NULL || out_code_size == NULL || fn_ptr == NULL) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: invalid native-trace thunk");
        return 0;
    }

    uint8_t code[64];
    size_t pos = 0;
    if (!x64_emit_u8(code, sizeof(code), &pos, 0x4D) || !x64_emit_u8(code, sizeof(code), &pos, 0x89) || !x64_emit_u8(code, sizeof(code), &pos, 0xC1) || /* mov r9, r8 */
        !x64_emit_u8(code, sizeof(code), &pos, 0x49) || !x64_emit_u8(code, sizeof(code), &pos, 0x89) || !x64_emit_u8(code, sizeof(code), &pos, 0xC8) || /* mov r8, rcx */
        !x64_emit_u8(code, sizeof(code), &pos, 0x48) || !x64_emit_u8(code, sizeof(code), &pos, 0x89) || !x64_emit_u8(code, sizeof(code), &pos, 0xD1) || /* mov rcx, rdx */
        !x64_emit_u8(code, sizeof(code), &pos, 0x48) || !x64_emit_u8(code, sizeof(code), &pos, 0x89) || !x64_emit_u8(code, sizeof(code), &pos, 0xF2) || /* mov rdx, rsi */
        !x64_emit_u8(code, sizeof(code), &pos, 0x48) || !x64_emit_u8(code, sizeof(code), &pos, 0x89) || !x64_emit_u8(code, sizeof(code), &pos, 0xFE) || /* mov rsi, rdi */
        !x64_emit_movabs_rdi(code, sizeof(code), &pos, (uint64_t)(uintptr_t)ctx) ||
        !x64_emit_movabs_rax(code, sizeof(code), &pos, (uint64_t)(uintptr_t)fn_ptr) ||
        !x64_emit_jmp_rax(code, sizeof(code), &pos)) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: native-trace thunk encode failed");
        return 0;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t map_size = ((pos + (size_t)page_size - 1u) / (size_t)page_size) * (size_t)page_size;
    void* mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: mmap failed");
        return 0;
    }
    memcpy(mem, code, pos);
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)mem, (char*)mem + pos);
#endif
    if (mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, map_size);
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: mprotect rx failed");
        return 0;
    }

    X64CodeBundle* bundle = (X64CodeBundle*)calloc(1, sizeof(X64CodeBundle));
    if (bundle == NULL) {
        munmap(mem, map_size);
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: bundle alloc failed");
        return 0;
    }
    bundle->magic = X64_CODE_BUNDLE_MAGIC;
    bundle->exec_mem = mem;
    bundle->exec_size = map_size;
    bundle->ctx = (void*)ctx;

    *out_fn = mem;
    *out_code_mem = bundle;
    *out_code_size = sizeof(X64CodeBundle);
    return 1;
}

static uint64_t x64_qfor_empty_runner(uint64_t var, uint64_t end) {
    while (var < end) var++;
    return var;
}

static void x64_math_reduce_runner(
    double* acc_ptr,
    double* var_ptr,
    double* end_ptr,
    double c1,
    double c2,
    double c3) {
    if (acc_ptr == NULL || var_ptr == NULL || end_ptr == NULL) return;
    double acc = *acc_ptr;
    double var = *var_ptr;
    double end = *end_ptr;
    while (++var <= end) {
        acc += (var * c1) / (fmod(var, c2) + c3);
    }
    *acc_ptr = acc;
    *var_ptr = var;
}

static void x64_numeric_loop_runner(
    const X64NumericLoopCtx* ctx,
    double* acc_ptr,
    double* var_ptr,
    double* end_ptr,
    const double* consts) {
    if (ctx == NULL || acc_ptr == NULL || var_ptr == NULL || end_ptr == NULL || consts == NULL) return;

    double acc = *acc_ptr;
    double var = *var_ptr;
    double end = *end_ptr;
    while (++var <= end) {
        double stack[64];
        int sp = 0;
        for (int i = 0; i < ctx->instr_count; i++) {
            uint8_t op = ctx->instrs[i].op;
            uint8_t arg = ctx->instrs[i].arg;
            switch (op) {
                case TOI_JIT_NUM_PUSH_ACC:
                    if (sp >= 64) return;
                    stack[sp++] = acc;
                    break;
                case TOI_JIT_NUM_PUSH_VAR:
                    if (sp >= 64) return;
                    stack[sp++] = var;
                    break;
                case TOI_JIT_NUM_PUSH_END:
                    if (sp >= 64) return;
                    stack[sp++] = end;
                    break;
                case TOI_JIT_NUM_PUSH_CONST:
                    if (sp >= 64) return;
                    stack[sp++] = consts[arg];
                    break;
                case TOI_JIT_NUM_BIN_ADD:
                case TOI_JIT_NUM_BIN_SUB:
                case TOI_JIT_NUM_BIN_MUL:
                case TOI_JIT_NUM_BIN_DIV:
                case TOI_JIT_NUM_BIN_MOD: {
                    if (sp < 2) return;
                    double b = stack[--sp];
                    double a = stack[--sp];
                    double out = 0.0;
                    if (op == TOI_JIT_NUM_BIN_ADD) out = a + b;
                    else if (op == TOI_JIT_NUM_BIN_SUB) out = a - b;
                    else if (op == TOI_JIT_NUM_BIN_MUL) out = a * b;
                    else if (op == TOI_JIT_NUM_BIN_DIV) out = a / b;
                    else out = fmod(a, b);
                    stack[sp++] = out;
                    break;
                }
                case TOI_JIT_NUM_STORE_ACC:
                    if (sp <= 0) return;
                    acc = stack[sp - 1];
                    break;
                case TOI_JIT_NUM_POP:
                    if (sp <= 0) return;
                    sp--;
                    break;
                default:
                    return;
            }
        }
    }
    *acc_ptr = acc;
    *var_ptr = var;
}

static void x64_slot_loop_runner(
    const X64SlotLoopCtx* ctx,
    double* slots,
    const double* consts) {
    if (ctx == NULL || slots == NULL || consts == NULL) return;
    double* var_ptr = &slots[ctx->var_slot];
    double end = slots[ctx->end_slot];
    while (++(*var_ptr) <= end) {
        double stack[64];
        int sp = 0;
        int pc = 0;
        uint32_t guard = 0;
        while (pc >= 0 && pc < ctx->instr_count) {
            if (++guard > 4096u) return;
            const ToiJitSlotInstr* in = &ctx->instrs[pc];
            switch (in->op) {
                case TOI_JIT_SLOT_PUSH_SLOT:
                    if (sp >= 64) return;
                    stack[sp++] = slots[in->arg];
                    pc++;
                    break;
                case TOI_JIT_SLOT_PUSH_CONST:
                    if (sp >= 64) return;
                    stack[sp++] = consts[in->arg];
                    pc++;
                    break;
                case TOI_JIT_SLOT_BIN_ADD:
                case TOI_JIT_SLOT_BIN_SUB:
                case TOI_JIT_SLOT_BIN_MUL:
                case TOI_JIT_SLOT_BIN_DIV:
                case TOI_JIT_SLOT_BIN_MOD: {
                    if (sp < 2) return;
                    double b = stack[--sp];
                    double a = stack[--sp];
                    double out = 0.0;
                    if (in->op == TOI_JIT_SLOT_BIN_ADD) out = a + b;
                    else if (in->op == TOI_JIT_SLOT_BIN_SUB) out = a - b;
                    else if (in->op == TOI_JIT_SLOT_BIN_MUL) out = a * b;
                    else if (in->op == TOI_JIT_SLOT_BIN_DIV) out = a / b;
                    else out = fmod(a, b);
                    stack[sp++] = out;
                    pc++;
                    break;
                }
                case TOI_JIT_SLOT_CMP_LT:
                case TOI_JIT_SLOT_CMP_GT:
                case TOI_JIT_SLOT_CMP_EQ: {
                    if (sp < 2) return;
                    double b = stack[--sp];
                    double a = stack[--sp];
                    double out = 0.0;
                    if (in->op == TOI_JIT_SLOT_CMP_LT) out = (a < b) ? 1.0 : 0.0;
                    else if (in->op == TOI_JIT_SLOT_CMP_GT) out = (a > b) ? 1.0 : 0.0;
                    else out = (a == b) ? 1.0 : 0.0;
                    stack[sp++] = out;
                    pc++;
                    break;
                }
                case TOI_JIT_SLOT_STORE_SLOT:
                    if (sp <= 0) return;
                    slots[in->arg] = stack[sp - 1];
                    pc++;
                    break;
                case TOI_JIT_SLOT_JUMP:
                    pc = (int)in->arg;
                    break;
                case TOI_JIT_SLOT_JUMP_IF_FALSE:
                case TOI_JIT_SLOT_JUMP_IF_TRUE: {
                    if (sp <= 0) return;
                    int cond = (stack[sp - 1] == 0.0);
                    if (in->op == TOI_JIT_SLOT_JUMP_IF_TRUE) cond = !cond;
                    pc = cond ? (int)in->arg : (pc + 1);
                    break;
                }
                case TOI_JIT_SLOT_POP:
                    if (sp <= 0) return;
                    sp--;
                    pc++;
                    break;
                default:
                    return;
            }
        }
    }
}

static int jit_value_get_table_num_fast(Value table_v, Value key, uint8_t op, Value* out) {
    if (out == NULL || !IS_TABLE(table_v)) return 0;
    ObjTable* t = AS_TABLE(table_v);
    if (!t->no_metatable_fast) return 0;
    Value result = NIL_VAL;
    if (IS_NUMBER(key)) {
        double num_key = AS_NUMBER(key);
        int idx = (int)num_key;
        if (num_key != (double)idx) return 0;
        if (idx < 0) idx = t->table.array_max + idx + 1;
        if (idx >= 1 && idx <= t->table.array_capacity) {
            Value arr_val = t->table.array[idx - 1];
            if (!IS_NIL(arr_val)) {
                *out = arr_val;
                return 1;
            }
        }
        if (table_get_array(&t->table, idx, &result)) {
            *out = result;
            return 1;
        }
        if (op == TOI_JIT_VAL_GET_TABLE_NUM && t->no_metatable_fast) {
            *out = NIL_VAL;
            return 1;
        }
        return 0;
    }
    if (op == TOI_JIT_VAL_GET_TABLE && IS_STRING(key) && t->no_metatable_fast) {
        if (table_get(&t->table, AS_STRING(key), &result)) *out = result;
        else *out = NIL_VAL;
        return 1;
    }
    return 0;
}

static int jit_value_set_table_fast(Value table_v, Value key, Value value, uint8_t op, Value* out) {
    if (out == NULL || !IS_TABLE(table_v)) return 0;
    ObjTable* t = AS_TABLE(table_v);
    if (!t->no_metatable_fast) return 0;
    if (IS_NUMBER(key)) {
        double num_key = AS_NUMBER(key);
        int idx = (int)num_key;
        int is_array = 0;
        if (num_key == (double)idx) {
            if (idx < 0) idx = t->table.array_max + idx + 1;
            if (table_set_array(&t->table, idx, value)) is_array = 1;
        }
        if (!is_array) {
            ObjString* n_key = number_key_string(num_key);
            table_set(&t->table, n_key, value);
        }
    } else if (op == TOI_JIT_VAL_SET_TABLE && IS_STRING(key)) {
        table_set(&t->table, AS_STRING(key), value);
    } else {
        return 0;
    }
    *out = value;
    return 1;
}

static int jit_value_append_fast(Value table_v, Value value, Value* out) {
    if (out == NULL || !IS_TABLE(table_v)) return 0;
    ObjTable* table = AS_TABLE(table_v);
    if (!table->no_metatable_fast) return 0;
    int index = table->table.array_max + 1;
    if (!table_set_array(&table->table, index, value)) {
        ObjString* key = number_key_string((double)index);
        table_set(&table->table, key, value);
    }
    *out = NUMBER_VAL((double)index);
    return 1;
}

static int x64_value_loop_runner(
    VM* vm,
    ObjClosure* closure,
    Value* slots,
    int slot_count,
    const ToiJitValueInstr* instrs,
    int instr_count,
    uint8_t var_slot,
    uint8_t end_slot) {
    if (vm == NULL || closure == NULL || slots == NULL || instrs == NULL) return 0;
    if (slot_count <= 0 || instr_count <= 0 || var_slot >= (uint8_t)slot_count || end_slot >= (uint8_t)slot_count) return 0;

    Value stack[TOI_JIT_TRACE_STACK_CAP];
    uint32_t iter_guard = 0;
    while (1) {
        iter_guard++;
        if ((iter_guard & 0x3fffu) == 0u && iter_guard > 20000000u) return 0;
        Value var_v = slots[var_slot];
        Value end_v = slots[end_slot];
        if (!IS_NUMBER(var_v) || !IS_NUMBER(end_v)) return 0;
        double next = AS_NUMBER(var_v) + 1.0;
        double endn = AS_NUMBER(end_v);
        slots[var_slot] = NUMBER_VAL(next);
        if (next > endn) return 1;

        int sp = 0;
        int pc = 0;
        uint32_t body_guard = 0;
        while (pc >= 0 && pc < instr_count) {
            body_guard++;
            if ((body_guard & 0x3ffu) == 0u && body_guard > 200000u) return 0;
            const ToiJitValueInstr* in = &instrs[pc];
            switch (in->op) {
                case TOI_JIT_VAL_PUSH_LOCAL:
                    if ((int)in->a >= slot_count || sp >= TOI_JIT_TRACE_STACK_CAP) return 0;
                    stack[sp++] = slots[in->a];
                    pc++;
                    break;
                case TOI_JIT_VAL_PUSH_UPVALUE:
                    if ((int)in->a >= closure->upvalue_count || sp >= TOI_JIT_TRACE_STACK_CAP) return 0;
                    if (closure->upvalues[in->a] == NULL || closure->upvalues[in->a]->location == NULL) return 0;
                    stack[sp++] = *closure->upvalues[in->a]->location;
                    pc++;
                    break;
                case TOI_JIT_VAL_PUSH_CONST:
                    if (sp >= TOI_JIT_TRACE_STACK_CAP) return 0;
                    stack[sp++] = in->constant;
                    pc++;
                    break;
                case TOI_JIT_VAL_PUSH_NIL:
                    if (sp >= TOI_JIT_TRACE_STACK_CAP) return 0;
                    stack[sp++] = NIL_VAL;
                    pc++;
                    break;
                case TOI_JIT_VAL_PUSH_TRUE:
                    if (sp >= TOI_JIT_TRACE_STACK_CAP) return 0;
                    stack[sp++] = BOOL_VAL(1);
                    pc++;
                    break;
                case TOI_JIT_VAL_PUSH_FALSE:
                    if (sp >= TOI_JIT_TRACE_STACK_CAP) return 0;
                    stack[sp++] = BOOL_VAL(0);
                    pc++;
                    break;
                case TOI_JIT_VAL_STORE_LOCAL:
                    if ((int)in->a >= slot_count || sp <= 0) return 0;
                    slots[in->a] = stack[sp - 1];
                    pc++;
                    break;
                case TOI_JIT_VAL_POP:
                    if (sp <= 0) return 0;
                    sp--;
                    pc++;
                    break;
                case TOI_JIT_VAL_BIN_ADD:
                case TOI_JIT_VAL_BIN_SUB:
                case TOI_JIT_VAL_BIN_MUL:
                case TOI_JIT_VAL_BIN_DIV:
                case TOI_JIT_VAL_BIN_MOD: {
                    if (sp < 2) return 0;
                    Value b = stack[--sp];
                    Value a = stack[--sp];
                    if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
                    double da = AS_NUMBER(a);
                    double db = AS_NUMBER(b);
                    double out = 0.0;
                    if (in->op == TOI_JIT_VAL_BIN_ADD) out = da + db;
                    else if (in->op == TOI_JIT_VAL_BIN_SUB) out = da - db;
                    else if (in->op == TOI_JIT_VAL_BIN_MUL) out = da * db;
                    else if (in->op == TOI_JIT_VAL_BIN_DIV) out = da / db;
                    else out = fmod(da, db);
                    stack[sp++] = NUMBER_VAL(out);
                    pc++;
                    break;
                }
                case TOI_JIT_VAL_CMP_EQ: {
                    if (sp < 2) return 0;
                    Value b = stack[--sp];
                    Value a = stack[--sp];
                    int eq = 0;
                    if (a.type == b.type) {
                        if (IS_NIL(a)) eq = 1;
                        else if (IS_BOOL(a)) eq = (AS_BOOL(a) == AS_BOOL(b));
                        else if (IS_NUMBER(a)) eq = (AS_NUMBER(a) == AS_NUMBER(b));
                        else if (IS_OBJ(a)) eq = (AS_OBJ(a) == AS_OBJ(b));
                    }
                    stack[sp++] = BOOL_VAL(eq);
                    pc++;
                    break;
                }
                case TOI_JIT_VAL_CMP_LT:
                case TOI_JIT_VAL_CMP_GT: {
                    if (sp < 2) return 0;
                    Value b = stack[--sp];
                    Value a = stack[--sp];
                    if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
                    int c = (in->op == TOI_JIT_VAL_CMP_LT) ? (AS_NUMBER(a) < AS_NUMBER(b)) : (AS_NUMBER(a) > AS_NUMBER(b));
                    stack[sp++] = BOOL_VAL(c);
                    pc++;
                    break;
                }
                case TOI_JIT_VAL_NOT:
                    if (sp <= 0) return 0;
                    stack[sp - 1] = BOOL_VAL(jit_value_falsey(stack[sp - 1]));
                    pc++;
                    break;
                case TOI_JIT_VAL_APPEND: {
                    if (sp < 2) return 0;
                    Value result = NIL_VAL;
                    if (!jit_value_append_fast(stack[sp - 2], stack[sp - 1], &result)) return 0;
                    stack[sp - 2] = result;
                    sp--;
                    pc++;
                    break;
                }
                case TOI_JIT_VAL_GET_TABLE:
                case TOI_JIT_VAL_GET_TABLE_NUM: {
                    if (sp < 2) return 0;
                    Value result = NIL_VAL;
                    if (!jit_value_get_table_num_fast(stack[sp - 2], stack[sp - 1], in->op, &result)) return 0;
                    stack[sp - 2] = result;
                    sp--;
                    pc++;
                    break;
                }
                case TOI_JIT_VAL_SET_TABLE:
                case TOI_JIT_VAL_SET_TABLE_NUM: {
                    if (sp < 3) return 0;
                    Value result = NIL_VAL;
                    if (!jit_value_set_table_fast(stack[sp - 3], stack[sp - 2], stack[sp - 1], in->op, &result)) return 0;
                    stack[sp - 3] = result;
                    sp -= 2;
                    pc++;
                    break;
                }
                case TOI_JIT_VAL_CALL: {
                    int argc = (int)in->a;
                    if (argc < 0 || sp < argc + 1) return 0;
                    int callee_idx = sp - 1 - argc;
                    Value out = NIL_VAL;
                    if (!jit_call_primitive(vm, stack[callee_idx], &stack[callee_idx + 1], argc, &out)) return 0;
                    stack[callee_idx] = out;
                    sp = callee_idx + 1;
                    pc++;
                    break;
                }
                case TOI_JIT_VAL_INVOKE: {
                    int argc = (int)in->a;
                    if (argc < 0 || sp < argc + 1) return 0;
                    int recv_idx = sp - 1 - argc;
                    Value out = NIL_VAL;
                    if (!jit_invoke_primitive(vm, stack[recv_idx], in->constant, &stack[recv_idx + 1], argc, &out)) return 0;
                    stack[recv_idx] = out;
                    sp = recv_idx + 1;
                    pc++;
                    break;
                }
                case TOI_JIT_VAL_BUILD_STRING2:
                case TOI_JIT_VAL_BUILD_STRING3:
                case TOI_JIT_VAL_BUILD_STRING4:
                case TOI_JIT_VAL_BUILD_STRING5: {
                    int n = (in->op == TOI_JIT_VAL_BUILD_STRING2) ? 2 :
                            (in->op == TOI_JIT_VAL_BUILD_STRING3) ? 3 :
                            (in->op == TOI_JIT_VAL_BUILD_STRING4) ? 4 : 5;
                    if (sp < n) return 0;
                    Value out = NIL_VAL;
                    if (!jit_build_string_primitive(vm, &stack[sp - n], n, &out)) return 0;
                    stack[sp - n] = out;
                    sp = sp - n + 1;
                    pc++;
                    break;
                }
                case TOI_JIT_VAL_JUMP:
                    if ((int)in->target < 0 || (int)in->target > instr_count) return 0;
                    pc = (int)in->target;
                    break;
                case TOI_JIT_VAL_JUMP_IF_FALSE:
                    if (sp <= 0 || (int)in->target < 0 || (int)in->target > instr_count) return 0;
                    pc = jit_value_falsey(stack[sp - 1]) ? (int)in->target : (pc + 1);
                    break;
                case TOI_JIT_VAL_JUMP_IF_TRUE:
                    if (sp <= 0 || (int)in->target < 0 || (int)in->target > instr_count) return 0;
                    pc = jit_value_falsey(stack[sp - 1]) ? (pc + 1) : (int)in->target;
                    break;
                case TOI_JIT_VAL_ADJUST_STACK:
                    if ((int)in->a < 0 || (int)in->a > TOI_JIT_TRACE_STACK_CAP) return 0;
                    sp = (int)in->a;
                    pc++;
                    break;
                default:
                    return 0;
            }
        }
        if (pc != instr_count) return 0;
    }
}

static ToiJitNativeExecResult x64_native_trace_bridge_runner(
    VM* vm,
    ObjClosure* closure,
    Value* slots,
    int slot_count,
    ToiJitNativeExecState* out_state,
    const ToiJitNativeTraceInstr* instrs,
    uint64_t packed_meta,
    uint32_t max_iters) {
    if (slot_count <= 0) return TOI_JIT_NATIVE_EXEC_DEOPT;
    Value slot_backup[slot_count];
    int dirty_slots[slot_count];
    int dirty_count = 0;
    size_t slot_saved_bytes = ((size_t)slot_count + 7u) >> 3;
    uint8_t slot_saved_bits[slot_saved_bytes];
    memset(slot_saved_bits, 0, slot_saved_bytes);
#define X64_NTRACE_SLOT_WRITE(slot_idx, slot_val)                                                                       \
    do {                                                                                                                \
        int __slot_idx = (int)(slot_idx);                                                                              \
        size_t __slot_byte = ((size_t)__slot_idx) >> 3;                                                                \
        uint8_t __slot_mask = (uint8_t)(1u << ((uint8_t)__slot_idx & 7u));                                             \
        if ((slot_saved_bits[__slot_byte] & __slot_mask) == 0) {                                                       \
            slot_saved_bits[__slot_byte] |= __slot_mask;                                                               \
            slot_backup[__slot_idx] = slots[__slot_idx];                                                               \
            dirty_slots[dirty_count++] = __slot_idx;                                                                   \
        }                                                                                                               \
        slots[__slot_idx] = (slot_val);                                                                                \
    } while (0)
#define X64_NTRACE_ROLLBACK()                                                                                           \
    do {                                                                                                                \
        for (int __i = dirty_count - 1; __i >= 0; __i--) {                                                             \
            int __slot_idx = dirty_slots[__i];                                                                         \
            slots[__slot_idx] = slot_backup[__slot_idx];                                                               \
        }                                                                                                               \
    } while (0)
#define X64_NTRACE_DEOPT(code)                                                                                          \
    do {                                                                                                                \
        X64_NTRACE_ROLLBACK();                                                                                          \
        if (out_state != NULL) {                                                                                        \
            out_state->resume_offset = last_resume_offset;                                                              \
            out_state->op = last_op;                                                                                    \
            out_state->reason = (uint8_t)(code);                                                                        \
        }                                                                                                               \
        return TOI_JIT_NATIVE_EXEC_DEOPT;                                                                               \
    } while (0)

    int instr_count = (int)(packed_meta & 0xffffu);
    uint8_t var_slot = (uint8_t)((packed_meta >> 16) & 0xffu);
    uint8_t end_slot = (uint8_t)((packed_meta >> 24) & 0xffu);
    if (max_iters == 0u) max_iters = 1u;
    uint32_t first_resume_offset = (instrs != NULL && instr_count > 0) ? instrs[0].deopt_offset : 0xffffffffu;
    if (out_state != NULL) {
        out_state->resume_offset = first_resume_offset;
        out_state->op = 0;
        out_state->reason = TOI_JIT_NATIVE_DEOPT_NONE;
        out_state->stack_count = 0;
    }
    uint32_t last_resume_offset = first_resume_offset;
    uint8_t last_op = 0;
    if (vm == NULL || closure == NULL || slots == NULL || instrs == NULL) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_RUNTIME);
    if (instr_count <= 0) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_RUNTIME);
    if ((int)var_slot >= slot_count || (int)end_slot >= slot_count) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);

    Value stack[TOI_JIT_TRACE_STACK_CAP];
    for (uint32_t iter = 0; iter < max_iters; iter++) {
        Value var_v = slots[var_slot];
        Value end_v = slots[end_slot];
        if (!IS_NUMBER(var_v) || !IS_NUMBER(end_v)) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
        double next = AS_NUMBER(var_v) + 1.0;
        double endn = AS_NUMBER(end_v);
        X64_NTRACE_SLOT_WRITE(var_slot, NUMBER_VAL(next));
        if (next > endn) return TOI_JIT_NATIVE_EXEC_EXIT;

        int sp = 0;
        int pc = 0;
        uint32_t guard = 0;
        int hit_backedge = 0;
        while (pc >= 0 && pc < instr_count) {
            if (++guard > 4096u) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_CONTROL_FLOW);
            const ToiJitNativeTraceInstr* in = &instrs[pc];
            last_resume_offset = in->deopt_offset;
            uint8_t op = in->op;
            last_op = op;
            switch (op) {
                case TOI_JIT_NATIVE_OP_PUSH_LOCAL:
                    if ((int)in->a >= slot_count || sp >= TOI_JIT_TRACE_STACK_CAP) {
                        X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    }
                    stack[sp++] = slots[in->a];
                    pc++;
                    break;
                case TOI_JIT_NATIVE_OP_PUSH_UPVALUE:
                    if ((int)in->a >= closure->upvalue_count || sp >= TOI_JIT_TRACE_STACK_CAP) {
                        X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    }
                    if (closure->upvalues[in->a] == NULL || closure->upvalues[in->a]->location == NULL) {
                        X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_RUNTIME);
                    }
                    stack[sp++] = *closure->upvalues[in->a]->location;
                    pc++;
                    break;
                case TOI_JIT_NATIVE_OP_PUSH_CONST:
                    if (sp >= TOI_JIT_TRACE_STACK_CAP) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    stack[sp++] = in->constant;
                    pc++;
                    break;
                case TOI_JIT_NATIVE_OP_PUSH_NIL:
                    if (sp >= TOI_JIT_TRACE_STACK_CAP) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    stack[sp++] = NIL_VAL;
                    pc++;
                    break;
                case TOI_JIT_NATIVE_OP_PUSH_TRUE:
                    if (sp >= TOI_JIT_TRACE_STACK_CAP) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    stack[sp++] = BOOL_VAL(1);
                    pc++;
                    break;
                case TOI_JIT_NATIVE_OP_PUSH_FALSE:
                    if (sp >= TOI_JIT_TRACE_STACK_CAP) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    stack[sp++] = BOOL_VAL(0);
                    pc++;
                    break;
                case TOI_JIT_NATIVE_OP_STORE_LOCAL:
                    if ((int)in->a >= slot_count || sp <= 0) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    X64_NTRACE_SLOT_WRITE(in->a, stack[sp - 1]);
                    pc++;
                    break;
                case TOI_JIT_NATIVE_OP_POP:
                    if (sp <= 0) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    sp--;
                    pc++;
                    break;
                case TOI_JIT_NATIVE_OP_BIN_ADD:
                case TOI_JIT_NATIVE_OP_BIN_SUB:
                case TOI_JIT_NATIVE_OP_BIN_MUL:
                case TOI_JIT_NATIVE_OP_BIN_DIV:
                case TOI_JIT_NATIVE_OP_BIN_MOD: {
                    if (sp < 2) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    Value b = stack[--sp];
                    Value a = stack[--sp];
                    if (!IS_NUMBER(a) || !IS_NUMBER(b)) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
                    double da = AS_NUMBER(a);
                    double db = AS_NUMBER(b);
                    double out = 0.0;
                    if (op == TOI_JIT_NATIVE_OP_BIN_ADD) out = da + db;
                    else if (op == TOI_JIT_NATIVE_OP_BIN_SUB) out = da - db;
                    else if (op == TOI_JIT_NATIVE_OP_BIN_MUL) out = da * db;
                    else if (op == TOI_JIT_NATIVE_OP_BIN_DIV) out = da / db;
                    else out = fmod(da, db);
                    stack[sp++] = NUMBER_VAL(out);
                    pc++;
                    break;
                }
                case TOI_JIT_NATIVE_OP_CMP_EQ: {
                    if (sp < 2) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    Value b = stack[--sp];
                    Value a = stack[--sp];
                    int eq = 0;
                    if (a.type == b.type) {
                        if (IS_NIL(a)) eq = 1;
                        else if (IS_BOOL(a)) eq = (AS_BOOL(a) == AS_BOOL(b));
                        else if (IS_NUMBER(a)) eq = (AS_NUMBER(a) == AS_NUMBER(b));
                        else if (IS_OBJ(a)) eq = (AS_OBJ(a) == AS_OBJ(b));
                    }
                    stack[sp++] = BOOL_VAL(eq);
                    pc++;
                    break;
                }
                case TOI_JIT_NATIVE_OP_CMP_LT:
                case TOI_JIT_NATIVE_OP_CMP_GT: {
                    if (sp < 2) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    Value b = stack[--sp];
                    Value a = stack[--sp];
                    if (!IS_NUMBER(a) || !IS_NUMBER(b)) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
                    int c = (op == TOI_JIT_NATIVE_OP_CMP_LT) ? (AS_NUMBER(a) < AS_NUMBER(b)) : (AS_NUMBER(a) > AS_NUMBER(b));
                    stack[sp++] = BOOL_VAL(c);
                    pc++;
                    break;
                }
                case TOI_JIT_NATIVE_OP_NOT:
                    if (sp <= 0) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    stack[sp - 1] = BOOL_VAL(jit_value_falsey(stack[sp - 1]));
                    pc++;
                    break;
                case TOI_JIT_NATIVE_OP_APPEND: {
                    if (sp < 2) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    Value result = NIL_VAL;
                    if (!jit_value_append_fast(stack[sp - 2], stack[sp - 1], &result)) {
                        X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TABLE_GUARD);
                    }
                    stack[sp - 2] = result;
                    sp--;
                    pc++;
                    break;
                }
                case TOI_JIT_NATIVE_OP_GET_TABLE:
                case TOI_JIT_NATIVE_OP_GET_TABLE_NUM: {
                    if (sp < 2) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    Value result = NIL_VAL;
                    uint8_t src_op = (op == TOI_JIT_NATIVE_OP_GET_TABLE_NUM) ? TOI_JIT_VAL_GET_TABLE_NUM : TOI_JIT_VAL_GET_TABLE;
                    if (!jit_value_get_table_num_fast(stack[sp - 2], stack[sp - 1], src_op, &result)) {
                        X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TABLE_GUARD);
                    }
                    stack[sp - 2] = result;
                    sp--;
                    pc++;
                    break;
                }
                case TOI_JIT_NATIVE_OP_SET_TABLE:
                case TOI_JIT_NATIVE_OP_SET_TABLE_NUM: {
                    if (sp < 3) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    Value result = NIL_VAL;
                    uint8_t src_op = (op == TOI_JIT_NATIVE_OP_SET_TABLE_NUM) ? TOI_JIT_VAL_SET_TABLE_NUM : TOI_JIT_VAL_SET_TABLE;
                    if (!jit_value_set_table_fast(stack[sp - 3], stack[sp - 2], stack[sp - 1], src_op, &result)) {
                        X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TABLE_GUARD);
                    }
                    stack[sp - 3] = result;
                    sp -= 2;
                    pc++;
                    break;
                }
                case TOI_JIT_NATIVE_OP_CALL: {
                    int argc = (int)in->a;
                    if (argc < 0 || sp < argc + 1) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    int callee_idx = sp - 1 - argc;
                    Value out = NIL_VAL;
                    if (!jit_call_primitive(vm, stack[callee_idx], &stack[callee_idx + 1], argc, &out)) {
                        if (out_state != NULL) {
                            out_state->resume_offset = last_resume_offset;
                            out_state->op = last_op;
                            out_state->reason = TOI_JIT_NATIVE_DEOPT_CALL_TARGET;
                            int n = sp;
                            if (n > TOI_JIT_NATIVE_EXEC_STACK_CAP) n = TOI_JIT_NATIVE_EXEC_STACK_CAP;
                            out_state->stack_count = (uint16_t)n;
                            for (int i = 0; i < n; i++) out_state->stack[i] = stack[i];
                        }
                        return TOI_JIT_NATIVE_EXEC_CONTINUE;
                    }
                    stack[callee_idx] = out;
                    sp = callee_idx + 1;
                    pc++;
                    break;
                }
                case TOI_JIT_NATIVE_OP_INVOKE: {
                    int argc = (int)in->a;
                    if (argc < 0 || sp < argc + 1) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    int recv_idx = sp - 1 - argc;
                    Value out = NIL_VAL;
                    if (!jit_invoke_primitive(vm, stack[recv_idx], in->constant, &stack[recv_idx + 1], argc, &out)) {
                        if (out_state != NULL) {
                            out_state->resume_offset = last_resume_offset;
                            out_state->op = last_op;
                            out_state->reason = TOI_JIT_NATIVE_DEOPT_INVOKE_TARGET;
                            int n = sp;
                            if (n > TOI_JIT_NATIVE_EXEC_STACK_CAP) n = TOI_JIT_NATIVE_EXEC_STACK_CAP;
                            out_state->stack_count = (uint16_t)n;
                            for (int i = 0; i < n; i++) out_state->stack[i] = stack[i];
                        }
                        return TOI_JIT_NATIVE_EXEC_CONTINUE;
                    }
                    stack[recv_idx] = out;
                    sp = recv_idx + 1;
                    pc++;
                    break;
                }
                case TOI_JIT_NATIVE_OP_BUILD_STRING2:
                case TOI_JIT_NATIVE_OP_BUILD_STRING3:
                case TOI_JIT_NATIVE_OP_BUILD_STRING4:
                case TOI_JIT_NATIVE_OP_BUILD_STRING5: {
                    int n = (op == TOI_JIT_NATIVE_OP_BUILD_STRING2) ? 2 :
                            (op == TOI_JIT_NATIVE_OP_BUILD_STRING3) ? 3 :
                            (op == TOI_JIT_NATIVE_OP_BUILD_STRING4) ? 4 : 5;
                    if (sp < n) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    Value out = NIL_VAL;
                    if (!jit_build_string_primitive(vm, &stack[sp - n], n, &out)) {
                        X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_RUNTIME);
                    }
                    stack[sp - n] = out;
                    sp = sp - n + 1;
                    pc++;
                    break;
                }
                case TOI_JIT_NATIVE_OP_ADJUST_STACK:
                    if ((int)in->a < 0 || (int)in->a > TOI_JIT_TRACE_STACK_CAP) {
                        X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    }
                    sp = (int)in->a;
                    pc++;
                    break;
                case TOI_JIT_NATIVE_OP_JUMP: {
                    int tgt = (int)in->target;
                    if (tgt < 0 || tgt > instr_count) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_CONTROL_FLOW);
                    if (tgt == instr_count) return TOI_JIT_NATIVE_EXEC_EXIT;
                    if (tgt == 0) {
                        hit_backedge = 1;
                        pc = -1;
                        break;
                    }
                    pc = tgt;
                    break;
                }
                case TOI_JIT_NATIVE_OP_JUMP_IF_FALSE:
                case TOI_JIT_NATIVE_OP_JUMP_IF_TRUE: {
                    if (sp <= 0) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                    int tgt = (int)in->target;
                    if (tgt < 0 || tgt > instr_count) X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_CONTROL_FLOW);
                    int take = (op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE) ? jit_value_falsey(stack[sp - 1]) : !jit_value_falsey(stack[sp - 1]);
                    if (!take) {
                        pc++;
                        break;
                    }
                    if (tgt == instr_count) return TOI_JIT_NATIVE_EXEC_EXIT;
                    if (tgt == 0) {
                        hit_backedge = 1;
                        pc = -1;
                        break;
                    }
                    pc = tgt;
                    break;
                }
                case TOI_JIT_NATIVE_OP_NOP:
                    pc++;
                    break;
                default:
                    X64_NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_UNSUPPORTED);
            }
            if (hit_backedge) break;
        }
        if (!hit_backedge) {
            if (out_state != NULL) {
                out_state->resume_offset = first_resume_offset;
                out_state->stack_count = 0;
            }
            return TOI_JIT_NATIVE_EXEC_CONTINUE;
        }
        if (iter + 1u >= max_iters) {
            if (out_state != NULL) {
                out_state->resume_offset = first_resume_offset;
                out_state->stack_count = 0;
            }
            return TOI_JIT_NATIVE_EXEC_CONTINUE;
        }
    }
    return TOI_JIT_NATIVE_EXEC_CONTINUE;

#undef X64_NTRACE_DEOPT
#undef X64_NTRACE_ROLLBACK
#undef X64_NTRACE_SLOT_WRITE
}

static ToiJitNativeExecResult x64_native_trace_bridge_entry(
    const X64NativeTraceCtx* ctx,
    VM* vm,
    ObjClosure* closure,
    Value* slots,
    int slot_count,
    ToiJitNativeExecState* out_state) {
    if (ctx == NULL) {
        if (out_state != NULL) {
            out_state->resume_offset = 0xffffffffu;
            out_state->op = 0;
            out_state->reason = TOI_JIT_NATIVE_DEOPT_RUNTIME;
            out_state->stack_count = 0;
        }
        return TOI_JIT_NATIVE_EXEC_DEOPT;
    }
    return x64_native_trace_bridge_runner(
        vm,
        closure,
        slots,
        slot_count,
        out_state,
        ctx->instrs,
        ctx->packed_meta,
        ctx->max_iters);
}

int toi_jit_x64_host_supported(void) {
#if defined(__x86_64__) && (defined(__APPLE__) || defined(__linux__))
    return 1;
#else
    return 0;
#endif
}

int toi_jit_x64_compile_qfor_num_stub(
    const uint8_t* code, int code_count, int loop_offset, char* err_buf, size_t err_buf_size) {
    if (code == NULL || code_count <= 0 || loop_offset < 0 || loop_offset >= code_count) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "invalid qfor trace input");
        }
        return 0;
    }
    (void)memchr(code, 0, (size_t)code_count);
    return 1;
}

int toi_jit_x64_compile_qfor_empty(
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (!toi_jit_x64_host_supported()) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: host unsupported");
        return 0;
    }
    return x64_emit_tailcall_same_sig(
        (void*)&x64_qfor_empty_runner,
        out_fn,
        out_code_mem,
        out_code_size,
        err_buf,
        err_buf_size);
}

int toi_jit_x64_compile_math_reduce(
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (!toi_jit_x64_host_supported()) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: host unsupported");
        return 0;
    }
    return x64_emit_tailcall_same_sig(
        (void*)&x64_math_reduce_runner,
        out_fn,
        out_code_mem,
        out_code_size,
        err_buf,
        err_buf_size);
}

int toi_jit_x64_compile_numeric_loop(
    const ToiJitNumInstr* instrs,
    int instr_count,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: invalid outputs");
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;
    if (!toi_jit_x64_host_supported()) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: host unsupported");
        return 0;
    }
    if (instrs == NULL || instr_count <= 0 || instr_count > 128) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: invalid numeric loop ir");
        return 0;
    }

    X64NumericLoopCtx* ctx = (X64NumericLoopCtx*)calloc(1, sizeof(X64NumericLoopCtx));
    if (ctx == NULL) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: numeric ctx alloc failed");
        return 0;
    }
    ctx->instr_count = instr_count;
    memcpy(ctx->instrs, instrs, (size_t)instr_count * sizeof(ToiJitNumInstr));

    if (!x64_emit_tailcall_numeric_ctx(
            ctx,
            (void*)&x64_numeric_loop_runner,
            out_fn,
            out_code_mem,
            out_code_size,
            err_buf,
            err_buf_size)) {
        free(ctx);
        return 0;
    }
    return 1;
}

int toi_jit_x64_compile_slot_loop(
    const ToiJitSlotInstr* instrs,
    int instr_count,
    uint8_t var_slot,
    uint8_t end_slot,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: invalid outputs");
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;
    if (!toi_jit_x64_host_supported()) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: host unsupported");
        return 0;
    }
    if (instrs == NULL || instr_count <= 0 || instr_count > 256) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: invalid slot loop ir");
        return 0;
    }

    X64SlotLoopCtx* ctx = (X64SlotLoopCtx*)calloc(1, sizeof(X64SlotLoopCtx));
    if (ctx == NULL) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: slot ctx alloc failed");
        return 0;
    }
    ctx->instr_count = instr_count;
    ctx->var_slot = var_slot;
    ctx->end_slot = end_slot;
    memcpy(ctx->instrs, instrs, (size_t)instr_count * sizeof(ToiJitSlotInstr));

    if (!x64_emit_tailcall_slot_ctx(
            ctx,
            (void*)&x64_slot_loop_runner,
            out_fn,
            out_code_mem,
            out_code_size,
            err_buf,
            err_buf_size)) {
        free(ctx);
        return 0;
    }
    return 1;
}

int toi_jit_x64_compile_value_loop(
    const ToiJitValueInstr* instrs,
    int instr_count,
    uint8_t var_slot,
    uint8_t end_slot,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: invalid outputs");
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;
    if (!toi_jit_x64_host_supported()) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: host unsupported");
        return 0;
    }
    if (instrs == NULL || instr_count <= 0) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: invalid value ir");
        return 0;
    }
    for (int i = 0; i < instr_count; i++) {
        uint8_t op = instrs[i].op;
        switch (op) {
            case TOI_JIT_VAL_PUSH_LOCAL:
            case TOI_JIT_VAL_PUSH_UPVALUE:
            case TOI_JIT_VAL_PUSH_CONST:
            case TOI_JIT_VAL_PUSH_NIL:
            case TOI_JIT_VAL_PUSH_TRUE:
            case TOI_JIT_VAL_PUSH_FALSE:
            case TOI_JIT_VAL_STORE_LOCAL:
            case TOI_JIT_VAL_POP:
            case TOI_JIT_VAL_BIN_ADD:
            case TOI_JIT_VAL_BIN_SUB:
            case TOI_JIT_VAL_BIN_MUL:
            case TOI_JIT_VAL_BIN_DIV:
            case TOI_JIT_VAL_BIN_MOD:
            case TOI_JIT_VAL_CMP_EQ:
            case TOI_JIT_VAL_CMP_LT:
            case TOI_JIT_VAL_CMP_GT:
            case TOI_JIT_VAL_NOT:
            case TOI_JIT_VAL_JUMP:
            case TOI_JIT_VAL_JUMP_IF_FALSE:
            case TOI_JIT_VAL_JUMP_IF_TRUE:
            case TOI_JIT_VAL_APPEND:
            case TOI_JIT_VAL_GET_TABLE:
            case TOI_JIT_VAL_GET_TABLE_NUM:
            case TOI_JIT_VAL_SET_TABLE:
            case TOI_JIT_VAL_SET_TABLE_NUM:
            case TOI_JIT_VAL_ADJUST_STACK:
            case TOI_JIT_VAL_CALL:
            case TOI_JIT_VAL_INVOKE:
            case TOI_JIT_VAL_BUILD_STRING2:
            case TOI_JIT_VAL_BUILD_STRING3:
            case TOI_JIT_VAL_BUILD_STRING4:
            case TOI_JIT_VAL_BUILD_STRING5:
                break;
            default:
                if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: unsupported value op %u", (unsigned)op);
                return 0;
        }
    }
    (void)var_slot;
    (void)end_slot;
    *out_fn = (void*)&x64_value_loop_runner;
    return 1;
}

int toi_jit_x64_compile_native_trace(
    const struct ToiJitNativeTraceInstr* instrs,
    int instr_count,
    const struct ToiJitNativeGuard* guards,
    int guard_count,
    uint8_t var_slot,
    uint8_t end_slot,
    uint8_t* out_is_direct_core,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    (void)guards;
    (void)guard_count;
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: invalid outputs");
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;
    if (out_is_direct_core != NULL) *out_is_direct_core = 0;
    if (!toi_jit_x64_host_supported()) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: host unsupported");
        return 0;
    }
    if (instrs == NULL || instr_count <= 0 || instr_count > 256) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: invalid native trace");
        return 0;
    }

    X64NativeTraceCtx* ctx = (X64NativeTraceCtx*)calloc(1, sizeof(X64NativeTraceCtx));
    if (ctx == NULL) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "x64: native-trace ctx alloc failed");
        return 0;
    }
    ctx->instrs = instrs;
    ctx->packed_meta = ((uint64_t)(uint32_t)instr_count & 0xffffu) |
                       (((uint64_t)var_slot & 0xffu) << 16) |
                       (((uint64_t)end_slot & 0xffu) << 24);
    ctx->max_iters = 128u;

    if (!x64_emit_tailcall_native_trace_ctx(
            ctx,
            (void*)&x64_native_trace_bridge_entry,
            out_fn,
            out_code_mem,
            out_code_size,
            err_buf,
            err_buf_size)) {
        free(ctx);
        return 0;
    }
    return 1;
}

ToiJitNativeExecResult toi_jit_x64_run_native_trace(
    struct VM* vm,
    struct ObjClosure* closure,
    Value* slots,
    int slot_count,
    const struct ToiJitNativeTraceInstr* instrs,
    int instr_count,
    uint8_t var_slot,
    uint8_t end_slot,
    ToiJitNativeExecState* out_state) {
    uint64_t packed_meta = ((uint64_t)(uint16_t)instr_count) |
                           ((uint64_t)var_slot << 16) |
                           ((uint64_t)end_slot << 24);
    uint32_t max_iters = 1u;
    int has_callish = 0;
    for (int i = 0; i < instr_count; i++) {
        uint8_t op = instrs[i].op;
        if (op == TOI_JIT_NATIVE_OP_CALL ||
            op == TOI_JIT_NATIVE_OP_INVOKE ||
            op == TOI_JIT_NATIVE_OP_BUILD_STRING2 ||
            op == TOI_JIT_NATIVE_OP_BUILD_STRING3 ||
            op == TOI_JIT_NATIVE_OP_BUILD_STRING4 ||
            op == TOI_JIT_NATIVE_OP_BUILD_STRING5) {
            has_callish = 1;
            break;
        }
    }
    if (!has_callish) {
        max_iters = 8u;
    }
    return x64_native_trace_bridge_runner(
        vm,
        closure,
        slots,
        slot_count,
        out_state,
        instrs,
        packed_meta,
        max_iters);
}

void toi_jit_x64_free_code(void* code_mem, size_t code_size) {
    (void)code_size;
    X64CodeBundle* bundle = (X64CodeBundle*)code_mem;
    if (bundle == NULL) return;
    if (bundle->magic != X64_CODE_BUNDLE_MAGIC) {
        return;
    }
    if (bundle->exec_mem != NULL && bundle->exec_size != 0) {
        munmap(bundle->exec_mem, bundle->exec_size);
    }
    free(bundle->ctx);
    free(bundle);
}
