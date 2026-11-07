#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
#if !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS 0x1000
#endif

#include "jit_arm64.h"
#include "jit.h"
#include "jit_internal.h"
#include "jit_ir.h"
#include "jit_native_trace.h"
#include "chunk.h"
#include "vm.h"
#include "table.h"
#include "vm/build_string.h"

#ifndef TOI_JIT_NATIVE_TRACE_STRICT_CORE
#define TOI_JIT_NATIVE_TRACE_STRICT_CORE 0
#endif

#define TOI_JIT_TRACE_STACK_CAP 256
#define TOI_JIT_NATIVE_CALLSITE_PIC_CAP 1024
#define TOI_JIT_NATIVE_CALLEE_ENTRY_CAP 2048
#define TOI_JIT_NATIVE_PROGRAM_ENTRY_CAP 1024
#define TOI_JIT_NATIVE_INVOKE_IC_CAP 2048
#define TOI_JIT_NATIVE_CALL_POLY_PIC_CAP 1024

typedef int (*JitNativeCalleeEntryFn)(VM* vm, ObjClosure* callee, const Value* args, Value* out);
typedef enum {
    JIT_NATIVE_CALLEE_RUNNER_DIRECT = 0,
    JIT_NATIVE_CALLEE_RUNNER_PROGRAM_ENTRY = 1,
} JitNativeCalleeRunnerKind;

typedef struct {
    ObjClosure* caller;
    ObjClosure* callee;
    void* program_ptr;
    JitNativeCalleeEntryFn entry_fn;
    uint32_t opcode_offset;
    uint8_t argc;
    uint8_t valid;
} JitNativeCallsitePicEntry;

typedef struct {
    void* program_ptr;
    JitNativeCalleeEntryFn entry_fn;
    void* code_mem;
    size_t code_size;
    uint8_t argc;
    uint8_t runner_kind;
    uint8_t chain_direct_only;
    uint8_t valid;
} JitNativeCalleeEntry;

typedef struct {
    void* program_ptr;
    ToiJitNativeTraceFn fn;
    void* code_mem;
    size_t code_size;
    uint8_t argc;
    uint8_t ret_slot;
    uint8_t var_slot;
    uint8_t end_slot;
    uint8_t slot_count;
    uint8_t valid;
    uint8_t failed;
    uint8_t runtime_disabled;
    uint16_t exec_attempts;
    uint16_t exec_hits;
} JitNativeProgramEntry;

typedef struct {
    ObjTable* table;
    ObjString* method;
    ObjClosure* callee;
    uint32_t shape_version;
    uint8_t argc;
    uint8_t valid;
} JitNativeInvokeIcEntry;

typedef struct {
    ObjClosure* caller;
    uint32_t opcode_offset;
    uint8_t argc;
    uint8_t valid;
    ObjClosure* callee0;
    JitNativeCalleeEntryFn entry0;
    void* program0;
    ObjClosure* callee1;
    JitNativeCalleeEntryFn entry1;
    void* program1;
} JitNativeCallPolyPicEntry;

static JitNativeCallsitePicEntry jit_native_callsite_pic[TOI_JIT_NATIVE_CALLSITE_PIC_CAP];
static JitNativeCalleeEntry jit_native_callee_entries[TOI_JIT_NATIVE_CALLEE_ENTRY_CAP];
static JitNativeProgramEntry jit_native_program_entries[TOI_JIT_NATIVE_PROGRAM_ENTRY_CAP];
static JitNativeInvokeIcEntry jit_native_invoke_ic[TOI_JIT_NATIVE_INVOKE_IC_CAP];
static JitNativeCallPolyPicEntry jit_native_call_poly_pic[TOI_JIT_NATIVE_CALL_POLY_PIC_CAP];
static uint64_t jit_native_chain_attempts = 0;
static uint64_t jit_native_chain_hits = 0;
static uint64_t jit_native_chain_misses = 0;

static uint32_t jit_native_callsite_pic_hash(
    ObjClosure* caller,
    uint32_t opcode_offset,
    ObjClosure* callee,
    int argc) {
    uintptr_t a = (uintptr_t)caller;
    uintptr_t b = (uintptr_t)callee;
    uint32_t h = (uint32_t)(a >> 4) ^ (uint32_t)(a >> 17) ^ (uint32_t)(b >> 7) ^ (uint32_t)(b >> 21);
    h ^= opcode_offset * 2654435761u;
    h ^= (uint32_t)argc * 2246822519u;
    return h & (TOI_JIT_NATIVE_CALLSITE_PIC_CAP - 1u);
}

static uint32_t jit_native_call_poly_pic_hash(
    ObjClosure* caller,
    uint32_t opcode_offset,
    int argc) {
    uintptr_t a = (uintptr_t)caller;
    uint32_t h = (uint32_t)(a >> 4) ^ (uint32_t)(a >> 17);
    h ^= opcode_offset * 2654435761u;
    h ^= (uint32_t)argc * 2246822519u;
    return h & (TOI_JIT_NATIVE_CALL_POLY_PIC_CAP - 1u);
}

static uint32_t jit_native_callee_entry_hash(
    void* program_ptr,
    int argc) {
    uintptr_t b = (uintptr_t)program_ptr;
    uint32_t h = (uint32_t)(b >> 6) ^ (uint32_t)(b >> 19);
    h ^= (uint32_t)argc * 2246822519u;
    return h & (TOI_JIT_NATIVE_CALLEE_ENTRY_CAP - 1u);
}

static int jit_native_callee_entry_eligible(ObjClosure* callee, int argc);
static int jit_native_program_entry_enabled(void);
static int jit_native_program_entry_prefer_for_callee(void* program_ptr, ObjClosure* callee, int argc);
static int jit_native_call_closure_dispatch(
    VM* vm,
    ObjClosure* closure,
    const Value* args,
    int argc,
    void* program_ptr,
    Value* out_value);
static int jit_native_call_closure_callsite_dispatch(
    VM* vm,
    ObjClosure* caller_closure,
    uint32_t opcode_offset,
    ObjClosure* callee_closure,
    const Value* args,
    int argc,
    void* program_ptr,
    Value* out_value);

static void jit_native_plan_callee_entry(
    ObjClosure* callee,
    void* program_ptr,
    int argc,
    JitNativeCalleeRunnerKind* out_runner_kind,
    uint8_t* out_chain_direct_only) {
    JitNativeCalleeRunnerKind runner_kind = JIT_NATIVE_CALLEE_RUNNER_DIRECT;
    uint8_t chain_direct_only = 0;
    if (callee != NULL && program_ptr != NULL &&
        jit_native_callee_entry_eligible(callee, argc)) {
        int chain_safe = toi_jit_program_chain_safe(program_ptr) ? 1 : 0;
        if (chain_safe) {
            chain_direct_only = 1;
            if (jit_native_program_entry_enabled() &&
                jit_native_program_entry_prefer_for_callee(program_ptr, callee, argc)) {
                runner_kind = JIT_NATIVE_CALLEE_RUNNER_PROGRAM_ENTRY;
                chain_direct_only = 0;
            }
        }
    }
    if (out_runner_kind != NULL) *out_runner_kind = runner_kind;
    if (out_chain_direct_only != NULL) *out_chain_direct_only = chain_direct_only;
}

static uint32_t jit_native_program_entry_hash(void* program_ptr, int argc) {
    uintptr_t p = (uintptr_t)program_ptr;
    uint32_t h = (uint32_t)(p >> 6) ^ (uint32_t)(p >> 19);
    h ^= (uint32_t)argc * 3266489917u;
    return h & (TOI_JIT_NATIVE_PROGRAM_ENTRY_CAP - 1u);
}

static uint32_t jit_native_invoke_ic_hash(ObjTable* table, ObjString* method, int argc) {
    uintptr_t t = (uintptr_t)table;
    uintptr_t m = (uintptr_t)method;
    uint32_t h = (uint32_t)(t >> 6) ^ (uint32_t)(t >> 19) ^ (uint32_t)(m >> 5) ^ (uint32_t)(m >> 17);
    h ^= (uint32_t)argc * 2246822519u;
    return h & (TOI_JIT_NATIVE_INVOKE_IC_CAP - 1u);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static int jit_native_program_entry_enabled(void) {
    const char* v = getenv("TOI_JIT_NATIVE_PROGRAM_ENTRY");
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

static int jit_native_program_entry_debug(void) {
    const char* v = getenv("TOI_JIT_DEBUG_PROGRAM_ENTRY");
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

static int jit_native_call_poly_enabled(void) {
    const char* v = getenv("TOI_JIT_NATIVE_CALL_POLY");
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static int jit_native_program_entry_prefer_for_callee(void* program_ptr, ObjClosure* callee, int argc) {
    if (program_ptr == NULL || argc < 0) return 0;
    if (!toi_jit_program_chain_safe(program_ptr)) return 0;
    const JitProgram* p = (const JitProgram*)program_ptr;
    if (p == NULL || p->ops == NULL) return 0;
    if (p->op_count <= 0 || p->op_count > 128) return 0;
    if (p->max_slot < argc - 1 || p->max_slot > 32) return 0;
    if (callee == NULL || callee->function == NULL) return 0;
    // Entry-native lowering is profitable on hot call-dense bodies.
    // Keep string/table-heavy kernels on direct program dispatch until
    // dedicated lowerings are measurably faster.
    if (callee->function->hot_count < 128u) return 0;
    int call_sites = 0;
    int slow_heavy_ops = 0;
    for (int i = 0; i < p->op_count; i++) {
        JitOpCode op = p->ops[i].op;
        if (op == JOP_CALL || op == JOP_QCALL2 || op == JOP_INVOKE) {
            call_sites++;
            continue;
        }
        if (op == JOP_APPEND ||
            op == JOP_GET_TABLE ||
            op == JOP_GET_TABLE_NUM ||
            op == JOP_SET_TABLE ||
            op == JOP_SET_TABLE_NUM ||
            op == JOP_BUILD_STRING2 ||
            op == JOP_BUILD_STRING3 ||
            op == JOP_BUILD_STRING4 ||
            op == JOP_BUILD_STRING5) {
            slow_heavy_ops++;
            if (slow_heavy_ops > 0) return 0;
        }
    }
    if (call_sites == 0) return 0;
    if (p->op_count > 48 && call_sites < 2) return 0;
    return 1;
}

static int toi_jit_arm64_compile_native_trace_direct_core(
    const ToiJitNativeTraceInstr* instrs,
    int instr_count,
    const ToiJitNativeGuard* guards,
    int guard_count,
    uint8_t var_slot,
    uint8_t end_slot,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size);

static int jit_native_callee_entry_eligible(ObjClosure* callee, int argc) {
    if (callee == NULL || argc < 0) return 0;
    ObjFunction* function = callee->function;
    if (function == NULL) return 0;
    if (function->is_variadic || function->is_generator) return 0;
    if (function->defaults_count != 0) return 0;
    if (function->arity != argc) return 0;
    if (function->param_types_count > 0 && function->param_types != NULL) return 0;
    return 1;
}

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

static int jit_value_to_string_primitive_arm(Value in, Value* out) {
    if (out == NULL) return 0;
    if (IS_STRING(in)) {
        *out = in;
        return 1;
    }
    if (IS_NUMBER(in)) {
        char num_buf[32];
        int n = snprintf(num_buf, sizeof(num_buf), "%.14g", AS_NUMBER(in));
        if (n < 0) return 0;
        *out = OBJ_VAL(copy_string(num_buf, n));
        return 1;
    }
    if (IS_BOOL(in)) {
        *out = OBJ_VAL(copy_string(AS_BOOL(in) ? "true" : "false", AS_BOOL(in) ? 4 : 5));
        return 1;
    }
    if (IS_NIL(in)) {
        *out = OBJ_VAL(copy_string("nil", 3));
        return 1;
    }
    return 0;
}

static int jit_build_string_primitive_arm(VM* vm, Value* parts, int count, Value* out) {
    if (parts == NULL || out == NULL || count < 2 || count > 5) return 0;
    int primitive_only = 1;
    for (int i = 0; i < count; i++) {
        Value p = parts[i];
        if (!IS_STRING(p) && !IS_NUMBER(p) && !IS_BOOL(p) && !IS_NIL(p)) {
            primitive_only = 0;
            break;
        }
    }
    if (primitive_only) {
        ObjThread* thread = vm_current_thread(vm);
        if (thread != NULL && thread->stack_top + count <= thread->stack + thread->stack_capacity) {
            Value* saved_top = thread->stack_top;
            for (int i = 0; i < count; i++) *thread->stack_top++ = parts[i];
            if (vm_build_string(vm, (uint8_t)count)) {
                *out = pop(vm);
                thread->stack_top = saved_top;
                return 1;
            }
            thread->stack_top = saved_top;
            return 0;
        }
    }
    int total_len = 0;
    for (int i = 0; i < count; i++) {
        Value p = parts[i];
        if (IS_STRING(p)) total_len += AS_STRING(p)->length;
        else if (IS_NUMBER(p)) {
            char num_buf[32];
            int n = snprintf(num_buf, sizeof(num_buf), "%.14g", AS_NUMBER(p));
            if (n < 0) return 0;
            total_len += n;
        } else if (IS_BOOL(p)) total_len += AS_BOOL(p) ? 4 : 5;
        else if (IS_NIL(p)) total_len += 3;
        else return 0;
    }
    char* buf = (char*)malloc((size_t)total_len + 1);
    if (buf == NULL) return 0;
    char* cur = buf;
    for (int i = 0; i < count; i++) {
        Value p = parts[i];
        if (IS_STRING(p)) {
            ObjString* s = AS_STRING(p);
            memcpy(cur, s->chars, (size_t)s->length);
            cur += s->length;
        } else if (IS_NUMBER(p)) {
            int n = snprintf(cur, 32, "%.14g", AS_NUMBER(p));
            if (n < 0) {
                free(buf);
                return 0;
            }
            cur += n;
        } else if (IS_BOOL(p)) {
            if (AS_BOOL(p)) {
                memcpy(cur, "true", 4);
                cur += 4;
            } else {
                memcpy(cur, "false", 5);
                cur += 5;
            }
        } else {
            memcpy(cur, "nil", 3);
            cur += 3;
        }
    }
    *cur = '\0';
    *out = OBJ_VAL(take_string(buf, total_len));
    return 1;
}

static int jit_value_call_closure_interp_arm(VM* vm, ObjClosure* closure, Value* args, int argc, Value* out) {
    if (vm == NULL || closure == NULL || args == NULL || out == NULL || argc < 0) return 0;
    ObjThread* thread = vm_current_thread(vm);
    if (thread == NULL) return 0;
    if (thread->stack_top + argc + 1 > thread->stack + thread->stack_capacity) return 0;

    int saved_frame_count = thread->frame_count;
    push(vm, OBJ_VAL(closure));
    for (int i = 0; i < argc; i++) push(vm, args[i]);

    if (!call(vm, closure, argc)) return 0;
    if (vm_run(vm, saved_frame_count) != INTERPRET_OK) return 0;

    ObjThread* after = vm_current_thread(vm);
    if (after == NULL || after->stack_top <= after->stack) return 0;
    *out = after->stack_top[-1];
    after->stack_top--;
    return 1;
}

static int jit_value_call_primitive_arm(VM* vm, Value callee, Value* args, int argc, Value* out) {
    if (vm == NULL || out == NULL) return 0;
    if (IS_CLOSURE(callee)) {
        ObjClosure* closure = AS_CLOSURE(callee);
        if (closure == NULL) return 0;
        if (toi_jit_try_execute_with_args(vm, closure, argc, args, out)) return 1;
        return jit_value_call_closure_interp_arm(vm, closure, args, argc, out);
    }
    if (IS_BOUND_METHOD(callee)) {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        if (bound == NULL || bound->method == NULL) return 0;
        Value method_val = OBJ_VAL(bound->method);
        Value shifted_args[16];
        if (argc + 1 > 16) return 0;
        shifted_args[0] = bound->receiver;
        for (int i = 0; i < argc; i++) shifted_args[i + 1] = args[i];
        return jit_value_call_primitive_arm(vm, method_val, shifted_args, argc + 1, out);
    }
    if (IS_NATIVE(callee)) {
        ObjNative* n = AS_NATIVE_OBJ(callee);
        if (n == NULL) return 0;
        if (argc == 1 && n->name != NULL && n->name->length == 3 && memcmp(n->name->chars, "str", 3) == 0) {
            return jit_value_to_string_primitive_arm(args[0], out);
        }
        if (argc == 1 && n->name != NULL && n->name->length == 8 && memcmp(n->name->chars, "tostring", 8) == 0) {
            return jit_value_to_string_primitive_arm(args[0], out);
        }
        ObjThread* thread = vm_current_thread(vm);
        if (thread == NULL || thread->stack_top + argc > thread->stack + thread->stack_capacity) return 0;
        Value* saved_top = thread->stack_top;
        for (int i = 0; i < argc; i++) *thread->stack_top++ = args[i];
        Value* native_args = saved_top;
        int retc = n->function(vm, argc, native_args);
        if (retc <= 0) {
            thread->stack_top = saved_top;
            return 0;
        }
        ObjThread* after = vm_current_thread(vm);
        if (after != thread || thread->stack_top - saved_top < retc) {
            thread->stack_top = saved_top;
            return 0;
        }
        *out = thread->stack_top[-retc];
        thread->stack_top = saved_top;
        return 1;
    }
    return 0;
}

static int jit_value_is_self_callable_arm(Value v) {
    if (IS_CLOSURE(v)) return AS_CLOSURE(v)->function->is_self;
    if (IS_NATIVE(v)) return AS_NATIVE_OBJ(v)->is_self;
    return 0;
}

static int jit_objstring_eq_name_arm(ObjString* a, ObjString* b, const char* cstr) {
    if (a == NULL) return 0;
    if (b != NULL && a == b) return 1;
    if (cstr == NULL) return 0;
    size_t n = strlen(cstr);
    if ((size_t)a->length != n) return 0;
    return memcmp(a->chars, cstr, n) == 0;
}

static int jit_invoke_primitive_arm(VM* vm, Value receiver, Value method_name, Value* args, int argc, Value* out) {
    if (vm == NULL || out == NULL) return 0;
    if (!IS_STRING(method_name) || argc < 0) return 0;
    ObjString* method = AS_STRING(method_name);

    if (IS_STRING(receiver) && argc == 0) {
        if (jit_objstring_eq_name_arm(method, vm->str_lower_name, "lower")) {
            ObjString* src = AS_STRING(receiver);
            int len = src->length;
            char* buf = (char*)malloc((size_t)len + 1);
            if (buf == NULL) return 0;
            for (int i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)src->chars[i]);
            buf[len] = '\0';
            *out = OBJ_VAL(take_string(buf, len));
            return 1;
        }
        if (jit_objstring_eq_name_arm(method, vm->str_upper_name, "upper")) {
            ObjString* src = AS_STRING(receiver);
            int len = src->length;
            char* buf = (char*)malloc((size_t)len + 1);
            if (buf == NULL) return 0;
            for (int i = 0; i < len; i++) buf[i] = (char)toupper((unsigned char)src->chars[i]);
            buf[len] = '\0';
            *out = OBJ_VAL(take_string(buf, len));
            return 1;
        }
    }

    if (IS_TABLE(receiver)) {
        ObjTable* t = AS_TABLE(receiver);
        if (t != NULL) {
            Value raw_method = NIL_VAL;
            if (table_get(&t->table, method, &raw_method)) {
                if (!t->is_module && jit_value_is_self_callable_arm(raw_method)) {
                    Value shifted_args[16];
                    if (argc + 1 > 16) return 0;
                    shifted_args[0] = receiver;
                    for (int i = 0; i < argc; i++) shifted_args[i + 1] = args[i];
                    return jit_value_call_primitive_arm(vm, raw_method, shifted_args, argc + 1, out);
                }
                return jit_value_call_primitive_arm(vm, raw_method, args, argc, out);
            }
        }
    }
    return 0;
}

static int jit_value_get_table_num_fast(Value table_v, Value key, uint8_t op, Value* out) {
    if (out == NULL || !IS_TABLE(table_v)) return 0;
    ObjTable* t = AS_TABLE(table_v);
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

static int jit_value_loop_runner(
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
                    Value v = stack[sp - 1];
                    Value t = stack[sp - 2];
                    if (!IS_TABLE(t)) return 0;
                    ObjTable* table = AS_TABLE(t);
                    if (!table->no_metatable_fast) return 0;
                    int index = table->table.array_max + 1;
                    if (!table_set_array(&table->table, index, v)) {
                        ObjString* key = number_key_string((double)index);
                        table_set(&table->table, key, v);
                    }
                    stack[sp - 2] = NUMBER_VAL((double)index);
                    sp--;
                    pc++;
                    break;
                }
                case TOI_JIT_VAL_GET_TABLE:
                case TOI_JIT_VAL_GET_TABLE_NUM: {
                    if (sp < 2) return 0;
                    Value key = stack[sp - 1];
                    Value table_v = stack[sp - 2];
                    Value result = NIL_VAL;
                    if (jit_value_get_table_num_fast(table_v, key, in->op, &result)) {
                        stack[sp - 2] = result;
                        sp--;
                        pc++;
                        break;
                    }
                    if (!IS_TABLE(table_v)) return 0;
                    ObjTable* t = AS_TABLE(table_v);
                    if (in->op != TOI_JIT_VAL_GET_TABLE || !IS_STRING(key)) return 0;
                    if (!t->no_metatable_fast) return 0;
                    if (table_get(&t->table, AS_STRING(key), &result)) stack[sp - 2] = result;
                    else stack[sp - 2] = NIL_VAL;
                    sp--;
                    pc++;
                    break;
                }
                case TOI_JIT_VAL_SET_TABLE:
                case TOI_JIT_VAL_SET_TABLE_NUM: {
                    if (sp < 3) return 0;
                    Value value = stack[sp - 1];
                    Value key = stack[sp - 2];
                    Value table_v = stack[sp - 3];
                    Value result = NIL_VAL;
                    if (!jit_value_set_table_fast(table_v, key, value, in->op, &result)) return 0;
                    stack[sp - 3] = result;
                    sp -= 2;
                    pc++;
                    break;
                }
                case TOI_JIT_VAL_ADJUST_STACK:
                    if ((int)in->a < 0 || (int)in->a > TOI_JIT_TRACE_STACK_CAP) return 0;
                    sp = (int)in->a;
                    pc++;
                    break;
                case TOI_JIT_VAL_CALL: {
                    int argc = (int)in->a;
                    if (argc < 0 || sp < argc + 1) return 0;
                    int callee_idx = sp - 1 - argc;
                    Value out = NIL_VAL;
                    if (!jit_value_call_primitive_arm(vm, stack[callee_idx], &stack[callee_idx + 1], argc, &out)) return 0;
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
                    if (!jit_invoke_primitive_arm(vm, stack[recv_idx], in->constant, &stack[recv_idx + 1], argc, &out)) return 0;
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
                    if (!jit_build_string_primitive_arm(vm, &stack[sp - n], n, &out)) return 0;
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
                default:
                    return 0;
            }
        }
        if (pc != instr_count) return 0;
    }
}

typedef struct {
    uint32_t* words;
    int count;
    int capacity;
} Arm64CodeBuf;

typedef struct {
    uint8_t reg;
    uint8_t is_temp;
} A64ValReg;

static uint32_t a64_fadd(uint8_t rd, uint8_t rn, uint8_t rm) {
    return 0x1e602800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static uint32_t a64_fsub(uint8_t rd, uint8_t rn, uint8_t rm) {
    return 0x1e603800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static uint32_t a64_fmul(uint8_t rd, uint8_t rn, uint8_t rm) {
    return 0x1e600800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static uint32_t a64_fdiv(uint8_t rd, uint8_t rn, uint8_t rm) {
    return 0x1e601800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static uint32_t a64_frintz(uint8_t rd, uint8_t rn) {
    return 0x1e65c000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static uint32_t a64_fmov_rr(uint8_t rd, uint8_t rn) {
    return 0x1e604000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static uint32_t a64_fcmp(uint8_t rn, uint8_t rm) {
    return 0x1e602000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5);
}

static uint32_t a64_fcvtzs_x_d(uint8_t rd, uint8_t rn) {
    return 0x9E780000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static uint32_t a64_scvtf_d_x(uint8_t rd, uint8_t rn) {
    return 0x9E620000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static uint32_t a64_ldr_d_imm(uint8_t rt, uint8_t rn, uint16_t byte_offset) {
    uint32_t imm12 = (uint32_t)(byte_offset >> 3);
    return 0xfd400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static uint32_t a64_str_d_imm(uint8_t rt, uint8_t rn, uint16_t byte_offset) {
    uint32_t imm12 = (uint32_t)(byte_offset >> 3);
    return 0xfd000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static uint32_t a64_movz_x(uint8_t rd, uint16_t imm16, uint8_t shift16) {
    return 0xD2800000u | ((uint32_t)(shift16 & 0x3u) << 21) | ((uint32_t)imm16 << 5) | (uint32_t)rd;
}

static uint32_t a64_movk_x(uint8_t rd, uint16_t imm16, uint8_t shift16) {
    return 0xF2800000u | ((uint32_t)(shift16 & 0x3u) << 21) | ((uint32_t)imm16 << 5) | (uint32_t)rd;
}

static uint32_t a64_blr(uint8_t rn) {
    return 0xD63F0000u | ((uint32_t)rn << 5);
}

static uint32_t a64_str_x_imm(uint8_t rt, uint8_t rn, uint16_t byte_offset) {
    uint32_t imm12 = (uint32_t)(byte_offset >> 3);
    return 0xF9000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static uint32_t a64_ldr_x_imm(uint8_t rt, uint8_t rn, uint16_t byte_offset) {
    uint32_t imm12 = (uint32_t)(byte_offset >> 3);
    return 0xF9400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static uint32_t a64_ldr_w_imm(uint8_t rt, uint8_t rn, uint16_t byte_offset) {
    uint32_t imm12 = (uint32_t)(byte_offset >> 2);
    return 0xB9400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static uint32_t a64_ldr_b_imm(uint8_t rt, uint8_t rn, uint16_t byte_offset) {
    return 0x39400000u | (((uint32_t)byte_offset & 0x0fffu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static uint32_t a64_sub_imm(uint8_t rd, uint8_t rn, uint16_t imm12) {
    return 0xD1000000u | (((uint32_t)imm12 & 0x0fffu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static uint32_t a64_add_imm(uint8_t rd, uint8_t rn, uint16_t imm12) {
    return 0x91000000u | (((uint32_t)imm12 & 0x0fffu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static uint32_t a64_mov_x(uint8_t rd, uint8_t rn) {
    return 0xAA0003E0u | ((uint32_t)rn << 16) | (uint32_t)rd;
}

static uint32_t a64_cmp_w_imm(uint8_t rn, uint16_t imm12) {
    return 0x7100001Fu | (((uint32_t)imm12 & 0x0fffu) << 10) | ((uint32_t)rn << 5);
}

static uint32_t a64_subs_imm(uint8_t rd, uint8_t rn, uint16_t imm12) {
    return 0xF1000000u | (((uint32_t)imm12 & 0x0fffu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static uint32_t a64_subs_reg(uint8_t rd, uint8_t rn, uint8_t rm) {
    return 0xEB000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static uint32_t a64_add_shifted(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t lsl_imm) {
    return 0x8B000000u |
           (((uint32_t)lsl_imm & 0x3fu) << 10) |
           ((uint32_t)rm << 16) |
           ((uint32_t)rn << 5) |
           (uint32_t)rd;
}

static uint8_t a64_alloc_temp(uint16_t* used_mask) {
    // Reserve x30(LR)/x31(SP); use a scratch window in x16..x29.
    for (uint8_t i = 0; i < 14; i++) {
        uint16_t bit = (uint16_t)(1u << i);
        if ((*used_mask & bit) == 0) {
            *used_mask |= bit;
            return (uint8_t)(16 + i);
        }
    }
    return 0;
}

static void a64_free_temp(uint16_t* used_mask, uint8_t reg) {
    if (reg < 16 || reg > 29) return;
    uint8_t idx = (uint8_t)(reg - 16);
    *used_mask &= (uint16_t)~(1u << idx);
}

static int a64_emit(Arm64CodeBuf* buf, uint32_t word) {
    if (buf == NULL) return 0;
    if (buf->count >= buf->capacity) {
        int new_capacity = buf->capacity < 16 ? 16 : (buf->capacity * 2);
        uint32_t* grown = (uint32_t*)realloc(buf->words, sizeof(uint32_t) * (size_t)new_capacity);
        if (grown == NULL) return 0;
        buf->words = grown;
        buf->capacity = new_capacity;
    }
    buf->words[buf->count++] = word;
    return 1;
}

static int a64_emit_mov_imm64(Arm64CodeBuf* buf, uint8_t rd, uint64_t imm) {
    uint16_t part0 = (uint16_t)(imm & 0xffffu);
    uint16_t part1 = (uint16_t)((imm >> 16) & 0xffffu);
    uint16_t part2 = (uint16_t)((imm >> 32) & 0xffffu);
    uint16_t part3 = (uint16_t)((imm >> 48) & 0xffffu);
    if (!a64_emit(buf, a64_movz_x(rd, part0, 0))) return 0;
    if (part1 != 0 && !a64_emit(buf, a64_movk_x(rd, part1, 1))) return 0;
    if (part2 != 0 && !a64_emit(buf, a64_movk_x(rd, part2, 2))) return 0;
    if (part3 != 0 && !a64_emit(buf, a64_movk_x(rd, part3, 3))) return 0;
    return 1;
}

static ToiJitNativeExecResult jit_native_trace_bridge_runner(
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
#define NTRACE_SLOT_WRITE(slot_idx, slot_val)                                                                          \
    do {                                                                                                                \
        int __slot_idx = (int)(slot_idx);                                                                              \
        size_t __slot_byte = ((size_t)__slot_idx) >> 3;                                                                \
        uint8_t __slot_mask = (uint8_t)(1u << ((uint8_t)__slot_idx & 7u));                                            \
        if ((slot_saved_bits[__slot_byte] & __slot_mask) == 0) {                                                      \
            slot_saved_bits[__slot_byte] |= __slot_mask;                                                               \
            slot_backup[__slot_idx] = slots[__slot_idx];                                                               \
            dirty_slots[dirty_count++] = __slot_idx;                                                                   \
        }                                                                                                               \
        slots[__slot_idx] = (slot_val);                                                                                \
    } while (0)
#define NTRACE_ROLLBACK()                                                                                               \
    do {                                                                                                                \
        for (int __i = dirty_count - 1; __i >= 0; __i--) {                                                             \
            int __slot_idx = dirty_slots[__i];                                                                         \
            slots[__slot_idx] = slot_backup[__slot_idx];                                                               \
        }                                                                                                               \
    } while (0)
#define NTRACE_DEOPT(code)                                                                                              \
    do {                                                                                                                \
        NTRACE_ROLLBACK();                                                                                              \
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
    if (out_state != NULL) {
        out_state->resume_offset = (instr_count > 0) ? instrs[0].deopt_offset : 0xffffffffu;
        out_state->op = 0;
        out_state->reason = TOI_JIT_NATIVE_DEOPT_NONE;
        out_state->stack_count = 0;
    }
    uint32_t last_resume_offset = (instr_count > 0) ? instrs[0].deopt_offset : 0xffffffffu;
    uint8_t last_op = 0;
    if (vm == NULL || closure == NULL || slots == NULL || instrs == NULL) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_RUNTIME);
    if (instr_count <= 0) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_RUNTIME);
    if ((int)var_slot >= slot_count || (int)end_slot >= slot_count) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);

    Value stack[TOI_JIT_TRACE_STACK_CAP];
#if defined(__GNUC__) || defined(__clang__)
    void* fast_dispatch[TOI_JIT_NATIVE_OP_ADJUST_STACK + 1];
    for (int i = 0; i <= TOI_JIT_NATIVE_OP_ADJUST_STACK; i++) fast_dispatch[i] = &&L_FALLBACK;
    fast_dispatch[TOI_JIT_NATIVE_OP_PUSH_LOCAL] = &&L_PUSH_LOCAL;
    fast_dispatch[TOI_JIT_NATIVE_OP_PUSH_UPVALUE] = &&L_PUSH_UPVALUE;
    fast_dispatch[TOI_JIT_NATIVE_OP_PUSH_CONST] = &&L_PUSH_CONST;
    fast_dispatch[TOI_JIT_NATIVE_OP_PUSH_NIL] = &&L_PUSH_NIL;
    fast_dispatch[TOI_JIT_NATIVE_OP_PUSH_TRUE] = &&L_PUSH_TRUE;
    fast_dispatch[TOI_JIT_NATIVE_OP_PUSH_FALSE] = &&L_PUSH_FALSE;
    fast_dispatch[TOI_JIT_NATIVE_OP_STORE_LOCAL] = &&L_STORE_LOCAL;
    fast_dispatch[TOI_JIT_NATIVE_OP_POP] = &&L_POP;
    fast_dispatch[TOI_JIT_NATIVE_OP_BIN_ADD] = &&L_BIN_ARITH;
    fast_dispatch[TOI_JIT_NATIVE_OP_BIN_SUB] = &&L_BIN_ARITH;
    fast_dispatch[TOI_JIT_NATIVE_OP_BIN_MUL] = &&L_BIN_ARITH;
    fast_dispatch[TOI_JIT_NATIVE_OP_BIN_DIV] = &&L_BIN_ARITH;
    fast_dispatch[TOI_JIT_NATIVE_OP_BIN_MOD] = &&L_BIN_ARITH;
    fast_dispatch[TOI_JIT_NATIVE_OP_CMP_EQ] = &&L_CMP_EQ;
    fast_dispatch[TOI_JIT_NATIVE_OP_CMP_LT] = &&L_CMP_REL;
    fast_dispatch[TOI_JIT_NATIVE_OP_CMP_GT] = &&L_CMP_REL;
    fast_dispatch[TOI_JIT_NATIVE_OP_NOT] = &&L_NOT;
    fast_dispatch[TOI_JIT_NATIVE_OP_GET_TABLE] = &&L_GET_TABLE;
    fast_dispatch[TOI_JIT_NATIVE_OP_GET_TABLE_NUM] = &&L_GET_TABLE;
    fast_dispatch[TOI_JIT_NATIVE_OP_ADJUST_STACK] = &&L_ADJUST_STACK;
    fast_dispatch[TOI_JIT_NATIVE_OP_JUMP] = &&L_JUMP;
    fast_dispatch[TOI_JIT_NATIVE_OP_JUMP_IF_FALSE] = &&L_JUMP_IF;
    fast_dispatch[TOI_JIT_NATIVE_OP_JUMP_IF_TRUE] = &&L_JUMP_IF;
#endif
    for (uint32_t iter = 0; iter < max_iters; iter++) {
        Value var_v = slots[var_slot];
        Value end_v = slots[end_slot];
        if (!IS_NUMBER(var_v) || !IS_NUMBER(end_v)) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
        double next = AS_NUMBER(var_v) + 1.0;
        double endn = AS_NUMBER(end_v);
        NTRACE_SLOT_WRITE(var_slot, NUMBER_VAL(next));
        if (next > endn) return TOI_JIT_NATIVE_EXEC_EXIT;

        int sp = 0;
        int pc = 0;
        uint32_t guard = 0;
        int hit_backedge = 0;
        while (pc >= 0 && pc < instr_count) {
            if (++guard > 4096u) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_CONTROL_FLOW);
            const ToiJitNativeTraceInstr* in = &instrs[pc];
            last_resume_offset = in->deopt_offset;
            uint8_t op = in->op;
            last_op = op;
#if defined(__GNUC__) || defined(__clang__)
            if ((int)op > TOI_JIT_NATIVE_OP_ADJUST_STACK) goto L_FALLBACK;
            goto *fast_dispatch[op];
L_PUSH_LOCAL:
            if ((int)in->a >= slot_count || sp >= TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            stack[sp++] = slots[in->a];
            pc++;
            goto L_NTRACE_NEXT;
L_PUSH_UPVALUE:
            if ((int)in->a >= closure->upvalue_count || sp >= TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            if (closure->upvalues[in->a] == NULL || closure->upvalues[in->a]->location == NULL) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_RUNTIME);
            stack[sp++] = *closure->upvalues[in->a]->location;
            pc++;
            goto L_NTRACE_NEXT;
L_PUSH_CONST:
            if (sp >= TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            stack[sp++] = in->constant;
            pc++;
            goto L_NTRACE_NEXT;
L_PUSH_NIL:
            if (sp >= TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            stack[sp++] = NIL_VAL;
            pc++;
            goto L_NTRACE_NEXT;
L_PUSH_TRUE:
            if (sp >= TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            stack[sp++] = BOOL_VAL(1);
            pc++;
            goto L_NTRACE_NEXT;
L_PUSH_FALSE:
            if (sp >= TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            stack[sp++] = BOOL_VAL(0);
            pc++;
            goto L_NTRACE_NEXT;
L_STORE_LOCAL:
            if ((int)in->a >= slot_count || sp <= 0) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            NTRACE_SLOT_WRITE(in->a, stack[sp - 1]);
            pc++;
            goto L_NTRACE_NEXT;
L_POP:
            if (sp <= 0) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            sp--;
            pc++;
            goto L_NTRACE_NEXT;
L_BIN_ARITH: {
            if (sp < 2) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            Value b = stack[--sp];
            Value a = stack[--sp];
            if (!IS_NUMBER(a) || !IS_NUMBER(b)) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
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
            goto L_NTRACE_NEXT;
}
L_CMP_EQ: {
            if (sp < 2) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
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
            goto L_NTRACE_NEXT;
}
L_CMP_REL: {
            if (sp < 2) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            Value b = stack[--sp];
            Value a = stack[--sp];
            if (!IS_NUMBER(a) || !IS_NUMBER(b)) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
            int c = (op == TOI_JIT_NATIVE_OP_CMP_LT) ? (AS_NUMBER(a) < AS_NUMBER(b)) : (AS_NUMBER(a) > AS_NUMBER(b));
            stack[sp++] = BOOL_VAL(c);
            pc++;
            goto L_NTRACE_NEXT;
}
L_NOT:
            if (sp <= 0) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            stack[sp - 1] = BOOL_VAL(jit_value_falsey(stack[sp - 1]));
            pc++;
            goto L_NTRACE_NEXT;
L_GET_TABLE: {
#if TOI_JIT_NATIVE_TRACE_STRICT_CORE
            if (op != TOI_JIT_NATIVE_OP_GET_TABLE_NUM) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_UNSUPPORTED);
#endif
            if (sp < 2) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            Value table_v = stack[sp - 2];
            Value key = stack[sp - 1];
            if (!IS_TABLE(table_v)) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TABLE_GUARD);
            ObjTable* t = AS_TABLE(table_v);
            Value result = NIL_VAL;
            if (IS_NUMBER(key)) {
                double num_key = AS_NUMBER(key);
                int idx = (int)num_key;
                if (num_key != (double)idx) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TABLE_GUARD);
                if (idx < 0) idx = t->table.array_max + idx + 1;
                if (idx >= 1 && idx <= t->table.array_capacity) {
                    Value arr_val = t->table.array[idx - 1];
                    if (!IS_NIL(arr_val)) {
                        result = arr_val;
                    } else if (op == TOI_JIT_NATIVE_OP_GET_TABLE_NUM && t->no_metatable_fast) {
                        result = NIL_VAL;
                    } else {
                        NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TABLE_GUARD);
                    }
                } else if (op == TOI_JIT_NATIVE_OP_GET_TABLE_NUM && t->no_metatable_fast) {
                    result = NIL_VAL;
                } else {
                    NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TABLE_GUARD);
                }
            } else if (op == TOI_JIT_NATIVE_OP_GET_TABLE && IS_STRING(key) && t->no_metatable_fast) {
                if (table_get(&t->table, AS_STRING(key), &result)) {
                    // found
                } else {
                    result = NIL_VAL;
                }
            } else {
                NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TABLE_GUARD);
            }
            stack[sp - 2] = result;
            sp--;
            pc++;
            goto L_NTRACE_NEXT;
}
L_ADJUST_STACK:
            if ((int)in->a < 0 || (int)in->a > TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            sp = (int)in->a;
            pc++;
            goto L_NTRACE_NEXT;
L_JUMP: {
            int tgt = (int)in->target;
            if (tgt < 0 || tgt > instr_count) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_CONTROL_FLOW);
            if (tgt == instr_count) return TOI_JIT_NATIVE_EXEC_EXIT;
            if (tgt == 0) {
                hit_backedge = 1;
                pc = -1;
                goto L_NTRACE_NEXT;
            }
            pc = tgt;
            goto L_NTRACE_NEXT;
}
L_JUMP_IF: {
            if (sp <= 0) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
            int tgt = (int)in->target;
            if (tgt < 0 || tgt > instr_count) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_CONTROL_FLOW);
            int take = (op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE) ? jit_value_falsey(stack[sp - 1]) : !jit_value_falsey(stack[sp - 1]);
            if (!take) {
                pc++;
                goto L_NTRACE_NEXT;
            }
            if (tgt == instr_count) return TOI_JIT_NATIVE_EXEC_EXIT;
            if (tgt == 0) {
                hit_backedge = 1;
                pc = -1;
                goto L_NTRACE_NEXT;
            }
            pc = tgt;
            goto L_NTRACE_NEXT;
}
L_FALLBACK:
#endif
            switch (op) {
            case TOI_JIT_NATIVE_OP_PUSH_LOCAL:
                if ((int)in->a >= slot_count || sp >= TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                stack[sp++] = slots[in->a];
                pc++;
                break;
            case TOI_JIT_NATIVE_OP_PUSH_UPVALUE:
                if ((int)in->a >= closure->upvalue_count || sp >= TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                if (closure->upvalues[in->a] == NULL || closure->upvalues[in->a]->location == NULL) {
                    NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_RUNTIME);
                }
                stack[sp++] = *closure->upvalues[in->a]->location;
                pc++;
                break;
            case TOI_JIT_NATIVE_OP_PUSH_CONST:
                if (sp >= TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                stack[sp++] = in->constant;
                pc++;
                break;
            case TOI_JIT_NATIVE_OP_PUSH_NIL:
                if (sp >= TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                stack[sp++] = NIL_VAL;
                pc++;
                break;
            case TOI_JIT_NATIVE_OP_PUSH_TRUE:
                if (sp >= TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                stack[sp++] = BOOL_VAL(1);
                pc++;
                break;
            case TOI_JIT_NATIVE_OP_PUSH_FALSE:
                if (sp >= TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                stack[sp++] = BOOL_VAL(0);
                pc++;
                break;
            case TOI_JIT_NATIVE_OP_STORE_LOCAL:
                if ((int)in->a >= slot_count || sp <= 0) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                NTRACE_SLOT_WRITE(in->a, stack[sp - 1]);
                pc++;
                break;
            case TOI_JIT_NATIVE_OP_POP:
                if (sp <= 0) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                sp--;
                pc++;
                break;
            case TOI_JIT_NATIVE_OP_BIN_ADD:
            case TOI_JIT_NATIVE_OP_BIN_SUB:
            case TOI_JIT_NATIVE_OP_BIN_MUL:
            case TOI_JIT_NATIVE_OP_BIN_DIV:
            case TOI_JIT_NATIVE_OP_BIN_MOD: {
                if (sp < 2) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                Value b = stack[--sp];
                Value a = stack[--sp];
                if (!IS_NUMBER(a) || !IS_NUMBER(b)) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
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
                if (sp < 2) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
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
                if (sp < 2) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                Value b = stack[--sp];
                Value a = stack[--sp];
                if (!IS_NUMBER(a) || !IS_NUMBER(b)) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TYPE_GUARD);
                int c = (op == TOI_JIT_NATIVE_OP_CMP_LT) ? (AS_NUMBER(a) < AS_NUMBER(b)) : (AS_NUMBER(a) > AS_NUMBER(b));
                stack[sp++] = BOOL_VAL(c);
                pc++;
                break;
            }
            case TOI_JIT_NATIVE_OP_NOT:
                if (sp <= 0) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                stack[sp - 1] = BOOL_VAL(jit_value_falsey(stack[sp - 1]));
                pc++;
                break;
            case TOI_JIT_NATIVE_OP_APPEND: {
#if TOI_JIT_NATIVE_TRACE_STRICT_CORE
                NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_UNSUPPORTED);
#else
                if (sp < 2) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                Value result = NIL_VAL;
                if (!jit_value_append_fast(stack[sp - 2], stack[sp - 1], &result)) {
                    NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TABLE_GUARD);
                }
                stack[sp - 2] = result;
                sp--;
                pc++;
                break;
#endif
            }
            case TOI_JIT_NATIVE_OP_GET_TABLE:
            case TOI_JIT_NATIVE_OP_GET_TABLE_NUM: {
#if TOI_JIT_NATIVE_TRACE_STRICT_CORE
                if (op != TOI_JIT_NATIVE_OP_GET_TABLE_NUM) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_UNSUPPORTED);
#endif
                if (sp < 2) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                Value result = NIL_VAL;
                uint8_t src_op = (op == TOI_JIT_NATIVE_OP_GET_TABLE_NUM) ? TOI_JIT_VAL_GET_TABLE_NUM : TOI_JIT_VAL_GET_TABLE;
                if (!jit_value_get_table_num_fast(stack[sp - 2], stack[sp - 1], src_op, &result)) {
                    NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TABLE_GUARD);
                }
                stack[sp - 2] = result;
                sp--;
                pc++;
                break;
            }
            case TOI_JIT_NATIVE_OP_SET_TABLE:
            case TOI_JIT_NATIVE_OP_SET_TABLE_NUM: {
#if TOI_JIT_NATIVE_TRACE_STRICT_CORE
                NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_UNSUPPORTED);
#else
                if (sp < 3) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                Value result = NIL_VAL;
                uint8_t src_op = (op == TOI_JIT_NATIVE_OP_SET_TABLE_NUM) ? TOI_JIT_VAL_SET_TABLE_NUM : TOI_JIT_VAL_SET_TABLE;
                if (!jit_value_set_table_fast(stack[sp - 3], stack[sp - 2], stack[sp - 1], src_op, &result)) {
                    NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_TABLE_GUARD);
                }
                stack[sp - 3] = result;
                sp -= 2;
                pc++;
                break;
#endif
            }
            case TOI_JIT_NATIVE_OP_CALL: {
#if TOI_JIT_NATIVE_TRACE_STRICT_CORE
                NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_UNSUPPORTED);
#else
                int argc = (int)in->a;
                if (argc < 0 || sp < argc + 1) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                int callee_idx = sp - 1 - argc;
                Value out = NIL_VAL;
                if (!jit_value_call_primitive_arm(vm, stack[callee_idx], &stack[callee_idx + 1], argc, &out)) {
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
#endif
            }
            case TOI_JIT_NATIVE_OP_INVOKE: {
#if TOI_JIT_NATIVE_TRACE_STRICT_CORE
                NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_UNSUPPORTED);
#else
                int argc = (int)in->a;
                if (argc < 0 || sp < argc + 1) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                int recv_idx = sp - 1 - argc;
                Value out = NIL_VAL;
                if (!jit_invoke_primitive_arm(vm, stack[recv_idx], in->constant, &stack[recv_idx + 1], argc, &out)) {
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
#endif
            }
            case TOI_JIT_NATIVE_OP_BUILD_STRING2:
            case TOI_JIT_NATIVE_OP_BUILD_STRING3:
            case TOI_JIT_NATIVE_OP_BUILD_STRING4:
            case TOI_JIT_NATIVE_OP_BUILD_STRING5: {
#if TOI_JIT_NATIVE_TRACE_STRICT_CORE
                NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_UNSUPPORTED);
#else
                int n = (op == TOI_JIT_NATIVE_OP_BUILD_STRING2) ? 2 :
                        (op == TOI_JIT_NATIVE_OP_BUILD_STRING3) ? 3 :
                        (op == TOI_JIT_NATIVE_OP_BUILD_STRING4) ? 4 : 5;
                if (sp < n) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                Value out = NIL_VAL;
                if (!jit_build_string_primitive_arm(vm, &stack[sp - n], n, &out)) {
                    NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_RUNTIME);
                }
                stack[sp - n] = out;
                sp = sp - n + 1;
                pc++;
                break;
#endif
            }
            case TOI_JIT_NATIVE_OP_ADJUST_STACK:
                if ((int)in->a < 0 || (int)in->a > TOI_JIT_TRACE_STACK_CAP) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                sp = (int)in->a;
                pc++;
                break;
            case TOI_JIT_NATIVE_OP_JUMP: {
                int tgt = (int)in->target;
                if (tgt < 0 || tgt > instr_count) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_CONTROL_FLOW);
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
                if (sp <= 0) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS);
                int tgt = (int)in->target;
                if (tgt < 0 || tgt > instr_count) NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_CONTROL_FLOW);
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
            default:
                NTRACE_DEOPT(TOI_JIT_NATIVE_DEOPT_UNSUPPORTED);
            }
#if defined(__GNUC__) || defined(__clang__)
L_NTRACE_NEXT:
#endif
            if (hit_backedge) break;
        }
        if (!hit_backedge) {
            if (out_state != NULL) {
                if (instr_count > 0) out_state->resume_offset = instrs[0].deopt_offset;
                out_state->stack_count = 0;
            }
            return TOI_JIT_NATIVE_EXEC_CONTINUE;
        }
        if (iter + 1u >= max_iters) {
            if (out_state != NULL) {
                if (instr_count > 0) out_state->resume_offset = instrs[0].deopt_offset;
                out_state->stack_count = 0;
            }
            return TOI_JIT_NATIVE_EXEC_CONTINUE;
        }
    }
    return TOI_JIT_NATIVE_EXEC_CONTINUE;
#undef NTRACE_DEOPT
#undef NTRACE_ROLLBACK
#undef NTRACE_SLOT_WRITE
}

static int a64_patch_b_cond(Arm64CodeBuf* buf, int branch_index, int target_index, uint32_t cond) {
    if (buf == NULL || branch_index < 0 || branch_index >= buf->count) return 0;
    int32_t off = (int32_t)(target_index - branch_index);
    if (off < -(1 << 18) || off > ((1 << 18) - 1)) return 0;
    uint32_t imm19 = (uint32_t)(off & 0x7ffff);
    buf->words[branch_index] = 0x54000000u | (imm19 << 5) | (cond & 0xfu);
    return 1;
}

static int a64_patch_b(Arm64CodeBuf* buf, int branch_index, int target_index) {
    if (buf == NULL || branch_index < 0 || branch_index >= buf->count) return 0;
    int32_t off = (int32_t)(target_index - branch_index);
    if (off < -(1 << 25) || off > ((1 << 25) - 1)) return 0;
    uint32_t imm26 = (uint32_t)(off & 0x03ffffffu);
    buf->words[branch_index] = 0x14000000u | imm26;
    return 1;
}

static int native_trace_has_local_number_guard(
    uint8_t slot,
    const ToiJitNativeGuard* guards,
    int guard_count) {
    if (guards == NULL || guard_count <= 0) return 0;
    for (int i = 0; i < guard_count; i++) {
        if (guards[i].kind == TOI_JIT_NATIVE_GUARD_LOCAL_IS_NUMBER &&
            guards[i].slot_a == slot) {
            return 1;
        }
    }
    return 0;
}

static int native_trace_has_upvalue_number_guard(
    uint8_t slot,
    const ToiJitNativeGuard* guards,
    int guard_count) {
    if (guards == NULL || guard_count <= 0) return 0;
    for (int i = 0; i < guard_count; i++) {
        if (guards[i].kind == TOI_JIT_NATIVE_GUARD_UPVALUE_IS_NUMBER &&
            guards[i].slot_a == slot) {
            return 1;
        }
    }
    return 0;
}

static int native_trace_has_local_string_guard(
    uint8_t slot,
    const ToiJitNativeGuard* guards,
    int guard_count) {
    if (guards == NULL || guard_count <= 0) return 0;
    for (int i = 0; i < guard_count; i++) {
        if (guards[i].kind == TOI_JIT_NATIVE_GUARD_LOCAL_IS_STRING &&
            guards[i].slot_a == slot) {
            return 1;
        }
    }
    return 0;
}

static int native_trace_has_upvalue_string_guard(
    uint8_t slot,
    const ToiJitNativeGuard* guards,
    int guard_count) {
    if (guards == NULL || guard_count <= 0) return 0;
    for (int i = 0; i < guard_count; i++) {
        if (guards[i].kind == TOI_JIT_NATIVE_GUARD_UPVALUE_IS_STRING &&
            guards[i].slot_a == slot) {
            return 1;
        }
    }
    return 0;
}

static int native_trace_has_table_shape_guard(
    uint8_t slot,
    uint8_t require_no_metatable_fast,
    const ToiJitNativeGuard* guards,
    int guard_count) {
    if (guards == NULL || guard_count <= 0) return 0;
    for (int i = 0; i < guard_count; i++) {
        if (guards[i].kind == TOI_JIT_NATIVE_GUARD_TABLE_SHAPE &&
            guards[i].slot_b == 0 &&
            guards[i].slot_a == slot &&
            ((guards[i].aux & 1u) == (require_no_metatable_fast ? 1u : 0u))) {
            return 1;
        }
    }
    return 0;
}

static int native_trace_has_upvalue_table_shape_guard(
    uint8_t slot,
    uint8_t require_no_metatable_fast,
    const ToiJitNativeGuard* guards,
    int guard_count) {
    if (guards == NULL || guard_count <= 0) return 0;
    for (int i = 0; i < guard_count; i++) {
        if (guards[i].kind == TOI_JIT_NATIVE_GUARD_TABLE_SHAPE &&
            guards[i].slot_b == 1 &&
            guards[i].slot_a == slot &&
            ((guards[i].aux & 1u) == (require_no_metatable_fast ? 1u : 0u))) {
            return 1;
        }
    }
    return 0;
}

static int jit_native_get_table_num_slot_as_number(
    Value* slots,
    int slot_count,
    int table_slot,
    double key,
    double* out_number) {
    if (slots == NULL || out_number == NULL) return 0;
    if (table_slot < 0 || table_slot >= slot_count) return 0;
    Value table_v = slots[table_slot];
    if (!IS_TABLE(table_v)) return 0;
    ObjTable* t = AS_TABLE(table_v);
    if (t == NULL || !t->no_metatable_fast) return 0;

    int idx = (int)key;
    if (key != (double)idx) return 0;
    if (idx < 0) idx = t->table.array_max + idx + 1;

    Value result = NIL_VAL;
    if (idx >= 1 && idx <= t->table.array_capacity) {
        Value arr_val = t->table.array[idx - 1];
        if (!IS_NIL(arr_val)) {
            result = arr_val;
        } else if (!table_get_array(&t->table, idx, &result)) {
            return 0;
        }
    } else {
        if (!table_get_array(&t->table, idx, &result)) return 0;
    }

    if (!IS_NUMBER(result)) return 0;
    *out_number = AS_NUMBER(result);
    return 1;
}

static int jit_native_get_table_num_slot_as_value(
    Value* slots,
    int slot_count,
    int table_slot,
    double key,
    Value* out_value) {
    if (slots == NULL || out_value == NULL) return 0;
    if (table_slot < 0 || table_slot >= slot_count) return 0;
    Value table_v = slots[table_slot];
    if (!IS_TABLE(table_v)) return 0;
    ObjTable* t = AS_TABLE(table_v);
    if (t == NULL || !t->no_metatable_fast) return 0;

    int idx = (int)key;
    if (key != (double)idx) return 0;
    if (idx < 0) idx = t->table.array_max + idx + 1;

    Value result = NIL_VAL;
    if (idx >= 1 && idx <= t->table.array_capacity) {
        Value arr_val = t->table.array[idx - 1];
        if (!IS_NIL(arr_val)) {
            result = arr_val;
        } else if (!table_get_array(&t->table, idx, &result)) {
            result = NIL_VAL;
        }
    } else {
        if (!table_get_array(&t->table, idx, &result)) result = NIL_VAL;
    }
    *out_value = result;
    return 1;
}

static int jit_native_get_table_str_slot_as_number(
    Value* slots,
    int slot_count,
    int table_slot,
    ObjString* key,
    double* out_number) {
    if (slots == NULL || out_number == NULL || key == NULL) return 0;
    if (table_slot < 0 || table_slot >= slot_count) return 0;
    Value table_v = slots[table_slot];
    if (!IS_TABLE(table_v)) return 0;
    ObjTable* t = AS_TABLE(table_v);
    if (t == NULL || !t->no_metatable_fast) return 0;

    Value result = NIL_VAL;
    if (!table_get(&t->table, key, &result)) return 0;
    if (!IS_NUMBER(result)) return 0;
    *out_number = AS_NUMBER(result);
    return 1;
}

static int jit_native_get_table_str_slot_as_value(
    Value* slots,
    int slot_count,
    int table_slot,
    ObjString* key,
    Value* out_value) {
    if (slots == NULL || out_value == NULL || key == NULL) return 0;
    if (table_slot < 0 || table_slot >= slot_count) return 0;
    Value table_v = slots[table_slot];
    if (!IS_TABLE(table_v)) return 0;
    ObjTable* t = AS_TABLE(table_v);
    if (t == NULL || !t->no_metatable_fast) return 0;

    Value result = NIL_VAL;
    if (!table_get(&t->table, key, &result)) result = NIL_VAL;
    *out_value = result;
    return 1;
}

static int jit_objstring_equals_arm(ObjString* a, ObjString* b) {
    if (a == b) return 1;
    if (a == NULL || b == NULL) return 0;
    if (a->length != b->length) return 0;
    if (objstring_hash(a) != objstring_hash(b)) return 0;
    return memcmp(a->chars, b->chars, (size_t)a->length) == 0;
}

static int jit_native_get_table_ic_try_lane_arm(
    ObjTable* t,
    ObjString* key_str,
    uint32_t shape_version,
    ObjTable** tables,
    ObjString** keys,
    int* slots,
    uint32_t* versions,
    int site,
    Value* out_value) {
    if (tables == NULL || keys == NULL || slots == NULL || versions == NULL) return 0;
    if (keys[site] == NULL || !jit_objstring_equals_arm(keys[site], key_str)) return 0;

    int slot = slots[site];
    if (slot == -2) {
        if (tables[site] != t || versions[site] != shape_version) return 0;
        *out_value = NIL_VAL;
        return 1;
    }
    if (slot < 0 || slot >= t->table.capacity) return 0;
    Entry* entry = &t->table.entries[slot];
    if (!jit_objstring_equals_arm(entry->key, key_str)) return 0;
    *out_value = entry->value;
    return 1;
}

static int jit_native_get_table_ic_try_all_lanes_arm(
    Chunk* chunk,
    int site,
    ObjTable* t,
    ObjString* key,
    Value* out_value) {
    if (chunk == NULL || site < 0 || site >= chunk->capacity) return 0;
    uint32_t shape = t->table.shape_version;
    if (jit_native_get_table_ic_try_lane_arm(
            t, key, shape,
            chunk->get_table_ic_tables,
            chunk->get_table_ic_keys,
            chunk->get_table_ic_slots,
            chunk->get_table_ic_versions,
            site,
            out_value)) {
        return 1;
    }
    if (jit_native_get_table_ic_try_lane_arm(
            t, key, shape,
            chunk->get_table_ic2_tables,
            chunk->get_table_ic2_keys,
            chunk->get_table_ic2_slots,
            chunk->get_table_ic2_versions,
            site,
            out_value)) {
        return 1;
    }
    if (jit_native_get_table_ic_try_lane_arm(
            t, key, shape,
            chunk->get_table_ic3_tables,
            chunk->get_table_ic3_keys,
            chunk->get_table_ic3_slots,
            chunk->get_table_ic3_versions,
            site,
            out_value)) {
        return 1;
    }
    if (jit_native_get_table_ic_try_lane_arm(
            t, key, shape,
            chunk->get_table_ic4_tables,
            chunk->get_table_ic4_keys,
            chunk->get_table_ic4_slots,
            chunk->get_table_ic4_versions,
            site,
            out_value)) {
        return 1;
    }
    return 0;
}

static void jit_native_get_table_ic_write_lane_arm(
    ObjTable** tables,
    ObjString** keys,
    int* slots,
    uint32_t* versions,
    int site,
    ObjTable* table,
    ObjString* key,
    int slot,
    uint32_t version) {
    if (tables == NULL || keys == NULL || slots == NULL || versions == NULL) return;
    tables[site] = table;
    keys[site] = key;
    slots[site] = slot;
    versions[site] = version;
}

static void jit_native_update_get_table_ic_arm(
    Chunk* chunk,
    int site,
    ObjTable* table,
    ObjString* key,
    int slot) {
    if (chunk == NULL || site < 0 || site >= chunk->capacity) return;
    if (chunk->get_table_ic_tables == NULL || chunk->get_table_ic_keys == NULL ||
        chunk->get_table_ic_slots == NULL || chunk->get_table_ic_versions == NULL) {
        return;
    }

    uint32_t version = table->table.shape_version;
    ObjTable** tables_arr[4] = {
        chunk->get_table_ic_tables, chunk->get_table_ic2_tables,
        chunk->get_table_ic3_tables, chunk->get_table_ic4_tables
    };
    ObjString** keys_arr[4] = {
        chunk->get_table_ic_keys, chunk->get_table_ic2_keys,
        chunk->get_table_ic3_keys, chunk->get_table_ic4_keys
    };
    int* slots_arr[4] = {
        chunk->get_table_ic_slots, chunk->get_table_ic2_slots,
        chunk->get_table_ic3_slots, chunk->get_table_ic4_slots
    };
    uint32_t* versions_arr[4] = {
        chunk->get_table_ic_versions, chunk->get_table_ic2_versions,
        chunk->get_table_ic3_versions, chunk->get_table_ic4_versions
    };
    for (int lane = 0; lane < 4; lane++) {
        if (tables_arr[lane] == NULL || keys_arr[lane] == NULL ||
            slots_arr[lane] == NULL || versions_arr[lane] == NULL) {
            return;
        }
    }

    int target_lane = -1;
    for (int lane = 0; lane < 4; lane++) {
        if (keys_arr[lane] == NULL) continue;
        ObjString* lane_key = keys_arr[lane][site];
        if (lane_key != NULL && jit_objstring_equals_arm(lane_key, key)) {
            target_lane = lane;
            break;
        }
    }
    if (target_lane < 0) {
        for (int lane = 0; lane < 4; lane++) {
            if (keys_arr[lane] == NULL) continue;
            if (keys_arr[lane][site] == NULL) {
                target_lane = lane;
                break;
            }
        }
    }
    if (target_lane < 0) {
        for (int lane = 3; lane >= 1; lane--) {
            jit_native_get_table_ic_write_lane_arm(
                tables_arr[lane],
                keys_arr[lane],
                slots_arr[lane],
                versions_arr[lane],
                site,
                tables_arr[lane - 1][site],
                keys_arr[lane - 1][site],
                slots_arr[lane - 1][site],
                versions_arr[lane - 1][site]);
        }
        target_lane = 0;
    }

    jit_native_get_table_ic_write_lane_arm(
        tables_arr[target_lane],
        keys_arr[target_lane],
        slots_arr[target_lane],
        versions_arr[target_lane],
        site,
        table,
        key,
        slot,
        version);
}

static int jit_native_build3_get_table_slot_ic(
    VM* vm,
    ObjClosure* closure,
    Value* slots,
    int slot_count,
    int table_slot,
    int site,
    Value* parts3,
    Value* out_pair) {
    if (vm == NULL || closure == NULL || slots == NULL || parts3 == NULL || out_pair == NULL) return 0;
    if (table_slot < 0 || table_slot >= slot_count) return 0;

    Value key_v = NIL_VAL;
    ObjString* key = NULL;
    if (IS_STRING(parts3[0]) && IS_STRING(parts3[1]) && IS_STRING(parts3[2])) {
        ObjString* s0 = AS_STRING(parts3[0]);
        ObjString* s1 = AS_STRING(parts3[1]);
        ObjString* s2 = AS_STRING(parts3[2]);
        uint32_t slot = (uint32_t)((((uintptr_t)s0 >> 4) ^
                                    ((uintptr_t)s1 >> 9) ^
                                    ((uintptr_t)s2 >> 14)) & 4095u);
        BuildString3CacheEntry* e = &vm->build_string3_cache[slot];
        if (e->a == s0 && e->b == s1 && e->c == s2 && e->out != NULL) {
            key = e->out;
        } else {
            int total_len = s0->length + s1->length + s2->length;
            if (total_len <= 255) {
                char buffer[256];
                char* dst = buffer;
                memcpy(dst, s0->chars, (size_t)s0->length);
                dst += s0->length;
                memcpy(dst, s1->chars, (size_t)s1->length);
                dst += s1->length;
                memcpy(dst, s2->chars, (size_t)s2->length);
                dst += s2->length;
                key = copy_string(buffer, total_len);
            } else {
                char* buffer = (char*)malloc((size_t)total_len + 1u);
                if (buffer == NULL) return 0;
                char* dst = buffer;
                memcpy(dst, s0->chars, (size_t)s0->length);
                dst += s0->length;
                memcpy(dst, s1->chars, (size_t)s1->length);
                dst += s1->length;
                memcpy(dst, s2->chars, (size_t)s2->length);
                dst += s2->length;
                key = take_string(buffer, total_len);
            }
            if (key == NULL) return 0;
            e->a = s0;
            e->b = s1;
            e->c = s2;
            e->out = key;
        }
        key_v = OBJ_VAL(key);
    } else {
        if (!jit_build_string_primitive_arm(vm, parts3, 3, &key_v)) return 0;
        if (!IS_STRING(key_v)) return 0;
        key = AS_STRING(key_v);
    }

    Value table_v = slots[table_slot];
    if (!IS_TABLE(table_v)) return 0;
    ObjTable* t = AS_TABLE(table_v);
    if (t == NULL || !t->no_metatable_fast) return 0;

    ObjFunction* fn = closure->function;
    if (fn == NULL) return 0;
    Chunk* chunk = &fn->chunk;

    Value result = NIL_VAL;
    if (site >= 0 && site < chunk->capacity &&
        chunk->get_table_ic_tables != NULL && chunk->get_table_ic_keys != NULL &&
        chunk->get_table_ic_slots != NULL && chunk->get_table_ic_versions != NULL &&
        chunk->get_table_ic2_tables != NULL && chunk->get_table_ic2_keys != NULL &&
        chunk->get_table_ic2_slots != NULL && chunk->get_table_ic2_versions != NULL &&
        chunk->get_table_ic3_tables != NULL && chunk->get_table_ic3_keys != NULL &&
        chunk->get_table_ic3_slots != NULL && chunk->get_table_ic3_versions != NULL &&
        chunk->get_table_ic4_tables != NULL && chunk->get_table_ic4_keys != NULL &&
        chunk->get_table_ic4_slots != NULL && chunk->get_table_ic4_versions != NULL) {
        if (!jit_native_get_table_ic_try_all_lanes_arm(chunk, site, t, key, &result)) {
            int slot = -1;
            if (table_get_with_slot(&t->table, key, &result, &slot)) {
                jit_native_update_get_table_ic_arm(chunk, site, t, key, slot);
            } else {
                result = NIL_VAL;
                jit_native_update_get_table_ic_arm(chunk, site, t, key, -2);
            }
        }
    } else {
        if (!table_get(&t->table, key, &result)) result = NIL_VAL;
    }

    out_pair[0] = key_v;
    out_pair[1] = result;
    return 1;
}

static int jit_native_set_table_num_slot_from_value(
    Value* slots,
    int slot_count,
    int table_slot,
    double key,
    const Value* value_ptr) {
    if (slots == NULL || value_ptr == NULL) return 0;
    if (table_slot < 0 || table_slot >= slot_count) return 0;
    Value table_v = slots[table_slot];
    if (!IS_TABLE(table_v)) return 0;
    ObjTable* t = AS_TABLE(table_v);
    if (t == NULL || !t->no_metatable_fast) return 0;

    Value value = *value_ptr;
    int idx = (int)key;
    int is_array = 0;
    if (key == (double)idx) {
        if (idx < 0) idx = t->table.array_max + idx + 1;
        if (table_set_array(&t->table, idx, value)) is_array = 1;
    }
    if (!is_array) {
        ObjString* n_key = number_key_string(key);
        table_set(&t->table, n_key, value);
    }
    return 1;
}

static int jit_native_set_table_str_slot_from_value(
    Value* slots,
    int slot_count,
    int table_slot,
    ObjString* key,
    const Value* value_ptr) {
    if (slots == NULL || key == NULL || value_ptr == NULL) return 0;
    if (table_slot < 0 || table_slot >= slot_count) return 0;
    Value table_v = slots[table_slot];
    if (!IS_TABLE(table_v)) return 0;
    ObjTable* t = AS_TABLE(table_v);
    if (t == NULL || !t->no_metatable_fast) return 0;
    table_set(&t->table, key, *value_ptr);
    return 1;
}

static int jit_native_set_table_strkey_slot_from_value(
    Value* slots,
    int slot_count,
    int table_slot,
    int key_slot,
    const Value* value_ptr) {
    if (slots == NULL || value_ptr == NULL) return 0;
    if (table_slot < 0 || table_slot >= slot_count) return 0;
    if (key_slot < 0 || key_slot >= slot_count) return 0;
    Value table_v = slots[table_slot];
    Value key_v = slots[key_slot];
    if (!IS_TABLE(table_v) || !IS_STRING(key_v)) return 0;
    ObjTable* t = AS_TABLE(table_v);
    if (t == NULL || !t->no_metatable_fast) return 0;
    table_set(&t->table, AS_STRING(key_v), *value_ptr);
    return 1;
}

static int jit_native_value_falsey_slot(
    Value* slots,
    int slot_count,
    int slot) {
    if (slots == NULL) return 0;
    if (slot < 0 || slot >= slot_count) return 0;
    return jit_value_falsey(slots[slot]) ? 1 : 0;
}

static int jit_native_get_upvalue_slot_as_value(
    ObjClosure* closure,
    int upvalue_slot,
    Value* out_value);

static int jit_native_get_upvalue_slot_as_number(
    ObjClosure* closure,
    int upvalue_slot,
    double* out_number);

static int jit_native_value_falsey_upvalue(
    ObjClosure* closure,
    int upvalue_slot) {
    Value v = NIL_VAL;
    if (!jit_native_get_upvalue_slot_as_value(closure, upvalue_slot, &v)) return 1;
    return jit_value_falsey(v) ? 1 : 0;
}

static int jit_native_value_falsey_value_ptr(const Value* value_ptr) {
    if (value_ptr == NULL) return 1;
    return jit_value_falsey(*value_ptr) ? 1 : 0;
}

static int jit_native_get_number_slot(
    Value* slots,
    int slot_count,
    int slot,
    double* out_number) {
    if (slots == NULL || out_number == NULL) return 0;
    if (slot < 0 || slot >= slot_count) return 0;
    Value v = slots[slot];
    if (!IS_NUMBER(v)) return 0;
    *out_number = AS_NUMBER(v);
    return 1;
}

static int jit_native_get_number_upvalue(
    ObjClosure* closure,
    int upvalue_slot,
    double* out_number) {
    return jit_native_get_upvalue_slot_as_number(closure, upvalue_slot, out_number);
}

static int jit_native_get_number_value_ptr(
    const Value* value_ptr,
    double* out_number) {
    if (value_ptr == NULL || out_number == NULL) return 0;
    if (!IS_NUMBER(*value_ptr)) return 0;
    *out_number = AS_NUMBER(*value_ptr);
    return 1;
}

static int jit_native_values_equal_ptr(const Value* a, const Value* b) {
    if (a == NULL || b == NULL) return 0;
    if (a->type != b->type) return 0;
    switch (a->type) {
        case VAL_NIL:
            return 1;
        case VAL_BOOL:
            return a->as.boolean == b->as.boolean;
        case VAL_NUMBER:
            return a->as.number == b->as.number;
        case VAL_OBJ:
            return a->as.obj == b->as.obj;
        default:
            return 0;
    }
}

static int jit_native_cmp_eq_slot_const_number(
    Value* slots,
    int slot_count,
    int slot,
    double constant) {
    if (slots == NULL) return 0;
    if (slot < 0 || slot >= slot_count) return 0;
    Value v = slots[slot];
    if (!IS_NUMBER(v)) return 0;
    return AS_NUMBER(v) == constant ? 1 : 0;
}

static int jit_native_cmp_eq_slot_const_string(
    Value* slots,
    int slot_count,
    int slot,
    ObjString* constant) {
    if (slots == NULL || constant == NULL) return 0;
    if (slot < 0 || slot >= slot_count) return 0;
    Value v = slots[slot];
    if (!IS_STRING(v)) return 0;
    return AS_STRING(v) == constant ? 1 : 0;
}

static int jit_native_get_upvalue_slot_as_number(
    ObjClosure* closure,
    int upvalue_slot,
    double* out_number) {
    if (closure == NULL || out_number == NULL) return 0;
    if (upvalue_slot < 0 || upvalue_slot >= closure->upvalue_count) return 0;
    ObjUpvalue* up = closure->upvalues[upvalue_slot];
    if (up == NULL || up->location == NULL) return 0;
    Value v = *up->location;
    if (!IS_NUMBER(v)) return 0;
    *out_number = AS_NUMBER(v);
    return 1;
}

static int jit_native_get_upvalue_slot_as_value(
    ObjClosure* closure,
    int upvalue_slot,
    Value* out_value) {
    if (closure == NULL || out_value == NULL) return 0;
    if (upvalue_slot < 0 || upvalue_slot >= closure->upvalue_count) return 0;
    ObjUpvalue* up = closure->upvalues[upvalue_slot];
    if (up == NULL || up->location == NULL) return 0;
    *out_value = *up->location;
    return 1;
}

static int jit_native_get_table_num_upvalue_as_number(
    ObjClosure* closure,
    int upvalue_slot,
    double key,
    double* out_number) {
    if (out_number == NULL) return 0;
    Value table_v = NIL_VAL;
    if (!jit_native_get_upvalue_slot_as_value(closure, upvalue_slot, &table_v)) return 0;
    Value out = NIL_VAL;
    if (!jit_value_get_table_num_fast(table_v, NUMBER_VAL(key), TOI_JIT_VAL_GET_TABLE_NUM, &out)) return 0;
    if (!IS_NUMBER(out)) return 0;
    *out_number = AS_NUMBER(out);
    return 1;
}

static int jit_native_get_table_num_upvalue_as_value(
    ObjClosure* closure,
    int upvalue_slot,
    double key,
    Value* out_value) {
    if (out_value == NULL) return 0;
    Value table_v = NIL_VAL;
    if (!jit_native_get_upvalue_slot_as_value(closure, upvalue_slot, &table_v)) return 0;
    Value out = NIL_VAL;
    if (!jit_value_get_table_num_fast(table_v, NUMBER_VAL(key), TOI_JIT_VAL_GET_TABLE_NUM, &out)) return 0;
    *out_value = out;
    return 1;
}

static int jit_native_get_table_str_upvalue_as_number(
    ObjClosure* closure,
    int upvalue_slot,
    ObjString* key,
    double* out_number) {
    if (key == NULL || out_number == NULL) return 0;
    Value table_v = NIL_VAL;
    if (!jit_native_get_upvalue_slot_as_value(closure, upvalue_slot, &table_v)) return 0;
    Value out = NIL_VAL;
    if (!jit_value_get_table_num_fast(table_v, OBJ_VAL(key), TOI_JIT_VAL_GET_TABLE, &out)) return 0;
    if (!IS_NUMBER(out)) return 0;
    *out_number = AS_NUMBER(out);
    return 1;
}

static int jit_native_get_table_str_upvalue_as_value(
    ObjClosure* closure,
    int upvalue_slot,
    ObjString* key,
    Value* out_value) {
    if (key == NULL || out_value == NULL) return 0;
    Value table_v = NIL_VAL;
    if (!jit_native_get_upvalue_slot_as_value(closure, upvalue_slot, &table_v)) return 0;
    Value out = NIL_VAL;
    if (!jit_value_get_table_num_fast(table_v, OBJ_VAL(key), TOI_JIT_VAL_GET_TABLE, &out)) return 0;
    *out_value = out;
    return 1;
}


static int jit_native_set_table_num_upvalue_from_value(
    ObjClosure* closure,
    int upvalue_slot,
    double key,
    const Value* value_ptr) {
    if (value_ptr == NULL) return 0;
    Value table_v = NIL_VAL;
    if (!jit_native_get_upvalue_slot_as_value(closure, upvalue_slot, &table_v)) return 0;
    Value out = NIL_VAL;
    return jit_value_set_table_fast(
        table_v,
        NUMBER_VAL(key),
        *value_ptr,
        TOI_JIT_VAL_SET_TABLE_NUM,
        &out);
}

static int jit_native_set_table_str_upvalue_from_value(
    ObjClosure* closure,
    int upvalue_slot,
    ObjString* key,
    const Value* value_ptr) {
    if (key == NULL || value_ptr == NULL) return 0;
    Value table_v = NIL_VAL;
    if (!jit_native_get_upvalue_slot_as_value(closure, upvalue_slot, &table_v)) return 0;
    Value out = NIL_VAL;
    return jit_value_set_table_fast(
        table_v,
        OBJ_VAL(key),
        *value_ptr,
        TOI_JIT_VAL_SET_TABLE,
        &out);
}

static int jit_native_build_string_from_parts(
    VM* vm,
    Value* parts,
    int count,
    ObjString** out_string) {
    if (vm == NULL || parts == NULL || out_string == NULL) return 0;
    Value out = NIL_VAL;
    if (!jit_build_string_primitive_arm(vm, parts, count, &out)) return 0;
    if (!IS_STRING(out)) return 0;
    *out_string = AS_STRING(out);
    return 1;
}

static int jit_native_append_from_values(
    const Value* table_value,
    const Value* append_value,
    Value* out_value) {
    if (table_value == NULL || append_value == NULL || out_value == NULL) return 0;
    return jit_value_append_fast(*table_value, *append_value, out_value);
}

typedef struct {
    int patch_insn;
    uint16_t target_op;
} JitNativeLowerPatch;

#define JIT_NATIVE_TARGET_EXIT 0xFFFFu

static int jit_native_emit_instr(
    ToiJitNativeTraceInstr* out_instrs,
    int* io_count,
    uint8_t op,
    uint8_t a,
    uint8_t b,
    uint16_t target,
    uint32_t deopt_offset,
    uint64_t aux_data,
    Value constant) {
    if (out_instrs == NULL || io_count == NULL) return 0;
    int idx = *io_count;
    if (idx < 0 || idx >= 256) return 0;
    ToiJitNativeTraceInstr* in = &out_instrs[idx];
    in->op = op;
    in->a = a;
    in->b = b;
    in->target = target;
    in->deopt_offset = deopt_offset;
    in->aux_data = aux_data;
    in->constant = constant;
    *io_count = idx + 1;
    return 1;
}

static int jit_native_lower_chain_safe_program(
    const JitProgram* program,
    int argc,
    ToiJitNativeTraceInstr* out_instrs,
    int* out_instr_count,
    uint8_t* out_ret_slot,
    uint8_t* out_var_slot,
    uint8_t* out_end_slot,
    uint8_t* out_slot_count) {
    if (program == NULL || program->ops == NULL || program->op_count <= 0 ||
        out_instrs == NULL || out_instr_count == NULL ||
        out_ret_slot == NULL || out_var_slot == NULL ||
        out_end_slot == NULL || out_slot_count == NULL) {
        return 0;
    }
    if (argc < 0) return 0;
    if (program->max_slot < argc - 1) return 0;
    if (program->max_slot > 252) return 0;
    // Keep lowering bounded by instruction buffer capacity; do not impose a tiny
    // program-length gate here because it blocks real hot handlers.
    if (program->op_count > 1024) return 0;
    uint8_t ret_slot = (uint8_t)(program->max_slot + 1);
    uint8_t var_slot = (uint8_t)(program->max_slot + 2);
    uint8_t end_slot = (uint8_t)(program->max_slot + 3);
    uint8_t slot_count = (uint8_t)(program->max_slot + 4);

    int op_to_insn[1024];
    for (int i = 0; i < program->op_count; i++) op_to_insn[i] = -1;

    JitNativeLowerPatch patches[256];
    int patch_count = 0;
    int insn_count = 0;

    for (int i = 0; i < program->op_count; i++) {
        const JitOp* op = &program->ops[i];
        op_to_insn[i] = insn_count;
        switch (op->op) {
            case JOP_CONSTANT:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_CONST, 0, 0, 0, op->byte_offset, 0, op->constant)) return 0;
                break;
            case JOP_NIL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_NIL, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_TRUE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_TRUE, 0, 0, 0, op->byte_offset, 0, BOOL_VAL(1))) return 0;
                break;
            case JOP_FALSE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_FALSE, 0, 0, 0, op->byte_offset, 0, BOOL_VAL(0))) return 0;
                break;
            case JOP_POP:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_POP, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_GET_LOCAL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_LOCAL, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_GET2_LOCAL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_LOCAL, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_LOCAL, op->b, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_GET_UPVALUE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_UPVALUE, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_SET_LOCAL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_STORE_LOCAL, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_SET_LOCAL_POP:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_STORE_LOCAL, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_POP, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_INC_LOCAL:
            case JOP_SUB_LOCAL_CONST:
            case JOP_MUL_LOCAL_CONST:
            case JOP_DIV_LOCAL_CONST:
            case JOP_MOD_LOCAL_CONST: {
                uint8_t bin_op = TOI_JIT_NATIVE_OP_BIN_ADD;
                if (op->op == JOP_SUB_LOCAL_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_SUB;
                else if (op->op == JOP_MUL_LOCAL_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_MUL;
                else if (op->op == JOP_DIV_LOCAL_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_DIV;
                else if (op->op == JOP_MOD_LOCAL_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_MOD;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_LOCAL, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_CONST, 0, 0, 0, op->byte_offset, 0, op->constant)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, bin_op, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_STORE_LOCAL, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            }
            case JOP_ADD_CONST:
            case JOP_SUB_CONST:
            case JOP_MUL_CONST:
            case JOP_DIV_CONST:
            case JOP_MOD_CONST: {
                uint8_t bin_op = TOI_JIT_NATIVE_OP_BIN_ADD;
                if (op->op == JOP_SUB_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_SUB;
                else if (op->op == JOP_MUL_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_MUL;
                else if (op->op == JOP_DIV_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_DIV;
                else if (op->op == JOP_MOD_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_MOD;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_CONST, 0, 0, 0, op->byte_offset, 0, op->constant)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, bin_op, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            }
            case JOP_NEGATE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_CONST, 0, 0, 0, op->byte_offset, 0, NUMBER_VAL(-1.0))) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BIN_MUL, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_ADD:
            case JOP_ADD_INPLACE:
            case JOP_IADD:
            case JOP_FADD:
            case JOP_QFADD:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BIN_ADD, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_SUBTRACT:
            case JOP_ISUB:
            case JOP_FSUB:
            case JOP_QFSUB:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BIN_SUB, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_MULTIPLY:
            case JOP_IMUL:
            case JOP_FMUL:
            case JOP_QFMUL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BIN_MUL, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_DIVIDE:
            case JOP_IDIV:
            case JOP_FDIV:
            case JOP_QFDIV:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BIN_DIV, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_MODULO:
            case JOP_IMOD:
            case JOP_FMOD:
            case JOP_QFMOD:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BIN_MOD, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_NOT:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_NOT, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_EQUAL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_CMP_EQ, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_QGREATER_NUM:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_CMP_GT, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_QLESS_NUM:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_CMP_LT, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_APPEND:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_APPEND, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_GET_TABLE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_GET_TABLE, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_GET_TABLE_NUM:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_GET_TABLE_NUM, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_SET_TABLE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_SET_TABLE, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_SET_TABLE_NUM:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_SET_TABLE_NUM, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_CALL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_CALL, op->a, 0, 0, op->byte_offset, op->target, op->constant)) return 0;
                break;
            case JOP_QCALL2:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_CALL, 2, 0, 0, op->byte_offset, op->target, op->constant)) return 0;
                break;
            case JOP_INVOKE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_INVOKE, op->a, 0, 0, op->byte_offset, op->target, op->constant)) return 0;
                break;
            case JOP_BUILD_STRING2:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BUILD_STRING2, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_BUILD_STRING3:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BUILD_STRING3, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_BUILD_STRING4:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BUILD_STRING4, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_BUILD_STRING5:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BUILD_STRING5, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_JUMP:
            case JOP_JUMP_IF_FALSE:
            case JOP_JUMP_IF_TRUE: {
                uint8_t nop = TOI_JIT_NATIVE_OP_JUMP;
                if (op->op == JOP_JUMP_IF_FALSE) nop = TOI_JIT_NATIVE_OP_JUMP_IF_FALSE;
                else if (op->op == JOP_JUMP_IF_TRUE) nop = TOI_JIT_NATIVE_OP_JUMP_IF_TRUE;
                if (!jit_native_emit_instr(out_instrs, &insn_count, nop, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (patch_count >= 256) return 0;
                patches[patch_count++] = (JitNativeLowerPatch){insn_count - 1, op->target};
                break;
            }
            case JOP_ADJUST_STACK:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_ADJUST_STACK, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_RETURN:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_STORE_LOCAL, ret_slot, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_JUMP, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (patch_count >= 256) return 0;
                patches[patch_count++] = (JitNativeLowerPatch){insn_count - 1, JIT_NATIVE_TARGET_EXIT};
                break;
            case JOP_TINY_INLINE:
            case JOP_LENGTH:
            case JOP_SLICE:
            case JOP_QFOR_PREP_NUM:
            case JOP_QFOR_LOOP_NUM:
                return 0;
            default:
                return 0;
        }
    }

    if (insn_count <= 0 || insn_count >= 256) return 0;
    int exit_target = insn_count;
    for (int i = 0; i < patch_count; i++) {
        int target_insn = -1;
        uint16_t target_op = patches[i].target_op;
        if (target_op == JIT_NATIVE_TARGET_EXIT) {
            target_insn = exit_target;
        } else if ((int)target_op >= 0 && (int)target_op < program->op_count) {
            target_insn = op_to_insn[target_op];
        }
        if (target_insn < 0 || target_insn > exit_target) return 0;
        out_instrs[patches[i].patch_insn].target = (uint16_t)target_insn;
    }

    *out_instr_count = insn_count;
    *out_ret_slot = ret_slot;
    *out_var_slot = var_slot;
    *out_end_slot = end_slot;
    *out_slot_count = slot_count;
    return 1;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static int jit_native_get_or_compile_program_entry(
    void* program_ptr,
    int argc,
    ToiJitNativeTraceFn* out_fn,
    uint8_t* out_ret_slot,
    uint8_t* out_var_slot,
    uint8_t* out_end_slot,
    uint8_t* out_slot_count,
    JitNativeProgramEntry** out_entry) {
    if (out_fn == NULL || out_ret_slot == NULL || out_var_slot == NULL ||
        out_end_slot == NULL || out_slot_count == NULL) {
        return 0;
    }
    *out_fn = NULL;
    if (out_entry != NULL) *out_entry = NULL;
    if (program_ptr == NULL || argc < 0 || argc > 255) return 0;
    uint32_t h = jit_native_program_entry_hash(program_ptr, argc);
    JitNativeProgramEntry* e = &jit_native_program_entries[h];
    if (e->valid &&
        e->program_ptr == program_ptr &&
        (int)e->argc == argc &&
        e->fn != NULL) {
        *out_fn = e->fn;
        *out_ret_slot = e->ret_slot;
        *out_var_slot = e->var_slot;
        *out_end_slot = e->end_slot;
        *out_slot_count = e->slot_count;
        if (out_entry != NULL) *out_entry = e;
        if (jit_native_program_entry_debug()) {
            static int dbg_hits = 20;
            if (dbg_hits-- > 0) {
                fprintf(stderr, "[jit-program-entry-hit] prog=%p argc=%d slot_count=%u\n",
                        program_ptr, argc, (unsigned)e->slot_count);
            }
        }
        return 1;
    }
    if (e->failed &&
        e->program_ptr == program_ptr &&
        (int)e->argc == argc) {
        return 0;
    }

    const JitProgram* program = (const JitProgram*)program_ptr;
    const char* prog_fn_name =
        (program != NULL && program->function != NULL && program->function->name != NULL)
            ? program->function->name->chars
            : "<script>";
    ToiJitNativeTraceInstr instrs[256];
    int instr_count = 0;
    uint8_t ret_slot = 0;
    uint8_t var_slot = 0;
    uint8_t end_slot = 0;
    uint8_t slot_count = 0;
    if (!jit_native_lower_chain_safe_program(
            program,
            argc,
            instrs,
            &instr_count,
            &ret_slot,
            &var_slot,
            &end_slot,
            &slot_count)) {
        e->failed = 1;
        e->valid = 0;
        e->program_ptr = program_ptr;
        e->argc = (uint8_t)argc;
        if (jit_native_program_entry_debug()) {
            static int dbg_fail_lower = 20;
            if (dbg_fail_lower-- > 0) {
                int op_count = (program != NULL) ? program->op_count : -1;
                fprintf(stderr,
                        "[jit-program-entry-lower-fail] fn=%s prog=%p argc=%d ops=%d\n",
                        prog_fn_name,
                        program_ptr,
                        argc,
                        op_count);
            }
        }
        return 0;
    }

    void* fn_mem = NULL;
    void* code_mem = NULL;
    size_t code_size = 0;
    if (!toi_jit_arm64_compile_native_trace_direct_core(
            instrs,
            instr_count,
            NULL,
            0,
            var_slot,
            end_slot,
            &fn_mem,
            &code_mem,
            &code_size)) {
        e->failed = 1;
        e->valid = 0;
        e->program_ptr = program_ptr;
        e->argc = (uint8_t)argc;
        if (jit_native_program_entry_debug()) {
            static int dbg_fail_compile = 20;
            if (dbg_fail_compile-- > 0) {
                fprintf(stderr, "[jit-program-entry-compile-fail] prog=%p argc=%d instrs=%d\n",
                        program_ptr, argc, instr_count);
            }
        }
        return 0;
    }

    if (e->valid && e->code_mem != NULL && e->code_size > 0) {
        munmap(e->code_mem, e->code_size);
    }
    e->program_ptr = program_ptr;
    e->fn = (ToiJitNativeTraceFn)fn_mem;
    e->code_mem = code_mem;
    e->code_size = code_size;
    e->argc = (uint8_t)argc;
    e->ret_slot = ret_slot;
    e->var_slot = var_slot;
    e->end_slot = end_slot;
    e->slot_count = slot_count;
    e->valid = 1;
    e->failed = 0;
    e->runtime_disabled = 0;
    e->exec_attempts = 0;
    e->exec_hits = 0;
    if (jit_native_program_entry_debug()) {
        static int dbg_compile_ok = 20;
        if (dbg_compile_ok-- > 0) {
            fprintf(stderr, "[jit-program-entry-compile-ok] prog=%p argc=%d instrs=%d slot_count=%u\n",
                    program_ptr, argc, instr_count, (unsigned)slot_count);
            fprintf(stderr, "[jit-program-entry-compile-fn] fn=%s\n", prog_fn_name);
        }
    }

    *out_fn = e->fn;
    *out_ret_slot = ret_slot;
    *out_var_slot = var_slot;
    *out_end_slot = end_slot;
    *out_slot_count = slot_count;
    if (out_entry != NULL) *out_entry = e;
    return 1;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static int jit_native_execute_program_entry_cached(
    VM* vm,
    ObjClosure* callee,
    void* entry_ptr,
    int argc,
    const Value* args,
    Value* out) {
    if (vm == NULL || callee == NULL || entry_ptr == NULL || args == NULL || out == NULL) return 0;
    int dbg = jit_native_program_entry_debug();
    static int dbg_budget = 40;
    JitNativeProgramEntry* e = (JitNativeProgramEntry*)entry_ptr;
    ToiJitState* st = (ToiJitState*)vm->native_jit;

    if (!e->runtime_disabled &&
        e->valid &&
        e->fn != NULL &&
        e->slot_count > 0 &&
        (int)e->argc == argc) {
        if (e->exec_attempts < 0xffffu) e->exec_attempts++;
        int slot_count = (int)e->slot_count;
        Value locals[slot_count];
        for (int i = 0; i < slot_count; i++) locals[i] = NIL_VAL;
        if (slot_count > 0) {
            locals[0] = OBJ_VAL(callee);
        }
        for (int i = 0; i < argc && (i + 1) < slot_count; i++) {
            locals[i + 1] = args[i];
        }
        // One-shot execution for direct-core loop ABI: var starts at -1, end at 0.
        // The entry loop increments first, then runs body exactly once.
        locals[e->var_slot] = NUMBER_VAL(-1.0);
        locals[e->end_slot] = NUMBER_VAL(0.0);

        ToiJitNativeExecState state;
        memset(&state, 0, sizeof(state));
        if (st != NULL) st->native_trace_active_depth++;
        int rr = e->fn(vm, callee, locals, slot_count, &state);
        if (st != NULL && st->native_trace_active_depth > 0) st->native_trace_active_depth--;
        if (rr == (int)TOI_JIT_NATIVE_EXEC_EXIT) {
            if (e->exec_hits < 0xffffu) e->exec_hits++;
            *out = locals[e->ret_slot];
            return 1;
        }
        e->runtime_disabled = 1;
    }

    if (toi_jit_execute_program_direct_with_args(vm, callee, e->program_ptr, argc, (Value*)args, out)) return 1;
    if (dbg && dbg_budget > 0) {
        dbg_budget--;
        const char* fn_name =
            (callee->function != NULL && callee->function->name != NULL)
                ? callee->function->name->chars
                : "<script>";
        fprintf(stderr,
                "[jit-program-entry-fail] fn=%s argc=%d prog=%p\n",
                fn_name,
                argc,
                e->program_ptr);
    }
    return 0;
}

static JitNativeCalleeEntryFn jit_native_compile_callee_entry(
    void* runner_fn_ptr,
    uintptr_t runner_arg,
    int argc,
    void** out_code_mem,
    size_t* out_code_size) {
    if (out_code_mem != NULL) *out_code_mem = NULL;
    if (out_code_size != NULL) *out_code_size = 0;
    if (runner_fn_ptr == NULL || argc < 0 || argc > 255) return NULL;

    Arm64CodeBuf buf;
    memset(&buf, 0, sizeof(buf));

    if (!a64_emit(&buf, a64_sub_imm(31, 31, 16))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(30, 31, 0))) goto emit_fail;

    // abi: x0=vm, x1=callee closure, x2=args, x3=out
    if (!a64_emit(&buf, a64_mov_x(5, 3))) goto emit_fail; // x5 = out
    if (!a64_emit(&buf, a64_mov_x(4, 2))) goto emit_fail; // x4 = args
    if (!a64_emit_mov_imm64(&buf, 3, (uint64_t)argc)) goto emit_fail; // x3 = argc
    if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)runner_arg)) goto emit_fail; // x2 = program or entry
    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)runner_fn_ptr)) goto emit_fail;
    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;

    if (!a64_emit(&buf, a64_ldr_x_imm(30, 31, 0))) goto emit_fail;
    if (!a64_emit(&buf, a64_add_imm(31, 31, 16))) goto emit_fail;
    if (!a64_emit(&buf, 0xD65F03C0u)) goto emit_fail;

    size_t code_size = (size_t)buf.count * sizeof(uint32_t);
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t map_size = ((code_size + (size_t)page_size - 1u) / (size_t)page_size) * (size_t)page_size;

    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_JIT
    mmap_flags |= MAP_JIT;
#endif
    void* mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
    if (mem == MAP_FAILED) goto emit_fail;
    memcpy(mem, buf.words, code_size);
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)mem, (char*)mem + code_size);
#endif
    if (mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, map_size);
        goto emit_fail;
    }
    free(buf.words);
    if (out_code_mem != NULL) *out_code_mem = mem;
    if (out_code_size != NULL) *out_code_size = map_size;
    return (JitNativeCalleeEntryFn)mem;

emit_fail:
    free(buf.words);
    return NULL;
}

static JitNativeCalleeEntryFn jit_native_compile_callee_entry_program_entry_direct(
    ToiJitNativeTraceFn entry_fn,
    int argc,
    uint8_t ret_slot,
    uint8_t var_slot,
    uint8_t end_slot,
    uint8_t slot_count,
    void** out_code_mem,
    size_t* out_code_size) {
    if (out_code_mem != NULL) *out_code_mem = NULL;
    if (out_code_size != NULL) *out_code_size = 0;
    if (entry_fn == NULL || argc < 0 || argc > 255) return NULL;
    if ((int)slot_count <= 0 || (int)slot_count > 255) return NULL;
    if ((int)ret_slot >= (int)slot_count ||
        (int)var_slot >= (int)slot_count ||
        (int)end_slot >= (int)slot_count) {
        return NULL;
    }

    Arm64CodeBuf buf;
    memset(&buf, 0, sizeof(buf));

    const uint16_t off_lr = 0;
    const uint16_t off_vm = 8;
    const uint16_t off_callee = 16;
    const uint16_t off_args = 24;
    const uint16_t off_out = 32;
    const uint16_t locals_off = 64;
    uint16_t locals_bytes = (uint16_t)((uint16_t)slot_count * 16u);
    uint16_t frame_size = (uint16_t)(locals_off + locals_bytes);
    frame_size = (uint16_t)((frame_size + 15u) & ~15u);
    if (frame_size == 0) frame_size = 16;
    if (frame_size > 4095u) return NULL;

    // Prologue and argument spills.
    if (!a64_emit(&buf, a64_sub_imm(31, 31, frame_size))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(30, 31, off_lr))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(0, 31, off_vm))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(1, 31, off_callee))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(2, 31, off_args))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(3, 31, off_out))) goto emit_fail;
    if (!a64_emit_mov_imm64(&buf, 10, 0u)) goto emit_fail; // reused zero payload

    // Chain attempt counter (native).
    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_chain_attempts)) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_x_imm(11, 9, 0))) goto emit_fail;
    if (!a64_emit(&buf, a64_add_imm(11, 11, 1))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(11, 9, 0))) goto emit_fail;

    // Zero-init locals to NIL.
    for (int i = 0; i < (int)slot_count; i++) {
        uint16_t base = (uint16_t)(locals_off + (uint16_t)i * 16u);
        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_NIL)) goto emit_fail;
        if (!a64_emit(&buf, a64_str_x_imm(9, 31, base))) goto emit_fail;
        if (!a64_emit(&buf, a64_str_x_imm(10, 31, (uint16_t)(base + 8u)))) goto emit_fail;
    }

    // locals[0] = callee (Value OBJ).
    {
        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_OBJ)) goto emit_fail;
        if (!a64_emit(&buf, a64_str_x_imm(9, 31, locals_off))) goto emit_fail;
        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, off_callee))) goto emit_fail;
        if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(locals_off + 8u)))) goto emit_fail;
    }

    // Copy args -> locals[1..argc].
    {
        int max_copy = argc;
        if (max_copy > (int)slot_count - 1) max_copy = (int)slot_count - 1;
        if (!a64_emit(&buf, a64_ldr_x_imm(12, 31, off_args))) goto emit_fail; // x12 = args*
        for (int i = 0; i < max_copy; i++) {
            uint16_t src = (uint16_t)((uint16_t)i * 16u);
            uint16_t dst = (uint16_t)(locals_off + (uint16_t)(i + 1) * 16u);
            if (!a64_emit(&buf, a64_ldr_x_imm(9, 12, src))) goto emit_fail;
            if (!a64_emit(&buf, a64_str_x_imm(9, 31, dst))) goto emit_fail;
            if (!a64_emit(&buf, a64_ldr_x_imm(9, 12, (uint16_t)(src + 8u)))) goto emit_fail;
            if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(dst + 8u)))) goto emit_fail;
        }
    }

    // One-shot loop ABI setup: var=-1, end=0 as NUMBER values.
    {
        uint16_t var_base = (uint16_t)(locals_off + (uint16_t)var_slot * 16u);
        uint16_t end_base = (uint16_t)(locals_off + (uint16_t)end_slot * 16u);
        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_NUMBER)) goto emit_fail;
        if (!a64_emit(&buf, a64_str_x_imm(9, 31, var_base))) goto emit_fail;
        if (!a64_emit_mov_imm64(&buf, 9, 0xBFF0000000000000ULL)) goto emit_fail; // -1.0
        if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(var_base + 8u)))) goto emit_fail;
        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_NUMBER)) goto emit_fail;
        if (!a64_emit(&buf, a64_str_x_imm(9, 31, end_base))) goto emit_fail;
        if (!a64_emit(&buf, a64_str_x_imm(10, 31, (uint16_t)(end_base + 8u)))) goto emit_fail; // +0.0
    }

    // Call compiled native program entry directly.
    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, off_vm))) goto emit_fail;       // vm
    if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, off_callee))) goto emit_fail;   // callee
    if (!a64_emit(&buf, a64_add_imm(2, 31, locals_off))) goto emit_fail;     // locals
    if (!a64_emit_mov_imm64(&buf, 3, (uint64_t)slot_count)) goto emit_fail;  // slot_count
    if (!a64_emit_mov_imm64(&buf, 4, 0u)) goto emit_fail;                     // out_state = NULL
    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)entry_fn)) goto emit_fail;
    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
    if (!a64_emit(&buf, a64_cmp_w_imm(0, TOI_JIT_NATIVE_EXEC_EXIT))) goto emit_fail;
    int b_not_exit = buf.count;
    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fail

    // Success: out = locals[ret_slot], return 1.
    {
        uint16_t ret_base = (uint16_t)(locals_off + (uint16_t)ret_slot * 16u);
        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_chain_hits)) goto emit_fail;
        if (!a64_emit(&buf, a64_ldr_x_imm(11, 9, 0))) goto emit_fail;
        if (!a64_emit(&buf, a64_add_imm(11, 11, 1))) goto emit_fail;
        if (!a64_emit(&buf, a64_str_x_imm(11, 9, 0))) goto emit_fail;
        if (!a64_emit(&buf, a64_ldr_x_imm(12, 31, off_out))) goto emit_fail;
        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, ret_base))) goto emit_fail;
        if (!a64_emit(&buf, a64_str_x_imm(9, 12, 0))) goto emit_fail;
        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, (uint16_t)(ret_base + 8u)))) goto emit_fail;
        if (!a64_emit(&buf, a64_str_x_imm(9, 12, 8))) goto emit_fail;
        if (!a64_emit_mov_imm64(&buf, 0, 1u)) goto emit_fail;
        if (!a64_emit(&buf, a64_ldr_x_imm(30, 31, off_lr))) goto emit_fail;
        if (!a64_emit(&buf, a64_add_imm(31, 31, frame_size))) goto emit_fail;
        if (!a64_emit(&buf, 0xD65F03C0u)) goto emit_fail;
    }

    int fail_label = buf.count;
    if (!a64_patch_b_cond(&buf, b_not_exit, fail_label, 0x1u)) goto emit_fail; // ne
    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_chain_misses)) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_x_imm(11, 9, 0))) goto emit_fail;
    if (!a64_emit(&buf, a64_add_imm(11, 11, 1))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(11, 9, 0))) goto emit_fail;
    if (!a64_emit_mov_imm64(&buf, 0, 0u)) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_x_imm(30, 31, off_lr))) goto emit_fail;
    if (!a64_emit(&buf, a64_add_imm(31, 31, frame_size))) goto emit_fail;
    if (!a64_emit(&buf, 0xD65F03C0u)) goto emit_fail;

    size_t code_size = (size_t)buf.count * sizeof(uint32_t);
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t map_size = ((code_size + (size_t)page_size - 1u) / (size_t)page_size) * (size_t)page_size;

    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_JIT
    mmap_flags |= MAP_JIT;
#endif
    void* mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
    if (mem == MAP_FAILED) goto emit_fail;
    memcpy(mem, buf.words, code_size);
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)mem, (char*)mem + code_size);
#endif
    if (mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, map_size);
        goto emit_fail;
    }
    free(buf.words);
    if (out_code_mem != NULL) *out_code_mem = mem;
    if (out_code_size != NULL) *out_code_size = map_size;
    return (JitNativeCalleeEntryFn)mem;

emit_fail:
    free(buf.words);
    return NULL;
}

static JitNativeCalleeEntryFn jit_native_get_or_create_callee_entry(
    ObjClosure* callee,
    void* program_ptr,
    int argc) {
    if (callee == NULL || program_ptr == NULL || argc < 0 || argc > 255) return NULL;
    if (!jit_native_callee_entry_eligible(callee, argc)) return NULL;
    uint32_t h = jit_native_callee_entry_hash(program_ptr, argc);
    JitNativeCalleeEntry* e = &jit_native_callee_entries[h];
    if (e->valid &&
        e->program_ptr == program_ptr &&
        (int)e->argc == argc &&
        e->entry_fn != NULL) {
        return e->entry_fn;
    }

    JitNativeCalleeRunnerKind runner_kind = JIT_NATIVE_CALLEE_RUNNER_DIRECT;
    uint8_t chain_direct_only = 0;
    jit_native_plan_callee_entry(
        callee,
        program_ptr,
        argc,
        &runner_kind,
        &chain_direct_only);

    void* callee_runner = (void*)&toi_jit_execute_program_direct_with_args;
    uintptr_t runner_arg = (uintptr_t)program_ptr;
    if (runner_kind == JIT_NATIVE_CALLEE_RUNNER_PROGRAM_ENTRY) {
        ToiJitNativeTraceFn entry_fn = NULL;
        uint8_t ret_slot = 0;
        uint8_t var_slot = 0;
        uint8_t end_slot = 0;
        uint8_t slot_count = 0;
        JitNativeProgramEntry* entry = NULL;
        if (jit_native_get_or_compile_program_entry(
                program_ptr,
                argc,
                &entry_fn,
                &ret_slot,
                &var_slot,
                &end_slot,
                &slot_count,
                &entry) &&
            entry_fn != NULL &&
            entry != NULL) {
            callee_runner = (void*)&jit_native_execute_program_entry_cached;
            runner_arg = (uintptr_t)entry;
            chain_direct_only = 0;
        } else {
            runner_kind = JIT_NATIVE_CALLEE_RUNNER_DIRECT;
            chain_direct_only = toi_jit_program_chain_safe(program_ptr) ? 1 : 0;
        }
    } else {
        runner_kind = JIT_NATIVE_CALLEE_RUNNER_DIRECT;
    }

    void* code_mem = NULL;
    size_t code_size = 0;
    JitNativeCalleeEntryFn fn = NULL;
    if (runner_kind == JIT_NATIVE_CALLEE_RUNNER_PROGRAM_ENTRY) {
        JitNativeProgramEntry* pe = (JitNativeProgramEntry*)(uintptr_t)runner_arg;
        if (pe != NULL && pe->fn != NULL) {
            fn = jit_native_compile_callee_entry_program_entry_direct(
                pe->fn,
                argc,
                pe->ret_slot,
                pe->var_slot,
                pe->end_slot,
                pe->slot_count,
                &code_mem,
                &code_size);
        }
        if (fn == NULL) {
            fn = jit_native_compile_callee_entry(callee_runner, runner_arg, argc, &code_mem, &code_size);
        }
    } else {
        fn = jit_native_compile_callee_entry(callee_runner, runner_arg, argc, &code_mem, &code_size);
    }
    if (fn == NULL) return NULL;

    if (e->valid && e->code_mem != NULL && e->code_size > 0) {
        munmap(e->code_mem, e->code_size);
    }
    e->valid = 1;
    e->program_ptr = program_ptr;
    e->entry_fn = fn;
    e->code_mem = code_mem;
    e->code_size = code_size;
    e->argc = (uint8_t)argc;
    e->runner_kind = (uint8_t)runner_kind;
    e->chain_direct_only = chain_direct_only;
    return fn;
}

static int jit_native_call_dispatch(
    VM* vm,
    ObjClosure* caller_closure,
    uint32_t opcode_offset,
    const Value* callee_ptr,
    const Value* args,
    int argc,
    struct Obj* target_hint,
    Value* out_value) {
    if (vm == NULL || callee_ptr == NULL || args == NULL || out_value == NULL || argc < 0) return 0;
    Value callee = *callee_ptr;
    int has_hint = (target_hint != NULL);

    if (IS_CLOSURE(callee)) {
        ObjClosure* target = AS_CLOSURE(callee);
        if ((!has_hint || AS_OBJ(callee) == target_hint) &&
            caller_closure != NULL && argc >= 0 && argc <= 255 &&
            jit_native_call_poly_enabled()) {
            uint32_t hp = jit_native_call_poly_pic_hash(caller_closure, opcode_offset, argc);
            JitNativeCallPolyPicEntry* pe = &jit_native_call_poly_pic[hp];
            if (pe->valid &&
                pe->caller == caller_closure &&
                pe->opcode_offset == opcode_offset &&
                (int)pe->argc == argc) {
                if (pe->callee0 == target && pe->entry0 != NULL) {
                    if (pe->entry0(vm, target, args, out_value)) return 1;
                }
                if (pe->callee1 == target && pe->entry1 != NULL) {
                    if (pe->entry1(vm, target, args, out_value)) return 1;
                }
            }
            void* program_ptr = NULL;
            if (toi_jit_get_compiled_program_ptr_at_callsite(
                    vm,
                    caller_closure,
                    opcode_offset,
                    target,
                    &program_ptr) &&
                program_ptr != NULL) {
                JitNativeCalleeEntryFn fn = jit_native_get_or_create_callee_entry(target, program_ptr, argc);
                if (fn != NULL && fn(vm, target, args, out_value)) {
                    if (!pe->valid ||
                        pe->caller != caller_closure ||
                        pe->opcode_offset != opcode_offset ||
                        (int)pe->argc != argc) {
                        pe->valid = 1;
                        pe->caller = caller_closure;
                        pe->opcode_offset = opcode_offset;
                        pe->argc = (uint8_t)argc;
                        pe->callee0 = target;
                        pe->entry0 = fn;
                        pe->program0 = program_ptr;
                        pe->callee1 = NULL;
                        pe->entry1 = NULL;
                        pe->program1 = NULL;
                    } else if (pe->callee0 == NULL || pe->callee0 == target) {
                        pe->callee0 = target;
                        pe->entry0 = fn;
                        pe->program0 = program_ptr;
                    } else {
                        pe->callee1 = target;
                        pe->entry1 = fn;
                        pe->program1 = program_ptr;
                    }
                    return 1;
                }
            }
        }
        if (!has_hint || AS_OBJ(callee) == target_hint) {
            if (caller_closure != NULL) {
                if (jit_native_call_closure_callsite_dispatch(
                        vm,
                        caller_closure,
                        opcode_offset,
                        target,
                        args,
                        argc,
                        NULL,
                        out_value)) {
                    return 1;
                }
            } else {
                if (jit_native_call_closure_dispatch(vm, target, args, argc, NULL, out_value)) {
                    return 1;
                }
            }
        }
    } else if (IS_NATIVE(callee)) {
        if ((!has_hint || AS_OBJ(callee) == target_hint) &&
            jit_value_call_primitive_arm(vm, callee, (Value*)args, argc, out_value)) {
            return 1;
        }
    }

    // Generic fallback for polymorphic/native/bound-method cases.
    return jit_value_call_primitive_arm(vm, callee, (Value*)args, argc, out_value);
}

static int jit_native_call_closure_dispatch(
    VM* vm,
    ObjClosure* closure,
    const Value* args,
    int argc,
    void* program_ptr,
    Value* out_value) {
    if (vm == NULL || closure == NULL || args == NULL || out_value == NULL || argc < 0) return 0;
    if (program_ptr != NULL) {
        JitNativeCalleeEntryFn fn = jit_native_get_or_create_callee_entry(closure, program_ptr, argc);
        if (fn != NULL && fn(vm, closure, args, out_value)) {
            return 1;
        }
    }
    if (program_ptr != NULL &&
        toi_jit_execute_program_direct_with_args(vm, closure, program_ptr, argc, (Value*)args, out_value)) {
        return 1;
    }
    if (toi_jit_try_execute_linked_with_args(vm, closure, argc, (Value*)args, out_value)) return 1;
    if (toi_jit_try_execute_compiled_with_args(vm, closure, argc, (Value*)args, out_value)) return 1;
    if (toi_jit_try_execute_with_args(vm, closure, argc, (Value*)args, out_value)) return 1;
    return jit_value_call_primitive_arm(vm, OBJ_VAL(closure), (Value*)args, argc, out_value);
}

static int jit_native_call_closure_dispatch_after_entry_miss(
    VM* vm,
    ObjClosure* closure,
    const Value* args,
    int argc,
    void* program_ptr,
    Value* out_value) {
    if (vm == NULL || closure == NULL || args == NULL || out_value == NULL || argc < 0) return 0;
    if (program_ptr != NULL &&
        toi_jit_execute_program_direct_with_args(vm, closure, program_ptr, argc, (Value*)args, out_value)) {
        return 1;
    }
    if (toi_jit_try_execute_linked_with_args(vm, closure, argc, (Value*)args, out_value)) return 1;
    if (toi_jit_try_execute_compiled_with_args(vm, closure, argc, (Value*)args, out_value)) return 1;
    if (toi_jit_try_execute_with_args(vm, closure, argc, (Value*)args, out_value)) return 1;
    return jit_value_call_primitive_arm(vm, OBJ_VAL(closure), (Value*)args, argc, out_value);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static int jit_native_call_closure_callsite_dispatch(
    VM* vm,
    ObjClosure* caller_closure,
    uint32_t opcode_offset,
    ObjClosure* callee_closure,
    const Value* args,
    int argc,
    void* program_ptr,
    Value* out_value) {
    if (vm == NULL || callee_closure == NULL || args == NULL || out_value == NULL || argc < 0) return 0;
    if (caller_closure != NULL && argc >= 0 && argc <= 255) {
        uint32_t h = jit_native_callsite_pic_hash(caller_closure, opcode_offset, callee_closure, argc);
        JitNativeCallsitePicEntry* e = &jit_native_callsite_pic[h];
        if (e->valid &&
            e->caller == caller_closure &&
            e->callee == callee_closure &&
            e->opcode_offset == opcode_offset &&
            (int)e->argc == argc &&
            e->entry_fn != NULL) {
            if (e->entry_fn(vm, callee_closure, args, out_value)) {
                return 1;
            }
            e->valid = 0;
            e->caller = NULL;
            e->callee = NULL;
            e->program_ptr = NULL;
            e->entry_fn = NULL;
            e->opcode_offset = 0;
            e->argc = 0;
        }
    }
    if (program_ptr != NULL) {
        JitNativeCalleeEntryFn fn = jit_native_get_or_create_callee_entry(callee_closure, program_ptr, argc);
        if (fn != NULL && fn(vm, callee_closure, args, out_value)) {
            return 1;
        }
    }
    if (program_ptr != NULL &&
        toi_jit_execute_program_direct_with_args(vm, callee_closure, program_ptr, argc, (Value*)args, out_value)) {
        return 1;
    }
    if (caller_closure != NULL) {
        void* callsite_program_ptr = NULL;
        if (toi_jit_get_compiled_program_ptr_at_callsite(
                vm,
                caller_closure,
                opcode_offset,
                callee_closure,
                &callsite_program_ptr) &&
            callsite_program_ptr != NULL) {
            if (argc >= 0 && argc <= 255) {
                uint32_t h = jit_native_callsite_pic_hash(caller_closure, opcode_offset, callee_closure, argc);
                JitNativeCallsitePicEntry* e = &jit_native_callsite_pic[h];
                JitNativeCalleeEntryFn fn = jit_native_get_or_create_callee_entry(
                    callee_closure, callsite_program_ptr, argc);
                if (fn != NULL) {
                    e->valid = 1;
                    e->caller = caller_closure;
                    e->callee = callee_closure;
                    e->program_ptr = callsite_program_ptr;
                    e->entry_fn = fn;
                    e->opcode_offset = opcode_offset;
                    e->argc = (uint8_t)argc;
                }
            }
            JitNativeCalleeEntryFn fn =
                jit_native_get_or_create_callee_entry(callee_closure, callsite_program_ptr, argc);
            if (fn != NULL && fn(vm, callee_closure, args, out_value)) {
                return 1;
            }
            if (toi_jit_execute_program_direct_with_args(
                    vm, callee_closure, callsite_program_ptr, argc, (Value*)args, out_value)) {
                return 1;
            }
        }
    }
    if (caller_closure != NULL &&
        toi_jit_try_execute_compiled_at_callsite_with_args(
            vm,
            caller_closure,
            opcode_offset,
            callee_closure,
            argc,
            (Value*)args,
            out_value)) {
        return 1;
    }
    return jit_native_call_closure_dispatch(vm, callee_closure, args, argc, program_ptr, out_value);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static int jit_native_invoke_dispatch(
    VM* vm,
    const Value* receiver_ptr,
    const Value* args,
    int argc,
    ObjString* method,
    Value* out_value) {
    if (vm == NULL || receiver_ptr == NULL || args == NULL || out_value == NULL || method == NULL || argc < 0) return 0;
    return jit_invoke_primitive_arm(vm, *receiver_ptr, OBJ_VAL(method), (Value*)args, argc, out_value);
}

static int jit_native_invoke_dispatch_chained(
    VM* vm,
    const Value* receiver_ptr,
    const Value* args,
    int argc,
    ObjString* method,
    Value* out_value) {
    if (vm == NULL || receiver_ptr == NULL || args == NULL || out_value == NULL || method == NULL || argc < 0) return 0;
    Value receiver = *receiver_ptr;
    if (IS_TABLE(receiver)) {
        ObjTable* t = AS_TABLE(receiver);
        if (t != NULL && t->no_metatable_fast) {
            uint32_t ih = jit_native_invoke_ic_hash(t, method, argc);
            JitNativeInvokeIcEntry* ie = &jit_native_invoke_ic[ih];
            if (ie->valid &&
                ie->table == t &&
                ie->method == method &&
                ie->shape_version == t->table.shape_version &&
                (int)ie->argc == argc &&
                ie->callee != NULL) {
                int margc = argc + 1;
                Value invoke_args[margc];
                invoke_args[0] = receiver;
                for (int i = 0; i < argc; i++) invoke_args[i + 1] = args[i];
                if (jit_native_call_closure_dispatch(
                        vm,
                        ie->callee,
                        invoke_args,
                        margc,
                        NULL,
                        out_value)) {
                    return 1;
                }
                ie->valid = 0;
            }
            Value raw_method = NIL_VAL;
            if (table_get(&t->table, method, &raw_method) && IS_CLOSURE(raw_method)) {
                ObjClosure* callee = AS_CLOSURE(raw_method);
                int margc = argc + 1;
                Value invoke_args[margc];
                invoke_args[0] = receiver;
                for (int i = 0; i < argc; i++) invoke_args[i + 1] = args[i];
                ie->table = t;
                ie->method = method;
                ie->callee = callee;
                ie->shape_version = t->table.shape_version;
                ie->argc = (uint8_t)argc;
                ie->valid = 1;
                if (jit_native_call_closure_dispatch(
                        vm,
                        callee,
                        invoke_args,
                        margc,
                        NULL,
                        out_value)) {
                    return 1;
                }
            }
        }
    }
    return jit_invoke_primitive_arm(vm, receiver, OBJ_VAL(method), (Value*)args, argc, out_value);
}

static int jit_native_trace_force_bridge_wrapper(void) {
    const char* v = getenv("TOI_JIT_NATIVE_TRACE_FORCE_BRIDGE");
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

static int toi_jit_arm64_compile_native_trace_direct_core(
    const ToiJitNativeTraceInstr* instrs,
    int instr_count,
    const ToiJitNativeGuard* guards,
    int guard_count,
    uint8_t var_slot,
    uint8_t end_slot,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) return 0;
    if (instrs == NULL || instr_count <= 0 || instr_count > 256) return 0;
    int debug_direct_core = 0;
    const char* dbg_env = getenv("TOI_JIT_DEBUG_DIRECT_CORE");
    if (dbg_env != NULL && dbg_env[0] != '\0' && dbg_env[0] != '0') debug_direct_core = 1;
    const char* fail_reason = "unknown";
    int fail_index = -1;
#define DC_FAIL(REASON, IDX) do { fail_reason = (REASON); fail_index = (IDX); goto direct_core_fail; } while (0)
    int entry_depth[257];
    for (int i = 0; i < 257; i++) entry_depth[i] = -1;

    // Direct-core restrictions:
    // - Native table/get/set/build-string/call paths require guard-proven operands.
    for (int i = 0; i < instr_count; i++) {
        const ToiJitNativeTraceInstr* in = &instrs[i];
        fail_index = i;
        switch (in->op) {
            case TOI_JIT_NATIVE_OP_NOP:
            case TOI_JIT_NATIVE_OP_PUSH_LOCAL:
            case TOI_JIT_NATIVE_OP_PUSH_UPVALUE:
            case TOI_JIT_NATIVE_OP_PUSH_CONST:
            case TOI_JIT_NATIVE_OP_PUSH_NIL:
            case TOI_JIT_NATIVE_OP_PUSH_TRUE:
            case TOI_JIT_NATIVE_OP_PUSH_FALSE:
            case TOI_JIT_NATIVE_OP_STORE_LOCAL:
            case TOI_JIT_NATIVE_OP_POP:
            case TOI_JIT_NATIVE_OP_BIN_ADD:
            case TOI_JIT_NATIVE_OP_BIN_SUB:
            case TOI_JIT_NATIVE_OP_BIN_MUL:
            case TOI_JIT_NATIVE_OP_BIN_DIV:
            case TOI_JIT_NATIVE_OP_BIN_MOD:
            case TOI_JIT_NATIVE_OP_CMP_LT:
            case TOI_JIT_NATIVE_OP_CMP_GT:
            case TOI_JIT_NATIVE_OP_CMP_EQ:
            case TOI_JIT_NATIVE_OP_NOT:
            case TOI_JIT_NATIVE_OP_BUILD_STRING2:
            case TOI_JIT_NATIVE_OP_BUILD_STRING3:
            case TOI_JIT_NATIVE_OP_BUILD_STRING4:
            case TOI_JIT_NATIVE_OP_BUILD_STRING5:
            case TOI_JIT_NATIVE_OP_CALL:
            case TOI_JIT_NATIVE_OP_INVOKE:
            case TOI_JIT_NATIVE_OP_JUMP:
            case TOI_JIT_NATIVE_OP_JUMP_IF_FALSE:
            case TOI_JIT_NATIVE_OP_JUMP_IF_TRUE:
            case TOI_JIT_NATIVE_OP_ADJUST_STACK:
                break;
            case TOI_JIT_NATIVE_OP_APPEND:
            case TOI_JIT_NATIVE_OP_GET_TABLE:
            case TOI_JIT_NATIVE_OP_GET_TABLE_NUM:
            case TOI_JIT_NATIVE_OP_SET_TABLE:
            case TOI_JIT_NATIVE_OP_SET_TABLE_NUM:
                break;
            default:
                DC_FAIL("unsupported-op", i);
        }
        if (in->op == TOI_JIT_NATIVE_OP_PUSH_LOCAL ||
            in->op == TOI_JIT_NATIVE_OP_STORE_LOCAL) {
            // Mixed Value locals are supported via slot-ref lowering.
        }
        if (in->op == TOI_JIT_NATIVE_OP_PUSH_CONST) {
            if (IS_NUMBER(in->constant)) {
                // Any finite number is materialized into a scratch slot.
            } else if (!IS_STRING(in->constant)) {
                DC_FAIL("push-const-type", i);
            }
        }
        if (in->op == TOI_JIT_NATIVE_OP_JUMP ||
            in->op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE ||
            in->op == TOI_JIT_NATIVE_OP_JUMP_IF_TRUE) {
            if ((int)in->target < 0 || (int)in->target > instr_count) {
                DC_FAIL("bad-jump-target", i);
            }
        }
    }

    // Control-flow-aware stack depth analysis so emit-time stack tracking
    // matches jump targets and ADJUST_STACK joins.
    {
        int queue[512];
        int qh = 0;
        int qt = 0;
        entry_depth[0] = 0;
        queue[qt++] = 0;
        while (qh < qt) {
            int pc = queue[qh++];
            if (pc < 0 || pc > instr_count) DC_FAIL("stack-pc-range", pc);
            if (pc == instr_count) continue;
            int depth = entry_depth[pc];
            if (depth < 0 || depth > 256) DC_FAIL("stack-depth-range", pc);
            const ToiJitNativeTraceInstr* in = &instrs[pc];
            int depth_out = depth;
            switch (in->op) {
                case TOI_JIT_NATIVE_OP_PUSH_LOCAL:
                case TOI_JIT_NATIVE_OP_PUSH_UPVALUE:
                case TOI_JIT_NATIVE_OP_PUSH_CONST:
                case TOI_JIT_NATIVE_OP_PUSH_NIL:
                case TOI_JIT_NATIVE_OP_PUSH_TRUE:
                case TOI_JIT_NATIVE_OP_PUSH_FALSE:
                    depth_out = depth + 1;
                    break;
                case TOI_JIT_NATIVE_OP_STORE_LOCAL:
                    if (depth < 1) DC_FAIL("stack-underflow-store", pc);
                    depth_out = depth;
                    break;
                case TOI_JIT_NATIVE_OP_POP:
                    if (depth < 1) DC_FAIL("stack-underflow-pop", pc);
                    depth_out = depth - 1;
                    break;
                case TOI_JIT_NATIVE_OP_BIN_ADD:
                case TOI_JIT_NATIVE_OP_BIN_SUB:
                case TOI_JIT_NATIVE_OP_BIN_MUL:
                case TOI_JIT_NATIVE_OP_BIN_DIV:
                case TOI_JIT_NATIVE_OP_BIN_MOD:
                case TOI_JIT_NATIVE_OP_CMP_EQ:
                case TOI_JIT_NATIVE_OP_CMP_LT:
                case TOI_JIT_NATIVE_OP_CMP_GT:
                case TOI_JIT_NATIVE_OP_GET_TABLE:
                case TOI_JIT_NATIVE_OP_GET_TABLE_NUM:
                case TOI_JIT_NATIVE_OP_APPEND:
                    if (depth < 2) DC_FAIL("stack-underflow-bin", pc);
                    depth_out = depth - 1;
                    break;
                case TOI_JIT_NATIVE_OP_SET_TABLE:
                case TOI_JIT_NATIVE_OP_SET_TABLE_NUM:
                    if (depth < 3) DC_FAIL("stack-underflow-set-table", pc);
                    depth_out = depth - 2;
                    break;
                case TOI_JIT_NATIVE_OP_CALL:
                case TOI_JIT_NATIVE_OP_INVOKE: {
                    int argc = (int)in->a;
                    if (argc < 0 || depth < argc + 1) DC_FAIL("stack-underflow-call", pc);
                    depth_out = depth - argc;
                    break;
                }
                case TOI_JIT_NATIVE_OP_BUILD_STRING2:
                case TOI_JIT_NATIVE_OP_BUILD_STRING3:
                case TOI_JIT_NATIVE_OP_BUILD_STRING4:
                case TOI_JIT_NATIVE_OP_BUILD_STRING5: {
                    int n = (in->op == TOI_JIT_NATIVE_OP_BUILD_STRING2) ? 2 :
                            (in->op == TOI_JIT_NATIVE_OP_BUILD_STRING3) ? 3 :
                            (in->op == TOI_JIT_NATIVE_OP_BUILD_STRING4) ? 4 : 5;
                    if (depth < n) DC_FAIL("stack-underflow-build-string", pc);
                    depth_out = depth - n + 1;
                    break;
                }
                case TOI_JIT_NATIVE_OP_NOT:
                case TOI_JIT_NATIVE_OP_JUMP_IF_FALSE:
                case TOI_JIT_NATIVE_OP_JUMP_IF_TRUE:
                    if (depth < 1) DC_FAIL("stack-underflow-test", pc);
                    depth_out = depth;
                    break;
                case TOI_JIT_NATIVE_OP_ADJUST_STACK:
                    if ((int)in->a < 0 || (int)in->a > 256) DC_FAIL("stack-adjust-range", pc);
                    depth_out = (int)in->a;
                    break;
                case TOI_JIT_NATIVE_OP_JUMP:
                case TOI_JIT_NATIVE_OP_NOP:
                    depth_out = depth;
                    break;
                default:
                    DC_FAIL("stack-op-unsupported", pc);
            }
            if (depth_out < 0 || depth_out > 256) DC_FAIL("stack-depth-out", pc);

            int succ_count = 0;
            int succs[2];
            if (in->op == TOI_JIT_NATIVE_OP_JUMP) {
                succs[succ_count++] = (int)in->target;
            } else if (in->op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE ||
                       in->op == TOI_JIT_NATIVE_OP_JUMP_IF_TRUE) {
                succs[succ_count++] = pc + 1;
                succs[succ_count++] = (int)in->target;
            } else {
                succs[succ_count++] = pc + 1;
            }

            for (int si = 0; si < succ_count; si++) {
                int succ = succs[si];
                if (succ < 0 || succ > instr_count) DC_FAIL("stack-succ-range", pc);
                if (succ == instr_count) continue;
                if (entry_depth[succ] < 0) {
                    entry_depth[succ] = depth_out;
                    if (qt >= (int)(sizeof(queue) / sizeof(queue[0]))) DC_FAIL("stack-queue-overflow", succ);
                    queue[qt++] = succ;
                } else if (entry_depth[succ] != depth_out) {
                    DC_FAIL("stack-merge-conflict", succ);
                }
            }
        }
    }

    Arm64CodeBuf buf;
    memset(&buf, 0, sizeof(buf));

    typedef struct {
        int branch_pos;
        uint16_t target_insn;
        uint8_t is_cond;
        uint8_t cond;
    } A64JumpPatch;

    typedef struct {
        uint8_t kind; // 0=num reg, 1=table local slot ref, 2=string const, 3=string local slot ref, 4=value local slot ref, 5=immediate value const, 6=generic upvalue ref, 7=table upvalue ref, 8=string upvalue ref, 9=value scratch ref
        uint8_t reg;
        uint8_t slot;
        uint8_t is_temp;
        Value constant;
    } A64TraceVal;
    A64TraceVal tstack[256];
    int vsp = 0;
    uint16_t temp_used = 0;
    int insn_labels[257];
    for (int i = 0; i < 257; i++) insn_labels[i] = -1;
    A64JumpPatch jump_patches[256];
    int jump_patch_count = 0;
    int continue_branches[256];
    int continue_branch_count = 0;
    int deopt_branches[256];
    int deopt_branch_count = 0;

    uint8_t is_merge_label[257];
    memset(is_merge_label, 0, sizeof(is_merge_label));
    // Merge points (if/else, and/or short-circuits) require both incoming edges to
    // agree on where each pending value lives. When the trace has any, every pushed
    // value is also written to its canonical scratch slot, so a merge label can
    // simply treat the pending entries as scratch refs without emitting fixups.
    int has_merge_points = 0;
    for (int _mp = 0; _mp < instr_count; _mp++) {
        if ((instrs[_mp].op == TOI_JIT_NATIVE_OP_JUMP ||
             instrs[_mp].op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE ||
             instrs[_mp].op == TOI_JIT_NATIVE_OP_JUMP_IF_TRUE) &&
            (int)instrs[_mp].target != instr_count) {
            has_merge_points = 1;
            break;
        }
    }
    if (has_merge_points) {
        for (int _lt = 0; _lt < instr_count; _lt++) {
            if ((instrs[_lt].op == TOI_JIT_NATIVE_OP_JUMP ||
                 instrs[_lt].op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE ||
                 instrs[_lt].op == TOI_JIT_NATIVE_OP_JUMP_IF_TRUE) &&
                (int)instrs[_lt].target < instr_count) {
                is_merge_label[instrs[_lt].target] = 1;
            }
        }
    }
#define SPILL_TRACE_VALUE(_sv, _dst)                                                                                     do {                                                                                                                     switch ((_sv)->kind) {                                                                                                   case 0:                                                                                                                  if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_NUMBER)) goto emit_fail;                                               if (!a64_emit(&buf, a64_str_x_imm(9, 31, (_dst)))) goto emit_fail;                                                    if (!a64_emit(&buf, a64_str_d_imm((_sv)->reg, 31, (uint16_t)((_dst) + 8u)))) goto emit_fail;                          break;                                                                                                           case 1:                                                                                                              case 3:                                                                                                              case 4: {                                                                                                                uint16_t _src = (uint16_t)((uint16_t)(_sv)->slot * 16u);                                                              if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;                                                       if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, _src))) goto emit_fail;                                                       if (!a64_emit(&buf, a64_str_x_imm(9, 31, (_dst)))) goto emit_fail;                                                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, (uint16_t)(_src + 8u)))) goto emit_fail;                                      if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)((_dst) + 8u)))) goto emit_fail;                                   break;                                                                                                           }                                                                                                                    case 2:                                                                                                                  if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_OBJ)) goto emit_fail;                                                   if (!a64_emit(&buf, a64_str_x_imm(9, 31, (_dst)))) goto emit_fail;                                                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)AS_STRING((_sv)->constant))) goto emit_fail;                     if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)((_dst) + 8u)))) goto emit_fail;                                   break;                                                                                                           case 5: {                                                                                                                uint64_t _tag = (uint64_t)(_sv)->constant.type;                                                                       uint64_t _pay = 0u;                                                                                                   if (IS_BOOL((_sv)->constant)) _pay = (uint64_t)AS_BOOL((_sv)->constant);                                              else if (IS_OBJ((_sv)->constant)) _pay = (uint64_t)(uintptr_t)AS_OBJ((_sv)->constant);                                else if (IS_NUMBER((_sv)->constant)) {                                                                                    double _cd = AS_NUMBER((_sv)->constant);                                                                              uint64_t _bits = 0u;                                                                                                  memcpy(&_bits, &_cd, sizeof(_bits));                                                                                  _pay = _bits;                                                                                                     }                                                                                                                     if (!a64_emit_mov_imm64(&buf, 9, _tag)) goto emit_fail;                                                               if (!a64_emit(&buf, a64_str_x_imm(9, 31, (_dst)))) goto emit_fail;                                                    if (!a64_emit_mov_imm64(&buf, 9, _pay)) goto emit_fail;                                                               if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)((_dst) + 8u)))) goto emit_fail;                                   break;                                                                                                           }                                                                                                                    case 9: {                                                                                                                uint16_t _src = (uint16_t)(256u + (uint16_t)(_sv)->slot * 16u);                                                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, _src))) goto emit_fail;                                                      if (!a64_emit(&buf, a64_str_x_imm(9, 31, (_dst)))) goto emit_fail;                                                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, (uint16_t)(_src + 8u)))) goto emit_fail;                                     if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)((_dst) + 8u)))) goto emit_fail;                                   break;                                                                                                           }                                                                                                                    default:                                                                                                                 goto emit_fail; /* upvalue refs: leave those traces to the IR path */                                         }                                                                                                                } while (0)
#define WRITE_THROUGH_PENDING()                                                                                      \
    do {                                                                                                             \
        if (has_merge_points) {                                                                                      \
            for (int _wi = 0; _wi < vsp; _wi++) {                                                                    \
                SPILL_TRACE_VALUE(&tstack[_wi], (uint16_t)(256u + (uint16_t)_wi * 16u));                              \
            }                                                                                                        \
        }                                                                                                            \
    } while (0)
#define MERGE_TO_SCRATCH()                                                                                           \
    do {                                                                                                             \
        if (has_merge_points) {                                                                                      \
            for (int _mi = 0; _mi < vsp; _mi++) {                                                                    \
                if (tstack[_mi].kind == 0 && tstack[_mi].is_temp) {                                                   \
                    a64_free_temp(&temp_used, tstack[_mi].reg);                                                      \
                    tstack[_mi].is_temp = 0;                                                                         \
                }                                                                                                    \
                if (tstack[_mi].kind != 5) {                                                                         \
                    tstack[_mi].kind = 9;                                                                            \
                    tstack[_mi].slot = (uint8_t)_mi;                                                                 \
                }                                                                                                    \
            }                                                                                                        \
        }                                                                                                            \
    } while (0)

    uint16_t var_off = (uint16_t)((uint16_t)var_slot * (uint16_t)sizeof(Value) + (uint16_t)offsetof(Value, as.number));
    uint16_t end_off = (uint16_t)((uint16_t)end_slot * (uint16_t)sizeof(Value) + (uint16_t)offsetof(Value, as.number));
    uint16_t need_slots = (uint16_t)(((uint16_t)((var_slot > end_slot) ? var_slot : end_slot)) + 1u);

    // x0=vm x1=closure x2=slots x3=slot_count x4=out_state
    // Stack frame layout:
    //   [sp+0]   helper scratch (double/value)
    //   [sp+8]   helper scratch payload
    //   [sp+224] slot_count spill
    //   [sp+16]  closure spill
    //   [sp+24]  vm spill
    //   [sp+32..] build-string Value parts scratch
    //   [sp+176] build-string ObjString* out
    //   [sp+184] append temp table Value
    //   [sp+200] append temp value Value
    //   [sp+216] append out Value
    //   [sp+240] slots spill
    //   [sp+248] out_state spill
    //   [sp+256..2303] direct-core value scratch slots (128 x Value)
    //   [sp+2304..3071] call/invoke staging buffer (48 x Value)
    //   [sp+3072] saved LR (x30) for helper blr calls
    const uint16_t CALL_STAGE_BASE = 2304u;
    const int CALL_STAGE_CAP = 48;
    if (!a64_emit(&buf, a64_sub_imm(31, 31, 3088))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(30, 31, 3072))) goto emit_fail; // save LR
    if (!a64_emit(&buf, a64_str_x_imm(3, 31, 224))) goto emit_fail;  // spill slot_count
    if (!a64_emit(&buf, a64_str_x_imm(1, 31, 16))) goto emit_fail; // spill closure
    if (!a64_emit(&buf, a64_str_x_imm(0, 31, 24))) goto emit_fail; // spill vm
    if (!a64_emit(&buf, a64_str_x_imm(2, 31, 240))) goto emit_fail; // spill slots
    if (!a64_emit(&buf, a64_str_x_imm(4, 31, 248))) goto emit_fail; // spill out_state
    {
        // Seed out_state with a conservative runtime deopt marker so
        // direct-core deopt exits never report reason=none.
        uint64_t initial_deopt_state =
            ((uint64_t)instrs[0].deopt_offset) |
            ((uint64_t)instrs[0].op << 32) |
            ((uint64_t)TOI_JIT_NATIVE_DEOPT_RUNTIME << 40);
        if (!a64_emit_mov_imm64(&buf, 9, initial_deopt_state)) goto emit_fail;
        if (!a64_emit(&buf, a64_str_x_imm(9, 4, 0))) goto emit_fail;
    }
    if (!a64_emit(&buf, a64_cmp_w_imm(3, need_slots))) goto emit_fail;
    int b_bad_slot_count = buf.count;
    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.lt deopt

    int loop_header = buf.count;
    if (!a64_emit(&buf, a64_ldr_d_imm(4, 2, var_off))) goto emit_fail; // d4 = var
    if (!a64_emit(&buf, a64_ldr_d_imm(5, 2, end_off))) goto emit_fail; // d5 = end
    if (!a64_emit(&buf, 0x1e6e1006u)) goto emit_fail;                   // d6 = 1.0
    if (!a64_emit(&buf, a64_fadd(4, 4, 6))) goto emit_fail;             // var += 1
    if (!a64_emit(&buf, a64_str_d_imm(4, 2, var_off))) goto emit_fail;  // slots[var] = var
    if (!a64_emit(&buf, a64_fcmp(4, 5))) goto emit_fail;
    int b_var_gt_end = buf.count;
    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.gt exit

    if (debug_direct_core) {
        for (int _di = 0; _di < instr_count; _di++) {
            fprintf(stderr, "[dc] %3d op=%2u a=%u b=%u target=%u depth=%d\n",
                    _di, (unsigned)instrs[_di].op, (unsigned)instrs[_di].a,
                    (unsigned)instrs[_di].b, (unsigned)instrs[_di].target, entry_depth[_di]);
        }
    }
    for (int i = 0; i < instr_count; i++) {
        const ToiJitNativeTraceInstr* in = &instrs[i];
        // Keep the canonical slots in sync for this (fallthrough) edge; the fixup
        // must sit before the label so a jumping edge skips it.
        WRITE_THROUGH_PENDING();
        int target_vsp = entry_depth[i];
        if (target_vsp < 0) {
            // Unreachable in CFG; keep current state.
            target_vsp = vsp;
        }
        while (vsp > target_vsp) {
            A64TraceVal t = tstack[--vsp];
            if (t.is_temp) a64_free_temp(&temp_used, t.reg);
        }
        while (vsp < target_vsp) {
            if (vsp >= 256) goto emit_fail;
            tstack[vsp++] = (A64TraceVal){5, 0, 0, 0, NIL_VAL};
        }
        if (is_merge_label[i]) {
            // Both edges have their pending values in the canonical slots now; the
            // merged code can read them from there.
            MERGE_TO_SCRATCH();
        }
        insn_labels[i] = buf.count;
        fail_index = i;

        switch (in->op) {
            case TOI_JIT_NATIVE_OP_NOP:
                break;
            case TOI_JIT_NATIVE_OP_PUSH_LOCAL: {
                if (vsp >= 256) goto emit_fail;
                if (native_trace_has_table_shape_guard(in->a, 1, guards, guard_count) &&
                    !native_trace_has_local_number_guard(in->a, guards, guard_count) &&
                    in->a != var_slot && in->a != end_slot) {
                    tstack[vsp++] = (A64TraceVal){1, 0, in->a, 0, NIL_VAL};
                } else if (native_trace_has_local_string_guard(in->a, guards, guard_count)) {
                    tstack[vsp++] = (A64TraceVal){3, 0, in->a, 0, NIL_VAL};
                } else if (in->a != var_slot &&
                           in->a != end_slot &&
                           !native_trace_has_local_number_guard(in->a, guards, guard_count)) {
                    tstack[vsp++] = (A64TraceVal){4, 0, in->a, 0, NIL_VAL};
                } else {
                    uint8_t rd = a64_alloc_temp(&temp_used);
                    if (rd == 0) goto emit_fail;
                    uint16_t off = (uint16_t)((uint16_t)in->a * (uint16_t)sizeof(Value) + (uint16_t)offsetof(Value, as.number));
                    if (!a64_emit(&buf, a64_ldr_d_imm(rd, 2, off))) goto emit_fail;
                    tstack[vsp++] = (A64TraceVal){0, rd, 0, 1, NIL_VAL};
                }
                break;
            }
            case TOI_JIT_NATIVE_OP_PUSH_UPVALUE: {
                if (vsp >= 256) goto emit_fail;
                if (native_trace_has_upvalue_number_guard(in->a, guards, guard_count)) {
                    uint8_t rd = a64_alloc_temp(&temp_used);
                    if (rd == 0) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                    if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)in->a)) goto emit_fail; // x1 = upvalue slot
                    if (!a64_emit(&buf, a64_add_imm(2, 31, 0))) goto emit_fail; // x2 = &scratch
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_upvalue_slot_as_number)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    if (deopt_branch_count >= 256) goto emit_fail;
                    deopt_branches[deopt_branch_count++] = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq deopt
                    if (!a64_emit(&buf, a64_ldr_d_imm(rd, 31, 0))) goto emit_fail;
                    tstack[vsp++] = (A64TraceVal){0, rd, 0, 1, NIL_VAL};
                } else if (native_trace_has_upvalue_table_shape_guard(in->a, 1, guards, guard_count)) {
                    tstack[vsp++] = (A64TraceVal){7, 0, in->a, 0, NIL_VAL};
                } else if (native_trace_has_upvalue_string_guard(in->a, guards, guard_count)) {
                    tstack[vsp++] = (A64TraceVal){8, 0, in->a, 0, NIL_VAL};
                } else {
                    tstack[vsp++] = (A64TraceVal){6, 0, in->a, 0, NIL_VAL};
                }
                break;
            }
            case TOI_JIT_NATIVE_OP_PUSH_CONST: {
                if (vsp >= 256) goto emit_fail;
                if (IS_STRING(in->constant)) {
                    tstack[vsp++] = (A64TraceVal){2, 0, 0, 0, in->constant};
                } else {
                    uint8_t rd = a64_alloc_temp(&temp_used);
                    if (rd == 0) goto emit_fail;
                    double d = AS_NUMBER(in->constant);
                    if (d == 1.0) {
                        if (!a64_emit(&buf, 0x1e6e1000u | rd)) goto emit_fail; // fmov d<rd>, #1.0
                    } else if (d == 0.0) {
                        if (!a64_emit(&buf, a64_fsub(rd, rd, rd))) goto emit_fail; // d<rd> = 0.0
                    } else {
                        union { double d; uint64_t u; } bits;
                        bits.d = d;
                        if (!a64_emit_mov_imm64(&buf, 9, bits.u)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 0))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_d_imm(rd, 31, 0))) goto emit_fail;
                    }
                    tstack[vsp++] = (A64TraceVal){0, rd, 0, 1, in->constant};
                }
                break;
            }
            case TOI_JIT_NATIVE_OP_PUSH_NIL:
            case TOI_JIT_NATIVE_OP_PUSH_TRUE:
            case TOI_JIT_NATIVE_OP_PUSH_FALSE: {
                if (vsp >= 256) goto emit_fail;
                Value c = (in->op == TOI_JIT_NATIVE_OP_PUSH_NIL) ? NIL_VAL :
                          (in->op == TOI_JIT_NATIVE_OP_PUSH_TRUE) ? BOOL_VAL(true) :
                          BOOL_VAL(false);
                tstack[vsp++] = (A64TraceVal){5, 0, 0, 0, c};
                break;
            }
            case TOI_JIT_NATIVE_OP_STORE_LOCAL: {
                if (vsp < 1) goto emit_fail;
                A64TraceVal t = tstack[vsp - 1];
                uint16_t off = (uint16_t)((uint16_t)in->a * (uint16_t)sizeof(Value));
                if (t.kind == 0) {
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_NUMBER)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, off))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_d_imm(t.reg, 2, (uint16_t)(off + 8)))) goto emit_fail;
                } else if (t.kind == 3 || t.kind == 4 || t.kind == 1) {
                    uint16_t src = (uint16_t)((uint16_t)t.slot * (uint16_t)sizeof(Value));
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, src))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, off))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, (uint16_t)(src + 8)))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, (uint16_t)(off + 8)))) goto emit_fail;
                } else if (t.kind == 6 || t.kind == 7 || t.kind == 8) {
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                    if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)t.slot)) goto emit_fail; // x1 = upvalue slot
                    if (!a64_emit(&buf, a64_add_imm(2, 31, 0))) goto emit_fail; // x2 = &scratch Value
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_upvalue_slot_as_value)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    if (deopt_branch_count >= 256) goto emit_fail;
                    deopt_branches[deopt_branch_count++] = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq deopt
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 0))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, off))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 8))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, (uint16_t)(off + 8)))) goto emit_fail;
                } else if (t.kind == 9) {
                    uint16_t src = (uint16_t)(256 + (uint16_t)t.slot * 16u);
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, src))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, off))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, (uint16_t)(src + 8u)))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, (uint16_t)(off + 8)))) goto emit_fail;
                } else if (t.kind == 2 && IS_STRING(t.constant)) {
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_OBJ)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, off))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)AS_STRING(t.constant))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, (uint16_t)(off + 8)))) goto emit_fail;
                } else if (t.kind == 5) {
                    uint64_t tag = (uint64_t)t.constant.type;
                    uint64_t payload = 0u;
                    if (IS_BOOL(t.constant)) payload = (uint64_t)AS_BOOL(t.constant);
                    else if (IS_OBJ(t.constant)) payload = (uint64_t)(uintptr_t)AS_OBJ(t.constant);
                    if (!a64_emit_mov_imm64(&buf, 9, tag)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, off))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, payload)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, (uint16_t)(off + 8)))) goto emit_fail;
                } else {
                    goto emit_fail;
                }
                break;
            }
            case TOI_JIT_NATIVE_OP_POP: {
                if (vsp < 1) goto emit_fail;
                A64TraceVal t = tstack[--vsp];
                if (t.is_temp) a64_free_temp(&temp_used, t.reg);
                break;
            }
            case TOI_JIT_NATIVE_OP_BIN_ADD:
            case TOI_JIT_NATIVE_OP_BIN_SUB:
            case TOI_JIT_NATIVE_OP_BIN_MUL:
            case TOI_JIT_NATIVE_OP_BIN_DIV:
            case TOI_JIT_NATIVE_OP_BIN_MOD: {
                if (vsp < 2) goto emit_fail;
                A64TraceVal b = tstack[--vsp];
                A64TraceVal a = tstack[--vsp];
                if (a.kind != 0) {
                    uint8_t ar = a64_alloc_temp(&temp_used);
                    if (ar == 0) goto emit_fail;
                    if (a.kind == 4 || a.kind == 3 || a.kind == 1) {
                        if (!a64_emit(&buf, a64_mov_x(0, 2))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)a.slot)) goto emit_fail;
                        if (!a64_emit(&buf, a64_add_imm(3, 31, 0))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_number_slot)) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_d_imm(ar, 31, 0))) goto emit_fail;
                    } else if (a.kind == 6 || a.kind == 7 || a.kind == 8) {
                        if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)a.slot)) goto emit_fail;
                        if (!a64_emit(&buf, a64_add_imm(2, 31, 0))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_number_upvalue)) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_d_imm(ar, 31, 0))) goto emit_fail;
                    } else if (a.kind == 9) {
                        uint16_t src = (uint16_t)(256 + (uint16_t)a.slot * 16u);
                        if (!a64_emit(&buf, a64_add_imm(0, 31, src))) goto emit_fail;
                        if (!a64_emit(&buf, a64_add_imm(1, 31, 0))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_number_value_ptr)) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_d_imm(ar, 31, 0))) goto emit_fail;
                    } else if (a.kind == 5 && IS_NUMBER(a.constant)) {
                        double d = AS_NUMBER(a.constant);
                        if (d == 1.0) {
                            if (!a64_emit(&buf, 0x1e6e1000u | ar)) goto emit_fail;
                        } else if (d == 0.0) {
                            if (!a64_emit(&buf, a64_fsub(ar, ar, ar))) goto emit_fail;
                        } else {
                            union { double d; uint64_t u; } bits;
                            bits.d = d;
                            if (!a64_emit_mov_imm64(&buf, 9, bits.u)) goto emit_fail;
                            if (!a64_emit(&buf, a64_str_x_imm(9, 31, 0))) goto emit_fail;
                            if (!a64_emit(&buf, a64_ldr_d_imm(ar, 31, 0))) goto emit_fail;
                        }
                    } else {
                        goto emit_fail;
                    }
                    if (a.is_temp) a64_free_temp(&temp_used, a.reg);
                    a = (A64TraceVal){0, ar, 0, 1, NIL_VAL};
                }
                if (b.kind != 0) {
                    uint8_t br = a64_alloc_temp(&temp_used);
                    if (br == 0) goto emit_fail;
                    if (b.kind == 4 || b.kind == 3 || b.kind == 1) {
                        if (!a64_emit(&buf, a64_mov_x(0, 2))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)b.slot)) goto emit_fail;
                        if (!a64_emit(&buf, a64_add_imm(3, 31, 8))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_number_slot)) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_d_imm(br, 31, 8))) goto emit_fail;
                    } else if (b.kind == 6 || b.kind == 7 || b.kind == 8) {
                        if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)b.slot)) goto emit_fail;
                        if (!a64_emit(&buf, a64_add_imm(2, 31, 8))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_number_upvalue)) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_d_imm(br, 31, 8))) goto emit_fail;
                    } else if (b.kind == 9) {
                        uint16_t src = (uint16_t)(256 + (uint16_t)b.slot * 16u);
                        if (!a64_emit(&buf, a64_add_imm(0, 31, src))) goto emit_fail;
                        if (!a64_emit(&buf, a64_add_imm(1, 31, 8))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_number_value_ptr)) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_d_imm(br, 31, 8))) goto emit_fail;
                    } else if (b.kind == 5 && IS_NUMBER(b.constant)) {
                        double d = AS_NUMBER(b.constant);
                        if (d == 1.0) {
                            if (!a64_emit(&buf, 0x1e6e1000u | br)) goto emit_fail;
                        } else if (d == 0.0) {
                            if (!a64_emit(&buf, a64_fsub(br, br, br))) goto emit_fail;
                        } else {
                            union { double d; uint64_t u; } bits;
                            bits.d = d;
                            if (!a64_emit_mov_imm64(&buf, 9, bits.u)) goto emit_fail;
                            if (!a64_emit(&buf, a64_str_x_imm(9, 31, 8))) goto emit_fail;
                            if (!a64_emit(&buf, a64_ldr_d_imm(br, 31, 8))) goto emit_fail;
                        }
                    } else {
                        goto emit_fail;
                    }
                    if (b.is_temp) a64_free_temp(&temp_used, b.reg);
                    b = (A64TraceVal){0, br, 0, 1, NIL_VAL};
                }
                uint8_t rd = a64_alloc_temp(&temp_used);
                if (rd == 0) goto emit_fail;
                if (in->op == TOI_JIT_NATIVE_OP_BIN_ADD) {
                    if (!a64_emit(&buf, a64_fadd(rd, a.reg, b.reg))) goto emit_fail;
                } else if (in->op == TOI_JIT_NATIVE_OP_BIN_SUB) {
                    if (!a64_emit(&buf, a64_fsub(rd, a.reg, b.reg))) goto emit_fail;
                } else if (in->op == TOI_JIT_NATIVE_OP_BIN_MUL) {
                    if (!a64_emit(&buf, a64_fmul(rd, a.reg, b.reg))) goto emit_fail;
                } else if (in->op == TOI_JIT_NATIVE_OP_BIN_DIV) {
                    if (!a64_emit(&buf, a64_fdiv(rd, a.reg, b.reg))) goto emit_fail;
                } else {
                    uint8_t q = a64_alloc_temp(&temp_used);
                    if (q == 0) goto emit_fail;
                    if (!a64_emit(&buf, a64_fdiv(q, a.reg, b.reg))) goto emit_fail;
                    if (!a64_emit(&buf, a64_frintz(q, q))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fmul(q, q, b.reg))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fsub(rd, a.reg, q))) goto emit_fail;
                    a64_free_temp(&temp_used, q);
                }
                if (a.is_temp) a64_free_temp(&temp_used, a.reg);
                if (b.is_temp) a64_free_temp(&temp_used, b.reg);
                tstack[vsp++] = (A64TraceVal){0, rd, 0, 1, NIL_VAL};
                break;
            }
            case TOI_JIT_NATIVE_OP_CMP_LT:
            case TOI_JIT_NATIVE_OP_CMP_GT:
            case TOI_JIT_NATIVE_OP_CMP_EQ: {
                if (vsp < 2) goto emit_fail;
                A64TraceVal b = tstack[--vsp];
                A64TraceVal a = tstack[--vsp];
                uint8_t rd = a64_alloc_temp(&temp_used);
                if (rd == 0) goto emit_fail;
                if (a.kind == 0 && b.kind == 0) {
                    // Skip the "true" assignment when the comparison does not hold.
                    // (EQ -> ne, LT -> ge, GT -> le; ge/le also cover unordered,
                    // which is correct because lt/gt are false for NaN.)
                    uint32_t skip_cond = (in->op == TOI_JIT_NATIVE_OP_CMP_EQ) ? 0x1u
                                       : (in->op == TOI_JIT_NATIVE_OP_CMP_LT) ? 0xAu
                                                                             : 0xDu;
                    if (!a64_emit(&buf, a64_fsub(rd, rd, rd))) goto emit_fail; // rd = 0.0
                    if (!a64_emit(&buf, a64_fcmp(a.reg, b.reg))) goto emit_fail;
                    int bfalse = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    if (!a64_emit(&buf, 0x1e6e1000u | rd)) goto emit_fail; // rd = 1.0
                    int false_label = buf.count;
                    if (!a64_patch_b_cond(&buf, bfalse, false_label, skip_cond)) goto emit_fail;
                } else if (in->op != TOI_JIT_NATIVE_OP_CMP_EQ) {
                    // The slot/const and generic Value paths below only implement
                    // equality; leave ordering comparisons on the IR path.
                    goto emit_fail;
                } else if (a.kind == 4 && b.kind == 0 && IS_NUMBER(b.constant)) {
                    if (!a64_emit(&buf, a64_mov_x(0, 2))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)a.slot)) goto emit_fail;
                    if (!a64_emit(&buf, a64_fmov_rr(0, b.reg))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_cmp_eq_slot_const_number)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fsub(rd, rd, rd))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    int bfalse = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq false
                    if (!a64_emit(&buf, 0x1e6e1000u | rd)) goto emit_fail;
                    int done = buf.count;
                    if (!a64_patch_b_cond(&buf, bfalse, done, 0x0u)) goto emit_fail;
                } else if (a.kind == 3 && b.kind == 2 && IS_STRING(b.constant)) {
                    if (!a64_emit(&buf, a64_mov_x(0, 2))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)a.slot)) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 3, (uint64_t)(uintptr_t)AS_STRING(b.constant))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_cmp_eq_slot_const_string)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fsub(rd, rd, rd))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    int bfalse = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    if (!a64_emit(&buf, 0x1e6e1000u | rd)) goto emit_fail;
                    int done = buf.count;
                    if (!a64_patch_b_cond(&buf, bfalse, done, 0x0u)) goto emit_fail;
                } else {
                    // Generic Value equality fallback: materialize both operands and compare.
                    // a -> [sp+184], b -> [sp+200]
                    if (a.kind == 4 || a.kind == 3 || a.kind == 1) {
                        uint16_t src = (uint16_t)((uint16_t)a.slot * (uint16_t)sizeof(Value));
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, src))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 184))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, (uint16_t)(src + 8)))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 192))) goto emit_fail;
                    } else if (a.kind == 6 || a.kind == 7 || a.kind == 8) {
                        if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)a.slot)) goto emit_fail;
                        if (!a64_emit(&buf, a64_add_imm(2, 31, 184))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_upvalue_slot_as_value)) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    } else if (a.kind == 9) {
                        uint16_t src = (uint16_t)(256 + (uint16_t)a.slot * 16u);
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, src))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 184))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, (uint16_t)(src + 8u)))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 192))) goto emit_fail;
                    } else if (a.kind == 2 && IS_STRING(a.constant)) {
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_OBJ)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 184))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)AS_STRING(a.constant))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 192))) goto emit_fail;
                    } else if (a.kind == 0) {
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_NUMBER)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 184))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_d_imm(a.reg, 31, 192))) goto emit_fail;
                    } else if (a.kind == 5) {
                        uint64_t tag = (uint64_t)a.constant.type;
                        uint64_t payload = 0u;
                        if (IS_BOOL(a.constant)) payload = (uint64_t)AS_BOOL(a.constant);
                        else if (IS_OBJ(a.constant)) payload = (uint64_t)(uintptr_t)AS_OBJ(a.constant);
                        if (!a64_emit_mov_imm64(&buf, 9, tag)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 184))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, payload)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 192))) goto emit_fail;
                    } else {
                        goto emit_fail;
                    }

                    if (b.kind == 4 || b.kind == 3 || b.kind == 1) {
                        uint16_t src = (uint16_t)((uint16_t)b.slot * (uint16_t)sizeof(Value));
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, src))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, (uint16_t)(src + 8)))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                    } else if (b.kind == 6 || b.kind == 7 || b.kind == 8) {
                        if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)b.slot)) goto emit_fail;
                        if (!a64_emit(&buf, a64_add_imm(2, 31, 200))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_upvalue_slot_as_value)) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    } else if (b.kind == 9) {
                        uint16_t src = (uint16_t)(256 + (uint16_t)b.slot * 16u);
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, src))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, (uint16_t)(src + 8u)))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                    } else if (b.kind == 2 && IS_STRING(b.constant)) {
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_OBJ)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)AS_STRING(b.constant))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                    } else if (b.kind == 0) {
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_NUMBER)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_d_imm(b.reg, 31, 208))) goto emit_fail;
                    } else if (b.kind == 5) {
                        uint64_t tag = (uint64_t)b.constant.type;
                        uint64_t payload = 0u;
                        if (IS_BOOL(b.constant)) payload = (uint64_t)AS_BOOL(b.constant);
                        else if (IS_OBJ(b.constant)) payload = (uint64_t)(uintptr_t)AS_OBJ(b.constant);
                        if (!a64_emit_mov_imm64(&buf, 9, tag)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, payload)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                    } else {
                        goto emit_fail;
                    }

                    if (!a64_emit(&buf, a64_add_imm(0, 31, 184))) goto emit_fail;
                    if (!a64_emit(&buf, a64_add_imm(1, 31, 200))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_values_equal_ptr)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fsub(rd, rd, rd))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    int bfalse = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    if (!a64_emit(&buf, 0x1e6e1000u | rd)) goto emit_fail;
                    int done = buf.count;
                    if (!a64_patch_b_cond(&buf, bfalse, done, 0x0u)) goto emit_fail;
                }
                if (a.is_temp) a64_free_temp(&temp_used, a.reg);
                if (b.is_temp) a64_free_temp(&temp_used, b.reg);
                tstack[vsp++] = (A64TraceVal){0, rd, 0, 1, NIL_VAL};
                break;
            }
            case TOI_JIT_NATIVE_OP_NOT: {
                if (vsp < 1) goto emit_fail;
                A64TraceVal t = tstack[vsp - 1];
                if (t.kind == 0) {
                    uint8_t z = a64_alloc_temp(&temp_used);
                    if (z == 0) goto emit_fail;
                    if (!a64_emit(&buf, a64_fsub(z, z, z))) goto emit_fail; // z = 0
                    if (!a64_emit(&buf, a64_fcmp(t.reg, z))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fsub(t.reg, t.reg, t.reg))) goto emit_fail; // t = 0.0
                    int bne = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // if t != 0 keep 0
                    if (!a64_emit(&buf, 0x1e6e1000u | t.reg)) goto emit_fail; // else t = 1
                    int done = buf.count;
                    if (!a64_patch_b_cond(&buf, bne, done, 0x1u)) goto emit_fail; // b.ne done
                    a64_free_temp(&temp_used, z);
                } else if (t.kind == 4 || t.kind == 3 || t.kind == 1) {
                    uint8_t rd = a64_alloc_temp(&temp_used);
                    if (rd == 0) goto emit_fail;
                    if (!a64_emit(&buf, a64_mov_x(0, 2))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)t.slot)) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_value_falsey_slot)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fsub(rd, rd, rd))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    int bfalse = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    if (!a64_emit(&buf, 0x1e6e1000u | rd)) goto emit_fail;
                    int done = buf.count;
                    if (!a64_patch_b_cond(&buf, bfalse, done, 0x0u)) goto emit_fail;
                    tstack[vsp - 1] = (A64TraceVal){0, rd, 0, 1, NIL_VAL};
                } else if (t.kind == 9) {
                    uint8_t rd = a64_alloc_temp(&temp_used);
                    if (rd == 0) goto emit_fail;
                    uint16_t src = (uint16_t)(256 + (uint16_t)t.slot * 16u);
                    if (!a64_emit(&buf, a64_add_imm(0, 31, src))) goto emit_fail; // x0 = &value
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_value_falsey_value_ptr)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fsub(rd, rd, rd))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    int bfalse = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    if (!a64_emit(&buf, 0x1e6e1000u | rd)) goto emit_fail;
                    int done = buf.count;
                    if (!a64_patch_b_cond(&buf, bfalse, done, 0x0u)) goto emit_fail;
                    tstack[vsp - 1] = (A64TraceVal){0, rd, 0, 1, NIL_VAL};
                } else if (t.kind == 6 || t.kind == 7 || t.kind == 8) {
                    uint8_t rd = a64_alloc_temp(&temp_used);
                    if (rd == 0) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                    if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)t.slot)) goto emit_fail; // x1 = upvalue slot
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_value_falsey_upvalue)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fsub(rd, rd, rd))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    int bfalse = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    if (!a64_emit(&buf, 0x1e6e1000u | rd)) goto emit_fail;
                    int done = buf.count;
                    if (!a64_patch_b_cond(&buf, bfalse, done, 0x0u)) goto emit_fail;
                    tstack[vsp - 1] = (A64TraceVal){0, rd, 0, 1, NIL_VAL};
                } else if (t.kind == 5) {
                    uint8_t rd = a64_alloc_temp(&temp_used);
                    if (rd == 0) goto emit_fail;
                    int is_falsey = jit_value_falsey(t.constant) ? 1 : 0;
                    if (is_falsey) {
                        if (!a64_emit(&buf, 0x1e6e1000u | rd)) goto emit_fail; // 1.0
                    } else {
                        if (!a64_emit(&buf, a64_fsub(rd, rd, rd))) goto emit_fail; // 0.0
                    }
                    tstack[vsp - 1] = (A64TraceVal){0, rd, 0, 1, NIL_VAL};
                } else {
                    goto emit_fail;
                }
                break;
            }
            case TOI_JIT_NATIVE_OP_APPEND: {
                if (vsp < 2) goto emit_fail;
                int fused_store = (i + 1 < instr_count && instrs[i + 1].op == TOI_JIT_NATIVE_OP_STORE_LOCAL);
                uint8_t dst_slot = fused_store ? instrs[i + 1].a : 0;

                A64TraceVal value = tstack[--vsp];
                A64TraceVal table = tstack[--vsp];

                // table -> [sp+184]
                if (table.kind == 4 || table.kind == 3 || table.kind == 1) {
                    uint16_t src = (uint16_t)((uint16_t)table.slot * (uint16_t)sizeof(Value));
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, src))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 184))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, (uint16_t)(src + 8)))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 192))) goto emit_fail;
                } else if (table.kind == 6 || table.kind == 7 || table.kind == 8) {
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                    if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)table.slot)) goto emit_fail; // x1 = upvalue slot
                    if (!a64_emit(&buf, a64_add_imm(2, 31, 184))) goto emit_fail; // x2 = &table temp
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_upvalue_slot_as_value)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    if (deopt_branch_count >= 256) goto emit_fail;
                    deopt_branches[deopt_branch_count++] = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                } else if (table.kind == 9) {
                    uint16_t src = (uint16_t)(256 + (uint16_t)table.slot * 16u);
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, src))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 184))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, (uint16_t)(src + 8u)))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 192))) goto emit_fail;
                } else if (table.kind == 2 && IS_STRING(table.constant)) {
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_OBJ)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 184))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)AS_STRING(table.constant))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 192))) goto emit_fail;
                } else if (table.kind == 0) {
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_NUMBER)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 184))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_d_imm(table.reg, 31, 192))) goto emit_fail;
                } else if (table.kind == 5) {
                    uint64_t tag = (uint64_t)table.constant.type;
                    uint64_t payload = 0u;
                    if (IS_BOOL(table.constant)) payload = (uint64_t)AS_BOOL(table.constant);
                    else if (IS_OBJ(table.constant)) payload = (uint64_t)(uintptr_t)AS_OBJ(table.constant);
                    if (!a64_emit_mov_imm64(&buf, 9, tag)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 184))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, payload)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 192))) goto emit_fail;
                } else {
                    goto emit_fail;
                }

                // value -> [sp+200]
                if (value.kind == 4 || value.kind == 3 || value.kind == 1) {
                    uint16_t src = (uint16_t)((uint16_t)value.slot * (uint16_t)sizeof(Value));
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, src))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, (uint16_t)(src + 8)))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                } else if (value.kind == 6 || value.kind == 7 || value.kind == 8) {
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                    if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)value.slot)) goto emit_fail; // x1 = upvalue slot
                    if (!a64_emit(&buf, a64_add_imm(2, 31, 200))) goto emit_fail; // x2 = &value temp
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_upvalue_slot_as_value)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    if (deopt_branch_count >= 256) goto emit_fail;
                    deopt_branches[deopt_branch_count++] = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                } else if (value.kind == 9) {
                    uint16_t src = (uint16_t)(256 + (uint16_t)value.slot * 16u);
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, src))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, (uint16_t)(src + 8u)))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                } else if (value.kind == 2 && IS_STRING(value.constant)) {
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_OBJ)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)AS_STRING(value.constant))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                } else if (value.kind == 0) {
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_NUMBER)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_d_imm(value.reg, 31, 208))) goto emit_fail;
                } else if (value.kind == 5) {
                    uint64_t tag = (uint64_t)value.constant.type;
                    uint64_t payload = 0u;
                    if (IS_BOOL(value.constant)) payload = (uint64_t)AS_BOOL(value.constant);
                    else if (IS_OBJ(value.constant)) payload = (uint64_t)(uintptr_t)AS_OBJ(value.constant);
                    if (!a64_emit_mov_imm64(&buf, 9, tag)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, payload)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                } else {
                    goto emit_fail;
                }

                if (!a64_emit(&buf, a64_add_imm(0, 31, 184))) goto emit_fail; // &table value
                if (!a64_emit(&buf, a64_add_imm(1, 31, 200))) goto emit_fail; // &append value
                if (!a64_emit(&buf, a64_add_imm(2, 31, 216))) goto emit_fail; // &out value
                if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_append_from_values)) goto emit_fail;
                if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                if (deopt_branch_count >= 256) goto emit_fail;
                deopt_branches[deopt_branch_count++] = buf.count;
                if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;

                if (fused_store) {
                    // out value -> destination local
                    uint16_t dst = (uint16_t)((uint16_t)dst_slot * (uint16_t)sizeof(Value));
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 216))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, dst))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 224))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, (uint16_t)(dst + 8)))) goto emit_fail;
                } else {
                    // out value -> value scratch slot for stack result
                    if (vsp >= 128) goto emit_fail;
                    uint16_t sdst = (uint16_t)(256 + (uint16_t)vsp * 16u);
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 216))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, sdst))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 224))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(sdst + 8u)))) goto emit_fail;
                }

                if (table.is_temp) a64_free_temp(&temp_used, table.reg);
                if (value.is_temp) a64_free_temp(&temp_used, value.reg);
                if (fused_store) {
                    tstack[vsp++] = (A64TraceVal){4, 0, dst_slot, 0, NIL_VAL};
                    i++; // consume fused STORE_LOCAL
                } else {
                    uint8_t sval = (uint8_t)vsp;
                    tstack[vsp++] = (A64TraceVal){9, 0, sval, 0, NIL_VAL};
                }
                break;
            }
            case TOI_JIT_NATIVE_OP_GET_TABLE_NUM: {
                if (vsp < 2) goto emit_fail;
                A64TraceVal key = tstack[--vsp];
                A64TraceVal table = tstack[--vsp];
                if (key.kind != 0 || (table.kind != 1 && table.kind != 7)) goto emit_fail;
                if (i + 1 < instr_count && instrs[i + 1].op == TOI_JIT_NATIVE_OP_STORE_LOCAL) {
                    uint8_t dst_slot = instrs[i + 1].a;
                    if (table.kind == 1) {
                        int inline_fallback_br[12];
                        uint8_t inline_fallback_cond[12];
                        int inline_fallback_count = 0;
                        int inline_done_br = -1;

                        uint16_t table_off = (uint16_t)((uint16_t)table.slot * (uint16_t)sizeof(Value));
                        uint16_t table_obj_off = (uint16_t)(table_off + 8u);
                        uint16_t off_obj_type = (uint16_t)offsetof(struct Obj, type);
                        uint16_t off_obj_nmfast = (uint16_t)offsetof(ObjTable, no_metatable_fast);
                        uint16_t off_table_array = (uint16_t)(offsetof(ObjTable, table) + offsetof(Table, array));
                        uint16_t off_table_array_cap = (uint16_t)(offsetof(ObjTable, table) + offsetof(Table, array_capacity));

                        // Fast inline path for positive integer keys in table array; fallback to helper otherwise.
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, table_off))) goto emit_fail; // x9 = table Value.type
                        if (!a64_emit(&buf, a64_cmp_w_imm(9, (uint16_t)VAL_OBJ))) goto emit_fail;
                        if (inline_fallback_count >= 12) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, table_obj_off))) goto emit_fail; // x9 = Obj*
                        if (!a64_emit(&buf, a64_ldr_w_imm(10, 9, off_obj_type))) goto emit_fail; // w10 = obj type
                        if (!a64_emit(&buf, a64_cmp_w_imm(10, (uint16_t)OBJ_TABLE))) goto emit_fail;
                        if (inline_fallback_count >= 12) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_ldr_b_imm(10, 9, off_obj_nmfast))) goto emit_fail; // w10 = no_metatable_fast
                        if (!a64_emit(&buf, a64_cmp_w_imm(10, 0))) goto emit_fail;
                        if (inline_fallback_count >= 12) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                        if (!a64_emit(&buf, a64_fcvtzs_x_d(11, key.reg))) goto emit_fail; // x11 = (int64)key
                        if (!a64_emit(&buf, a64_scvtf_d_x(1, 11))) goto emit_fail;        // d1 = (double)x11
                        if (!a64_emit(&buf, a64_fcmp(1, key.reg))) goto emit_fail;
                        if (inline_fallback_count >= 12) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_subs_imm(31, 11, 1))) goto emit_fail; // cmp x11, #1
                        if (inline_fallback_count >= 12) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0xBu;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.lt fallback

                        if (!a64_emit(&buf, a64_ldr_w_imm(12, 9, off_table_array_cap))) goto emit_fail; // w12 = array_capacity
                        if (!a64_emit(&buf, a64_subs_reg(31, 11, 12))) goto emit_fail; // cmp x11, x12
                        if (inline_fallback_count >= 12) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0xCu;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.gt fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(13, 9, off_table_array))) goto emit_fail; // x13 = array ptr
                        if (!a64_emit(&buf, a64_sub_imm(14, 11, 1))) goto emit_fail; // x14 = idx-1
                        if (!a64_emit(&buf, a64_add_shifted(14, 13, 14, 4))) goto emit_fail; // x14 = &array[idx-1]
                        if (!a64_emit(&buf, a64_ldr_x_imm(15, 14, 0))) goto emit_fail; // x15 = elem.type
                        if (!a64_emit(&buf, a64_cmp_w_imm(15, (uint16_t)VAL_NIL))) goto emit_fail;
                        if (inline_fallback_count >= 12) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback
                        if (!a64_emit(&buf, a64_ldr_x_imm(16, 14, 8))) goto emit_fail; // x16 = elem.payload

                        {
                            uint16_t dst = (uint16_t)((uint16_t)dst_slot * (uint16_t)sizeof(Value));
                            if (!a64_emit(&buf, a64_str_x_imm(15, 2, dst))) goto emit_fail;
                            if (!a64_emit(&buf, a64_str_x_imm(16, 2, (uint16_t)(dst + 8)))) goto emit_fail;
                        }
                        inline_done_br = buf.count;
                        if (!a64_emit(&buf, 0x14000000u)) goto emit_fail; // b done-inline

                        int inline_fallback_label = buf.count;
                        for (int fb = 0; fb < inline_fallback_count; fb++) {
                            if (!a64_patch_b_cond(&buf, inline_fallback_br[fb], inline_fallback_label, inline_fallback_cond[fb])) goto emit_fail;
                        }

                        if (!a64_emit(&buf, a64_mov_x(0, 2))) goto emit_fail; // x0 = slots
                        if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail; // x1 = slot_count
                        if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)table.slot)) goto emit_fail; // x2 = table slot
                        if (!a64_emit(&buf, a64_fmov_rr(0, key.reg))) goto emit_fail; // d0 = key
                        if (!a64_emit(&buf, a64_add_imm(3, 31, 0))) goto emit_fail; // x3 = &Value scratch
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_num_slot_as_value)) goto emit_fail;

                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq deopt
                        {
                            uint16_t dst = (uint16_t)((uint16_t)dst_slot * (uint16_t)sizeof(Value));
                            if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 0))) goto emit_fail;
                            if (!a64_emit(&buf, a64_str_x_imm(9, 2, dst))) goto emit_fail;
                            if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 8))) goto emit_fail;
                            if (!a64_emit(&buf, a64_str_x_imm(9, 2, (uint16_t)(dst + 8)))) goto emit_fail;
                        }

                        int inline_done_label = buf.count;
                        if (!a64_patch_b(&buf, inline_done_br, inline_done_label)) goto emit_fail;
                    } else {
                        if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                        if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)table.slot)) goto emit_fail; // x1 = upvalue slot
                        if (!a64_emit(&buf, a64_fmov_rr(0, key.reg))) goto emit_fail; // d0 = key
                        if (!a64_emit(&buf, a64_add_imm(2, 31, 0))) goto emit_fail; // x2 = &Value scratch
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_num_upvalue_as_value)) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq deopt
                        uint16_t dst = (uint16_t)((uint16_t)dst_slot * (uint16_t)sizeof(Value));
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 0))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 2, dst))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 8))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 2, (uint16_t)(dst + 8)))) goto emit_fail;
                    }
                    if (key.is_temp) a64_free_temp(&temp_used, key.reg);
                    tstack[vsp++] = (A64TraceVal){4, 0, dst_slot, 0, NIL_VAL};
                    i++;
                    break;
                }
                if (table.kind == 1) {
                    if (!a64_emit(&buf, a64_mov_x(0, 2))) goto emit_fail; // x0 = slots
                    if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail; // x1 = slot_count
                    if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)table.slot)) goto emit_fail; // x2 = table slot
                    if (!a64_emit(&buf, a64_fmov_rr(0, key.reg))) goto emit_fail; // d0 = key
                    if (!a64_emit(&buf, a64_add_imm(3, 31, 0))) goto emit_fail; // x3 = &scratch
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_num_slot_as_number)) goto emit_fail;
                } else {
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                    if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)table.slot)) goto emit_fail; // x1 = upvalue slot
                    if (!a64_emit(&buf, a64_fmov_rr(0, key.reg))) goto emit_fail; // d0 = key
                    if (!a64_emit(&buf, a64_add_imm(2, 31, 0))) goto emit_fail; // x2 = &scratch
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_num_upvalue_as_number)) goto emit_fail;
                }
                if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                if (deopt_branch_count >= 256) goto emit_fail;
                deopt_branches[deopt_branch_count++] = buf.count;
                if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq deopt
                uint8_t rd = a64_alloc_temp(&temp_used);
                if (rd == 0) goto emit_fail;
                if (!a64_emit(&buf, a64_ldr_d_imm(rd, 31, 0))) goto emit_fail;
                if (key.is_temp) a64_free_temp(&temp_used, key.reg);
                tstack[vsp++] = (A64TraceVal){0, rd, 0, 1, NIL_VAL};
                break;
            }
            case TOI_JIT_NATIVE_OP_GET_TABLE: {
                if (vsp < 2) goto emit_fail;
                A64TraceVal key = tstack[--vsp];
                A64TraceVal table = tstack[--vsp];
                if (table.kind != 1 && table.kind != 7) goto emit_fail;
                if (i + 1 < instr_count && instrs[i + 1].op == TOI_JIT_NATIVE_OP_STORE_LOCAL) {
                    uint8_t dst_slot = instrs[i + 1].a;
                    if (table.kind == 1) {
                        if (!a64_emit(&buf, a64_mov_x(0, 2))) goto emit_fail; // x0 = slots
                        if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail; // x1 = slot_count
                        if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)table.slot)) goto emit_fail; // x2 = table slot
                        if (key.kind == 2 && IS_STRING(key.constant)) {
                            int inline_fallback_br[16];
                            uint8_t inline_fallback_cond[16];
                            int inline_fallback_count = 0;
                            int inline_done_br = -1;

                            uint16_t table_off = (uint16_t)((uint16_t)table.slot * (uint16_t)sizeof(Value));
                            uint16_t table_obj_off = (uint16_t)(table_off + 8u);
                            uint16_t off_obj_type = (uint16_t)offsetof(struct Obj, type);
                            uint16_t off_obj_nmfast = (uint16_t)offsetof(ObjTable, no_metatable_fast);
                            uint16_t off_closure_fn = (uint16_t)offsetof(ObjClosure, function);
                            uint16_t off_chunk_ic_tables = (uint16_t)(offsetof(ObjFunction, chunk) + offsetof(Chunk, get_table_ic_tables));
                            uint16_t off_chunk_ic_keys = (uint16_t)(offsetof(ObjFunction, chunk) + offsetof(Chunk, get_table_ic_keys));
                            uint16_t off_chunk_ic_slots = (uint16_t)(offsetof(ObjFunction, chunk) + offsetof(Chunk, get_table_ic_slots));
                            uint16_t off_chunk_ic_versions = (uint16_t)(offsetof(ObjFunction, chunk) + offsetof(Chunk, get_table_ic_versions));
                            uint16_t off_table_shape = (uint16_t)(offsetof(ObjTable, table) + offsetof(Table, shape_version));
                            uint16_t off_table_capacity = (uint16_t)(offsetof(ObjTable, table) + offsetof(Table, capacity));
                            uint16_t off_table_entries = (uint16_t)(offsetof(ObjTable, table) + offsetof(Table, entries));
                            uint16_t off_entry_key = (uint16_t)offsetof(Entry, key);
                            uint16_t off_entry_value = (uint16_t)offsetof(Entry, value);

                            if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, table_off))) goto emit_fail; // x9 = table Value.type
                            if (!a64_emit(&buf, a64_cmp_w_imm(9, (uint16_t)VAL_OBJ))) goto emit_fail;
                            if (inline_fallback_count >= 16) goto emit_fail;
                            inline_fallback_br[inline_fallback_count++] = buf.count;
                            inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                            if (!a64_emit(&buf, a64_ldr_x_imm(10, 2, table_obj_off))) goto emit_fail; // x10 = table Obj*
                            if (!a64_emit(&buf, a64_ldr_w_imm(11, 10, off_obj_type))) goto emit_fail; // w11 = obj type
                            if (!a64_emit(&buf, a64_cmp_w_imm(11, (uint16_t)OBJ_TABLE))) goto emit_fail;
                            if (inline_fallback_count >= 16) goto emit_fail;
                            inline_fallback_br[inline_fallback_count++] = buf.count;
                            inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                            if (!a64_emit(&buf, a64_ldr_b_imm(11, 10, off_obj_nmfast))) goto emit_fail; // w11 = no_metatable_fast
                            if (!a64_emit(&buf, a64_cmp_w_imm(11, 0))) goto emit_fail;
                            if (inline_fallback_count >= 16) goto emit_fail;
                            inline_fallback_br[inline_fallback_count++] = buf.count;
                            inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                            if (!a64_emit(&buf, a64_ldr_x_imm(11, 31, 16))) goto emit_fail; // x11 = closure
                            if (!a64_emit(&buf, a64_ldr_x_imm(11, 11, off_closure_fn))) goto emit_fail; // x11 = function

                            if (!a64_emit(&buf, a64_ldr_x_imm(12, 11, off_chunk_ic_tables))) goto emit_fail; // x12 = ic tables
                            if (!a64_emit(&buf, a64_subs_imm(31, 12, 0))) goto emit_fail;
                            if (inline_fallback_count >= 16) goto emit_fail;
                            inline_fallback_br[inline_fallback_count++] = buf.count;
                            inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                            if (!a64_emit(&buf, a64_ldr_x_imm(13, 11, off_chunk_ic_keys))) goto emit_fail; // x13 = ic keys
                            if (!a64_emit(&buf, a64_subs_imm(31, 13, 0))) goto emit_fail;
                            if (inline_fallback_count >= 16) goto emit_fail;
                            inline_fallback_br[inline_fallback_count++] = buf.count;
                            inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                            if (!a64_emit(&buf, a64_ldr_x_imm(14, 11, off_chunk_ic_slots))) goto emit_fail; // x14 = ic slots
                            if (!a64_emit(&buf, a64_subs_imm(31, 14, 0))) goto emit_fail;
                            if (inline_fallback_count >= 16) goto emit_fail;
                            inline_fallback_br[inline_fallback_count++] = buf.count;
                            inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                            if (!a64_emit(&buf, a64_ldr_x_imm(15, 11, off_chunk_ic_versions))) goto emit_fail; // x15 = ic versions
                            if (!a64_emit(&buf, a64_subs_imm(31, 15, 0))) goto emit_fail;
                            if (inline_fallback_count >= 16) goto emit_fail;
                            inline_fallback_br[inline_fallback_count++] = buf.count;
                            inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                            if (!a64_emit_mov_imm64(&buf, 16, (uint64_t)in->deopt_offset)) goto emit_fail; // x16 = site

                            if (!a64_emit(&buf, a64_add_shifted(12, 12, 16, 3))) goto emit_fail; // &tables[site]
                            if (!a64_emit(&buf, a64_ldr_x_imm(12, 12, 0))) goto emit_fail; // tables[site]
                            if (!a64_emit(&buf, a64_subs_reg(31, 12, 10))) goto emit_fail;
                            if (inline_fallback_count >= 16) goto emit_fail;
                            inline_fallback_br[inline_fallback_count++] = buf.count;
                            inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                            if (!a64_emit(&buf, a64_add_shifted(13, 13, 16, 3))) goto emit_fail; // &keys[site]
                            if (!a64_emit(&buf, a64_ldr_x_imm(13, 13, 0))) goto emit_fail; // keys[site]
                            if (!a64_emit_mov_imm64(&buf, 12, (uint64_t)(uintptr_t)AS_STRING(key.constant))) goto emit_fail; // x12 = const key
                            if (!a64_emit(&buf, a64_subs_reg(31, 13, 12))) goto emit_fail;
                            if (inline_fallback_count >= 16) goto emit_fail;
                            inline_fallback_br[inline_fallback_count++] = buf.count;
                            inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                            if (!a64_emit(&buf, a64_add_shifted(15, 15, 16, 2))) goto emit_fail; // &versions[site]
                            if (!a64_emit(&buf, a64_ldr_w_imm(15, 15, 0))) goto emit_fail; // w15 = ic version
                            if (!a64_emit(&buf, a64_ldr_w_imm(13, 10, off_table_shape))) goto emit_fail; // w13 = table shape
                            if (!a64_emit(&buf, a64_subs_reg(31, 15, 13))) goto emit_fail;
                            if (inline_fallback_count >= 16) goto emit_fail;
                            inline_fallback_br[inline_fallback_count++] = buf.count;
                            inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                            if (!a64_emit(&buf, a64_add_shifted(14, 14, 16, 2))) goto emit_fail; // &slots[site]
                            if (!a64_emit(&buf, a64_ldr_w_imm(14, 14, 0))) goto emit_fail; // w14 = slot (zero-extended)
                            if (!a64_emit(&buf, a64_ldr_w_imm(15, 10, off_table_capacity))) goto emit_fail; // w15 = capacity
                            if (!a64_emit(&buf, a64_subs_reg(31, 14, 15))) goto emit_fail;
                            if (inline_fallback_count >= 16) goto emit_fail;
                            inline_fallback_br[inline_fallback_count++] = buf.count;
                            inline_fallback_cond[inline_fallback_count - 1] = 0xAu;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ge fallback

                            if (!a64_emit(&buf, a64_ldr_x_imm(15, 10, off_table_entries))) goto emit_fail; // x15 = entries
                            if (!a64_emit(&buf, a64_add_shifted(15, 15, 14, 4))) goto emit_fail; // &entries[slot]
                            if (!a64_emit(&buf, a64_ldr_x_imm(16, 15, off_entry_key))) goto emit_fail; // x16 = entry key
                            if (!a64_emit(&buf, a64_subs_reg(31, 16, 12))) goto emit_fail;
                            if (inline_fallback_count >= 16) goto emit_fail;
                            inline_fallback_br[inline_fallback_count++] = buf.count;
                            inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                            {
                                uint16_t dst = (uint16_t)((uint16_t)dst_slot * (uint16_t)sizeof(Value));
                                if (!a64_emit(&buf, a64_ldr_x_imm(16, 15, off_entry_value))) goto emit_fail;
                                if (!a64_emit(&buf, a64_str_x_imm(16, 2, dst))) goto emit_fail;
                                if (!a64_emit(&buf, a64_ldr_x_imm(16, 15, (uint16_t)(off_entry_value + 8u)))) goto emit_fail;
                                if (!a64_emit(&buf, a64_str_x_imm(16, 2, (uint16_t)(dst + 8)))) goto emit_fail;
                            }

                            inline_done_br = buf.count;
                            if (!a64_emit(&buf, 0x14000000u)) goto emit_fail; // b done-inline

                            int inline_fallback_label = buf.count;
                            for (int fb = 0; fb < inline_fallback_count; fb++) {
                                if (!a64_patch_b_cond(&buf, inline_fallback_br[fb], inline_fallback_label, inline_fallback_cond[fb])) goto emit_fail;
                            }

                            if (!a64_emit(&buf, a64_mov_x(0, 2))) goto emit_fail; // x0 = slots
                            if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail; // x1 = slot_count
                            if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)table.slot)) goto emit_fail; // x2 = table slot
                            if (!a64_emit_mov_imm64(&buf, 3, (uint64_t)(uintptr_t)AS_STRING(key.constant))) goto emit_fail;
                            if (!a64_emit(&buf, a64_add_imm(4, 31, 0))) goto emit_fail; // &Value scratch
                            if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_str_slot_as_value)) goto emit_fail;

                            if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                            if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                            if (deopt_branch_count >= 256) goto emit_fail;
                            deopt_branches[deopt_branch_count++] = buf.count;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                            {
                                uint16_t dst = (uint16_t)((uint16_t)dst_slot * (uint16_t)sizeof(Value));
                                if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 0))) goto emit_fail;
                                if (!a64_emit(&buf, a64_str_x_imm(9, 2, dst))) goto emit_fail;
                                if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 8))) goto emit_fail;
                                if (!a64_emit(&buf, a64_str_x_imm(9, 2, (uint16_t)(dst + 8)))) goto emit_fail;
                            }

                            int inline_done_label = buf.count;
                            if (!a64_patch_b(&buf, inline_done_br, inline_done_label)) goto emit_fail;
                        } else if (key.kind == 3) {
                            uint16_t key_obj_off = (uint16_t)((uint16_t)key.slot * (uint16_t)sizeof(Value) + 8u);
                            if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 240))) goto emit_fail; // x0 = slots
                            if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail;   // x1 = slot_count
                            if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)table.slot)) goto emit_fail; // x2 = table slot
                            if (!a64_emit(&buf, a64_ldr_x_imm(3, 0, key_obj_off))) goto emit_fail; // x3 = key ObjString*
                            if (!a64_emit(&buf, a64_add_imm(4, 31, 0))) goto emit_fail; // x4 = &Value scratch
                            if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_str_slot_as_value)) goto emit_fail;
                        } else if (key.kind == 8) {
                            if (!a64_emit(&buf, a64_ldr_x_imm(10, 31, 16))) goto emit_fail; // x10 = closure
                            if (!a64_emit_mov_imm64(&buf, 11, (uint64_t)key.slot)) goto emit_fail;
                            if (!a64_emit(&buf, a64_add_imm(12, 31, 0))) goto emit_fail;
                            if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_upvalue_slot_as_value)) goto emit_fail;
                            if (!a64_emit(&buf, a64_mov_x(0, 10))) goto emit_fail;
                            if (!a64_emit(&buf, a64_mov_x(1, 11))) goto emit_fail;
                            if (!a64_emit(&buf, a64_mov_x(2, 12))) goto emit_fail;
                            if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                            if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                            if (deopt_branch_count >= 256) goto emit_fail;
                            deopt_branches[deopt_branch_count++] = buf.count;
                            if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                            if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 240))) goto emit_fail; // x0 = slots
                            if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail;   // x1 = slot_count
                            if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)table.slot)) goto emit_fail; // x2 = table slot
                            if (!a64_emit(&buf, a64_ldr_x_imm(3, 31, 8))) goto emit_fail; // key ptr from scratch
                            if (!a64_emit(&buf, a64_add_imm(4, 31, 0))) goto emit_fail;
                            if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_str_slot_as_value)) goto emit_fail;
                        } else {
                            goto emit_fail;
                        }
                    } else {
                        if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                        if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)table.slot)) goto emit_fail; // x1 = upvalue slot
                        if (key.kind == 2 && IS_STRING(key.constant)) {
                            if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)(uintptr_t)AS_STRING(key.constant))) goto emit_fail;
                            if (!a64_emit(&buf, a64_add_imm(3, 31, 0))) goto emit_fail;
                            if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_str_upvalue_as_value)) goto emit_fail;
                    } else if (key.kind == 3) {
                        uint16_t key_off = (uint16_t)((uint16_t)key.slot * (uint16_t)sizeof(Value) + 8u);
                        if (!a64_emit(&buf, a64_ldr_x_imm(10, 31, 240))) goto emit_fail; // x10 = slots
                        if (!a64_emit(&buf, a64_ldr_x_imm(2, 10, key_off))) goto emit_fail; // x2 = ObjString*
                        if (!a64_emit(&buf, a64_add_imm(3, 31, 0))) goto emit_fail; // x3 = &Value scratch
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_str_upvalue_as_value)) goto emit_fail;
                    } else {
                            goto emit_fail;
                        }
                    }
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    if (deopt_branch_count >= 256) goto emit_fail;
                    deopt_branches[deopt_branch_count++] = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    uint16_t dst = (uint16_t)((uint16_t)dst_slot * (uint16_t)sizeof(Value));
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 0))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, dst))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 8))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 2, (uint16_t)(dst + 8)))) goto emit_fail;
                    tstack[vsp++] = (A64TraceVal){4, 0, dst_slot, 0, NIL_VAL};
                    i++;
                    break;
                }
                int inline_skip_call_br = -1;
                if (table.kind == 1) {
                    if (!a64_emit(&buf, a64_mov_x(0, 2))) goto emit_fail; // x0 = slots
                    if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail; // x1 = slot_count
                    if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)table.slot)) goto emit_fail; // x2 = table slot
                    if (key.kind == 2 && IS_STRING(key.constant)) {
                        int inline_fallback_br[16];
                        uint8_t inline_fallback_cond[16];
                        int inline_fallback_count = 0;
                        uint16_t table_off = (uint16_t)((uint16_t)table.slot * (uint16_t)sizeof(Value));
                        uint16_t table_obj_off = (uint16_t)(table_off + 8u);
                        uint16_t off_obj_type = (uint16_t)offsetof(struct Obj, type);
                        uint16_t off_obj_nmfast = (uint16_t)offsetof(ObjTable, no_metatable_fast);
                        uint16_t off_closure_fn = (uint16_t)offsetof(ObjClosure, function);
                        uint16_t off_chunk_ic_tables = (uint16_t)(offsetof(ObjFunction, chunk) + offsetof(Chunk, get_table_ic_tables));
                        uint16_t off_chunk_ic_keys = (uint16_t)(offsetof(ObjFunction, chunk) + offsetof(Chunk, get_table_ic_keys));
                        uint16_t off_chunk_ic_slots = (uint16_t)(offsetof(ObjFunction, chunk) + offsetof(Chunk, get_table_ic_slots));
                        uint16_t off_chunk_ic_versions = (uint16_t)(offsetof(ObjFunction, chunk) + offsetof(Chunk, get_table_ic_versions));
                        uint16_t off_table_shape = (uint16_t)(offsetof(ObjTable, table) + offsetof(Table, shape_version));
                        uint16_t off_table_capacity = (uint16_t)(offsetof(ObjTable, table) + offsetof(Table, capacity));
                        uint16_t off_table_entries = (uint16_t)(offsetof(ObjTable, table) + offsetof(Table, entries));
                        uint16_t off_entry_key = (uint16_t)offsetof(Entry, key);
                        uint16_t off_entry_value = (uint16_t)offsetof(Entry, value);

                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, table_off))) goto emit_fail; // x9 = table Value.type
                        if (!a64_emit(&buf, a64_cmp_w_imm(9, (uint16_t)VAL_OBJ))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(10, 2, table_obj_off))) goto emit_fail; // x10 = table Obj*
                        if (!a64_emit(&buf, a64_ldr_w_imm(11, 10, off_obj_type))) goto emit_fail; // w11 = obj type
                        if (!a64_emit(&buf, a64_cmp_w_imm(11, (uint16_t)OBJ_TABLE))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_ldr_b_imm(11, 10, off_obj_nmfast))) goto emit_fail; // w11 = no_metatable_fast
                        if (!a64_emit(&buf, a64_cmp_w_imm(11, 0))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(11, 31, 16))) goto emit_fail; // x11 = closure
                        if (!a64_emit(&buf, a64_ldr_x_imm(11, 11, off_closure_fn))) goto emit_fail; // x11 = function

                        if (!a64_emit(&buf, a64_ldr_x_imm(12, 11, off_chunk_ic_tables))) goto emit_fail; // x12 = ic tables
                        if (!a64_emit(&buf, a64_subs_imm(31, 12, 0))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(13, 11, off_chunk_ic_keys))) goto emit_fail; // x13 = ic keys
                        if (!a64_emit(&buf, a64_subs_imm(31, 13, 0))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(14, 11, off_chunk_ic_slots))) goto emit_fail; // x14 = ic slots
                        if (!a64_emit(&buf, a64_subs_imm(31, 14, 0))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(15, 11, off_chunk_ic_versions))) goto emit_fail; // x15 = ic versions
                        if (!a64_emit(&buf, a64_subs_imm(31, 15, 0))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                        if (!a64_emit_mov_imm64(&buf, 16, (uint64_t)in->deopt_offset)) goto emit_fail; // x16 = site

                        if (!a64_emit(&buf, a64_add_shifted(12, 12, 16, 3))) goto emit_fail; // &tables[site]
                        if (!a64_emit(&buf, a64_ldr_x_imm(12, 12, 0))) goto emit_fail; // tables[site]
                        if (!a64_emit(&buf, a64_subs_reg(31, 12, 10))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_add_shifted(13, 13, 16, 3))) goto emit_fail; // &keys[site]
                        if (!a64_emit(&buf, a64_ldr_x_imm(13, 13, 0))) goto emit_fail; // keys[site]
                        if (!a64_emit_mov_imm64(&buf, 12, (uint64_t)(uintptr_t)AS_STRING(key.constant))) goto emit_fail; // x12 = const key
                        if (!a64_emit(&buf, a64_subs_reg(31, 13, 12))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_add_shifted(15, 15, 16, 2))) goto emit_fail; // &versions[site]
                        if (!a64_emit(&buf, a64_ldr_w_imm(15, 15, 0))) goto emit_fail; // w15 = ic version
                        if (!a64_emit(&buf, a64_ldr_w_imm(13, 10, off_table_shape))) goto emit_fail; // w13 = table shape
                        if (!a64_emit(&buf, a64_subs_reg(31, 15, 13))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_add_shifted(14, 14, 16, 2))) goto emit_fail; // &slots[site]
                        if (!a64_emit(&buf, a64_ldr_w_imm(14, 14, 0))) goto emit_fail; // w14 = slot
                        if (!a64_emit(&buf, a64_ldr_w_imm(15, 10, off_table_capacity))) goto emit_fail; // w15 = capacity
                        if (!a64_emit(&buf, a64_subs_reg(31, 14, 15))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0xAu;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ge fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(15, 10, off_table_entries))) goto emit_fail; // x15 = entries
                        if (!a64_emit(&buf, a64_add_shifted(15, 15, 14, 4))) goto emit_fail; // &entries[slot]
                        if (!a64_emit(&buf, a64_ldr_x_imm(16, 15, off_entry_key))) goto emit_fail; // x16 = entry key
                        if (!a64_emit(&buf, a64_subs_reg(31, 16, 12))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(16, 15, off_entry_value))) goto emit_fail; // x16 = entry value.type
                        if (!a64_emit(&buf, a64_cmp_w_imm(16, (uint16_t)VAL_NUMBER))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback
                        if (!a64_emit(&buf, a64_ldr_d_imm(1, 15, (uint16_t)(off_entry_value + 8u)))) goto emit_fail; // d1 = number
                        if (!a64_emit(&buf, a64_str_d_imm(1, 31, 0))) goto emit_fail; // scratch number
                        if (!a64_emit_mov_imm64(&buf, 0, 1u)) goto emit_fail; // w0/x0 = success
                        inline_skip_call_br = buf.count;
                        if (!a64_emit(&buf, 0x14000000u)) goto emit_fail; // b post-call

                        {
                            int inline_fallback_label = buf.count;
                            for (int fb = 0; fb < inline_fallback_count; fb++) {
                                if (!a64_patch_b_cond(&buf, inline_fallback_br[fb], inline_fallback_label, inline_fallback_cond[fb])) goto emit_fail;
                            }
                        }

                        if (!a64_emit_mov_imm64(&buf, 3, (uint64_t)(uintptr_t)AS_STRING(key.constant))) goto emit_fail; // x3 = key
                        if (!a64_emit(&buf, a64_add_imm(4, 31, 0))) goto emit_fail; // x4 = &scratch
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_str_slot_as_number)) goto emit_fail;
                    } else if (key.kind == 3) {
                        uint16_t key_obj_off = (uint16_t)((uint16_t)key.slot * (uint16_t)sizeof(Value) + 8u);
                        if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 240))) goto emit_fail; // x0 = slots
                        if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail;   // x1 = slot_count
                        if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)table.slot)) goto emit_fail; // x2 = table slot
                        if (!a64_emit(&buf, a64_ldr_x_imm(3, 0, key_obj_off))) goto emit_fail; // x3 = key ObjString*
                        if (!a64_emit(&buf, a64_add_imm(4, 31, 0))) goto emit_fail; // x4 = &scratch
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_str_slot_as_number)) goto emit_fail;
                    } else if (key.kind == 8) {
                        if (!a64_emit(&buf, a64_ldr_x_imm(10, 31, 16))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 11, (uint64_t)key.slot)) goto emit_fail;
                        if (!a64_emit(&buf, a64_add_imm(12, 31, 0))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_upvalue_slot_as_value)) goto emit_fail;
                        if (!a64_emit(&buf, a64_mov_x(0, 10))) goto emit_fail;
                        if (!a64_emit(&buf, a64_mov_x(1, 11))) goto emit_fail;
                        if (!a64_emit(&buf, a64_mov_x(2, 12))) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 240))) goto emit_fail; // x0 = slots
                        if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail;   // x1 = slot_count
                        if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)table.slot)) goto emit_fail; // x2 = table slot
                        if (!a64_emit(&buf, a64_ldr_x_imm(3, 31, 8))) goto emit_fail;
                        if (!a64_emit(&buf, a64_add_imm(4, 31, 0))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_str_slot_as_number)) goto emit_fail;
                    } else {
                        goto emit_fail;
                    }
                } else {
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                    if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)table.slot)) goto emit_fail; // x1 = upvalue slot
                    if (key.kind == 2 && IS_STRING(key.constant)) {
                        if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)(uintptr_t)AS_STRING(key.constant))) goto emit_fail;
                        if (!a64_emit(&buf, a64_add_imm(3, 31, 0))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_str_upvalue_as_number)) goto emit_fail;
                    } else if (key.kind == 3) {
                        uint16_t key_off = (uint16_t)((uint16_t)key.slot * (uint16_t)sizeof(Value) + 8u);
                        if (!a64_emit(&buf, a64_ldr_x_imm(10, 31, 240))) goto emit_fail; // x10 = slots
                        if (!a64_emit(&buf, a64_ldr_x_imm(2, 10, key_off))) goto emit_fail; // x2 = ObjString*
                        if (!a64_emit(&buf, a64_add_imm(3, 31, 0))) goto emit_fail; // x3 = &scratch
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_table_str_upvalue_as_number)) goto emit_fail;
                    } else {
                        goto emit_fail;
                    }
                }
                if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                if (table.kind == 1 && inline_skip_call_br >= 0) {
                    if (!a64_patch_b(&buf, inline_skip_call_br, buf.count)) goto emit_fail;
                }
                if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                if (deopt_branch_count >= 256) goto emit_fail;
                deopt_branches[deopt_branch_count++] = buf.count;
                if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq deopt
                uint8_t rd = a64_alloc_temp(&temp_used);
                if (rd == 0) goto emit_fail;
                if (!a64_emit(&buf, a64_ldr_d_imm(rd, 31, 0))) goto emit_fail;
                tstack[vsp++] = (A64TraceVal){0, rd, 0, 1, NIL_VAL};
                break;
            }
            case TOI_JIT_NATIVE_OP_SET_TABLE_NUM: {
                if (vsp < 3) goto emit_fail;
                A64TraceVal value = tstack[--vsp];
                A64TraceVal key = tstack[--vsp];
                A64TraceVal table = tstack[--vsp];
                if ((table.kind != 1 && table.kind != 7) || key.kind != 0) goto emit_fail;
                // Materialize value to [sp+200] as full Value for generic table set.
                if (value.kind == 4 || value.kind == 3 || value.kind == 1) {
                    uint16_t src = (uint16_t)((uint16_t)value.slot * (uint16_t)sizeof(Value));
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, src))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, (uint16_t)(src + 8)))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                } else if (value.kind == 6 || value.kind == 7 || value.kind == 8) {
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                    if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)value.slot)) goto emit_fail; // x1 = upvalue slot
                    if (!a64_emit(&buf, a64_add_imm(2, 31, 200))) goto emit_fail; // x2 = &value temp
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_upvalue_slot_as_value)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    if (deopt_branch_count >= 256) goto emit_fail;
                    deopt_branches[deopt_branch_count++] = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                } else if (value.kind == 9) {
                    uint16_t src = (uint16_t)(256 + (uint16_t)value.slot * 16u);
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, src))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, (uint16_t)(src + 8u)))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                } else if (value.kind == 2 && IS_STRING(value.constant)) {
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_OBJ)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)AS_STRING(value.constant))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                } else if (value.kind == 0) {
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_NUMBER)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_d_imm(value.reg, 31, 208))) goto emit_fail;
                } else if (value.kind == 5) {
                    uint64_t tag = (uint64_t)value.constant.type;
                    uint64_t payload = 0u;
                    if (IS_BOOL(value.constant)) payload = (uint64_t)AS_BOOL(value.constant);
                    else if (IS_OBJ(value.constant)) payload = (uint64_t)(uintptr_t)AS_OBJ(value.constant);
                    if (!a64_emit_mov_imm64(&buf, 9, tag)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, payload)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                } else {
                    goto emit_fail;
                }
                int inline_skip_call_br = -1;
                if (table.kind == 1) {
                    if (!a64_emit(&buf, a64_mov_x(0, 2))) goto emit_fail; // x0 = slots
                    if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail; // x1 = slot_count
                    if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)table.slot)) goto emit_fail; // x2 = table slot
                    if (!a64_emit(&buf, a64_fmov_rr(0, key.reg))) goto emit_fail; // d0 = key
                    if (!a64_emit(&buf, a64_add_imm(3, 31, 200))) goto emit_fail; // x3 = &value
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_set_table_num_slot_from_value)) goto emit_fail;
                } else {
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                    if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)table.slot)) goto emit_fail; // x1 = upvalue slot
                    if (!a64_emit(&buf, a64_fmov_rr(0, key.reg))) goto emit_fail; // d0 = key
                    if (!a64_emit(&buf, a64_add_imm(2, 31, 200))) goto emit_fail; // x2 = &value
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_set_table_num_upvalue_from_value)) goto emit_fail;
                }
                if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                if (table.kind == 1 && inline_skip_call_br >= 0) {
                    if (!a64_patch_b(&buf, inline_skip_call_br, buf.count)) goto emit_fail;
                }
                if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                if (deopt_branch_count >= 256) goto emit_fail;
                deopt_branches[deopt_branch_count++] = buf.count;
                if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq deopt
                if (key.is_temp) a64_free_temp(&temp_used, key.reg);
                tstack[vsp++] = value;
                break;
            }
            case TOI_JIT_NATIVE_OP_SET_TABLE: {
                if (vsp < 3) goto emit_fail;
                A64TraceVal value = tstack[--vsp];
                A64TraceVal key = tstack[--vsp];
                A64TraceVal table = tstack[--vsp];
                if ((table.kind != 1 && table.kind != 7)) goto emit_fail;
                // Materialize value to [sp+200] as full Value for generic table set.
                if (value.kind == 4 || value.kind == 3 || value.kind == 1) {
                    uint16_t src = (uint16_t)((uint16_t)value.slot * (uint16_t)sizeof(Value));
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, src))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, (uint16_t)(src + 8)))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                } else if (value.kind == 6 || value.kind == 7 || value.kind == 8) {
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)value.slot)) goto emit_fail;
                    if (!a64_emit(&buf, a64_add_imm(2, 31, 200))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_upvalue_slot_as_value)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    if (deopt_branch_count >= 256) goto emit_fail;
                    deopt_branches[deopt_branch_count++] = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                } else if (value.kind == 9) {
                    uint16_t src = (uint16_t)(256 + (uint16_t)value.slot * 16u);
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, src))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, (uint16_t)(src + 8u)))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                } else if (value.kind == 2 && IS_STRING(value.constant)) {
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_OBJ)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)AS_STRING(value.constant))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                } else if (value.kind == 0) {
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_NUMBER)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_d_imm(value.reg, 31, 208))) goto emit_fail;
                } else if (value.kind == 5) {
                    uint64_t tag = (uint64_t)value.constant.type;
                    uint64_t payload = 0u;
                    if (IS_BOOL(value.constant)) payload = (uint64_t)AS_BOOL(value.constant);
                    else if (IS_OBJ(value.constant)) payload = (uint64_t)(uintptr_t)AS_OBJ(value.constant);
                    if (!a64_emit_mov_imm64(&buf, 9, tag)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 200))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, payload)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, 208))) goto emit_fail;
                } else {
                    goto emit_fail;
                }
                int inline_skip_call_br = -1;
                if (table.kind == 1) {
                    if (!a64_emit(&buf, a64_mov_x(0, 2))) goto emit_fail; // x0 = slots
                    if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail; // x1 = slot_count
                    if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)table.slot)) goto emit_fail; // x2 = table slot
                    if (key.kind == 2 && IS_STRING(key.constant)) {
                        int inline_fallback_br[16];
                        uint8_t inline_fallback_cond[16];
                        int inline_fallback_count = 0;
                        uint16_t table_off = (uint16_t)((uint16_t)table.slot * (uint16_t)sizeof(Value));
                        uint16_t table_obj_off = (uint16_t)(table_off + 8u);
                        uint16_t off_obj_type = (uint16_t)offsetof(struct Obj, type);
                        uint16_t off_obj_nmfast = (uint16_t)offsetof(ObjTable, no_metatable_fast);
                        uint16_t off_closure_fn = (uint16_t)offsetof(ObjClosure, function);
                        uint16_t off_chunk_set_ic_tables = (uint16_t)(offsetof(ObjFunction, chunk) + offsetof(Chunk, set_table_ic_tables));
                        uint16_t off_chunk_set_ic_keys = (uint16_t)(offsetof(ObjFunction, chunk) + offsetof(Chunk, set_table_ic_keys));
                        uint16_t off_chunk_set_ic_slots = (uint16_t)(offsetof(ObjFunction, chunk) + offsetof(Chunk, set_table_ic_slots));
                        uint16_t off_chunk_set_ic_versions = (uint16_t)(offsetof(ObjFunction, chunk) + offsetof(Chunk, set_table_ic_versions));
                        uint16_t off_table_shape = (uint16_t)(offsetof(ObjTable, table) + offsetof(Table, shape_version));
                        uint16_t off_table_capacity = (uint16_t)(offsetof(ObjTable, table) + offsetof(Table, capacity));
                        uint16_t off_table_entries = (uint16_t)(offsetof(ObjTable, table) + offsetof(Table, entries));
                        uint16_t off_entry_key = (uint16_t)offsetof(Entry, key);
                        uint16_t off_entry_value = (uint16_t)offsetof(Entry, value);

                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, table_off))) goto emit_fail; // x9 = table Value.type
                        if (!a64_emit(&buf, a64_cmp_w_imm(9, (uint16_t)VAL_OBJ))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(10, 2, table_obj_off))) goto emit_fail; // x10 = table Obj*
                        if (!a64_emit(&buf, a64_ldr_w_imm(11, 10, off_obj_type))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(11, (uint16_t)OBJ_TABLE))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_ldr_b_imm(11, 10, off_obj_nmfast))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(11, 0))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(11, 31, 16))) goto emit_fail; // x11 = closure
                        if (!a64_emit(&buf, a64_ldr_x_imm(11, 11, off_closure_fn))) goto emit_fail; // x11 = function

                        if (!a64_emit(&buf, a64_ldr_x_imm(12, 11, off_chunk_set_ic_tables))) goto emit_fail;
                        if (!a64_emit(&buf, a64_subs_imm(31, 12, 0))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(13, 11, off_chunk_set_ic_keys))) goto emit_fail;
                        if (!a64_emit(&buf, a64_subs_imm(31, 13, 0))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(14, 11, off_chunk_set_ic_slots))) goto emit_fail;
                        if (!a64_emit(&buf, a64_subs_imm(31, 14, 0))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(15, 11, off_chunk_set_ic_versions))) goto emit_fail;
                        if (!a64_emit(&buf, a64_subs_imm(31, 15, 0))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x0u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq fallback

                        if (!a64_emit_mov_imm64(&buf, 16, (uint64_t)in->deopt_offset)) goto emit_fail; // x16 = site

                        if (!a64_emit(&buf, a64_add_shifted(12, 12, 16, 3))) goto emit_fail; // &tables[site]
                        if (!a64_emit(&buf, a64_ldr_x_imm(12, 12, 0))) goto emit_fail; // tables[site]
                        if (!a64_emit(&buf, a64_subs_reg(31, 12, 10))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_add_shifted(13, 13, 16, 3))) goto emit_fail; // &keys[site]
                        if (!a64_emit(&buf, a64_ldr_x_imm(13, 13, 0))) goto emit_fail; // keys[site]
                        if (!a64_emit_mov_imm64(&buf, 12, (uint64_t)(uintptr_t)AS_STRING(key.constant))) goto emit_fail;
                        if (!a64_emit(&buf, a64_subs_reg(31, 13, 12))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_add_shifted(15, 15, 16, 2))) goto emit_fail; // &versions[site]
                        if (!a64_emit(&buf, a64_ldr_w_imm(15, 15, 0))) goto emit_fail; // w15 = ic version
                        if (!a64_emit(&buf, a64_ldr_w_imm(13, 10, off_table_shape))) goto emit_fail; // w13 = shape
                        if (!a64_emit(&buf, a64_subs_reg(31, 15, 13))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_add_shifted(14, 14, 16, 2))) goto emit_fail; // &slots[site]
                        if (!a64_emit(&buf, a64_ldr_w_imm(14, 14, 0))) goto emit_fail; // w14 = slot
                        if (!a64_emit(&buf, a64_ldr_w_imm(15, 10, off_table_capacity))) goto emit_fail; // w15 = capacity
                        if (!a64_emit(&buf, a64_subs_reg(31, 14, 15))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0xAu;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ge fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(15, 10, off_table_entries))) goto emit_fail; // x15 = entries
                        if (!a64_emit(&buf, a64_add_shifted(15, 15, 14, 4))) goto emit_fail; // &entries[slot]
                        if (!a64_emit(&buf, a64_ldr_x_imm(16, 15, off_entry_key))) goto emit_fail;
                        if (!a64_emit(&buf, a64_subs_reg(31, 16, 12))) goto emit_fail;
                        if (inline_fallback_count >= 16) goto emit_fail;
                        inline_fallback_br[inline_fallback_count++] = buf.count;
                        inline_fallback_cond[inline_fallback_count - 1] = 0x1u;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne fallback

                        if (!a64_emit(&buf, a64_ldr_x_imm(16, 31, 200))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(16, 15, off_entry_value))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(16, 31, 208))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(16, 15, (uint16_t)(off_entry_value + 8u)))) goto emit_fail;

                        if (!a64_emit_mov_imm64(&buf, 0, 1u)) goto emit_fail; // success
                        inline_skip_call_br = buf.count;
                        if (!a64_emit(&buf, 0x14000000u)) goto emit_fail; // b post-call

                        {
                            int inline_fallback_label = buf.count;
                            for (int fb = 0; fb < inline_fallback_count; fb++) {
                                if (!a64_patch_b_cond(&buf, inline_fallback_br[fb], inline_fallback_label, inline_fallback_cond[fb])) goto emit_fail;
                            }
                        }
                        if (!a64_emit_mov_imm64(&buf, 3, (uint64_t)(uintptr_t)AS_STRING(key.constant))) goto emit_fail; // x3 = key
                        if (!a64_emit(&buf, a64_add_imm(4, 31, 200))) goto emit_fail; // x4 = &value
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_set_table_str_slot_from_value)) goto emit_fail;
                    } else if (key.kind == 3) {
                        if (!a64_emit_mov_imm64(&buf, 3, (uint64_t)key.slot)) goto emit_fail; // x3 = key slot
                        if (!a64_emit(&buf, a64_add_imm(4, 31, 200))) goto emit_fail; // x4 = &value
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_set_table_strkey_slot_from_value)) goto emit_fail;
                    } else if (key.kind == 8) {
                        if (!a64_emit(&buf, a64_ldr_x_imm(10, 31, 16))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 11, (uint64_t)key.slot)) goto emit_fail;
                        if (!a64_emit(&buf, a64_add_imm(12, 31, 0))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_upvalue_slot_as_value)) goto emit_fail;
                        if (!a64_emit(&buf, a64_mov_x(0, 10))) goto emit_fail;
                        if (!a64_emit(&buf, a64_mov_x(1, 11))) goto emit_fail;
                        if (!a64_emit(&buf, a64_mov_x(2, 12))) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(3, 31, 8))) goto emit_fail;
                        if (!a64_emit(&buf, a64_add_imm(4, 31, 200))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_set_table_str_slot_from_value)) goto emit_fail;
                    } else {
                        goto emit_fail;
                    }
                } else {
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                    if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)table.slot)) goto emit_fail; // x1 = upvalue slot
                    if (key.kind == 2 && IS_STRING(key.constant)) {
                        if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)(uintptr_t)AS_STRING(key.constant))) goto emit_fail; // x2 = key
                        if (!a64_emit(&buf, a64_add_imm(3, 31, 200))) goto emit_fail; // x3 = &value
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_set_table_str_upvalue_from_value)) goto emit_fail;
                    } else if (key.kind == 3) {
                        uint16_t key_off = (uint16_t)((uint16_t)key.slot * (uint16_t)sizeof(Value) + 8u);
                        if (!a64_emit(&buf, a64_ldr_x_imm(2, 2, key_off))) goto emit_fail; // x2 = ObjString*
                        if (!a64_emit(&buf, a64_add_imm(3, 31, 200))) goto emit_fail; // x3 = &value
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_set_table_str_upvalue_from_value)) goto emit_fail;
                    } else {
                        goto emit_fail;
                    }
                }
                if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                if (table.kind == 1 && inline_skip_call_br >= 0) {
                    if (!a64_patch_b(&buf, inline_skip_call_br, buf.count)) goto emit_fail;
                }
                if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                if (deopt_branch_count >= 256) goto emit_fail;
                deopt_branches[deopt_branch_count++] = buf.count;
                if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq deopt
                tstack[vsp++] = value;
                break;
            }
            case TOI_JIT_NATIVE_OP_BUILD_STRING2:
            case TOI_JIT_NATIVE_OP_BUILD_STRING3:
            case TOI_JIT_NATIVE_OP_BUILD_STRING4:
            case TOI_JIT_NATIVE_OP_BUILD_STRING5: {
                int n = (in->op == TOI_JIT_NATIVE_OP_BUILD_STRING2) ? 2 :
                        (in->op == TOI_JIT_NATIVE_OP_BUILD_STRING3) ? 3 :
                        (in->op == TOI_JIT_NATIVE_OP_BUILD_STRING4) ? 4 : 5;
                if (vsp < n) goto emit_fail;
                int fused_store = (i + 1 < instr_count && instrs[i + 1].op == TOI_JIT_NATIVE_OP_STORE_LOCAL);
                uint8_t dst_slot = fused_store ? instrs[i + 1].a : 0;
                if (fused_store && !native_trace_has_local_string_guard(dst_slot, guards, guard_count)) goto emit_fail;
                int fused_build_get = 0;
                uint8_t fused_table_slot = 0;
                uint8_t fused_val_slot = 0;
                uint32_t fused_get_site = 0;
                if (0 && n == 3 && fused_store && i + 5 < instr_count &&
                    instrs[i + 2].op == TOI_JIT_NATIVE_OP_PUSH_LOCAL &&
                    instrs[i + 3].op == TOI_JIT_NATIVE_OP_PUSH_LOCAL &&
                    instrs[i + 4].op == TOI_JIT_NATIVE_OP_GET_TABLE &&
                    instrs[i + 5].op == TOI_JIT_NATIVE_OP_STORE_LOCAL &&
                    instrs[i + 3].a == dst_slot) {
                    fused_build_get = 1;
                    fused_table_slot = instrs[i + 2].a;
                    fused_get_site = instrs[i + 4].deopt_offset;
                    fused_val_slot = instrs[i + 5].a;
                }

                // Materialize Value parts at [sp+32 + j*16].
                for (int j = 0; j < n; j++) {
                    A64TraceVal p = tstack[vsp - n + j];
                    uint16_t base = (uint16_t)(32 + j * 16);
                    if (p.kind == 2 && IS_STRING(p.constant)) {
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_OBJ)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, base))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)AS_STRING(p.constant))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(base + 8)))) goto emit_fail;
                    } else if (p.kind == 3 || p.kind == 4 || p.kind == 1) {
                        uint16_t src = (uint16_t)((uint16_t)p.slot * (uint16_t)sizeof(Value));
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, src))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, base))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, (uint16_t)(src + 8)))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(base + 8)))) goto emit_fail;
                    } else if (p.kind == 9) {
                        uint16_t src = (uint16_t)(256 + (uint16_t)p.slot * 16u);
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, src))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, base))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, (uint16_t)(src + 8u)))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(base + 8)))) goto emit_fail;
                    } else if (p.kind == 6 || p.kind == 7 || p.kind == 8) {
                        if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                        if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)p.slot)) goto emit_fail; // x1 = upvalue slot
                        if (!a64_emit(&buf, a64_add_imm(2, 31, base))) goto emit_fail; // x2 = &part
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_upvalue_slot_as_value)) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    } else if (p.kind == 0) {
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_NUMBER)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, base))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_d_imm(p.reg, 31, (uint16_t)(base + 8)))) goto emit_fail;
                    } else if (p.kind == 5) {
                        uint64_t tag = (uint64_t)p.constant.type;
                        uint64_t payload = 0u;
                        if (IS_BOOL(p.constant)) payload = (uint64_t)AS_BOOL(p.constant);
                        else if (IS_OBJ(p.constant)) payload = (uint64_t)(uintptr_t)AS_OBJ(p.constant);
                        if (!a64_emit_mov_imm64(&buf, 9, tag)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, base))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, payload)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(base + 8)))) goto emit_fail;
                    } else {
                        goto emit_fail;
                    }
                }

                if (fused_build_get) {
                    if (vsp - n + 1 >= 128) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 24))) goto emit_fail; // x0 = vm
                    if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 16))) goto emit_fail; // x1 = closure
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail; // x2 = slots
                    if (!a64_emit(&buf, a64_ldr_x_imm(3, 31, 224))) goto emit_fail; // x3 = slot_count
                    if (!a64_emit_mov_imm64(&buf, 4, (uint64_t)fused_table_slot)) goto emit_fail; // x4 = table slot
                    if (!a64_emit_mov_imm64(&buf, 5, (uint64_t)fused_get_site)) goto emit_fail; // x5 = site
                    if (!a64_emit(&buf, a64_add_imm(6, 31, 32))) goto emit_fail; // x6 = parts
                    if (!a64_emit(&buf, a64_add_imm(7, 31, 176))) goto emit_fail; // x7 = out_pair[2]
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_build3_get_table_slot_ic)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    if (deopt_branch_count >= 256) goto emit_fail;
                    deopt_branches[deopt_branch_count++] = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq deopt

                    // key -> key local
                    {
                        uint16_t dst = (uint16_t)((uint16_t)dst_slot * (uint16_t)sizeof(Value));
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 176))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 2, dst))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 184))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 2, (uint16_t)(dst + 8)))) goto emit_fail;
                    }
                    // value -> value local
                    {
                        uint16_t dst = (uint16_t)((uint16_t)fused_val_slot * (uint16_t)sizeof(Value));
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 192))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 2, dst))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 200))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 2, (uint16_t)(dst + 8)))) goto emit_fail;
                    }
                    // key/value -> stack value scratch
                    {
                        uint16_t sdst0 = (uint16_t)(256 + (uint16_t)(vsp - n) * 16u);
                        uint16_t sdst1 = (uint16_t)(sdst0 + 16u);
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 176))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, sdst0))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 184))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(sdst0 + 8u)))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 192))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, sdst1))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 200))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(sdst1 + 8u)))) goto emit_fail;
                    }
                } else {
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 24))) goto emit_fail; // x0 = vm
                    if (!a64_emit(&buf, a64_add_imm(1, 31, 32))) goto emit_fail;   // x1 = parts
                    if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)n)) goto emit_fail; // x2 = count
                    if (!a64_emit(&buf, a64_add_imm(3, 31, 176))) goto emit_fail;  // x3 = &out str
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_build_string_from_parts)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    if (deopt_branch_count >= 256) goto emit_fail;
                    deopt_branches[deopt_branch_count++] = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq deopt

                    if (fused_store) {
                        // Store built string to destination local as Value{VAL_OBJ, out_ptr}.
                        uint16_t dst = (uint16_t)((uint16_t)dst_slot * (uint16_t)sizeof(Value));
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_OBJ)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 2, dst))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 176))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 2, (uint16_t)(dst + 8)))) goto emit_fail;
                    } else {
                        if (vsp - n >= 128) goto emit_fail;
                        uint16_t sdst = (uint16_t)(256 + (uint16_t)(vsp - n) * 16u);
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_OBJ)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, sdst))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 176))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(sdst + 8u)))) goto emit_fail;
                    }
                }

                for (int j = 0; j < n; j++) {
                    A64TraceVal t = tstack[--vsp];
                    if (t.is_temp) a64_free_temp(&temp_used, t.reg);
                }
                if (fused_build_get) {
                    uint8_t sval0 = (uint8_t)vsp;
                    tstack[vsp++] = (A64TraceVal){9, 0, sval0, 0, NIL_VAL};
                    uint8_t sval1 = (uint8_t)vsp;
                    tstack[vsp++] = (A64TraceVal){9, 0, sval1, 0, NIL_VAL};
                    i += 5; // consume STORE_LOCAL + PUSH_LOCAL + PUSH_LOCAL + GET_TABLE + STORE_LOCAL
                } else if (fused_store) {
                    tstack[vsp++] = (A64TraceVal){3, 0, dst_slot, 0, NIL_VAL};
                    i++; // consume fused STORE_LOCAL
                } else {
                    uint8_t sval = (uint8_t)vsp;
                    tstack[vsp++] = (A64TraceVal){9, 0, sval, 0, NIL_VAL};
                }
                break;
            }
            case TOI_JIT_NATIVE_OP_CALL:
            case TOI_JIT_NATIVE_OP_INVOKE: {
                int argc = (int)in->a;
                int nvals = argc + 1; // callee/receiver + args
                if (argc < 0 || vsp < nvals) goto emit_fail;
                int out_slot = vsp - nvals;
                int guarded_closure_call = (in->op == TOI_JIT_NATIVE_OP_CALL &&
                                            IS_CLOSURE(in->constant) &&
                                            !jit_native_call_poly_enabled());
                uint64_t guarded_program_hint = guarded_closure_call ? in->aux_data : 0u;
                JitNativeCalleeEntryFn guarded_entry_fn = NULL;
                if (guarded_closure_call && guarded_program_hint != 0u) {
                    guarded_entry_fn = jit_native_get_or_create_callee_entry(
                        AS_CLOSURE(in->constant),
                        (void*)(uintptr_t)guarded_program_hint,
                        argc);
                }
                int direct_local_callee = 0;
                uint8_t direct_callee_slot = 0;
                if (!guarded_closure_call && in->op == TOI_JIT_NATIVE_OP_CALL) {
                    A64TraceVal c = tstack[out_slot];
                    if (c.kind == 1 || c.kind == 3 || c.kind == 4) {
                        direct_local_callee = 1;
                        direct_callee_slot = c.slot;
                    }
                }

                // Keep direct-core bounded: stage up to 48 Value operands.
                if (nvals > CALL_STAGE_CAP || out_slot >= 128) {
                    uint8_t reason = (in->op == TOI_JIT_NATIVE_OP_CALL)
                        ? (uint8_t)TOI_JIT_NATIVE_DEOPT_CALL_TARGET
                        : (uint8_t)TOI_JIT_NATIVE_DEOPT_INVOKE_TARGET;
                    uint64_t packed_state = (uint64_t)in->deopt_offset |
                                            ((uint64_t)in->op << 32) |
                                            ((uint64_t)reason << 40);
                    if (!a64_emit_mov_imm64(&buf, 9, packed_state)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 4, 0))) goto emit_fail; // resume/op/reason/stack_count=0
                    if (continue_branch_count >= 256) goto emit_fail;
                    continue_branches[continue_branch_count++] = buf.count;
                    if (!a64_emit(&buf, 0x14000000u)) goto emit_fail; // b continue
                    break;
                }

                int direct_local_args = 0;
                uint8_t direct_arg_base = 0;
                if (argc == 0) {
                    direct_local_args = 1;
                } else {
                    A64TraceVal first_arg = tstack[out_slot + 1];
                    if (first_arg.kind == 1 || first_arg.kind == 3 || first_arg.kind == 4) {
                        direct_local_args = 1;
                        direct_arg_base = first_arg.slot;
                        for (int j = 1; j < argc; j++) {
                            A64TraceVal aj = tstack[out_slot + 1 + j];
                            if (!(aj.kind == 1 || aj.kind == 3 || aj.kind == 4) ||
                                aj.slot != (uint8_t)(direct_arg_base + j)) {
                                direct_local_args = 0;
                                break;
                            }
                        }
                    }
                }

                // Materialize callee/receiver at CALL_STAGE_BASE and, when needed,
                // args at CALL_STAGE_BASE+16...
                for (int j = 0; j < nvals; j++) {
                    if (guarded_closure_call && j == 0) continue;
                    if (direct_local_callee && j == 0) continue;
                    if (direct_local_args && j > 0) continue;
                    A64TraceVal p = tstack[out_slot + j];
                    uint16_t base = (uint16_t)(CALL_STAGE_BASE + (uint16_t)j * 16u);
                    if (p.kind == 2 && IS_STRING(p.constant)) {
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_OBJ)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, base))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)AS_STRING(p.constant))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(base + 8)))) goto emit_fail;
                    } else if (p.kind == 3 || p.kind == 4 || p.kind == 1) {
                        uint16_t src = (uint16_t)((uint16_t)p.slot * (uint16_t)sizeof(Value));
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, src))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, base))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 2, (uint16_t)(src + 8)))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(base + 8)))) goto emit_fail;
                    } else if (p.kind == 9) {
                        uint16_t src = (uint16_t)(256 + (uint16_t)p.slot * 16u);
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, src))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, base))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, (uint16_t)(src + 8u)))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(base + 8)))) goto emit_fail;
                    } else if (p.kind == 6 || p.kind == 7 || p.kind == 8) {
                        if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail; // x0 = closure
                        if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)p.slot)) goto emit_fail; // x1 = upvalue slot
                        if (!a64_emit(&buf, a64_add_imm(2, 31, base))) goto emit_fail; // x2 = &staged value
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_get_upvalue_slot_as_value)) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        if (deopt_branch_count >= 256) goto emit_fail;
                        deopt_branches[deopt_branch_count++] = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq deopt
                    } else if (p.kind == 0) {
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)VAL_NUMBER)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, base))) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_d_imm(p.reg, 31, (uint16_t)(base + 8)))) goto emit_fail;
                    } else if (p.kind == 5) {
                        uint64_t tag = (uint64_t)p.constant.type;
                        uint64_t payload = 0u;
                        if (IS_BOOL(p.constant)) payload = (uint64_t)AS_BOOL(p.constant);
                        else if (IS_OBJ(p.constant)) payload = (uint64_t)(uintptr_t)AS_OBJ(p.constant);
                        if (!a64_emit_mov_imm64(&buf, 9, tag)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, base))) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 9, payload)) goto emit_fail;
                        if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(base + 8)))) goto emit_fail;
                    } else {
                        goto emit_fail;
                    }
                }

                int b_guarded_entry_success = -1;
                if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 24))) goto emit_fail; // x0 = vm
                if (guarded_closure_call) {
                    if (guarded_entry_fn != NULL) {
                        if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)(uintptr_t)AS_CLOSURE(in->constant))) goto emit_fail; // x1 = callee closure*
                        if (direct_local_args) {
                            if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail; // x2 = slots
                            if (argc > 0) {
                                uint16_t arg_off = (uint16_t)((uint16_t)direct_arg_base * (uint16_t)sizeof(Value));
                                if (!a64_emit(&buf, a64_add_imm(2, 2, arg_off))) goto emit_fail; // x2 = args ptr
                            }
                        } else {
                            if (!a64_emit(&buf, a64_add_imm(2, 31, (uint16_t)(CALL_STAGE_BASE + 16u)))) goto emit_fail; // x2 = staged args
                        }
                        if (!a64_emit(&buf, a64_add_imm(3, 31, 184))) goto emit_fail; // x3 = &out Value
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)guarded_entry_fn)) goto emit_fail;
                        if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                        if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                        if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                        b_guarded_entry_success = buf.count;
                        if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne success
                        if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 24))) goto emit_fail; // x0 = vm
                    }
                    // Guarded-entry miss path stays native: dispatch with known callee/program
                    // and only continue to VM if this fallback also rejects.
                    if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)(uintptr_t)AS_CLOSURE(in->constant))) goto emit_fail; // x1 = callee closure*
                    if (direct_local_args) {
                        if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail; // x2 = slots
                        if (argc > 0) {
                            uint16_t arg_off = (uint16_t)((uint16_t)direct_arg_base * (uint16_t)sizeof(Value));
                            if (!a64_emit(&buf, a64_add_imm(2, 2, arg_off))) goto emit_fail; // x2 = args ptr
                        }
                    } else {
                        if (!a64_emit(&buf, a64_add_imm(2, 31, (uint16_t)(CALL_STAGE_BASE + 16u)))) goto emit_fail; // x2 = staged args
                    }
                    if (!a64_emit_mov_imm64(&buf, 3, (uint64_t)argc)) goto emit_fail; // x3 = argc
                    if (!a64_emit_mov_imm64(&buf, 4, guarded_program_hint)) goto emit_fail; // x4 = program hint ptr
                    if (!a64_emit(&buf, a64_add_imm(5, 31, 184))) goto emit_fail; // x5 = &out Value
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_call_closure_dispatch_after_entry_miss)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                } else {
                    if (direct_local_callee) {
                        uint16_t callee_off = (uint16_t)((uint16_t)direct_callee_slot * (uint16_t)sizeof(Value));
                        if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 240))) goto emit_fail; // x1 = slots
                        if (!a64_emit(&buf, a64_add_imm(1, 1, callee_off))) goto emit_fail; // x1 = &slots[callee]
                    } else {
                        if (!a64_emit(&buf, a64_add_imm(1, 31, CALL_STAGE_BASE))) goto emit_fail; // x1 = callee/receiver ptr
                    }
                    if (direct_local_args) {
                        if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail; // x2 = slots
                        if (argc > 0) {
                            uint16_t arg_off = (uint16_t)((uint16_t)direct_arg_base * (uint16_t)sizeof(Value));
                            if (!a64_emit(&buf, a64_add_imm(2, 2, arg_off))) goto emit_fail; // x2 = args ptr
                        }
                    } else {
                        if (!a64_emit(&buf, a64_add_imm(2, 31, (uint16_t)(CALL_STAGE_BASE + 16u)))) goto emit_fail; // x2 = staged args
                    }
                    if (!a64_emit_mov_imm64(&buf, 3, (uint64_t)argc)) goto emit_fail; // x3 = argc
                    if (in->op == TOI_JIT_NATIVE_OP_CALL) {
                        if (!a64_emit(&buf, a64_mov_x(5, 3))) goto emit_fail; // x5 = argc
                        if (!a64_emit(&buf, a64_mov_x(4, 2))) goto emit_fail; // x4 = args ptr
                        if (!a64_emit(&buf, a64_mov_x(3, 1))) goto emit_fail; // x3 = callee ptr
                        if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 16))) goto emit_fail; // x1 = caller closure
                        if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)in->deopt_offset)) goto emit_fail; // x2 = callsite offset
                        if (!a64_emit_mov_imm64(&buf, 6, IS_OBJ(in->constant) ? (uint64_t)(uintptr_t)AS_OBJ(in->constant) : 0u)) goto emit_fail; // x6 = target hint obj*
                        if (!a64_emit(&buf, a64_add_imm(7, 31, 184))) goto emit_fail; // x7 = &out Value
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_call_dispatch)) goto emit_fail;
                    } else {
                        if (!IS_STRING(in->constant)) goto emit_fail;
                        if (!a64_emit_mov_imm64(&buf, 4, (uint64_t)(uintptr_t)AS_STRING(in->constant))) goto emit_fail; // x4 = method
                        if (!a64_emit(&buf, a64_add_imm(5, 31, 184))) goto emit_fail; // x5 = &out Value
                        if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_invoke_dispatch_chained)) goto emit_fail;
                    }
                }
                if (!guarded_closure_call) {
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                }
                int b_call_success = -1;
                if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                b_call_success = buf.count;
                if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne success

                // Primitive rejected target; continue in VM at this opcode.
                {
                    uint8_t reason = (in->op == TOI_JIT_NATIVE_OP_CALL)
                        ? (uint8_t)TOI_JIT_NATIVE_DEOPT_CALL_TARGET
                        : (uint8_t)TOI_JIT_NATIVE_DEOPT_INVOKE_TARGET;
                    uint64_t packed_state = (uint64_t)in->deopt_offset |
                                            ((uint64_t)in->op << 32) |
                                            ((uint64_t)reason << 40);
                    if (!a64_emit_mov_imm64(&buf, 9, packed_state)) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 4, 0))) goto emit_fail; // resume/op/reason/stack_count=0
                    if (continue_branch_count >= 256) goto emit_fail;
                    continue_branches[continue_branch_count++] = buf.count;
                    if (!a64_emit(&buf, 0x14000000u)) goto emit_fail; // b continue
                }

                int call_success_label = buf.count;
                if (b_call_success >= 0) {
                    if (!a64_patch_b_cond(&buf, b_call_success, call_success_label, 0x1u)) goto emit_fail; // ne
                }
                if (guarded_closure_call && b_guarded_entry_success >= 0) {
                    if (!a64_patch_b_cond(&buf, b_guarded_entry_success, call_success_label, 0x1u)) goto emit_fail; // ne
                }

                // Push call result as value-scratch at stack position after pop(argc+1)+push(1).
                {
                    uint16_t dst = (uint16_t)(256 + (uint16_t)out_slot * 16u);
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 184))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, dst))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, 192))) goto emit_fail;
                    if (!a64_emit(&buf, a64_str_x_imm(9, 31, (uint16_t)(dst + 8u)))) goto emit_fail;
                }

                for (int j = 0; j < nvals; j++) {
                    A64TraceVal t = tstack[--vsp];
                    if (t.is_temp) a64_free_temp(&temp_used, t.reg);
                }
                {
                    uint8_t sval = (uint8_t)vsp;
                    tstack[vsp++] = (A64TraceVal){9, 0, sval, 0, NIL_VAL};
                }
                break;
            }
            case TOI_JIT_NATIVE_OP_ADJUST_STACK: {
                if ((int)in->a < 0 || (int)in->a > 256) goto emit_fail;
                while (vsp > (int)in->a) {
                    A64TraceVal t = tstack[--vsp];
                    if (t.is_temp) a64_free_temp(&temp_used, t.reg);
                }
                while (vsp < (int)in->a) {
                    tstack[vsp++] = (A64TraceVal){5, 0, 0, 0, NIL_VAL};
                }
                break;
            }
            case TOI_JIT_NATIVE_OP_JUMP: {
                if (jump_patch_count >= 256) goto emit_fail;
                int bpos = buf.count;
                if (!a64_emit(&buf, 0x14000000u)) goto emit_fail;
                jump_patches[jump_patch_count++] = (A64JumpPatch){bpos, in->target, 0, 0};
                break;
            }
            case TOI_JIT_NATIVE_OP_JUMP_IF_FALSE:
            case TOI_JIT_NATIVE_OP_JUMP_IF_TRUE: {
                if (vsp < 1 || jump_patch_count >= 256) goto emit_fail;
                A64TraceVal t = tstack[vsp - 1];
                if (t.kind == 0) {
                    uint8_t z = a64_alloc_temp(&temp_used);
                    if (z == 0) goto emit_fail;
                    if (!a64_emit(&buf, a64_fsub(z, z, z))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fcmp(t.reg, z))) goto emit_fail;
                    int bpos = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    jump_patches[jump_patch_count++] = (A64JumpPatch){
                        bpos,
                        in->target,
                        1,
                        (uint8_t)(in->op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE ? 0x0u : 0x1u) // eq / ne
                    };
                    a64_free_temp(&temp_used, z);
                } else if (t.kind == 4 || t.kind == 3 || t.kind == 1) {
                    if (!a64_emit(&buf, a64_mov_x(0, 2))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, 224))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 2, (uint64_t)t.slot)) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_value_falsey_slot)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    int bpos = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    jump_patches[jump_patch_count++] = (A64JumpPatch){
                        bpos,
                        in->target,
                        1,
                        (uint8_t)(in->op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE ? 0x1u : 0x0u) // ne / eq after falsey()
                    };
                } else if (t.kind == 9) {
                    uint16_t src = (uint16_t)(256 + (uint16_t)t.slot * 16u);
                    if (!a64_emit(&buf, a64_add_imm(0, 31, src))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_value_falsey_value_ptr)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    int bpos = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    jump_patches[jump_patch_count++] = (A64JumpPatch){
                        bpos,
                        in->target,
                        1,
                        (uint8_t)(in->op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE ? 0x1u : 0x0u) // ne / eq after falsey()
                    };
                } else if (t.kind == 6 || t.kind == 7 || t.kind == 8) {
                    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, 16))) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 1, (uint64_t)t.slot)) goto emit_fail;
                    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)&jit_native_value_falsey_upvalue)) goto emit_fail;
                    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, 240))) goto emit_fail;
                    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, 248))) goto emit_fail;
                    if (!a64_emit(&buf, a64_cmp_w_imm(0, 0))) goto emit_fail;
                    int bpos = buf.count;
                    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                    jump_patches[jump_patch_count++] = (A64JumpPatch){
                        bpos,
                        in->target,
                        1,
                        (uint8_t)(in->op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE ? 0x1u : 0x0u) // ne / eq after falsey()
                    };
                } else if (t.kind == 5) {
                    int is_falsey = jit_value_falsey(t.constant) ? 1 : 0;
                    int should_jump = (in->op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE) ? is_falsey : !is_falsey;
                    if (should_jump) {
                        if (jump_patch_count >= 256) goto emit_fail;
                        int bpos = buf.count;
                        if (!a64_emit(&buf, 0x14000000u)) goto emit_fail;
                        jump_patches[jump_patch_count++] = (A64JumpPatch){bpos, in->target, 0, 0};
                    }
                } else {
                    goto emit_fail;
                }
                break;
            }
            default:
                goto emit_fail;
        }
    }

    insn_labels[instr_count] = buf.count;
    for (int i = 0; i < jump_patch_count; i++) {
        int target = (int)jump_patches[i].target_insn;
        int target_pos;
        if (target == 0) target_pos = loop_header;
        // Jumping past the last instruction is a `continue`: it must run the
        // loop increment (the next-iteration transition), not leave the loop.
        else if (target == instr_count) target_pos = insn_labels[instr_count];
        else target_pos = insn_labels[target];
        if (target_pos < 0) goto emit_fail;
        if (jump_patches[i].is_cond) {
            if (!a64_patch_b_cond(&buf, jump_patches[i].branch_pos, target_pos, jump_patches[i].cond)) goto emit_fail;
        } else {
            if (!a64_patch_b(&buf, jump_patches[i].branch_pos, target_pos)) goto emit_fail;
        }
    }

    if (!a64_emit(&buf, 0x14000000u)) goto emit_fail; // fallthrough body -> next iter
    int b_back_to_loop = buf.count - 1;

    int exit_label = buf.count;
    if (!a64_emit_mov_imm64(&buf, 0, (uint64_t)TOI_JIT_NATIVE_EXEC_EXIT)) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_x_imm(30, 31, 3072))) goto emit_fail; // restore LR
    if (!a64_emit(&buf, a64_add_imm(31, 31, 3088))) goto emit_fail;
    if (!a64_emit(&buf, 0xD65F03C0u)) goto emit_fail;

    int continue_label = buf.count;
    if (!a64_emit_mov_imm64(&buf, 0, (uint64_t)TOI_JIT_NATIVE_EXEC_CONTINUE)) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_x_imm(30, 31, 3072))) goto emit_fail; // restore LR
    if (!a64_emit(&buf, a64_add_imm(31, 31, 3088))) goto emit_fail;
    if (!a64_emit(&buf, 0xD65F03C0u)) goto emit_fail;

    int deopt_label = buf.count;
    if (!a64_emit_mov_imm64(&buf, 0, (uint64_t)TOI_JIT_NATIVE_EXEC_DEOPT)) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_x_imm(30, 31, 3072))) goto emit_fail; // restore LR
    if (!a64_emit(&buf, a64_add_imm(31, 31, 3088))) goto emit_fail;
    if (!a64_emit(&buf, 0xD65F03C0u)) goto emit_fail;

    if (!a64_patch_b_cond(&buf, b_bad_slot_count, deopt_label, 0xBu)) goto emit_fail; // lt
    if (!a64_patch_b_cond(&buf, b_var_gt_end, exit_label, 0xCu)) goto emit_fail;       // gt
    if (!a64_patch_b(&buf, b_back_to_loop, loop_header)) goto emit_fail;
    for (int i = 0; i < deopt_branch_count; i++) {
        if (!a64_patch_b_cond(&buf, deopt_branches[i], deopt_label, 0x0u)) goto emit_fail; // eq
    }
    for (int i = 0; i < continue_branch_count; i++) {
        if (!a64_patch_b(&buf, continue_branches[i], continue_label)) goto emit_fail;
    }

    size_t code_size = (size_t)buf.count * sizeof(uint32_t);
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t map_size = ((code_size + (size_t)page_size - 1u) / (size_t)page_size) * (size_t)page_size;

    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_JIT
    mmap_flags |= MAP_JIT;
#endif
    void* mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
    if (mem == MAP_FAILED) goto emit_fail;
    memcpy(mem, buf.words, code_size);
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)mem, (char*)mem + code_size);
#endif
    if (mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, map_size);
        goto emit_fail;
    }
    free(buf.words);
    *out_fn = mem;
    *out_code_mem = mem;
    *out_code_size = map_size;
    return 1;

emit_fail:
    free(buf.words);
    if (debug_direct_core) {
        fprintf(stderr, "[jit-direct-core] fail reason=emit-fail op_index=%d op=%u\n",
                fail_index,
                (unsigned)((fail_index >= 0 && fail_index < instr_count) ? instrs[fail_index].op : 255u));
    }
    return 0;

direct_core_fail:
    if (debug_direct_core) {
        fprintf(stderr, "[jit-direct-core] fail reason=%s op_index=%d op=%u\n",
                fail_reason,
                fail_index,
                (unsigned)((fail_index >= 0 && fail_index < instr_count) ? instrs[fail_index].op : 255u));
    }
    return 0;
}

static int read_u16_local(const uint8_t* code, int offset, int code_count) {
    if (code == NULL || offset < 0 || offset + 1 >= code_count) return -1;
    return ((int)code[offset] << 8) | (int)code[offset + 1];
}

int toi_jit_arm64_host_supported(void) {
#if defined(__aarch64__) && (defined(__APPLE__) || defined(__linux__))
    return 1;
#else
    return 0;
#endif
}

int toi_jit_arm64_compile_qfor_num_stub(
    const uint8_t* code, int code_count, int loop_offset, char* err_buf, size_t err_buf_size) {
    if (code == NULL || code_count <= 0 || loop_offset < 0 || loop_offset >= code_count) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "invalid qfor trace input");
        }
        return 0;
    }
    if (code[loop_offset] != OP_QFOR_LOOP_NUM && code[loop_offset] != OP_FOR_LOOP) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "arm64: unsupported loop opcode %u", (unsigned)code[loop_offset]);
        }
        return 0;
    }
    if (loop_offset + 4 >= code_count) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "arm64: truncated qfor loop");
        }
        return 0;
    }

    int var_slot = (int)code[loop_offset + 1];
    int end_slot = (int)code[loop_offset + 2];
    int jump = read_u16_local(code, loop_offset + 3, code_count);
    if (jump < 0) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "arm64: invalid qfor jump");
        }
        return 0;
    }
    int loop_start = (loop_offset + 5) - jump;
    if (loop_start < 0 || loop_start >= loop_offset) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "arm64: qfor jump target out of range");
        }
        return 0;
    }

    Arm64CodeBuf buf;
    memset(&buf, 0, sizeof(buf));

    // Prologue-ish stack reservation (placeholder until executable memory/runtime call ABI is wired).
    if (!a64_emit(&buf, 0xD10043FFu)) goto emit_fail; // sub sp, sp, #16

    // Seed virtual loop state from decoded slots to prove real decode->emit path.
    if (!a64_emit(&buf, 0xD2800000u | (((uint32_t)var_slot & 0xffffu) << 5) | 9u)) goto emit_fail;   // movz x9, #var_slot
    if (!a64_emit(&buf, 0xD2800000u | (((uint32_t)end_slot & 0xffffu) << 5) | 10u)) goto emit_fail;  // movz x10, #end_slot

    int loop_label = buf.count;
    if (!a64_emit(&buf, 0x91000529u)) goto emit_fail; // add x9, x9, #1
    if (!a64_emit(&buf, 0xEB0A013Fu)) goto emit_fail; // cmp x9, x10
    int b_pos = buf.count;
    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.cond <patch>

    if (!a64_emit(&buf, 0x910043FFu)) goto emit_fail; // add sp, sp, #16
    if (!a64_emit(&buf, 0xD65F03C0u)) goto emit_fail; // ret

    // LE (cond=0b1101): continue while x9 <= x10.
    if (!a64_patch_b_cond(&buf, b_pos, loop_label, 0xDu)) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "arm64: branch patch failed");
        }
        free(buf.words);
        return 0;
    }

    if (err_buf != NULL && err_buf_size > 0) {
        snprintf(
            err_buf,
            err_buf_size,
            "arm64 emitted %d words for qfor at %d (loop_start=%d)",
            buf.count,
            loop_offset,
            loop_start);
    }
    free(buf.words);
    (void)memchr(code, 0, (size_t)code_count);
    return 1;

emit_fail:
    free(buf.words);
    if (err_buf != NULL && err_buf_size > 0) {
        snprintf(err_buf, err_buf_size, "arm64: code emit failed");
    }
    return 0;
}

int toi_jit_arm64_compile_qfor_empty(
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "arm64: invalid output pointers");
        }
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;

    if (!toi_jit_arm64_host_supported()) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "arm64: host unsupported");
        }
        return 0;
    }

    // x0=var, x1=end
    // loop:
    //   add x0, x0, #1
    //   cmp x0, x1
    //   b.le loop
    //   ret
    uint32_t code_words[] = {
        0x91000400u, // add x0, x0, #1
        0xEB01001Fu, // cmp x0, x1
        0x54FFFFCDu, // b.le -2 instructions
        0xD65F03C0u  // ret
    };
    size_t code_size = sizeof(code_words);

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t map_size = ((code_size + (size_t)page_size - 1u) / (size_t)page_size) * (size_t)page_size;

    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_JIT
    mmap_flags |= MAP_JIT;
#endif
    void* mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
    if (mem == MAP_FAILED) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "arm64: mmap failed");
        }
        return 0;
    }

    memcpy(mem, code_words, code_size);
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)mem, (char*)mem + code_size);
#endif

    if (mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, map_size);
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "arm64: mprotect rx failed");
        }
        return 0;
    }

    *out_fn = mem;
    *out_code_mem = mem;
    *out_code_size = map_size;
    return 1;
}

int toi_jit_arm64_compile_math_reduce(
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "arm64: invalid output pointers");
        }
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;

    if (!toi_jit_arm64_host_supported()) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "arm64: host unsupported");
        }
        return 0;
    }

    // Signature:
    //   fn(double* acc_ptr, double* var_ptr, double* end_ptr, double c1, double c2, double c3)
    // Registers:
    //   x0=acc_ptr x1=var_ptr x2=end_ptr d0=c1 d1=c2 d2=c3
    // Kernel:
    //   while (++var <= end) acc += (var*c1) / ((var % c2) + c3)
    static const uint32_t code_words[] = {
        0xfd400003u, // ldr d3, [x0]
        0xfd400024u, // ldr d4, [x1]
        0xfd400045u, // ldr d5, [x2]
        0x1e6e1006u, // fmov d6, #1.0
        0x1e662884u, // fadd d4, d4, d6
        0x1e600887u, // fmul d7, d4, d0
        0x1e611888u, // fdiv d8, d4, d1
        0x1e65c108u, // frintz d8, d8
        0x1e610908u, // fmul d8, d8, d1
        0x1e683888u, // fsub d8, d4, d8
        0x1e622908u, // fadd d8, d8, d2
        0x1e6818e7u, // fdiv d7, d7, d8
        0x1e672863u, // fadd d3, d3, d7
        0x1e652080u, // fcmp d4, d5
        0x54fffecdu, // b.le to fadd d4,d4,d6
        0xfd000003u, // str d3, [x0]
        0xfd000024u, // str d4, [x1]
        0xd65f03c0u  // ret
    };
    size_t code_size = sizeof(code_words);

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t map_size = ((code_size + (size_t)page_size - 1u) / (size_t)page_size) * (size_t)page_size;

    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_JIT
    mmap_flags |= MAP_JIT;
#endif
    void* mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
    if (mem == MAP_FAILED) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "arm64: mmap failed");
        }
        return 0;
    }

    memcpy(mem, code_words, code_size);
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)mem, (char*)mem + code_size);
#endif
    if (mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, map_size);
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "arm64: mprotect rx failed");
        }
        return 0;
    }

    *out_fn = mem;
    *out_code_mem = mem;
    *out_code_size = map_size;
    return 1;
}

void toi_jit_arm64_free_code(void* code_mem, size_t code_size) {
    if (code_mem == NULL || code_size == 0) return;
    munmap(code_mem, code_size);
}

void toi_jit_arm64_get_chain_stats(uint64_t* attempts, uint64_t* hits, uint64_t* misses) {
    if (attempts != NULL) *attempts = jit_native_chain_attempts;
    if (hits != NULL) *hits = jit_native_chain_hits;
    if (misses != NULL) *misses = jit_native_chain_misses;
}

int toi_jit_arm64_compile_numeric_loop(
    const ToiJitNumInstr* instrs,
    int instr_count,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: invalid outputs");
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;
    if (!toi_jit_arm64_host_supported()) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: host unsupported");
        return 0;
    }
    if (instrs == NULL || instr_count <= 0) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: invalid loop ir");
        return 0;
    }

    Arm64CodeBuf buf;
    memset(&buf, 0, sizeof(buf));

    // x0=acc_ptr x1=var_ptr x2=end_ptr x3=consts_ptr
    // d3=acc d4=var d5=end d6=1.0
    if (!a64_emit(&buf, a64_ldr_d_imm(3, 0, 0))) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_d_imm(4, 1, 0))) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_d_imm(5, 2, 0))) goto emit_fail;
    if (!a64_emit(&buf, 0x1e6e1006u)) goto emit_fail; // fmov d6, #1.0

    // Entry semantics: the VM enters this helper from the loop backedge, so the
    // induction variable still holds the value of the last executed iteration.
    // Advance first and bail out when the range is exhausted: checking the bound
    // only at the tail would execute one extra iteration.
    int loop_label = buf.count;
    if (!a64_emit(&buf, a64_fadd(4, 4, 6))) goto emit_fail; // var += 1
    if (!a64_emit(&buf, a64_fcmp(4, 5))) goto emit_fail;    // fcmp d4, d5
    int b_skip = buf.count;
    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.cond (gt -> exit)

    A64ValReg vstack[64];
    int vsp = 0;
    uint16_t temp_used = 0;

    for (int i = 0; i < instr_count; i++) {
        uint8_t op = instrs[i].op;
        uint8_t arg = instrs[i].arg;
        switch (op) {
            case TOI_JIT_NUM_PUSH_ACC:
                if (vsp >= 64) goto emit_fail;
                vstack[vsp++] = (A64ValReg){3, 0};
                break;
            case TOI_JIT_NUM_PUSH_VAR:
                if (vsp >= 64) goto emit_fail;
                vstack[vsp++] = (A64ValReg){4, 0};
                break;
            case TOI_JIT_NUM_PUSH_END:
                if (vsp >= 64) goto emit_fail;
                vstack[vsp++] = (A64ValReg){5, 0};
                break;
            case TOI_JIT_NUM_PUSH_CONST: {
                if (vsp >= 64) goto emit_fail;
                uint8_t rd = a64_alloc_temp(&temp_used);
                if (rd == 0) goto emit_fail;
                uint16_t off = (uint16_t)((uint16_t)arg * 8u);
                if (!a64_emit(&buf, a64_ldr_d_imm(rd, 3, off))) goto emit_fail;
                vstack[vsp++] = (A64ValReg){rd, 1};
                break;
            }
            case TOI_JIT_NUM_BIN_ADD:
            case TOI_JIT_NUM_BIN_SUB:
            case TOI_JIT_NUM_BIN_MUL:
            case TOI_JIT_NUM_BIN_DIV:
            case TOI_JIT_NUM_BIN_MOD: {
                if (vsp < 2) goto emit_fail;
                A64ValReg b = vstack[--vsp];
                A64ValReg a = vstack[--vsp];
                uint8_t rd = a64_alloc_temp(&temp_used);
                if (rd == 0) goto emit_fail;
                if (op == TOI_JIT_NUM_BIN_ADD) {
                    if (!a64_emit(&buf, a64_fadd(rd, a.reg, b.reg))) goto emit_fail;
                } else if (op == TOI_JIT_NUM_BIN_SUB) {
                    if (!a64_emit(&buf, a64_fsub(rd, a.reg, b.reg))) goto emit_fail;
                } else if (op == TOI_JIT_NUM_BIN_MUL) {
                    if (!a64_emit(&buf, a64_fmul(rd, a.reg, b.reg))) goto emit_fail;
                } else if (op == TOI_JIT_NUM_BIN_DIV) {
                    if (!a64_emit(&buf, a64_fdiv(rd, a.reg, b.reg))) goto emit_fail;
                } else {
                    uint8_t q = a64_alloc_temp(&temp_used);
                    if (q == 0) goto emit_fail;
                    if (!a64_emit(&buf, a64_fdiv(q, a.reg, b.reg))) goto emit_fail;
                    if (!a64_emit(&buf, a64_frintz(q, q))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fmul(q, q, b.reg))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fsub(rd, a.reg, q))) goto emit_fail;
                    a64_free_temp(&temp_used, q);
                }
                if (a.is_temp) a64_free_temp(&temp_used, a.reg);
                if (b.is_temp) a64_free_temp(&temp_used, b.reg);
                vstack[vsp++] = (A64ValReg){rd, 1};
                break;
            }
            case TOI_JIT_NUM_STORE_ACC: {
                if (vsp < 1) goto emit_fail;
                A64ValReg t = vstack[vsp - 1];
                if (t.reg != 3) {
                    if (!a64_emit(&buf, a64_fmov_rr(3, t.reg))) goto emit_fail;
                }
                break;
            }
            case TOI_JIT_NUM_POP: {
                if (vsp < 1) goto emit_fail;
                A64ValReg t = vstack[--vsp];
                if (t.is_temp) a64_free_temp(&temp_used, t.reg);
                break;
            }
            default:
                goto emit_fail;
        }
    }

    if (!a64_emit(&buf, a64_fadd(4, 4, 6))) goto emit_fail; // var += 1
    if (!a64_emit(&buf, a64_fcmp(4, 5))) goto emit_fail;    // fcmp d4, d5
    int bpos = buf.count;
    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.cond
    if (!a64_patch_b_cond(&buf, bpos, loop_label + 3, 0xDu)) goto emit_fail; // le body
    if (!a64_patch_b_cond(&buf, b_skip, buf.count, 0xCu)) goto emit_fail;   // gt exit

    if (!a64_emit(&buf, a64_str_d_imm(3, 0, 0))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_d_imm(4, 1, 0))) goto emit_fail;
    if (!a64_emit(&buf, 0xd65f03c0u)) goto emit_fail; // ret

    size_t code_size = (size_t)buf.count * sizeof(uint32_t);
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t map_size = ((code_size + (size_t)page_size - 1u) / (size_t)page_size) * (size_t)page_size;

    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_JIT
    mmap_flags |= MAP_JIT;
#endif
    void* mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
    if (mem == MAP_FAILED) goto emit_fail;
    memcpy(mem, buf.words, code_size);
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)mem, (char*)mem + code_size);
#endif
    if (mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, map_size);
        goto emit_fail;
    }
    free(buf.words);
    *out_fn = mem;
    *out_code_mem = mem;
    *out_code_size = map_size;
    return 1;

emit_fail:
    free(buf.words);
    if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: numeric loop emit failed");
    return 0;
}

int toi_jit_arm64_compile_slot_loop(
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
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: invalid outputs");
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;
    if (!toi_jit_arm64_host_supported()) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: host unsupported");
        return 0;
    }
    if (instrs == NULL || instr_count <= 0) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: invalid slot loop ir");
        return 0;
    }

    Arm64CodeBuf buf;
    memset(&buf, 0, sizeof(buf));

    // x0=slots_ptr x1=consts_ptr
    uint16_t var_off = (uint16_t)((uint16_t)var_slot * 8u);
    uint16_t end_off = (uint16_t)((uint16_t)end_slot * 8u);
    if (!a64_emit(&buf, a64_ldr_d_imm(4, 0, var_off))) goto emit_fail; // d4 = var
    if (!a64_emit(&buf, a64_ldr_d_imm(5, 0, end_off))) goto emit_fail; // d5 = end
    if (!a64_emit(&buf, 0x1e6e1006u)) goto emit_fail;                   // d6 = 1.0

    // The body reads induction/accumulator values out of the slot array, so the
    // array must be the source of truth: store the advanced induction value
    // before every body execution and re-read it afterwards (the body may have
    // written it). Bound checks happen before each body, otherwise the helper
    // runs one extra iteration.
    if (!a64_emit(&buf, a64_fadd(4, 4, 6))) goto emit_fail;            // var += 1
    if (!a64_emit(&buf, a64_str_d_imm(4, 0, var_off))) goto emit_fail; // slots[var] = var
    if (!a64_emit(&buf, a64_fcmp(4, 5))) goto emit_fail;               // fcmp d4, d5
    int b_skip = buf.count;
    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.cond (gt -> exit)
    int loop_label = buf.count;

    typedef struct {
        int branch_pos;
        uint8_t target_insn;
        uint8_t is_cond;
        uint8_t cond;
    } A64JumpPatch;

    A64ValReg vstack[64];
    int insn_labels[256];
    for (int i = 0; i < 256; i++) insn_labels[i] = -1;
    A64JumpPatch jump_patches[256];
    int jump_patch_count = 0;
    int vsp = 0;
    uint16_t temp_used = 0;
    for (int i = 0; i < instr_count; i++) {
        insn_labels[i] = buf.count;
        uint8_t op = instrs[i].op;
        uint8_t arg = instrs[i].arg;
        switch (op) {
            case TOI_JIT_SLOT_PUSH_SLOT: {
                if (vsp >= 64) goto emit_fail;
                uint8_t rd = a64_alloc_temp(&temp_used);
                if (rd == 0) goto emit_fail;
                uint16_t off = (uint16_t)((uint16_t)arg * 8u);
                if (!a64_emit(&buf, a64_ldr_d_imm(rd, 0, off))) goto emit_fail;
                vstack[vsp++] = (A64ValReg){rd, 1};
                break;
            }
            case TOI_JIT_SLOT_PUSH_CONST: {
                if (vsp >= 64) goto emit_fail;
                uint8_t rd = a64_alloc_temp(&temp_used);
                if (rd == 0) goto emit_fail;
                uint16_t off = (uint16_t)((uint16_t)arg * 8u);
                if (!a64_emit(&buf, a64_ldr_d_imm(rd, 1, off))) goto emit_fail;
                vstack[vsp++] = (A64ValReg){rd, 1};
                break;
            }
            case TOI_JIT_SLOT_BIN_ADD:
            case TOI_JIT_SLOT_BIN_SUB:
            case TOI_JIT_SLOT_BIN_MUL:
            case TOI_JIT_SLOT_BIN_DIV:
            case TOI_JIT_SLOT_BIN_MOD: {
                if (vsp < 2) goto emit_fail;
                A64ValReg b = vstack[--vsp];
                A64ValReg a = vstack[--vsp];
                uint8_t rd = a64_alloc_temp(&temp_used);
                if (rd == 0) goto emit_fail;
                if (op == TOI_JIT_SLOT_BIN_ADD) {
                    if (!a64_emit(&buf, a64_fadd(rd, a.reg, b.reg))) goto emit_fail;
                } else if (op == TOI_JIT_SLOT_BIN_SUB) {
                    if (!a64_emit(&buf, a64_fsub(rd, a.reg, b.reg))) goto emit_fail;
                } else if (op == TOI_JIT_SLOT_BIN_MUL) {
                    if (!a64_emit(&buf, a64_fmul(rd, a.reg, b.reg))) goto emit_fail;
                } else if (op == TOI_JIT_SLOT_BIN_DIV) {
                    if (!a64_emit(&buf, a64_fdiv(rd, a.reg, b.reg))) goto emit_fail;
                } else {
                    uint8_t q = a64_alloc_temp(&temp_used);
                    if (q == 0) goto emit_fail;
                    if (!a64_emit(&buf, a64_fdiv(q, a.reg, b.reg))) goto emit_fail;
                    if (!a64_emit(&buf, a64_frintz(q, q))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fmul(q, q, b.reg))) goto emit_fail;
                    if (!a64_emit(&buf, a64_fsub(rd, a.reg, q))) goto emit_fail;
                    a64_free_temp(&temp_used, q);
                }
                if (a.is_temp) a64_free_temp(&temp_used, a.reg);
                if (b.is_temp) a64_free_temp(&temp_used, b.reg);
                vstack[vsp++] = (A64ValReg){rd, 1};
                break;
            }
            case TOI_JIT_SLOT_CMP_LT:
            case TOI_JIT_SLOT_CMP_GT:
            case TOI_JIT_SLOT_CMP_EQ: {
                if (vsp < 2) goto emit_fail;
                A64ValReg b = vstack[--vsp];
                A64ValReg a = vstack[--vsp];
                uint8_t rd = a64_alloc_temp(&temp_used);
                if (rd == 0) goto emit_fail;
                if (!a64_emit(&buf, a64_fsub(rd, 6, 6))) goto emit_fail; // rd = 0.0
                if (!a64_emit(&buf, a64_fcmp(a.reg, b.reg))) goto emit_fail;
                int btrue = buf.count;
                if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                if (!a64_emit(&buf, a64_fmov_rr(rd, 6))) goto emit_fail;  // rd = 1.0
                int bend = buf.count;
                if (!a64_emit(&buf, 0x14000000u)) goto emit_fail;
                int false_label = buf.count;
                if (!a64_patch_b_cond(&buf, btrue, false_label, 0x0u)) goto emit_fail; // b.eq false
                int end_label = buf.count;
                if (!a64_patch_b(&buf, bend, end_label)) goto emit_fail;
                if (op == TOI_JIT_SLOT_CMP_LT) {
                    if (!a64_patch_b_cond(&buf, btrue, false_label, 0xAu)) goto emit_fail; // b.ge false
                } else if (op == TOI_JIT_SLOT_CMP_GT) {
                    if (!a64_patch_b_cond(&buf, btrue, false_label, 0xDu)) goto emit_fail; // b.le false
                } else {
                    if (!a64_patch_b_cond(&buf, btrue, false_label, 0x1u)) goto emit_fail; // b.ne false
                }
                if (a.is_temp) a64_free_temp(&temp_used, a.reg);
                if (b.is_temp) a64_free_temp(&temp_used, b.reg);
                vstack[vsp++] = (A64ValReg){rd, 1};
                break;
            }
            case TOI_JIT_SLOT_STORE_SLOT: {
                if (vsp < 1) goto emit_fail;
                A64ValReg t = vstack[vsp - 1];
                uint16_t off = (uint16_t)((uint16_t)arg * 8u);
                if (!a64_emit(&buf, a64_str_d_imm(t.reg, 0, off))) goto emit_fail;
                break;
            }
            case TOI_JIT_SLOT_JUMP: {
                if (jump_patch_count >= 256) goto emit_fail;
                int bpos = buf.count;
                if (!a64_emit(&buf, 0x14000000u)) goto emit_fail;
                jump_patches[jump_patch_count++] = (A64JumpPatch){
                    bpos, arg, 0, 0
                };
                break;
            }
            case TOI_JIT_SLOT_JUMP_IF_FALSE:
            case TOI_JIT_SLOT_JUMP_IF_TRUE: {
                if (vsp < 1) goto emit_fail;
                if (jump_patch_count >= 256) goto emit_fail;
                A64ValReg t = vstack[vsp - 1];
                uint8_t zero = a64_alloc_temp(&temp_used);
                if (zero == 0) goto emit_fail;
                if (!a64_emit(&buf, a64_fsub(zero, 6, 6))) goto emit_fail; // zero = 0.0
                if (!a64_emit(&buf, a64_fcmp(t.reg, zero))) goto emit_fail;
                int bpos = buf.count;
                if (!a64_emit(&buf, 0x54000000u)) goto emit_fail;
                jump_patches[jump_patch_count++] = (A64JumpPatch){
                    bpos,
                    arg,
                    1,
                    (uint8_t)(op == TOI_JIT_SLOT_JUMP_IF_FALSE ? 0x0u : 0x1u) // eq / ne
                };
                a64_free_temp(&temp_used, zero);
                break;
            }
            case TOI_JIT_SLOT_POP: {
                if (vsp < 1) goto emit_fail;
                A64ValReg t = vstack[--vsp];
                if (t.is_temp) a64_free_temp(&temp_used, t.reg);
                break;
            }
            default:
                goto emit_fail;
        }
    }

    int body_end_label = buf.count;
    insn_labels[instr_count] = body_end_label;
    for (int i = 0; i < jump_patch_count; i++) {
        int target = (int)jump_patches[i].target_insn;
        if (target < 0 || target > instr_count) goto emit_fail;
        int target_pos = insn_labels[target];
        if (target_pos < 0) goto emit_fail;
        if (jump_patches[i].is_cond) {
            if (!a64_patch_b_cond(&buf, jump_patches[i].branch_pos, target_pos, jump_patches[i].cond)) goto emit_fail;
        } else {
            if (!a64_patch_b(&buf, jump_patches[i].branch_pos, target_pos)) goto emit_fail;
        }
    }

    if (!a64_emit(&buf, a64_ldr_d_imm(4, 0, var_off))) goto emit_fail; // reload var
    if (!a64_emit(&buf, a64_fadd(4, 4, 6))) goto emit_fail;            // var += 1
    if (!a64_emit(&buf, a64_str_d_imm(4, 0, var_off))) goto emit_fail; // slots[var] = var
    if (!a64_emit(&buf, a64_fcmp(4, 5))) goto emit_fail;               // fcmp d4, d5
    int bpos = buf.count;
    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.cond
    if (!a64_patch_b_cond(&buf, bpos, loop_label, 0xDu)) goto emit_fail;   // le body
    if (!a64_patch_b_cond(&buf, b_skip, buf.count, 0xCu)) goto emit_fail; // gt exit

    if (!a64_emit(&buf, 0xd65f03c0u)) goto emit_fail; // ret

    size_t code_size = (size_t)buf.count * sizeof(uint32_t);
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t map_size = ((code_size + (size_t)page_size - 1u) / (size_t)page_size) * (size_t)page_size;
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_JIT
    mmap_flags |= MAP_JIT;
#endif
    void* mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
    if (mem == MAP_FAILED) goto emit_fail;
    memcpy(mem, buf.words, code_size);
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)mem, (char*)mem + code_size);
#endif
    if (mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, map_size);
        goto emit_fail;
    }
    free(buf.words);
    *out_fn = mem;
    *out_code_mem = mem;
    *out_code_size = map_size;
    return 1;

emit_fail:
    free(buf.words);
    if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: slot loop emit failed");
    return 0;
}

int toi_jit_arm64_compile_value_loop(
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
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: invalid outputs");
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;
    if (instrs == NULL || instr_count <= 0) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: invalid value ir");
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
                if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: unsupported value op %u", (unsigned)op);
                return 0;
        }
    }
    (void)var_slot;
    (void)end_slot;
    *out_fn = (void*)jit_value_loop_runner;
    return 1;
}

int toi_jit_arm64_compile_native_trace(
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
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: invalid outputs");
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;
    if (out_is_direct_core != NULL) *out_is_direct_core = 0;
    if (!toi_jit_arm64_host_supported()) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: host unsupported");
        return 0;
    }
    if (instrs == NULL || instr_count <= 0 || instr_count > 256) {
        if (err_buf != NULL && err_buf_size > 0) snprintf(err_buf, err_buf_size, "arm64: invalid native trace");
        return 0;
    }

    // First try direct emitted trace-op execution (no C bridge dispatch),
    // unless explicitly forced to wrapper mode.
    if (!jit_native_trace_force_bridge_wrapper()) {
        if (toi_jit_arm64_compile_native_trace_direct_core(
                instrs,
                instr_count,
                guards,
                guard_count,
                var_slot,
                end_slot,
                out_fn,
                out_code_mem,
                out_code_size)) {
            if (out_is_direct_core != NULL) *out_is_direct_core = 1;
            return 1;
        }
    }

    Arm64CodeBuf buf;
    memset(&buf, 0, sizeof(buf));

    // AArch64 ABI input:
    // x0=vm x1=closure x2=slots x3=slot_count x4=out_state
    // Execute a native chunk budget inside one bridge call.
    enum { NTRACE_CHUNK_ITERS = 128 };
    enum { NTRACE_FRAME_BYTES = 80 };
    enum { OFF_VM = 0, OFF_CLOSURE = 8, OFF_SLOTS = 16, OFF_SLOT_COUNT = 24, OFF_STATE = 32, OFF_INSTRS = 40, OFF_META = 48, OFF_BRIDGE = 56, OFF_LR = 64 };

    uint64_t packed_meta = ((uint64_t)(uint32_t)instr_count & 0xffffu) |
                           (((uint64_t)var_slot & 0xffu) << 16) |
                           (((uint64_t)end_slot & 0xffu) << 24);

    if (!a64_emit(&buf, a64_sub_imm(31, 31, NTRACE_FRAME_BYTES))) goto emit_fail; // sub sp,sp,#64
    if (!a64_emit(&buf, a64_str_x_imm(0, 31, OFF_VM))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(1, 31, OFF_CLOSURE))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(2, 31, OFF_SLOTS))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(3, 31, OFF_SLOT_COUNT))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(4, 31, OFF_STATE))) goto emit_fail;
    if (!a64_emit_mov_imm64(&buf, 5, (uint64_t)(uintptr_t)instrs)) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(5, 31, OFF_INSTRS))) goto emit_fail;
    if (!a64_emit_mov_imm64(&buf, 6, packed_meta)) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(6, 31, OFF_META))) goto emit_fail;
    void* runner_fn = (void*)&jit_native_trace_bridge_runner;
    if (!a64_emit_mov_imm64(&buf, 9, (uint64_t)(uintptr_t)runner_fn)) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(9, 31, OFF_BRIDGE))) goto emit_fail;
    if (!a64_emit(&buf, a64_str_x_imm(30, 31, OFF_LR))) goto emit_fail;
    // Execute bridge chunks in native code until we hit a real stop:
    // EXIT/DEOPT, or CONTINUE with non-NONE reason (e.g. CALL/INVOKE soft-bail).
    // This removes per-chunk VM/native round-trips for long stable loops.
    if (!a64_emit(&buf, a64_movz_x(12, 65535u, 0))) goto emit_fail; // safety cap
    int loop_label = buf.count;
    if (!a64_emit(&buf, a64_ldr_x_imm(0, 31, OFF_VM))) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_x_imm(1, 31, OFF_CLOSURE))) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_x_imm(2, 31, OFF_SLOTS))) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_x_imm(3, 31, OFF_SLOT_COUNT))) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_x_imm(4, 31, OFF_STATE))) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_x_imm(5, 31, OFF_INSTRS))) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_x_imm(6, 31, OFF_META))) goto emit_fail;
    if (!a64_emit(&buf, a64_movz_x(7, NTRACE_CHUNK_ITERS, 0))) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_x_imm(9, 31, OFF_BRIDGE))) goto emit_fail;
    if (!a64_emit(&buf, a64_blr(9))) goto emit_fail;

    // if (result != CONTINUE) goto done;
    if (!a64_emit(&buf, a64_cmp_w_imm(0, TOI_JIT_NATIVE_EXEC_CONTINUE))) goto emit_fail;
    int b_not_continue = buf.count;
    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne done

    // if (out_state->reason != NONE) goto done;
    if (!a64_emit(&buf, a64_ldr_x_imm(10, 31, OFF_STATE))) goto emit_fail;
    if (!a64_emit(&buf, a64_ldr_b_imm(11, 10, 5))) goto emit_fail; // reason byte
    if (!a64_emit(&buf, a64_cmp_w_imm(11, TOI_JIT_NATIVE_DEOPT_NONE))) goto emit_fail;
    int b_reason_non_none = buf.count;
    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.ne done

    // Safety cap: if (--budget == 0) stop and return current result.
    if (!a64_emit(&buf, a64_subs_imm(12, 12, 1))) goto emit_fail;
    int b_budget_zero = buf.count;
    if (!a64_emit(&buf, 0x54000000u)) goto emit_fail; // b.eq done

    int b_loop = buf.count;
    if (!a64_emit(&buf, 0x14000000u)) goto emit_fail; // b loop

    int done_label = buf.count;
    if (!a64_patch_b_cond(&buf, b_not_continue, done_label, 0x1u)) goto emit_fail;   // ne
    if (!a64_patch_b_cond(&buf, b_reason_non_none, done_label, 0x1u)) goto emit_fail; // ne
    if (!a64_patch_b_cond(&buf, b_budget_zero, done_label, 0x0u)) goto emit_fail;     // eq
    if (!a64_patch_b(&buf, b_loop, loop_label)) goto emit_fail;

    if (!a64_emit(&buf, a64_ldr_x_imm(30, 31, OFF_LR))) goto emit_fail;
    if (!a64_emit(&buf, a64_add_imm(31, 31, NTRACE_FRAME_BYTES))) goto emit_fail; // add sp,sp,#64
    if (!a64_emit(&buf, 0xD65F03C0u)) goto emit_fail;                                // ret
    (void)done_label;

    size_t code_size = (size_t)buf.count * sizeof(uint32_t);
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t map_size = ((code_size + (size_t)page_size - 1u) / (size_t)page_size) * (size_t)page_size;

    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_JIT
    mmap_flags |= MAP_JIT;
#endif
    void* mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
    if (mem == MAP_FAILED) goto emit_fail;
    memcpy(mem, buf.words, code_size);
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)mem, (char*)mem + code_size);
#endif
    if (mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, map_size);
        goto emit_fail;
    }
    free(buf.words);
    *out_fn = mem;
    *out_code_mem = mem;
    *out_code_size = map_size;
    return 1;

emit_fail:
    free(buf.words);
    if (err_buf != NULL && err_buf_size > 0) {
        snprintf(err_buf, err_buf_size, "arm64 native-trace backend emit failed");
    }
    return 0;
}

ToiJitNativeExecResult toi_jit_arm64_run_native_trace(
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
    return jit_native_trace_bridge_runner(
        vm,
        closure,
        slots,
        slot_count,
        out_state,
        instrs,
        packed_meta,
        max_iters);
}
