#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#include "chunk.h"
#include "jit_arm64.h"
#include "jit_backend.h"
#include "memory.h"
#include "jit_native_trace.h"
#include "jit.h"
#include "jit_call_runtime.h"
#include "jit_internal.h"
#include "jit_ir.h"
#include "object.h"
#include "vm.h"

#include "jit_pipeline.h"

ToiJitState* jit_state(VM* vm) {
    return (ToiJitState*)vm->native_jit;
}

int toi_jit_in_native_trace(VM* vm) {
    ToiJitState* st = jit_state(vm);
    if (st == NULL) return 0;
    return st->native_trace_active_depth != 0;
}

static uint32_t jit_func_hash(ObjFunction* function) {
    uintptr_t p = (uintptr_t)function;
    uint32_t h = (uint32_t)(p >> 4) ^ (uint32_t)(p >> 17);
    h ^= (uint32_t)(p >> 29);
    return h & (TOI_JIT_FUNC_CACHE_SIZE - 1u);
}

static JitTrackedFunction* jit_find_tracked(ToiJitState* st, ObjFunction* function) {
    if (st == NULL || function == NULL) return NULL;
    uint32_t h = jit_func_hash(function);
    JitFuncCacheEntry* e = &st->func_cache[h];
    if (e->tracked_index_plus1 != 0 && e->function == function) {
        uint32_t idx = e->tracked_index_plus1 - 1u;
        if ((int)idx < st->count) return &st->tracked[idx];
        e->tracked_index_plus1 = 0;
        e->function = NULL;
    }
    for (int i = 0; i < st->count; i++) {
        if (st->tracked[i].function == function) {
            e->function = function;
            e->tracked_index_plus1 = (uint32_t)i + 1u;
            return &st->tracked[i];
        }
    }
    return NULL;
}

static JitTrackedFunction* jit_link_site_cache_lookup(
    ToiJitState* st,
    ObjFunction* caller,
    uint32_t opcode_offset,
    ObjFunction* callee) {
    if (st == NULL || st->next_pipeline == NULL || caller == NULL || callee == NULL) return NULL;
    uint32_t tracked_index_plus1 = jit_pipeline_link_site_cache_lookup(
        st->next_pipeline, caller, opcode_offset, callee);
    if (tracked_index_plus1 == 0) return NULL;
    uint32_t idx = tracked_index_plus1 - 1u;
    if ((int)idx >= st->count) {
        jit_pipeline_link_site_cache_store(st->next_pipeline, caller, opcode_offset, callee, 0u);
        return NULL;
    }
    JitTrackedFunction* entry = &st->tracked[idx];
    if (entry->function != callee) {
        jit_pipeline_link_site_cache_store(st->next_pipeline, caller, opcode_offset, callee, 0u);
        return NULL;
    }
    return entry;
}

static void jit_link_site_cache_store(
    ToiJitState* st,
    ObjFunction* caller,
    uint32_t opcode_offset,
    ObjFunction* callee,
    JitTrackedFunction* entry) {
    if (st == NULL || st->next_pipeline == NULL || caller == NULL || callee == NULL || entry == NULL) return;
    if (st->tracked == NULL || st->count <= 0) return;
    int raw_idx = (int)(entry - st->tracked);
    if (raw_idx < 0 || raw_idx >= st->count) return;
    jit_pipeline_link_site_cache_store(
        st->next_pipeline, caller, opcode_offset, callee, (uint32_t)raw_idx + 1u);
}

int execute_program(
    VM* vm,
    ObjClosure* closure,
    int arg_count,
    JitProgram* program,
    Value* args,
    Value* out);
// In-trace callee inlining (see jit_inline_call_program).
#define JIT_INLINE_MAX_SLOTS 10
#define JIT_INLINE_MAX_STACK 24
#define JIT_INLINE_MAX_OPS 24
static int jit_inline_call_program(ObjFunction* fn, JitProgram* program, const Value* args, int argc, Value* out);
static int jit_try_inline_trace_call(JitLoopTrace* trace, int pc, Value callee, const Value* args, int argc, Value* out);
static JitLoopTrace* jit_find_loop(ToiJitState* st, ObjFunction* function, int opcode_offset, uint8_t opcode);
int jit_eval_tiny_inline2_local(ObjFunction* function, Value arg0, Value arg1, Value* out);
static Value numeric_binop(JitOpCode op, double a, double b);
static int build_typed_trace_meta(JitLoopTrace* trace);
static uint8_t typed_exit_reason_for_op(JitOpCode op);
static void jit_record_typed_exit(ToiJitState* st, JitLoopTrace* trace, int pc, uint8_t reason);
static int jit_try_stitch_soft_continue_calls(
    VM* vm,
    CallFrame* frame,
    ToiJitNativeExecState* exec_state,
    uint32_t* io_off);
static uint32_t jit_typed_hot_exit_threshold(void);
static int jit_try_typed_side_exit_to_interp(
    ToiJitState* st,
    JitLoopTrace* trace,
    CallFrame* frame,
    ObjThread* thread,
    uint8_t** ip,
    const Value* work,
    int sp,
    int frame_window,
    int pc);
static void jit_record_side_exit_candidate(ToiJitState* st, JitLoopTrace* trace, uint16_t exit_id, int pc);
static void native_trace_seed_call_program_hints(VM* vm, JitLoopTrace* trace);

static int jit_program_ops_chain_safe(const JitProgram* program) {
    if (program == NULL || program->ops == NULL || program->op_count <= 0) return 0;
    for (int i = 0; i < program->op_count; i++) {
        JitOpCode op = program->ops[i].op;
        switch (op) {
            case JOP_TINY_INLINE:
            case JOP_CONSTANT:
            case JOP_NIL:
            case JOP_TRUE:
            case JOP_FALSE:
            case JOP_POP:
            case JOP_GET_LOCAL:
            case JOP_GET2_LOCAL:
            case JOP_GET_UPVALUE:
            case JOP_SET_LOCAL:
            case JOP_SET_LOCAL_POP:
            case JOP_INC_LOCAL:
            case JOP_SUB_LOCAL_CONST:
            case JOP_MUL_LOCAL_CONST:
            case JOP_DIV_LOCAL_CONST:
            case JOP_MOD_LOCAL_CONST:
            case JOP_ADD_CONST:
            case JOP_SUB_CONST:
            case JOP_MUL_CONST:
            case JOP_DIV_CONST:
            case JOP_MOD_CONST:
            case JOP_ADD:
            case JOP_ADD_INPLACE:
            case JOP_SUBTRACT:
            case JOP_MULTIPLY:
            case JOP_DIVIDE:
            case JOP_MODULO:
            case JOP_NEGATE:
            case JOP_NOT:
            case JOP_IADD:
            case JOP_ISUB:
            case JOP_IMUL:
            case JOP_IDIV:
            case JOP_IMOD:
            case JOP_FADD:
            case JOP_FSUB:
            case JOP_FMUL:
            case JOP_FDIV:
            case JOP_FMOD:
            case JOP_QFADD:
            case JOP_QFSUB:
            case JOP_QFMUL:
            case JOP_QFDIV:
            case JOP_QFMOD:
            case JOP_EQUAL:
            case JOP_QGREATER_NUM:
            case JOP_QLESS_NUM:
            case JOP_JUMP:
            case JOP_JUMP_IF_FALSE:
            case JOP_JUMP_IF_TRUE:
            case JOP_QFOR_PREP_NUM:
            case JOP_QFOR_LOOP_NUM:
            case JOP_ADJUST_STACK:
            case JOP_RETURN:
                break;
            default:
                return 0;
        }
    }
    return 1;
}

int toi_jit_program_chain_safe(void* program_ptr) {
    if (program_ptr == NULL) return 0;
    return jit_program_ops_chain_safe((const JitProgram*)program_ptr);
}

enum {
    LOOP_FAIL_NONE = 0,
    LOOP_FAIL_DECODE = 1,
    LOOP_FAIL_UNSUPPORTED = 2,
    LOOP_FAIL_STACK = 3,
    LOOP_FAIL_JUMP = 4
};

#ifndef TOI_JIT_DEBUG_NATIVE_TRACE_EXEC
#define TOI_JIT_DEBUG_NATIVE_TRACE_EXEC 1
#endif

#ifndef TOI_JIT_DEBUG_NATIVE_TRACE_SANDBOX
#define TOI_JIT_DEBUG_NATIVE_TRACE_SANDBOX 0
#endif

// Number of interpreter-executed (IR) calls inside a single traced loop
// execution before the trace is abandoned for the plain VM. Per-call IR
// execution (~execute_program) is far slower than the VM's quickened call
// path, so dense call loops must not stay in the mixed/typed trace executors.
#define TOI_JIT_IR_CALL_BAIL 64

static const char* loop_fail_reason_name(uint8_t reason) {
    switch (reason) {
        case LOOP_FAIL_DECODE: return "decode";
        case LOOP_FAIL_UNSUPPORTED: return "unsupported";
        case LOOP_FAIL_STACK: return "stack";
        case LOOP_FAIL_JUMP: return "jump";
        default: return "none";
    }
}

static const char* native_trace_disable_reason_name(uint8_t reason) {
    switch (reason) {
        case 1: return "no-hits";
        case 2: return "low-hit-rate";
        case 3: return "deopt-rate";
        default: return "none";
    }
}

static const char* native_trace_deopt_reason_name(uint8_t reason) {
    switch (reason) {
        case TOI_JIT_NATIVE_DEOPT_NONE: return "none";
        case TOI_JIT_NATIVE_DEOPT_UNSPEC: return "unspec";
        case TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS: return "stack";
        case TOI_JIT_NATIVE_DEOPT_TYPE_GUARD: return "type";
        case TOI_JIT_NATIVE_DEOPT_GUARD: return "guard";
        case TOI_JIT_NATIVE_DEOPT_CALL_TARGET: return "call";
        case TOI_JIT_NATIVE_DEOPT_INVOKE_TARGET: return "invoke";
        case TOI_JIT_NATIVE_DEOPT_TABLE_GUARD: return "table";
        case TOI_JIT_NATIVE_DEOPT_CONTROL_FLOW: return "control";
        case TOI_JIT_NATIVE_DEOPT_RUNTIME: return "runtime";
        case TOI_JIT_NATIVE_DEOPT_UNSUPPORTED: return "unsupported";
        default: return "other";
    }
}

static int jit_try_stitch_soft_continue_calls(
    VM* vm,
    CallFrame* frame,
    ToiJitNativeExecState* exec_state,
    uint32_t* io_off) {
    if (vm == NULL || frame == NULL || frame->closure == NULL || exec_state == NULL || io_off == NULL) return 0;
    if (exec_state->reason != TOI_JIT_NATIVE_DEOPT_CALL_TARGET &&
        exec_state->reason != TOI_JIT_NATIVE_DEOPT_INVOKE_TARGET) {
        return 0;
    }
    ObjFunction* function = frame->closure->function;
    if (function == NULL) return 0;
    Chunk* chunk = &function->chunk;
    int sp = (int)exec_state->stack_count;
    if (sp <= 0 || sp > TOI_JIT_NATIVE_EXEC_STACK_CAP) return 0;
    Value* st = exec_state->stack;
    uint32_t off = *io_off;
    uint8_t* code = chunk->code;
    int stitched_count = 0;
    const int stitch_budget = 32;
    while (stitched_count < stitch_budget && off < (uint32_t)chunk->count) {
        uint8_t op = code[off];
        int len = jit_ir_decode_instr_len(code, (int)off, chunk->count);
        if (len <= 0 || (int)off + len > chunk->count) break;
        if (op == OP_INVOKE) {
            int argc = (int)code[off + 2];
            if (argc < 0 || sp < argc + 1) return 0;
            int recv_idx = sp - 1 - argc;
            Value method = chunk->constants.values[(int)code[off + 1]];
            Value out = NIL_VAL;
            if (!jit_invoke_primitive(vm, st[recv_idx], method, &st[recv_idx + 1], argc, &out)) {
                if (stitched_count == 0) return 0;
                break;
            }
            st[recv_idx] = out;
            sp = recv_idx + 1;
            exec_state->stack_count = (uint16_t)sp;
            off += (uint32_t)len;
            stitched_count++;
            continue;
        }
        int argc = -1;
        if (op == OP_CALL) argc = (int)code[off + 1];
        else if (op == OP_CALL0 || op == OP_QCALL0) argc = 0;
        else if (op == OP_CALL1 || op == OP_QCALL1) argc = 1;
        else if (op == OP_CALL2 || op == OP_QCALL2) argc = 2;
        if (argc < 0) break;
        if (sp < argc + 1) return 0;
        int callee_idx = sp - 1 - argc;
        int out_count = 0;
        int out_cap = TOI_JIT_NATIVE_EXEC_STACK_CAP - callee_idx;
        if (out_cap <= 0) return 0;
        if (!jit_call_primitive_multi(
                vm,
                st[callee_idx],
                &st[callee_idx + 1],
                argc,
                &st[callee_idx],
                out_cap,
                &out_count)) {
            if (stitched_count == 0) return 0;
            break;
        }
        if (out_count <= 0 || callee_idx + out_count > TOI_JIT_NATIVE_EXEC_STACK_CAP) return 0;
        sp = callee_idx + out_count;
        exec_state->stack_count = (uint16_t)sp;
        off += (uint32_t)len;
        stitched_count++;
    }
    if (stitched_count == 0) return 0;
    *io_off = off;
    exec_state->reason = TOI_JIT_NATIVE_DEOPT_NONE;
    exec_state->resume_offset = *io_off;
    return 1;
}

static int jit_dump_loops_enabled(void) {
    const char* v = getenv("TOI_JIT_DUMP_LOOPS");
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

static int jit_debug_mixed_loop_enabled(void) {
    const char* v = getenv("TOI_JIT_DEBUG_MIXED_LOOP");
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

static int jit_debug_mixed_loop_limit(void) {
    const char* v = getenv("TOI_JIT_DEBUG_MIXED_LOOP_MAX");
    if (v == NULL || *v == '\0') return 200;
    char* endptr = NULL;
    long parsed = strtol(v, &endptr, 10);
    if (endptr == v) return 200;
    if (parsed < 1) return 1;
    if (parsed > 100000) return 100000;
    return (int)parsed;
}

static int jit_debug_mixed_loop_matches_fn(ObjFunction* function) {
    if (!jit_debug_mixed_loop_enabled()) return 0;
    const char* filter = getenv("TOI_JIT_DEBUG_MIXED_LOOP_FN");
    if (filter == NULL || *filter == '\0') return 1;
    const char* fn_name = "<script>";
    if (function != NULL && function->name != NULL && function->name->chars != NULL) {
        fn_name = function->name->chars;
    }
    return strstr(fn_name, filter) != NULL;
}

static const char* jit_value_kind_name(Value value) {
    if (IS_NIL(value)) return "nil";
    if (IS_BOOL(value)) return "bool";
    if (IS_NUMBER(value)) return "number";
    if (!IS_OBJ(value)) return "value";
    struct Obj* obj = AS_OBJ(value);
    if (obj == NULL) return "obj";
    switch (obj->type) {
        case OBJ_STRING: return "string";
        case OBJ_TABLE: return "table";
        case OBJ_FUNCTION: return "function";
        case OBJ_NATIVE: return "native";
        case OBJ_UPVALUE: return "upvalue";
        case OBJ_CLOSURE: return "closure";
        case OBJ_THREAD: return "thread";
        case OBJ_USERDATA: return "userdata";
        case OBJ_BOUND_METHOD: return "bound_method";
        default: return "obj";
    }
}

static int is_numeric_for_loop_opcode(uint8_t opcode) {
    return opcode == OP_QFOR_LOOP_NUM || opcode == OP_FOR_LOOP;
}

static int jit_mixed_loops_enabled(void) {
    return 1;
}

static uint32_t jit_loop_hot_threshold(void) {
    const char* v = getenv("TOI_JIT_LOOP_HOT");
    if (v == NULL || *v == '\0') return 1024u;
    char* endptr = NULL;
    unsigned long parsed = strtoul(v, &endptr, 10);
    if (endptr == v) return 1024u;
    if (parsed < 8ul) return 8u;
    if (parsed > 100000ul) return 100000u;
    return (uint32_t)parsed;
}

static uint32_t jit_typed_hot_exit_threshold(void) {
    const char* v = getenv("TOI_JIT_SIDE_TRACE_HOT");
    if (v == NULL || *v == '\0') return 128u;
    char* endptr = NULL;
    unsigned long parsed = strtoul(v, &endptr, 10);
    if (endptr == v || parsed < 8ul) return 8u;
    if (parsed > 100000ul) return 100000u;
    return (uint32_t)parsed;
}

// Whole-function JIT execution (execute_program driven from call sites) is not
// validated yet and can return wrong values for some shapes; it stays off by
// default. TOI_JIT_FUNC=1 enables it for development.
#ifndef TOI_JIT_FUNC_DEFAULT
#define TOI_JIT_FUNC_DEFAULT 0
#endif

static int jit_function_jit_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("TOI_JIT_FUNC");
        if (v == NULL || v[0] == '\0') cached = TOI_JIT_FUNC_DEFAULT;
        else cached = (strcmp(v, "0") != 0) ? 1 : 0;
    }
    return cached;
}

// Retire a compiled loop trace: it is never probed again, and the VM's per-backedge
// JIT check is skipped once no live traces remain for the whole program.
static void jit_retire_loop_trace(ToiJitState* st, JitLoopTrace* trace) {
    if (st == NULL || trace == NULL) return;
    if (trace->runtime_disabled) return;
    trace->runtime_disabled = 1;
    if (st->loop_live_count > 0) st->loop_live_count--;
    st->loop_runtime_disabled++;
}

// Loop traces containing dynamic call sites go through the per-site call cache
// plus the closure-call helper. Set TOI_JIT_CALL_TRACES=0 to retire such traces
// instead (useful when bisecting a call-site miscompile).
#ifndef TOI_JIT_CALL_TRACES_DEFAULT
#define TOI_JIT_CALL_TRACES_DEFAULT 1
#endif

static int jit_call_traces_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("TOI_JIT_CALL_TRACES");
        if (v == NULL || v[0] == '\0') cached = TOI_JIT_CALL_TRACES_DEFAULT;
        else cached = (strcmp(v, "0") != 0) ? 1 : 0;
    }
    return cached;
}

// The arm64 slot-loop lowering is currently a miscompiler: the generated loop
// does not advance the induction slot per iteration, so loop bodies observe the
// value the slot had when the trace was compiled (and run one extra iteration).
// It stays off by default until the codegen is fixed; TOI_JIT_SLOT_LOOP=1
// re-enables it for development.
#ifndef TOI_JIT_SLOT_LOOP_DEFAULT
#define TOI_JIT_SLOT_LOOP_DEFAULT 0
#endif

static int jit_slot_loop_lowering_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("TOI_JIT_SLOT_LOOP");
        if (v == NULL || v[0] == '\0') cached = TOI_JIT_SLOT_LOOP_DEFAULT;
        else cached = (strcmp(v, "0") != 0) ? 1 : 0;
    }
    return cached;
}

static void jit_discard_compiled_slot_loop(ToiJitState* st, JitLoopTrace* trace) {
    if (trace == NULL) return;
    if (st != NULL && trace->native_code_mem != NULL && trace->native_code_size != 0) {
        toi_jit_backend_free_code(st->backend_kind, trace->native_code_mem, trace->native_code_size);
    }
    trace->native_code_mem = NULL;
    trace->native_code_size = 0;
    trace->native_slot_loop_fn = NULL;
    trace->native_slot_loop_slot_count = 0;
    trace->native_slot_loop_used_count = 0;
    trace->native_slot_loop_written_count = 0;
    trace->native_slot_loop_upvalue_const_count = 0;
}

// Returns 1 when a freshly compiled slot loop may be used. When the lowering is
// disabled the code is discarded and 0 is returned so callers fall through to
// the (correct) IR/native-trace executors.
static int jit_accept_compiled_slot_loop(ToiJitState* st, JitLoopTrace* trace) {
    if (jit_slot_loop_lowering_enabled()) return 1;
    jit_discard_compiled_slot_loop(st, trace);
    return 0;
}


static const char* jit_op_name(JitOpCode op) {
    switch (op) {
        case JOP_CONSTANT: return "CONST";
        case JOP_NIL: return "NIL";
        case JOP_TRUE: return "TRUE";
        case JOP_FALSE: return "FALSE";
        case JOP_POP: return "POP";
        case JOP_GET_LOCAL: return "GET_LOCAL";
        case JOP_GET2_LOCAL: return "GET2_LOCAL";
        case JOP_GET_UPVALUE: return "GET_UPVALUE";
        case JOP_SET_LOCAL: return "SET_LOCAL";
        case JOP_SET_LOCAL_POP: return "SET_LOCAL_POP";
        case JOP_APPEND: return "APPEND";
        case JOP_GET_TABLE: return "GET_TABLE";
        case JOP_GET_TABLE_NUM: return "GET_TABLE_NUM";
        case JOP_SET_TABLE: return "SET_TABLE";
        case JOP_SET_TABLE_NUM: return "SET_TABLE_NUM";
        case JOP_INC_LOCAL: return "INC_LOCAL";
        case JOP_SUB_LOCAL_CONST: return "SUB_LOCAL_CONST";
        case JOP_MUL_LOCAL_CONST: return "MUL_LOCAL_CONST";
        case JOP_DIV_LOCAL_CONST: return "DIV_LOCAL_CONST";
        case JOP_MOD_LOCAL_CONST: return "MOD_LOCAL_CONST";
        case JOP_ADD_CONST: return "ADD_CONST";
        case JOP_SUB_CONST: return "SUB_CONST";
        case JOP_MUL_CONST: return "MUL_CONST";
        case JOP_DIV_CONST: return "DIV_CONST";
        case JOP_MOD_CONST: return "MOD_CONST";
        case JOP_ADD: return "ADD";
        case JOP_ADD_INPLACE: return "ADD_INPLACE";
        case JOP_SUBTRACT: return "SUB";
        case JOP_MULTIPLY: return "MUL";
        case JOP_DIVIDE: return "DIV";
        case JOP_MODULO: return "MOD";
        case JOP_NEGATE: return "NEG";
        case JOP_NOT: return "NOT";
        case JOP_IADD: return "IADD";
        case JOP_ISUB: return "ISUB";
        case JOP_IMUL: return "IMUL";
        case JOP_IDIV: return "IDIV";
        case JOP_IMOD: return "IMOD";
        case JOP_FADD: return "FADD";
        case JOP_FSUB: return "FSUB";
        case JOP_FMUL: return "FMUL";
        case JOP_FDIV: return "FDIV";
        case JOP_FMOD: return "FMOD";
        case JOP_QFADD: return "QFADD";
        case JOP_QFSUB: return "QFSUB";
        case JOP_QFMUL: return "QFMUL";
        case JOP_QFDIV: return "QFDIV";
        case JOP_QFMOD: return "QFMOD";
        case JOP_EQUAL: return "EQUAL";
        case JOP_QGREATER_NUM: return "QGT_NUM";
        case JOP_QLESS_NUM: return "QLT_NUM";
        case JOP_JUMP: return "JUMP";
        case JOP_JUMP_IF_FALSE: return "JIF_FALSE";
        case JOP_JUMP_IF_TRUE: return "JIF_TRUE";
        case JOP_CALL: return "CALL";
        case JOP_INVOKE: return "INVOKE";
        case JOP_LENGTH: return "LENGTH";
        case JOP_SLICE: return "SLICE";
        case JOP_BUILD_STRING2: return "BUILD_STRING2";
        case JOP_BUILD_STRING3: return "BUILD_STRING3";
        case JOP_BUILD_STRING4: return "BUILD_STRING4";
        case JOP_BUILD_STRING5: return "BUILD_STRING5";
        case JOP_QCALL2: return "QCALL2";
        case JOP_QFOR_PREP_NUM: return "QFOR_PREP_NUM";
        case JOP_QFOR_LOOP_NUM: return "QFOR_LOOP_NUM";
        case JOP_ADJUST_STACK: return "ADJUST_STACK";
        case JOP_RETURN: return "RETURN";
        case JOP_TINY_INLINE: return "TINY_INLINE";
        default: return "UNKNOWN";
    }
}

static uint8_t typed_exit_reason_for_op(JitOpCode op) {
    switch (op) {
        case JOP_CALL:
        case JOP_QCALL2:
            return TYPED_EXIT_REASON_CALL;
        case JOP_INVOKE:
            return TYPED_EXIT_REASON_INVOKE;
        case JOP_GET_TABLE:
        case JOP_GET_TABLE_NUM:
        case JOP_SET_TABLE:
        case JOP_SET_TABLE_NUM:
        case JOP_APPEND:
            return TYPED_EXIT_REASON_TABLE;
        case JOP_QFOR_PREP_NUM:
        case JOP_QFOR_LOOP_NUM:
        case JOP_JUMP:
        case JOP_JUMP_IF_FALSE:
        case JOP_JUMP_IF_TRUE:
        case JOP_ADJUST_STACK:
            return TYPED_EXIT_REASON_CONTROL;
        case JOP_CONSTANT:
        case JOP_NIL:
        case JOP_TRUE:
        case JOP_FALSE:
        case JOP_POP:
        case JOP_GET_LOCAL:
        case JOP_SET_LOCAL:
        case JOP_SET_LOCAL_POP:
            return TYPED_EXIT_REASON_STACK;
        case JOP_ADD:
        case JOP_ADD_INPLACE:
        case JOP_SUBTRACT:
        case JOP_MULTIPLY:
        case JOP_DIVIDE:
        case JOP_MODULO:
        case JOP_IADD:
        case JOP_ISUB:
        case JOP_IMUL:
        case JOP_IDIV:
        case JOP_IMOD:
        case JOP_FADD:
        case JOP_FSUB:
        case JOP_FMUL:
        case JOP_FDIV:
        case JOP_FMOD:
        case JOP_QFADD:
        case JOP_QFSUB:
        case JOP_QFMUL:
        case JOP_QFDIV:
        case JOP_QFMOD:
        case JOP_NOT:
        case JOP_EQUAL:
        case JOP_QGREATER_NUM:
        case JOP_QLESS_NUM:
        case JOP_BUILD_STRING2:
        case JOP_BUILD_STRING3:
        case JOP_BUILD_STRING4:
        case JOP_BUILD_STRING5:
            return TYPED_EXIT_REASON_TYPE;
        default:
            return TYPED_EXIT_REASON_UNSUPPORTED;
    }
}

static void clear_typed_trace_meta(JitLoopTrace* trace) {
    if (trace == NULL) return;
    free(trace->typed_pc_exit_ids);
    free(trace->typed_exit_reasons);
    free(trace->typed_exit_hits);
    free(trace->typed_exit_hot);
    trace->typed_pc_exit_ids = NULL;
    trace->typed_exit_reasons = NULL;
    trace->typed_exit_hits = NULL;
    trace->typed_exit_hot = NULL;
    trace->typed_exit_count = 0;
    trace->typed_exit_total = 0;
    trace->typed_last_exit_reason = TYPED_EXIT_REASON_NONE;
    trace->typed_hot_exit_threshold = 0;
    trace->typed_hot_exit_count = 0;
    trace->typed_ir_ready = 0;
}

static int build_typed_trace_meta(JitLoopTrace* trace) {
    if (trace == NULL || !trace->has_program || trace->ops == NULL || trace->op_count <= 0) return 0;
    clear_typed_trace_meta(trace);

    uint16_t* pc_exit_ids = (uint16_t*)malloc((size_t)trace->op_count * sizeof(uint16_t));
    uint8_t* exit_reasons = (uint8_t*)malloc((size_t)trace->op_count * sizeof(uint8_t));
    uint32_t* exit_hits = (uint32_t*)calloc((size_t)trace->op_count, sizeof(uint32_t));
    uint8_t* exit_hot = (uint8_t*)calloc((size_t)trace->op_count, sizeof(uint8_t));
    if (pc_exit_ids == NULL || exit_reasons == NULL || exit_hits == NULL || exit_hot == NULL) {
        free(pc_exit_ids);
        free(exit_reasons);
        free(exit_hits);
        free(exit_hot);
        return 0;
    }
    for (int i = 0; i < trace->op_count; i++) pc_exit_ids[i] = TYPED_EXIT_NONE;

    uint16_t exit_count = 0;
    for (int pc = 0; pc < trace->op_count; pc++) {
        uint8_t reason = typed_exit_reason_for_op(trace->ops[pc].op);
        if (reason == TYPED_EXIT_REASON_NONE) continue;
        pc_exit_ids[pc] = exit_count;
        exit_reasons[exit_count] = reason;
        exit_count++;
    }

    trace->typed_pc_exit_ids = pc_exit_ids;
    trace->typed_exit_reasons = exit_reasons;
    trace->typed_exit_hits = exit_hits;
    trace->typed_exit_hot = exit_hot;
    trace->typed_exit_count = exit_count;
    trace->typed_exit_total = 0;
    trace->typed_last_exit_reason = TYPED_EXIT_REASON_NONE;
    trace->typed_hot_exit_threshold = jit_typed_hot_exit_threshold();
    trace->typed_hot_exit_count = 0;
    trace->typed_ir_ready = 1;
    return 1;
}

static void jit_record_typed_exit(ToiJitState* st, JitLoopTrace* trace, int pc, uint8_t reason) {
    if (trace == NULL) return;
    trace->typed_exit_total++;
    trace->typed_last_exit_reason = reason;
    if (trace->typed_pc_exit_ids == NULL || trace->typed_exit_hits == NULL || trace->typed_exit_hot == NULL) return;
    if (pc < 0 || pc >= trace->op_count) return;
    uint16_t exit_id = trace->typed_pc_exit_ids[pc];
    if (exit_id == TYPED_EXIT_NONE || exit_id >= trace->typed_exit_count) return;
    uint32_t hits = ++trace->typed_exit_hits[exit_id];
    if (!trace->typed_exit_hot[exit_id] && hits >= trace->typed_hot_exit_threshold) {
        trace->typed_exit_hot[exit_id] = 1;
        trace->typed_hot_exit_count++;
        if (st != NULL) st->loop_typed_exit_promotions++;
    }
    if (trace->typed_exit_hot[exit_id]) {
        jit_record_side_exit_candidate(st, trace, exit_id, pc);
    }
}

static int jit_try_typed_side_exit_to_interp(
    ToiJitState* st,
    JitLoopTrace* trace,
    CallFrame* frame,
    ObjThread* thread,
    uint8_t** ip,
    const Value* work,
    int sp,
    int frame_window,
    int pc) {
    if (st == NULL || trace == NULL || frame == NULL || thread == NULL || ip == NULL || work == NULL) return 0;
    if (trace->typed_pc_exit_ids == NULL || trace->typed_exit_hot == NULL) return 0;
    if (pc < 0 || pc >= trace->op_count) return 0;

    uint16_t exit_id = trace->typed_pc_exit_ids[pc];
    if (exit_id == TYPED_EXIT_NONE || exit_id >= trace->typed_exit_count) return 0;
    if (!trace->typed_exit_hot[exit_id]) return 0;

    uint8_t reason = TYPED_EXIT_REASON_UNSUPPORTED;
    if (trace->typed_exit_reasons != NULL) reason = trace->typed_exit_reasons[exit_id];
    if (reason != TYPED_EXIT_REASON_CALL &&
        reason != TYPED_EXIT_REASON_INVOKE &&
        reason != TYPED_EXIT_REASON_TABLE) {
        return 0;
    }

    int commit_count = frame_window > sp ? frame_window : sp;
    if (commit_count < 0 || commit_count > 8192) return 0;
    for (int j = 0; j < commit_count; j++) frame->slots[j] = work[j];
    thread->stack_top = frame->slots + sp;
    *ip = frame->closure->function->chunk.code + trace->ops[pc].byte_offset;
    st->loop_exec_hits++;
    trace->exec_hits++;
    st->loop_typed_side_exit_stitches++;
    return 1;
}

static void jit_record_side_exit_candidate(ToiJitState* st, JitLoopTrace* trace, uint16_t exit_id, int pc) {
    if (st == NULL || trace == NULL) return;
    if (pc < 0 || pc >= trace->op_count) return;
    if (exit_id == TYPED_EXIT_NONE || exit_id >= trace->typed_exit_count) return;

    ObjFunction* fn = trace->function;
    uint32_t loop_off = trace->opcode_offset;
    uint32_t byte_off = trace->ops[pc].byte_offset;
    uint8_t reason = (trace->typed_exit_reasons != NULL) ? trace->typed_exit_reasons[exit_id] : TYPED_EXIT_REASON_UNSUPPORTED;

    for (int i = 0; i < st->side_exit_count; i++) {
        JitSideExitCandidate* c = &st->side_exits[i];
        if (c->function == fn &&
            c->loop_opcode_offset == loop_off &&
            c->exit_id == exit_id &&
            c->exit_byte_offset == byte_off) {
            c->hits++;
            st->loop_side_exit_hits++;
            return;
        }
    }

    if (st->side_exit_count == st->side_exit_capacity) {
        int new_cap = (st->side_exit_capacity < 16) ? 16 : (st->side_exit_capacity * 2);
        JitSideExitCandidate* grown =
            (JitSideExitCandidate*)realloc(st->side_exits, (size_t)new_cap * sizeof(JitSideExitCandidate));
        if (grown == NULL) return;
        st->side_exits = grown;
        st->side_exit_capacity = new_cap;
    }

    JitSideExitCandidate* c = &st->side_exits[st->side_exit_count++];
    c->function = fn;
    c->loop_opcode_offset = loop_off;
    c->exit_byte_offset = byte_off;
    c->exit_id = exit_id;
    c->reason = reason;
    c->stitched = 0;
    c->hits = 1;
    st->loop_side_exit_candidates++;
    st->loop_side_exit_hits++;
}

static void set_loop_fail(JitLoopTrace* trace, uint8_t reason, uint8_t opcode, int offset) {
    if (trace == NULL) return;
    trace->fail_reason = reason;
    trace->fail_opcode = opcode;
    trace->fail_offset = offset >= 0 ? (uint32_t)offset : 0;
}

static int to_int64_local(double x, int64_t* out) {
    if (x < (double)INT64_MIN || x > (double)INT64_MAX) return 0;
    int64_t i = (int64_t)x;
    if ((double)i != x) return 0;
    *out = i;
    return 1;
}

static int objstring_equals_jit(ObjString* a, ObjString* b) {
    if (a == b) return 1;
    if (a == NULL || b == NULL) return 0;
    if (a->length != b->length) return 0;
    if (objstring_hash(a) != objstring_hash(b)) return 0;
    return memcmp(a->chars, b->chars, (size_t)a->length) == 0;
}

static int jit_get_table_ic_try_lane(
    ObjTable* t,
    ObjString* key_str,
    uint32_t shape_version,
    ObjTable** tables,
    ObjString** keys,
    int* slots,
    uint32_t* versions,
    int site,
    Value* out_value,
    int* out_exact_hit) {
    if (tables == NULL || keys == NULL || slots == NULL || versions == NULL) return 0;
    if (keys[site] == NULL || !objstring_equals_jit(keys[site], key_str)) return 0;
    int slot = slots[site];
    if (slot == -2) {
        if (tables[site] != t || versions[site] != shape_version) return 0;
        *out_value = NIL_VAL;
        *out_exact_hit = 1;
        return 1;
    }
    if (slot < 0 || slot >= t->table.capacity) return 0;
    Entry* entry = &t->table.entries[slot];
    if (!objstring_equals_jit(entry->key, key_str)) return 0;
    *out_value = entry->value;
    *out_exact_hit = (tables[site] == t && versions[site] == shape_version);
    return 1;
}

static void jit_get_table_ic_update_lane(
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

static void jit_update_get_table_ic(Chunk* chunk, int site, ObjTable* t, ObjString* key_str, int slot) {
    if (chunk == NULL || site < 0 || site >= chunk->capacity) return;
    if (chunk->get_table_ic_tables == NULL || chunk->get_table_ic_keys == NULL ||
        chunk->get_table_ic_slots == NULL || chunk->get_table_ic_versions == NULL) {
        return;
    }

    uint32_t version = t->table.shape_version;
    jit_get_table_ic_update_lane(
        chunk->get_table_ic_tables,
        chunk->get_table_ic_keys,
        chunk->get_table_ic_slots,
        chunk->get_table_ic_versions,
        site,
        t,
        key_str,
        slot,
        version);
}

// Equality must match the interpreter (vm/ops_compare.c vm_handle_op_equal), which
// compares strings by value: strings produced at runtime are not interned, so
// pointer identity alone would report them as unequal.
static int jit_values_equal(Value a, Value b) {
    if (a.type != b.type) return 0;
    if (IS_NIL(a)) return 1;
    if (IS_BOOL(a)) return AS_BOOL(a) == AS_BOOL(b);
    if (IS_NUMBER(a)) return AS_NUMBER(a) == AS_NUMBER(b);
    if (IS_STRING(a) && IS_STRING(b)) {
        ObjString* sa = AS_STRING(a);
        ObjString* sb = AS_STRING(b);
        if (sa == sb) return 1;
        return sa->length == sb->length &&
               memcmp(sa->chars, sb->chars, (size_t)sa->length) == 0;
    }
    if (IS_OBJ(a)) return AS_OBJ(a) == AS_OBJ(b);
    return 0;
}

// Must match the interpreter's truthiness (vm/ops_control.c is_falsey_local):
// nil, false, 0, "" and empty tables are falsey.
static int is_falsey_jit(Value v) {
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

static Value jit_seed_call_hint_from_chunk(const Chunk* chunk, int off, int argc) {
    if (chunk == NULL || off < 0 || argc < 0 || off >= chunk->capacity) return NIL_VAL;
    ObjClosure* c = NULL;
    if (chunk->call_ic_closures != NULL && chunk->call_ic_argc != NULL) {
        c = chunk->call_ic_closures[off];
        if (c != NULL && (int)chunk->call_ic_argc[off] == argc) return OBJ_VAL(c);
    }
    if (chunk->call_ic2_closures != NULL && chunk->call_ic2_argc != NULL) {
        c = chunk->call_ic2_closures[off];
        if (c != NULL && (int)chunk->call_ic2_argc[off] == argc) return OBJ_VAL(c);
    }
    if (chunk->call_ic3_closures != NULL && chunk->call_ic3_argc != NULL) {
        c = chunk->call_ic3_closures[off];
        if (c != NULL && (int)chunk->call_ic3_argc[off] == argc) return OBJ_VAL(c);
    }
    if (chunk->call_ic4_closures != NULL && chunk->call_ic4_argc != NULL) {
        c = chunk->call_ic4_closures[off];
        if (c != NULL && (int)chunk->call_ic4_argc[off] == argc) return OBJ_VAL(c);
    }
    if (chunk->call_ic5_closures != NULL && chunk->call_ic5_argc != NULL) {
        c = chunk->call_ic5_closures[off];
        if (c != NULL && (int)chunk->call_ic5_argc[off] == argc) return OBJ_VAL(c);
    }
    return NIL_VAL;
}

JitTrackedFunction* jit_find_or_add(VM* vm, ObjFunction* function) {
    ToiJitState* st = jit_state(vm);
    if (st == NULL || function == NULL) return NULL;

    JitTrackedFunction* found = jit_find_tracked(st, function);
    if (found != NULL) return found;

    if (st->count + 1 > st->capacity) {
        int new_capacity = st->capacity < 16 ? 16 : st->capacity * 2;
        JitTrackedFunction* grown =
            (JitTrackedFunction*)realloc(st->tracked, sizeof(JitTrackedFunction) * (size_t)new_capacity);
        if (grown == NULL) return NULL;
        st->tracked = grown;
        st->capacity = new_capacity;
    }

    JitTrackedFunction* entry = &st->tracked[st->count++];
    memset(entry, 0, sizeof(*entry));
    entry->function = function;
    {
        uint32_t h = jit_func_hash(function);
        st->func_cache[h].function = function;
        st->func_cache[h].tracked_index_plus1 = (uint32_t)(st->count - 1) + 1u;
    }
    return entry;
}

static uint32_t jit_loop_hash(ObjFunction* function, int opcode_offset, uint8_t opcode) {
    uintptr_t p = (uintptr_t)function;
    uint32_t h = (uint32_t)(p >> 4) ^ (uint32_t)(p >> 19);
    h ^= (uint32_t)opcode_offset * 2654435761u;
    h ^= (uint32_t)opcode * 2246822519u;
    return h & (TOI_JIT_LOOP_CACHE_SIZE - 1u);
}

static JitLoopTrace* jit_find_loop(ToiJitState* st, ObjFunction* function, int opcode_offset, uint8_t opcode) {
    if (st == NULL || function == NULL || opcode_offset < 0) return NULL;

    uint32_t h = jit_loop_hash(function, opcode_offset, opcode);
    JitLoopCacheEntry* e = &st->loop_cache[h];
    if (e->loop_index_plus1 != 0 &&
        e->function == function &&
        (int)e->opcode_offset == opcode_offset &&
        e->opcode == opcode) {
        uint32_t idx = e->loop_index_plus1 - 1u;
        if ((int)idx < st->loop_count) {
            return &st->loops[idx];
        }
        e->loop_index_plus1 = 0;
    }

    for (int i = 0; i < st->loop_count; i++) {
        JitLoopTrace* t = &st->loops[i];
        if (t->function == function &&
            (int)t->opcode_offset == opcode_offset &&
            t->opcode == opcode) {
            e->function = function;
            e->opcode_offset = (uint32_t)opcode_offset;
            e->opcode = opcode;
            e->loop_index_plus1 = (uint32_t)i + 1u;
            return t;
        }
    }

    return NULL;
}

static JitLoopTrace* jit_find_or_add_loop(VM* vm, ObjFunction* function, int opcode_offset, uint8_t opcode) {
    ToiJitState* st = jit_state(vm);
    if (st == NULL || function == NULL || opcode_offset < 0) return NULL;

    JitLoopTrace* existing = jit_find_loop(st, function, opcode_offset, opcode);
    if (existing != NULL) return existing;

    if (st->loop_count + 1 > st->loop_capacity) {
        int new_capacity = st->loop_capacity < 16 ? 16 : st->loop_capacity * 2;
        JitLoopTrace* grown =
            (JitLoopTrace*)realloc(st->loops, sizeof(JitLoopTrace) * (size_t)new_capacity);
        if (grown == NULL) return NULL;
        st->loops = grown;
        st->loop_capacity = new_capacity;
    }

    JitLoopTrace* entry = &st->loops[st->loop_count++];
    memset(entry, 0, sizeof(*entry));
    entry->function = function;
    entry->opcode_offset = (uint32_t)opcode_offset;
    entry->opcode = opcode;

    uint32_t h = jit_loop_hash(function, opcode_offset, opcode);
    st->loop_cache[h].function = function;
    st->loop_cache[h].opcode_offset = (uint32_t)opcode_offset;
    st->loop_cache[h].opcode = opcode;
    st->loop_cache[h].loop_index_plus1 = (uint32_t)(st->loop_count - 1) + 1u;

    return entry;
}

// Direct per-opcode-offset lookup through the chunk's JIT backedge table, so the
// interpreter's per-iteration backedge probes do not re-hash (function, offset,
// opcode) on every iteration. The table stores indices into st->loops, whose
// entries are stable once created (st->loops only grows via realloc).
static JitLoopTrace* jit_loop_from_offset(ToiJitState* st, ObjFunction* function, int opcode_offset) {
    if (st == NULL || function == NULL) return NULL;
    Chunk* chunk = &function->chunk;
    if (chunk->jit_loop_indexes == NULL) return NULL;
    if (opcode_offset < 0 || opcode_offset >= chunk->count) return NULL;
    uint32_t idx_plus1 = chunk->jit_loop_indexes[opcode_offset];
    if (idx_plus1 == 0u) return NULL;
    uint32_t idx = idx_plus1 - 1u;
    if ((int)idx >= st->loop_count) return NULL;
    JitLoopTrace* trace = &st->loops[idx];
    if (trace->function != function || (int)trace->opcode_offset != opcode_offset) return NULL;
    return trace;
}

static void jit_loop_store_offset(ToiJitState* st, ObjFunction* function, int opcode_offset, JitLoopTrace* trace) {
    if (st == NULL || function == NULL || trace == NULL) return;
    Chunk* chunk = &function->chunk;
    if (opcode_offset < 0 || opcode_offset >= chunk->count) return;
    if (chunk->jit_loop_indexes == NULL) {
        uint32_t* table = (uint32_t*)calloc((size_t)chunk->count, sizeof(uint32_t));
        if (table == NULL) return; // OOM: fall back to hash-based lookups
        chunk->jit_loop_indexes = table;
    }
    int idx = (int)(trace - st->loops);
    if (idx < 0 || idx >= st->loop_count) return;
    chunk->jit_loop_indexes[opcode_offset] = (uint32_t)idx + 1u;
}

static int compile_qfor_loop_trace(ObjFunction* function, JitLoopTrace* trace, int loop_start, int loop_end) {
    Chunk* chunk = &function->chunk;
    const uint8_t* code = chunk->code;
    int range = loop_end - loop_start;
    if (range <= 0) return 0;

    int* byte_to_idx = (int*)malloc(sizeof(int) * (size_t)range);
    if (byte_to_idx == NULL) return 0;
    for (int i = 0; i < range; i++) byte_to_idx[i] = -1;

    int op_count = 0;
    int max_slot = 0;
    int stack_depth = 0;
    int max_stack = 0;

    for (int off = loop_start; off < loop_end;) {
        int len = jit_ir_decode_instr_len(code, off, chunk->count);
        if (len <= 0 || off + len > loop_end) {
            set_loop_fail(trace, LOOP_FAIL_DECODE, code[off], off);
            free(byte_to_idx);
            return 0;
        }
        JitOpCode mapped = JOP_POP;
        if (!jit_ir_map_supported_opcode(code[off], &mapped)) {
            set_loop_fail(trace, LOOP_FAIL_UNSUPPORTED, code[off], off);
            free(byte_to_idx);
            return 0;
        }
        if (mapped == JOP_RETURN) {
            set_loop_fail(trace, LOOP_FAIL_UNSUPPORTED, code[off], off);
            free(byte_to_idx);
            return 0;
        }
        byte_to_idx[off - loop_start] = op_count++;

        switch (code[off]) {
            case OP_CONSTANT:
            case OP_NIL:
            case OP_TRUE:
            case OP_FALSE:
            case OP_GET_LOCAL:
            case OP_GET_UPVALUE:
                stack_depth += 1;
                break;
            case OP_GET2_LOCAL:
                stack_depth += 2;
                break;
            case OP_POP:
            case OP_SET_LOCAL_POP:
                stack_depth -= 1;
                break;
            case OP_APPEND:
            case OP_GET_TABLE:
            case OP_GET_TABLE_NUM:
                stack_depth -= 1;
                break;
            case OP_SET_TABLE_NUM:
                stack_depth -= 2;
                break;
            case OP_SET_TABLE:
                stack_depth -= 2;
                break;
            case OP_CALL:
                stack_depth -= (int)code[off + 1];
                break;
            case OP_CALL1:
                stack_depth -= 1;
                break;
            case OP_CALL2:
                stack_depth -= 2;
                break;
            case OP_INVOKE:
                stack_depth -= (int)code[off + 2];
                break;
            case OP_SLICE:
                stack_depth -= 3;
                break;
            case OP_BUILD_STRING2:
                stack_depth -= 1;
                break;
            case OP_BUILD_STRING3:
                stack_depth -= 2;
                break;
            case OP_BUILD_STRING4:
                stack_depth -= 3;
                break;
            case OP_BUILD_STRING5:
                stack_depth -= 4;
                break;
            case OP_QCALL2:
                stack_depth -= 2;
                break;
            default:
                if (code[off] == OP_ADD || code[off] == OP_ADD_INPLACE || code[off] == OP_SUBTRACT || code[off] == OP_MULTIPLY ||
                    code[off] == OP_DIVIDE || code[off] == OP_MODULO ||
                    code[off] == OP_IADD || code[off] == OP_ISUB || code[off] == OP_IMUL ||
                    code[off] == OP_IDIV || code[off] == OP_IMOD ||
                    code[off] == OP_FADD || code[off] == OP_FSUB || code[off] == OP_FMUL ||
                    code[off] == OP_FDIV || code[off] == OP_FMOD ||
                    code[off] == OP_QFADD || code[off] == OP_QFSUB || code[off] == OP_QFMUL ||
                    code[off] == OP_QFDIV || code[off] == OP_QFMOD ||
                    code[off] == OP_QGREATER_NUM || code[off] == OP_QLESS_NUM ||
                    code[off] == OP_EQUAL) {
                    stack_depth -= 1;
                } else if (code[off] == OP_INC_LOCAL || code[off] == OP_SUB_LOCAL_CONST ||
                           code[off] == OP_MUL_LOCAL_CONST || code[off] == OP_DIV_LOCAL_CONST ||
                           code[off] == OP_MOD_LOCAL_CONST) {
                    stack_depth += 1;
                }
                break;
        }
        if (stack_depth < 0) stack_depth = 0;
        if (stack_depth > max_stack) max_stack = stack_depth;

        if (code[off] == OP_GET_LOCAL || code[off] == OP_SET_LOCAL || code[off] == OP_SET_LOCAL_POP ||
            code[off] == OP_INC_LOCAL || code[off] == OP_SUB_LOCAL_CONST || code[off] == OP_MUL_LOCAL_CONST ||
            code[off] == OP_DIV_LOCAL_CONST || code[off] == OP_MOD_LOCAL_CONST) {
            int slot = (int)code[off + 1];
            if (slot > max_slot) max_slot = slot;
        } else if (code[off] == OP_GET2_LOCAL) {
            int a = (int)code[off + 1];
            int b = (int)code[off + 2];
            if (a > max_slot) max_slot = a;
            if (b > max_slot) max_slot = b;
        } else if (code[off] == OP_QFOR_LOOP_NUM || code[off] == OP_QFOR_PREP_NUM || code[off] == OP_FOR_LOOP || code[off] == OP_FOR_PREP) {
            int a = (int)code[off + 1];
            int b = (int)code[off + 2];
            if (a > max_slot) max_slot = a;
            if (b > max_slot) max_slot = b;
        }

        off += len;
    }

    JitOp* ops = (JitOp*)calloc((size_t)op_count, sizeof(JitOp));
    ObjClosure** call_ic_closures = (ObjClosure**)calloc((size_t)op_count, sizeof(ObjClosure*));
    JitProgram** call_ic_programs = (JitProgram**)calloc((size_t)op_count, sizeof(JitProgram*));
    ObjClosure** call_ic2_closures = (ObjClosure**)calloc((size_t)op_count, sizeof(ObjClosure*));
    JitProgram** call_ic2_programs = (JitProgram**)calloc((size_t)op_count, sizeof(JitProgram*));
    ObjClosure** call_ic3_closures = (ObjClosure**)calloc((size_t)op_count, sizeof(ObjClosure*));
    JitProgram** call_ic3_programs = (JitProgram**)calloc((size_t)op_count, sizeof(JitProgram*));
    ObjClosure** call_ic4_closures = (ObjClosure**)calloc((size_t)op_count, sizeof(ObjClosure*));
    JitProgram** call_ic4_programs = (JitProgram**)calloc((size_t)op_count, sizeof(JitProgram*));
    ObjClosure** call_ic5_closures = (ObjClosure**)calloc((size_t)op_count, sizeof(ObjClosure*));
    JitProgram** call_ic5_programs = (JitProgram**)calloc((size_t)op_count, sizeof(JitProgram*));
    if (ops == NULL || call_ic_closures == NULL || call_ic_programs == NULL ||
        call_ic2_closures == NULL || call_ic2_programs == NULL ||
        call_ic3_closures == NULL || call_ic3_programs == NULL ||
        call_ic4_closures == NULL || call_ic4_programs == NULL ||
        call_ic5_closures == NULL || call_ic5_programs == NULL) {
        set_loop_fail(trace, LOOP_FAIL_STACK, OP_NIL, loop_start);
        free(byte_to_idx);
        free(ops);
        free(call_ic_closures);
        free(call_ic_programs);
        free(call_ic2_closures);
        free(call_ic2_programs);
        free(call_ic3_closures);
        free(call_ic3_programs);
        free(call_ic4_closures);
        free(call_ic4_programs);
        free(call_ic5_closures);
        free(call_ic5_programs);
        return 0;
    }

    int idx = 0;
    int entry_pc = -1;
    for (int off = loop_start; off < loop_end;) {
        uint8_t op = code[off];
        JitOp* out = &ops[idx++];
        out->byte_offset = (uint32_t)off;
        if (off == (int)trace->opcode_offset) entry_pc = idx - 1;
        if (!jit_ir_map_supported_opcode(op, &out->op)) {
            set_loop_fail(trace, LOOP_FAIL_UNSUPPORTED, op, off);
            free(byte_to_idx);
            free(ops);
            free(call_ic_closures);
            free(call_ic_programs);
            free(call_ic2_closures);
            free(call_ic2_programs);
            free(call_ic3_closures);
            free(call_ic3_programs);
            free(call_ic4_closures);
            free(call_ic4_programs);
            free(call_ic5_closures);
            free(call_ic5_programs);
            return 0;
        }
        switch (op) {
            case OP_CONSTANT:
                out->constant = chunk->constants.values[(int)code[off + 1]];
                break;
            case OP_GET_LOCAL:
            case OP_GET_UPVALUE:
            case OP_SET_LOCAL:
            case OP_SET_LOCAL_POP:
            case OP_ADJUST_STACK:
                out->a = code[off + 1];
                break;
            case OP_CALL:
                out->a = code[off + 1];
                out->constant = jit_seed_call_hint_from_chunk(chunk, off, (int)out->a);
                break;
            case OP_CALL0:
                out->a = 0;
                out->constant = jit_seed_call_hint_from_chunk(chunk, off, 0);
                break;
            case OP_CALL1:
                out->a = 1;
                out->constant = jit_seed_call_hint_from_chunk(chunk, off, 1);
                break;
            case OP_CALL2:
                out->a = 2;
                out->constant = jit_seed_call_hint_from_chunk(chunk, off, 2);
                break;
            case OP_INVOKE:
                out->a = code[off + 2]; // arg_count
                out->constant = chunk->constants.values[(int)code[off + 1]];
                break;
            case OP_GET2_LOCAL:
                out->a = code[off + 1];
                out->b = code[off + 2];
                break;
            case OP_INC_LOCAL:
            case OP_SUB_LOCAL_CONST:
            case OP_MUL_LOCAL_CONST:
            case OP_DIV_LOCAL_CONST:
            case OP_MOD_LOCAL_CONST:
                out->a = code[off + 1];
                out->constant = chunk->constants.values[(int)code[off + 2]];
                break;
            case OP_ADD_CONST:
            case OP_SUB_CONST:
            case OP_MUL_CONST:
            case OP_DIV_CONST:
            case OP_MOD_CONST:
                out->constant = chunk->constants.values[(int)code[off + 1]];
                break;
            case OP_JUMP:
            case OP_JUMP_IF_FALSE:
            case OP_JUMP_IF_TRUE:
            case OP_LOOP: {
                int sign = (op == OP_LOOP) ? -1 : 1;
                int jump = jit_ir_read_u16(code, off + 1);
                int target_byte = off + 3 + sign * jump;
                if (target_byte < loop_start || target_byte >= loop_end) {
                    if (op != OP_LOOP && target_byte >= loop_end) {
                        out->target = JIT_TARGET_EXIT;
                        break;
                    }
                    set_loop_fail(trace, LOOP_FAIL_JUMP, op, off);
                    free(byte_to_idx);
                    free(ops);
                    free(call_ic_closures);
                    free(call_ic_programs);
                    free(call_ic2_closures);
                    free(call_ic2_programs);
                    free(call_ic3_closures);
                    free(call_ic3_programs);
                    free(call_ic4_closures);
                    free(call_ic4_programs);
                    free(call_ic5_closures);
                    free(call_ic5_programs);
                    return 0;
                }
                int target = byte_to_idx[target_byte - loop_start];
                if (target < 0 || target >= op_count) {
                    set_loop_fail(trace, LOOP_FAIL_JUMP, op, off);
                    free(byte_to_idx);
                    free(ops);
                    free(call_ic_closures);
                    free(call_ic_programs);
                    free(call_ic2_closures);
                    free(call_ic2_programs);
                    free(call_ic3_closures);
                    free(call_ic3_programs);
                    free(call_ic4_closures);
                    free(call_ic4_programs);
                    free(call_ic5_closures);
                    free(call_ic5_programs);
                    return 0;
                }
                out->target = (uint16_t)target;
                break;
            }
            case OP_QFOR_PREP_NUM:
            case OP_QFOR_LOOP_NUM:
            case OP_FOR_PREP:
            case OP_FOR_LOOP: {
                out->a = code[off + 1];
                out->b = code[off + 2];
                int sign = (op == OP_QFOR_LOOP_NUM || op == OP_FOR_LOOP) ? -1 : 1;
                int jump = jit_ir_read_u16(code, off + 3);
                int target_byte = off + 5 + sign * jump;
                if ((op == OP_QFOR_PREP_NUM || op == OP_FOR_PREP) &&
                    (target_byte < loop_start || target_byte >= loop_end)) {
                    out->target = JIT_TARGET_EXIT;
                } else {
                    if (target_byte < loop_start || target_byte >= loop_end) {
                        set_loop_fail(trace, LOOP_FAIL_JUMP, op, off);
                        free(byte_to_idx);
                        free(ops);
                        free(call_ic_closures);
                        free(call_ic_programs);
                        free(call_ic2_closures);
                        free(call_ic2_programs);
                        free(call_ic3_closures);
                        free(call_ic3_programs);
                        free(call_ic4_closures);
                        free(call_ic4_programs);
                        free(call_ic5_closures);
                        free(call_ic5_programs);
                        return 0;
                    }
                    int target = byte_to_idx[target_byte - loop_start];
                    if (target < 0 || target >= op_count) {
                        set_loop_fail(trace, LOOP_FAIL_JUMP, op, off);
                        free(byte_to_idx);
                        free(ops);
                        free(call_ic_closures);
                        free(call_ic_programs);
                        free(call_ic2_closures);
                        free(call_ic2_programs);
                        free(call_ic3_closures);
                        free(call_ic3_programs);
                        free(call_ic4_closures);
                        free(call_ic4_programs);
                        free(call_ic5_closures);
                        free(call_ic5_programs);
                        return 0;
                    }
                    out->target = (uint16_t)target;
                }
                break;
            }
            default:
                break;
        }
        off += jit_ir_decode_instr_len(code, off, chunk->count);
    }

    free(trace->ops);
    free(trace->call_ic_closures);
    free(trace->call_ic_programs);
    free(trace->call_ic2_closures);
    free(trace->call_ic2_programs);
    free(trace->call_ic3_closures);
    free(trace->call_ic3_programs);
    free(trace->call_ic4_closures);
    free(trace->call_ic4_programs);
    free(trace->call_ic5_closures);
    free(trace->call_ic5_programs);
    clear_typed_trace_meta(trace);
    if (entry_pc < 0 || entry_pc >= op_count) {
        set_loop_fail(trace, LOOP_FAIL_JUMP, trace->opcode, (int)trace->opcode_offset);
        free(byte_to_idx);
        free(ops);
        free(call_ic_closures);
        free(call_ic_programs);
        free(call_ic2_closures);
        free(call_ic2_programs);
        free(call_ic3_closures);
        free(call_ic3_programs);
        free(call_ic4_closures);
        free(call_ic4_programs);
        free(call_ic5_closures);
        free(call_ic5_programs);
        return 0;
    }
    trace->ops = ops;
    trace->call_ic_closures = call_ic_closures;
    trace->call_ic_programs = call_ic_programs;
    trace->call_ic2_closures = call_ic2_closures;
    trace->call_ic2_programs = call_ic2_programs;
    trace->call_ic3_closures = call_ic3_closures;
    trace->call_ic3_programs = call_ic3_programs;
    trace->call_ic4_closures = call_ic4_closures;
    trace->call_ic4_programs = call_ic4_programs;
    trace->call_ic5_closures = call_ic5_closures;
    trace->call_ic5_programs = call_ic5_programs;
    trace->op_count = op_count;
    trace->entry_pc = (uint16_t)((entry_pc >= 0) ? entry_pc : 0);
    trace->max_slot = (uint16_t)max_slot;
    int loop_stack_cap = max_stack + 32;
    if (loop_stack_cap < 256) loop_stack_cap = 256;
    if (loop_stack_cap > 8192) loop_stack_cap = 8192;
    trace->max_stack = (uint16_t)loop_stack_cap;
    trace->has_program = 1;
    free(byte_to_idx);
    return 1;
}

static int trace_is_empty_qfor(const JitLoopTrace* trace) {
    if (trace == NULL || !trace->has_program || trace->ops == NULL) return 0;
    if (trace->op_count != 1) return 0;
    const JitOp* op = &trace->ops[0];
    if (op->op != JOP_QFOR_LOOP_NUM) return 0;
    return op->target == 0;
}

static int trace_is_numeric_control_only(const JitLoopTrace* trace) {
    if (trace == NULL || !trace->has_program || trace->ops == NULL) return 0;
    for (int i = 0; i < trace->op_count; i++) {
        JitOpCode op = trace->ops[i].op;
        switch (op) {
            case JOP_CONSTANT:
            case JOP_NIL:
            case JOP_TRUE:
            case JOP_FALSE:
            case JOP_POP:
            case JOP_GET_LOCAL:
            case JOP_GET2_LOCAL:
            case JOP_SET_LOCAL:
            case JOP_SET_LOCAL_POP:
            case JOP_INC_LOCAL:
            case JOP_SUB_LOCAL_CONST:
            case JOP_MUL_LOCAL_CONST:
            case JOP_DIV_LOCAL_CONST:
            case JOP_MOD_LOCAL_CONST:
            case JOP_ADD_CONST:
            case JOP_SUB_CONST:
            case JOP_MUL_CONST:
            case JOP_DIV_CONST:
            case JOP_MOD_CONST:
            case JOP_ADD:
            case JOP_ADD_INPLACE:
            case JOP_SUBTRACT:
            case JOP_MULTIPLY:
            case JOP_DIVIDE:
            case JOP_MODULO:
            case JOP_NEGATE:
            case JOP_NOT:
            case JOP_IADD:
            case JOP_ISUB:
            case JOP_IMUL:
            case JOP_IDIV:
            case JOP_IMOD:
            case JOP_FADD:
            case JOP_FSUB:
            case JOP_FMUL:
            case JOP_FDIV:
            case JOP_FMOD:
            case JOP_QFADD:
            case JOP_QFSUB:
            case JOP_QFMUL:
            case JOP_QFDIV:
            case JOP_QFMOD:
            case JOP_EQUAL:
            case JOP_QGREATER_NUM:
            case JOP_QLESS_NUM:
            case JOP_JUMP:
            case JOP_JUMP_IF_FALSE:
            case JOP_JUMP_IF_TRUE:
            case JOP_QFOR_PREP_NUM:
            case JOP_QFOR_LOOP_NUM:
            case JOP_ADJUST_STACK:
                break;
            default:
                return 0;
        }
    }
    return 1;
}

static int trace_has_dynamic_calls(const JitLoopTrace* trace) {
    if (trace == NULL || !trace->has_program || trace->ops == NULL) return 0;
    for (int i = 0; i < trace->op_count; i++) {
        JitOpCode op = trace->ops[i].op;
        if (op == JOP_CALL || op == JOP_QCALL2 || op == JOP_INVOKE) return 1;
    }
    return 0;
}

static uint16_t trace_runtime_probe_limit(const JitLoopTrace* trace) {
    if (trace == NULL) return 128;
    if (trace->native_slot_loop_fn != NULL) return 8192;
    if (trace->native_value_loop_fn != NULL) return 8192;
    if (trace->native_numeric_loop_fn != NULL) return 8192;
    if (trace->native_qfor_empty_fn != NULL) return 4096;
    if (trace->numeric_fast_eligible) return 4096;
    if (trace_is_numeric_control_only(trace) || trace_is_empty_qfor(trace)) return 1024;
    if (!trace_has_dynamic_calls(trace)) return jit_mixed_loops_enabled() ? 128 : 0;
    return 128;
}

uint16_t program_runtime_probe_limit(const JitProgram* program) {
    if (program == NULL || program->ops == NULL || program->op_count <= 0) return 128;
    int has_call = 0;
    int has_invoke = 0;
    for (int i = 0; i < program->op_count; i++) {
        JitOpCode op = program->ops[i].op;
        if (op == JOP_CALL || op == JOP_QCALL2) has_call = 1;
        else if (op == JOP_INVOKE) has_invoke = 1;
    }
    if (has_invoke) return 128;
    if (has_call) return 256;
    return 4096;
}

// True when the trace body contains a nested loop (an induction pair other than
// the trace's own prep/backedge). The numeric and typed fast executors model a
// single induction pair and would mis-execute the inner loop, so those traces
// must fall back to the generic IR executor.
static int trace_has_nested_loop(const JitLoopTrace* trace) {
    if (trace == NULL || !trace->has_program || trace->ops == NULL) return 1;
    if (trace->op_count < 2) return 1;
    for (int i = 1; i < trace->op_count - 1; i++) {
        JitOpCode op = trace->ops[i].op;
        if (op == JOP_QFOR_PREP_NUM || op == JOP_QFOR_LOOP_NUM) return 1;
    }
    return 0;
}

static int trace_is_numeric_fast_eligible(const JitLoopTrace* trace) {
    if (trace == NULL || !trace->has_program || trace->ops == NULL) return 0;
    if (trace_has_nested_loop(trace)) return 0;
    if (trace->ops[0].op != JOP_QFOR_PREP_NUM) return 0;
    if (trace->ops[trace->op_count - 1].op != JOP_QFOR_LOOP_NUM) return 0;
    for (int i = 0; i < trace->op_count; i++) {
        JitOpCode op = trace->ops[i].op;
        switch (op) {
            case JOP_QFOR_PREP_NUM:
            case JOP_QFOR_LOOP_NUM:
            case JOP_GET_LOCAL:
            case JOP_CONSTANT:
            case JOP_SET_LOCAL:
            case JOP_POP:
            case JOP_ADJUST_STACK:
            case JOP_ADD:
            case JOP_ADD_INPLACE:
            case JOP_SUBTRACT:
            case JOP_MULTIPLY:
            case JOP_DIVIDE:
            case JOP_MODULO:
            case JOP_IADD:
            case JOP_ISUB:
            case JOP_IMUL:
            case JOP_IDIV:
            case JOP_IMOD:
            case JOP_FADD:
            case JOP_FSUB:
            case JOP_FMUL:
            case JOP_FDIV:
            case JOP_FMOD:
            case JOP_QFADD:
            case JOP_QFSUB:
            case JOP_QFMUL:
            case JOP_QFDIV:
            case JOP_QFMOD:
                break;
            default:
                return 0;
        }
    }
    return 1;
}

static int trace_is_typed_fast_eligible(const JitLoopTrace* trace) {
    if (trace == NULL || !trace->has_program || trace->ops == NULL) return 0;
    if (trace->op_count > 96) return 0;
    if (trace_has_nested_loop(trace)) return 0;
    for (int i = 0; i < trace->op_count; i++) {
        JitOpCode op = trace->ops[i].op;
        switch (op) {
            case JOP_CONSTANT:
            case JOP_NIL:
            case JOP_TRUE:
            case JOP_FALSE:
            case JOP_POP:
            case JOP_GET_LOCAL:
            case JOP_GET2_LOCAL:
            case JOP_GET_UPVALUE:
            case JOP_SET_LOCAL:
            case JOP_SET_LOCAL_POP:
            case JOP_APPEND:
            case JOP_ADD:
            case JOP_ADD_INPLACE:
            case JOP_SUBTRACT:
            case JOP_MULTIPLY:
            case JOP_DIVIDE:
            case JOP_MODULO:
            case JOP_IADD:
            case JOP_ISUB:
            case JOP_IMUL:
            case JOP_IDIV:
            case JOP_IMOD:
            case JOP_FADD:
            case JOP_FSUB:
            case JOP_FMUL:
            case JOP_FDIV:
            case JOP_FMOD:
            case JOP_QFADD:
            case JOP_QFSUB:
            case JOP_QFMUL:
            case JOP_QFDIV:
            case JOP_QFMOD:
            case JOP_NOT:
            case JOP_EQUAL:
            case JOP_QGREATER_NUM:
            case JOP_QLESS_NUM:
            case JOP_JUMP:
            case JOP_JUMP_IF_FALSE:
            case JOP_JUMP_IF_TRUE:
            case JOP_LENGTH:
            case JOP_SLICE:
            case JOP_BUILD_STRING2:
            case JOP_BUILD_STRING3:
            case JOP_BUILD_STRING4:
            case JOP_BUILD_STRING5:
            case JOP_GET_TABLE_NUM:
            case JOP_GET_TABLE:
            case JOP_SET_TABLE:
            case JOP_SET_TABLE_NUM:
            case JOP_ADJUST_STACK:
            case JOP_QFOR_PREP_NUM:
            case JOP_QFOR_LOOP_NUM:
                break;
            default:
                return 0;
        }
    }
    return 1;
}

static int trace_is_value_fast_eligible(const JitLoopTrace* trace) {
    if (trace == NULL || !trace->has_program || trace->ops == NULL) return 0;
    if (trace->op_count <= 0 || trace->op_count > 128) return 0;
    if (trace_has_dynamic_calls(trace)) return 0;
    return 1;
}

static int op_is_add_like(JitOpCode op) {
    return op == JOP_FADD || op == JOP_QFADD || op == JOP_ADD || op == JOP_ADD_INPLACE || op == JOP_IADD;
}

static int op_is_mul_like(JitOpCode op) {
    return op == JOP_FMUL || op == JOP_QFMUL || op == JOP_MULTIPLY || op == JOP_IMUL;
}

static int op_is_div_like(JitOpCode op) {
    return op == JOP_FDIV || op == JOP_QFDIV || op == JOP_DIVIDE || op == JOP_IDIV;
}

static int op_is_mod_like(JitOpCode op) {
    return op == JOP_FMOD || op == JOP_QFMOD || op == JOP_MODULO || op == JOP_IMOD;
}

static int add_const_index(double value, double* consts, int* const_count) {
    for (int i = 0; i < *const_count; i++) {
        if (consts[i] == value) return i;
    }
    if (*const_count >= 32) return -1;
    consts[*const_count] = value;
    (*const_count)++;
    return (*const_count) - 1;
}

static int build_numeric_loop_ir(
    const JitLoopTrace* trace,
    ToiJitNumInstr* out_instrs,
    int* out_instr_count,
    uint8_t* out_acc_slot,
    uint8_t* out_var_slot,
    uint8_t* out_end_slot,
    double* out_consts,
    int* out_const_count) {
    if (trace == NULL || !trace->has_program || trace->ops == NULL) return 0;
    if (trace->op_count < 2) return 0;
    if (out_instrs == NULL || out_instr_count == NULL ||
        out_acc_slot == NULL || out_var_slot == NULL || out_end_slot == NULL ||
        out_consts == NULL || out_const_count == NULL) return 0;
    if (trace->ops[0].op != JOP_QFOR_PREP_NUM) return 0;
    const JitOp* tail = &trace->ops[trace->op_count - 1];
    if (tail->op != JOP_QFOR_LOOP_NUM || tail->target != 0) return 0;

    int insn_count = 0;
    int const_count = 0;
    int have_acc = 0;
    uint8_t acc_slot = 0;
    uint8_t var_slot = tail->a;
    uint8_t end_slot = tail->b;
    int saw_store = 0;

    for (int i = 1; i < trace->op_count - 1; i++) {
        const JitOp* op = &trace->ops[i];
        if (insn_count >= 128) return 0;
        switch (op->op) {
            case JOP_GET_LOCAL: {
                ToiJitNumInstr insn;
                if (op->a == var_slot) insn.op = TOI_JIT_NUM_PUSH_VAR;
                else if (op->a == end_slot) insn.op = TOI_JIT_NUM_PUSH_END;
                else {
                    if (!have_acc) {
                        acc_slot = op->a;
                        have_acc = 1;
                    }
                    if (op->a != acc_slot) return 0;
                    insn.op = TOI_JIT_NUM_PUSH_ACC;
                }
                insn.arg = 0;
                out_instrs[insn_count++] = insn;
                break;
            }
            case JOP_CONSTANT: {
                if (!IS_NUMBER(op->constant)) return 0;
                int idx = add_const_index(AS_NUMBER(op->constant), out_consts, &const_count);
                if (idx < 0 || idx > 255) return 0;
                out_instrs[insn_count++] = (ToiJitNumInstr){TOI_JIT_NUM_PUSH_CONST, (uint8_t)idx};
                break;
            }
            case JOP_SET_LOCAL:
                if (!have_acc) {
                    acc_slot = op->a;
                    have_acc = 1;
                }
                if (op->a != acc_slot) return 0;
                out_instrs[insn_count++] = (ToiJitNumInstr){TOI_JIT_NUM_STORE_ACC, 0};
                saw_store = 1;
                break;
            case JOP_POP:
                out_instrs[insn_count++] = (ToiJitNumInstr){TOI_JIT_NUM_POP, 0};
                break;
            case JOP_ADJUST_STACK:
                break;
            default: {
                uint8_t bop = 0;
                if (op_is_add_like(op->op)) bop = TOI_JIT_NUM_BIN_ADD;
                else if (op->op == JOP_SUBTRACT || op->op == JOP_FSUB || op->op == JOP_QFSUB || op->op == JOP_ISUB) {
                    bop = TOI_JIT_NUM_BIN_SUB;
                } else if (op_is_mul_like(op->op)) {
                    bop = TOI_JIT_NUM_BIN_MUL;
                } else if (op_is_div_like(op->op)) {
                    bop = TOI_JIT_NUM_BIN_DIV;
                } else if (op_is_mod_like(op->op)) {
                    bop = TOI_JIT_NUM_BIN_MOD;
                } else {
                    return 0;
                }
                out_instrs[insn_count++] = (ToiJitNumInstr){bop, 0};
                break;
            }
        }
    }

    if (!have_acc || !saw_store) return 0;
    *out_instr_count = insn_count;
    *out_acc_slot = acc_slot;
    *out_var_slot = var_slot;
    *out_end_slot = end_slot;
    *out_const_count = const_count;
    return 1;
}

static int add_slot_unique(uint8_t slot, uint8_t* slots, int* count) {
    for (int i = 0; i < *count; i++) {
        if (slots[i] == slot) return 1;
    }
    if (*count >= 64) return 0;
    slots[*count] = slot;
    (*count)++;
    return 1;
}

typedef struct {
    int insn_index;
    int target_op;
} SlotJumpPatch;

static int build_slot_loop_ir(
    const JitLoopTrace* trace,
    ToiJitSlotInstr* out_instrs,
    int* out_instr_count,
    uint8_t* out_var_slot,
    uint8_t* out_end_slot,
    uint8_t* out_slot_count,
    double* out_consts,
    int* out_const_count,
    uint8_t* out_used_slots,
    int* out_used_count,
    uint8_t* out_written_slots,
    int* out_written_count,
    uint8_t* out_fail_op) {
    if (trace == NULL || !trace->has_program || trace->ops == NULL) return 0;
    if (trace->op_count < 2) return 0;
    if (out_instrs == NULL || out_instr_count == NULL ||
        out_var_slot == NULL || out_end_slot == NULL || out_slot_count == NULL ||
        out_consts == NULL || out_const_count == NULL ||
        out_used_slots == NULL || out_used_count == NULL ||
        out_written_slots == NULL || out_written_count == NULL) {
        return 0;
    }
    if (out_fail_op != NULL) *out_fail_op = 255;
#define SLOT_FAIL(opcode_val)                                                                                           \
    do {                                                                                                                \
        if (out_fail_op != NULL) *out_fail_op = (uint8_t)(opcode_val);                                                 \
        return 0;                                                                                                       \
    } while (0)
    if (trace->ops[0].op != JOP_QFOR_PREP_NUM) return 0;
    const JitOp* tail = &trace->ops[trace->op_count - 1];
    if (tail->op != JOP_QFOR_LOOP_NUM || tail->target != 0) return 0;

    int slot_count = (int)trace->max_slot + 1;
    if (slot_count <= 0 || slot_count > 255) return 0;

    int insn_count = 0;
    int const_count = 0;
    int used_count = 0;
    int written_count = 0;
    int jump_patch_count = 0;
    int vsp = 0;
    int op_to_insn[512];
    if (trace->op_count > (int)(sizeof(op_to_insn) / sizeof(op_to_insn[0]))) return 0;
    for (int i = 0; i < trace->op_count; i++) op_to_insn[i] = -1;
    SlotJumpPatch jump_patches[192];

    for (int i = 1; i < trace->op_count - 1; i++) {
        const JitOp* op = &trace->ops[i];
        if (insn_count >= 192) return 0;
        op_to_insn[i] = insn_count;
        switch (op->op) {
            case JOP_GET_LOCAL:
                if (!add_slot_unique(op->a, out_used_slots, &used_count)) return 0;
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_SLOT, op->a};
                vsp++;
                break;
            case JOP_NIL: {
                int idx = add_const_index(0.0, out_consts, &const_count);
                if (idx < 0 || idx > 255) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                vsp++;
                break;
            }
            case JOP_TRUE: {
                int idx = add_const_index(1.0, out_consts, &const_count);
                if (idx < 0 || idx > 255) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                vsp++;
                break;
            }
            case JOP_FALSE: {
                int idx = add_const_index(0.0, out_consts, &const_count);
                if (idx < 0 || idx > 255) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                vsp++;
                break;
            }
            case JOP_CONSTANT: {
                if (!IS_NUMBER(op->constant)) SLOT_FAIL(op->op);
                int idx = add_const_index(AS_NUMBER(op->constant), out_consts, &const_count);
                if (idx < 0 || idx > 255) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                vsp++;
                break;
            }
            case JOP_SET_LOCAL:
                if (vsp < 1) SLOT_FAIL(op->op);
                if (op->a == tail->a || op->a == tail->b) SLOT_FAIL(op->op);
                if (!add_slot_unique(op->a, out_written_slots, &written_count)) SLOT_FAIL(op->op);
                if (!add_slot_unique(op->a, out_used_slots, &used_count)) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_STORE_SLOT, op->a};
                break;
            case JOP_POP:
                if (vsp < 1) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_POP, 0};
                vsp--;
                break;
            case JOP_ADJUST_STACK:
                // No-op in slot IR: stack effects are represented by explicit push/pop ops above.
                break;
            case JOP_ADD:
            case JOP_ADD_INPLACE:
            case JOP_IADD:
            case JOP_FADD:
            case JOP_QFADD:
                if (vsp < 2) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_ADD, 0};
                vsp--;
                break;
            case JOP_SUBTRACT:
            case JOP_ISUB:
            case JOP_FSUB:
            case JOP_QFSUB:
                if (vsp < 2) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_SUB, 0};
                vsp--;
                break;
            case JOP_MULTIPLY:
            case JOP_IMUL:
            case JOP_FMUL:
            case JOP_QFMUL:
                if (vsp < 2) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_MUL, 0};
                vsp--;
                break;
            case JOP_DIVIDE:
            case JOP_IDIV:
            case JOP_FDIV:
            case JOP_QFDIV:
                if (vsp < 2) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_DIV, 0};
                vsp--;
                break;
            case JOP_MODULO:
            case JOP_IMOD:
            case JOP_FMOD:
            case JOP_QFMOD:
                if (vsp < 2) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_MOD, 0};
                vsp--;
                break;
            case JOP_ADD_CONST:
            case JOP_SUB_CONST:
            case JOP_MUL_CONST:
            case JOP_DIV_CONST:
            case JOP_MOD_CONST: {
                if (vsp < 1) SLOT_FAIL(op->op);
                if (!IS_NUMBER(op->constant)) SLOT_FAIL(op->op);
                if (insn_count + 2 > 192) SLOT_FAIL(op->op);
                int idx = add_const_index(AS_NUMBER(op->constant), out_consts, &const_count);
                if (idx < 0 || idx > 255) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                uint8_t bop = TOI_JIT_SLOT_BIN_ADD;
                if (op->op == JOP_ADD_CONST) bop = TOI_JIT_SLOT_BIN_ADD;
                else if (op->op == JOP_SUB_CONST) bop = TOI_JIT_SLOT_BIN_SUB;
                else if (op->op == JOP_MUL_CONST) bop = TOI_JIT_SLOT_BIN_MUL;
                else if (op->op == JOP_DIV_CONST) bop = TOI_JIT_SLOT_BIN_DIV;
                else bop = TOI_JIT_SLOT_BIN_MOD;
                out_instrs[insn_count++] = (ToiJitSlotInstr){bop, 0};
                break;
            }
            case JOP_QLESS_NUM:
                if (vsp < 2) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_CMP_LT, 0};
                vsp--;
                break;
            case JOP_QGREATER_NUM:
                if (vsp < 2) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_CMP_GT, 0};
                vsp--;
                break;
            case JOP_EQUAL:
                if (vsp < 2) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_CMP_EQ, 0};
                vsp--;
                break;
            case JOP_NOT: {
                if (vsp < 1) SLOT_FAIL(op->op);
                if (insn_count + 2 > 192) SLOT_FAIL(op->op);
                int idx = add_const_index(0.0, out_consts, &const_count);
                if (idx < 0 || idx > 255) SLOT_FAIL(op->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_CMP_EQ, 0};
                break;
            }
            case JOP_JUMP:
            case JOP_JUMP_IF_FALSE:
            case JOP_JUMP_IF_TRUE: {
                if (op->target == 0) SLOT_FAIL(op->op);
                if (op->target >= (uint16_t)trace->op_count) SLOT_FAIL(op->op);
                if (op->op != JOP_JUMP && vsp < 1) SLOT_FAIL(op->op);
                if (jump_patch_count >= (int)(sizeof(jump_patches) / sizeof(jump_patches[0]))) SLOT_FAIL(op->op);
                uint8_t jop = TOI_JIT_SLOT_JUMP;
                if (op->op == JOP_JUMP_IF_FALSE) jop = TOI_JIT_SLOT_JUMP_IF_FALSE;
                else if (op->op == JOP_JUMP_IF_TRUE) jop = TOI_JIT_SLOT_JUMP_IF_TRUE;
                out_instrs[insn_count++] = (ToiJitSlotInstr){jop, 0};
                jump_patches[jump_patch_count].insn_index = insn_count - 1;
                jump_patches[jump_patch_count].target_op = (int)op->target;
                jump_patch_count++;
                break;
            }
            default:
                SLOT_FAIL(op->op);
        }
    }
    if (vsp != 0) return 0;
    if (insn_count <= 0) return 0;
    for (int i = 0; i < jump_patch_count; i++) {
        int target_op = jump_patches[i].target_op;
        int target_insn = insn_count;
        if (target_op != trace->op_count - 1) {
            if (target_op <= 0 || target_op >= trace->op_count - 1) return 0;
            target_insn = op_to_insn[target_op];
            if (target_insn < 0) return 0;
        }
        if (target_insn < 0 || target_insn > 255) return 0;
        out_instrs[jump_patches[i].insn_index].arg = (uint8_t)target_insn;
    }

    *out_instr_count = insn_count;
    *out_var_slot = tail->a;
    *out_end_slot = tail->b;
    *out_slot_count = (uint8_t)slot_count;
    *out_const_count = const_count;
    *out_used_count = used_count;
    *out_written_count = written_count;
    return 1;
#undef SLOT_FAIL
}

static int build_value_loop_ir(
    const JitLoopTrace* trace,
    ToiJitValueInstr* out_instrs,
    uint32_t* out_deopt_offsets,
    int* out_instr_count,
    uint8_t* out_var_slot,
    uint8_t* out_end_slot,
    uint8_t* out_fail_op) {
    if (trace == NULL || !trace->has_program || trace->ops == NULL) return 0;
    if (trace->op_count < 2) return 0;
    if (out_instrs == NULL || out_instr_count == NULL || out_var_slot == NULL || out_end_slot == NULL) return 0;
    if (out_fail_op != NULL) *out_fail_op = 255;
    if (trace->ops[0].op != JOP_QFOR_PREP_NUM) return 0;
    const JitOp* tail = &trace->ops[trace->op_count - 1];
    if (tail->op != JOP_QFOR_LOOP_NUM || tail->target != 0) return 0;

    typedef struct {
        int insn_index;
        int target_op;
    } ValueJumpPatch;
    ValueJumpPatch jump_patches[256];
    int jump_patch_count = 0;
    int op_to_insn[512];
    if (trace->op_count > (int)(sizeof(op_to_insn) / sizeof(op_to_insn[0]))) return 0;
    for (int i = 0; i < trace->op_count; i++) op_to_insn[i] = -1;

    int insn_count = 0;
    for (int i = 1; i < trace->op_count - 1; i++) {
        const JitOp* op = &trace->ops[i];
        if (insn_count >= 256) return 0;
        op_to_insn[i] = insn_count;
        ToiJitValueInstr insn;
        memset(&insn, 0, sizeof(insn));
        switch (op->op) {
            case JOP_GET_LOCAL: insn.op = TOI_JIT_VAL_PUSH_LOCAL; insn.a = op->a; break;
            case JOP_GET_UPVALUE: insn.op = TOI_JIT_VAL_PUSH_UPVALUE; insn.a = op->a; break;
            case JOP_CONSTANT: insn.op = TOI_JIT_VAL_PUSH_CONST; insn.constant = op->constant; break;
            case JOP_NIL: insn.op = TOI_JIT_VAL_PUSH_NIL; break;
            case JOP_TRUE: insn.op = TOI_JIT_VAL_PUSH_TRUE; break;
            case JOP_FALSE: insn.op = TOI_JIT_VAL_PUSH_FALSE; break;
            case JOP_SET_LOCAL: insn.op = TOI_JIT_VAL_STORE_LOCAL; insn.a = op->a; break;
            case JOP_POP: insn.op = TOI_JIT_VAL_POP; break;
            case JOP_ADD:
            case JOP_ADD_INPLACE:
            case JOP_IADD:
            case JOP_FADD:
            case JOP_QFADD:
                insn.op = TOI_JIT_VAL_BIN_ADD;
                break;
            case JOP_SUBTRACT:
            case JOP_ISUB:
            case JOP_FSUB:
            case JOP_QFSUB:
                insn.op = TOI_JIT_VAL_BIN_SUB;
                break;
            case JOP_MULTIPLY:
            case JOP_IMUL:
            case JOP_FMUL:
            case JOP_QFMUL:
                insn.op = TOI_JIT_VAL_BIN_MUL;
                break;
            case JOP_DIVIDE:
            case JOP_IDIV:
            case JOP_FDIV:
            case JOP_QFDIV:
                insn.op = TOI_JIT_VAL_BIN_DIV;
                break;
            case JOP_MODULO:
            case JOP_IMOD:
            case JOP_FMOD:
            case JOP_QFMOD:
                insn.op = TOI_JIT_VAL_BIN_MOD;
                break;
            case JOP_EQUAL: insn.op = TOI_JIT_VAL_CMP_EQ; break;
            case JOP_QLESS_NUM: insn.op = TOI_JIT_VAL_CMP_LT; break;
            case JOP_QGREATER_NUM: insn.op = TOI_JIT_VAL_CMP_GT; break;
            case JOP_NOT: insn.op = TOI_JIT_VAL_NOT; break;
            case JOP_APPEND: insn.op = TOI_JIT_VAL_APPEND; break;
            case JOP_GET_TABLE: insn.op = TOI_JIT_VAL_GET_TABLE; break;
            case JOP_GET_TABLE_NUM: insn.op = TOI_JIT_VAL_GET_TABLE_NUM; break;
            case JOP_SET_TABLE: insn.op = TOI_JIT_VAL_SET_TABLE; break;
            case JOP_SET_TABLE_NUM: insn.op = TOI_JIT_VAL_SET_TABLE_NUM; break;
            case JOP_CALL:
                insn.op = TOI_JIT_VAL_CALL;
                insn.a = op->a;
                insn.constant = op->constant;
                break;
            case JOP_INVOKE:
                insn.op = TOI_JIT_VAL_INVOKE;
                insn.a = op->a;
                insn.constant = op->constant;
                break;
            case JOP_BUILD_STRING2: insn.op = TOI_JIT_VAL_BUILD_STRING2; break;
            case JOP_BUILD_STRING3: insn.op = TOI_JIT_VAL_BUILD_STRING3; break;
            case JOP_BUILD_STRING4: insn.op = TOI_JIT_VAL_BUILD_STRING4; break;
            case JOP_BUILD_STRING5: insn.op = TOI_JIT_VAL_BUILD_STRING5; break;
            case JOP_ADJUST_STACK: insn.op = TOI_JIT_VAL_ADJUST_STACK; insn.a = op->a; break;
            case JOP_JUMP:
            case JOP_JUMP_IF_FALSE:
            case JOP_JUMP_IF_TRUE:
                if (op->target == 0 || op->target >= (uint16_t)trace->op_count) {
                    if (out_fail_op != NULL) *out_fail_op = (uint8_t)op->op;
                    return 0;
                }
                if (jump_patch_count >= 256) {
                    if (out_fail_op != NULL) *out_fail_op = (uint8_t)op->op;
                    return 0;
                }
                insn.op = (op->op == JOP_JUMP) ? TOI_JIT_VAL_JUMP :
                          (op->op == JOP_JUMP_IF_FALSE) ? TOI_JIT_VAL_JUMP_IF_FALSE :
                          TOI_JIT_VAL_JUMP_IF_TRUE;
                jump_patches[jump_patch_count].insn_index = insn_count;
                jump_patches[jump_patch_count].target_op = (int)op->target;
                jump_patch_count++;
                break;
            default:
                if (out_fail_op != NULL) *out_fail_op = (uint8_t)op->op;
                return 0;
        }
        out_instrs[insn_count++] = insn;
        if (out_deopt_offsets != NULL) out_deopt_offsets[insn_count - 1] = op->byte_offset;
    }
    for (int i = 0; i < jump_patch_count; i++) {
        int target_op = jump_patches[i].target_op;
        int target_insn = insn_count;
        if (target_op != trace->op_count - 1) {
            if (target_op <= 0 || target_op >= trace->op_count - 1) {
                if (out_fail_op != NULL) *out_fail_op = (uint8_t)trace->ops[jump_patches[i].insn_index + 1].op;
                return 0;
            }
            target_insn = op_to_insn[target_op];
            if (target_insn < 0) {
                if (out_fail_op != NULL) *out_fail_op = (uint8_t)trace->ops[jump_patches[i].insn_index + 1].op;
                return 0;
            }
        }
        out_instrs[jump_patches[i].insn_index].target = (uint16_t)target_insn;
    }

    *out_instr_count = insn_count;
    *out_var_slot = tail->a;
    *out_end_slot = tail->b;
    return insn_count > 0;
}

static int build_slot_loop_from_value_ir(
    const ToiJitValueInstr* value_instrs,
    int value_instr_count,
    uint8_t var_slot,
    uint8_t end_slot,
    int slot_count,
    ToiJitSlotInstr* out_instrs,
    int* out_instr_count,
    double* out_consts,
    int* out_const_count,
    uint8_t* out_used_slots,
    int* out_used_count,
    uint8_t* out_written_slots,
    int* out_written_count,
    uint8_t* out_upvalue_const_idx,
    uint8_t* out_upvalue_idx,
    int* out_upvalue_count,
    uint8_t* out_fail_op) {
    if (value_instrs == NULL || value_instr_count <= 0) return 0;
    if (out_instrs == NULL || out_instr_count == NULL || out_consts == NULL || out_const_count == NULL ||
        out_used_slots == NULL || out_used_count == NULL ||
        out_written_slots == NULL || out_written_count == NULL ||
        out_upvalue_const_idx == NULL || out_upvalue_idx == NULL || out_upvalue_count == NULL) {
        return 0;
    }
    if (slot_count <= 0 || slot_count > 255) return 0;
    if (out_fail_op != NULL) *out_fail_op = 255;

#define VALUE_SLOT_FAIL(opcode_val)                                                                                      \
    do {                                                                                                                 \
        if (out_fail_op != NULL) *out_fail_op = (uint8_t)(opcode_val);                                                  \
        return 0;                                                                                                        \
    } while (0)

    typedef struct {
        int slot_insn_index;
        int target_value_index;
    } ValueToSlotJumpPatch;

    int insn_count = 0;
    int const_count = 0;
    int used_count = 0;
    int written_count = 0;
    int upvalue_count = 0;
    int vsp = 0;
    int value_to_slot[256];
    int upvalue_to_const[256];
    if (value_instr_count > 256) return 0;
    for (int i = 0; i < value_instr_count; i++) value_to_slot[i] = -1;
    for (int i = 0; i < 256; i++) upvalue_to_const[i] = -1;
    ValueToSlotJumpPatch jump_patches[256];
    int jump_patch_count = 0;

    for (int i = 0; i < value_instr_count; i++) {
        const ToiJitValueInstr* in = &value_instrs[i];
        value_to_slot[i] = insn_count;
        if (insn_count >= 256) VALUE_SLOT_FAIL(in->op);
        switch (in->op) {
            case TOI_JIT_VAL_PUSH_LOCAL:
                if ((int)in->a >= slot_count) VALUE_SLOT_FAIL(in->op);
                if (!add_slot_unique(in->a, out_used_slots, &used_count)) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_SLOT, in->a};
                vsp++;
                break;
            case TOI_JIT_VAL_PUSH_CONST:
                if (!IS_NUMBER(in->constant)) VALUE_SLOT_FAIL(in->op);
                {
                    int idx = add_const_index(AS_NUMBER(in->constant), out_consts, &const_count);
                    if (idx < 0 || idx > 255) VALUE_SLOT_FAIL(in->op);
                    out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                    vsp++;
                }
                break;
            case TOI_JIT_VAL_PUSH_UPVALUE: {
                int up_idx = (int)in->a;
                if (up_idx < 0 || up_idx > 255) VALUE_SLOT_FAIL(in->op);
                int idx = upvalue_to_const[up_idx];
                if (idx < 0) {
                    if (const_count >= 32 || upvalue_count >= 16) VALUE_SLOT_FAIL(in->op);
                    idx = const_count++;
                    out_consts[idx] = 0.0;
                    upvalue_to_const[up_idx] = idx;
                    out_upvalue_const_idx[upvalue_count] = (uint8_t)idx;
                    out_upvalue_idx[upvalue_count] = (uint8_t)up_idx;
                    upvalue_count++;
                }
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                vsp++;
                break;
            }
            case TOI_JIT_VAL_PUSH_TRUE: {
                int idx = add_const_index(1.0, out_consts, &const_count);
                if (idx < 0 || idx > 255) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                vsp++;
                break;
            }
            case TOI_JIT_VAL_PUSH_FALSE: {
                int idx = add_const_index(0.0, out_consts, &const_count);
                if (idx < 0 || idx > 255) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                vsp++;
                break;
            }
            case TOI_JIT_VAL_PUSH_NIL: {
                int idx = add_const_index(0.0, out_consts, &const_count);
                if (idx < 0 || idx > 255) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                vsp++;
                break;
            }
            case TOI_JIT_VAL_STORE_LOCAL:
                if ((int)in->a >= slot_count || vsp < 1) VALUE_SLOT_FAIL(in->op);
                if (in->a == var_slot || in->a == end_slot) VALUE_SLOT_FAIL(in->op);
                if (!add_slot_unique(in->a, out_written_slots, &written_count)) VALUE_SLOT_FAIL(in->op);
                if (!add_slot_unique(in->a, out_used_slots, &used_count)) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_STORE_SLOT, in->a};
                break;
            case TOI_JIT_VAL_POP:
                if (vsp < 1) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_POP, 0};
                vsp--;
                break;
            case TOI_JIT_VAL_BIN_ADD:
                if (vsp < 2) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_ADD, 0};
                vsp--;
                break;
            case TOI_JIT_VAL_BIN_SUB:
                if (vsp < 2) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_SUB, 0};
                vsp--;
                break;
            case TOI_JIT_VAL_BIN_MUL:
                if (vsp < 2) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_MUL, 0};
                vsp--;
                break;
            case TOI_JIT_VAL_BIN_DIV:
                if (vsp < 2) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_DIV, 0};
                vsp--;
                break;
            case TOI_JIT_VAL_BIN_MOD:
                if (vsp < 2) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_MOD, 0};
                vsp--;
                break;
            case TOI_JIT_VAL_CMP_LT:
                if (vsp < 2) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_CMP_LT, 0};
                vsp--;
                break;
            case TOI_JIT_VAL_CMP_GT:
                if (vsp < 2) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_CMP_GT, 0};
                vsp--;
                break;
            case TOI_JIT_VAL_CMP_EQ:
                if (vsp < 2) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_CMP_EQ, 0};
                vsp--;
                break;
            case TOI_JIT_VAL_NOT: {
                if (vsp < 1) VALUE_SLOT_FAIL(in->op);
                if (insn_count + 2 > 256) VALUE_SLOT_FAIL(in->op);
                int idx = add_const_index(0.0, out_consts, &const_count);
                if (idx < 0 || idx > 255) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_CMP_EQ, 0};
                break;
            }
            case TOI_JIT_VAL_JUMP:
            case TOI_JIT_VAL_JUMP_IF_FALSE:
            case TOI_JIT_VAL_JUMP_IF_TRUE:
                if (jump_patch_count >= 256) VALUE_SLOT_FAIL(in->op);
                if (in->op != TOI_JIT_VAL_JUMP && vsp < 1) VALUE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){
                    (in->op == TOI_JIT_VAL_JUMP) ? TOI_JIT_SLOT_JUMP :
                    (in->op == TOI_JIT_VAL_JUMP_IF_FALSE) ? TOI_JIT_SLOT_JUMP_IF_FALSE : TOI_JIT_SLOT_JUMP_IF_TRUE,
                    0
                };
                jump_patches[jump_patch_count].slot_insn_index = insn_count - 1;
                jump_patches[jump_patch_count].target_value_index = (int)in->target;
                jump_patch_count++;
                break;
            case TOI_JIT_VAL_ADJUST_STACK:
                // Stack discipline is explicit in lowered slot ops.
                break;
            default:
                VALUE_SLOT_FAIL(in->op);
        }
    }

    if (vsp != 0) return 0;
    if (insn_count <= 0) return 0;

    for (int i = 0; i < jump_patch_count; i++) {
        int value_target = jump_patches[i].target_value_index;
        int slot_target = insn_count;
        if (value_target != value_instr_count) {
            if (value_target < 0 || value_target >= value_instr_count) return 0;
            slot_target = value_to_slot[value_target];
            if (slot_target < 0) return 0;
        }
        if (slot_target < 0 || slot_target > 255) return 0;
        out_instrs[jump_patches[i].slot_insn_index].arg = (uint8_t)slot_target;
    }

    *out_instr_count = insn_count;
    *out_const_count = const_count;
    *out_used_count = used_count;
    *out_written_count = written_count;
    *out_upvalue_count = upvalue_count;
    return 1;
#undef VALUE_SLOT_FAIL
}

static int build_slot_loop_from_native_trace_ir(
    const ToiJitNativeTraceInstr* trace_instrs,
    int trace_instr_count,
    uint8_t var_slot,
    uint8_t end_slot,
    int slot_count,
    ToiJitSlotInstr* out_instrs,
    int* out_instr_count,
    double* out_consts,
    int* out_const_count,
    uint8_t* out_used_slots,
    int* out_used_count,
    uint8_t* out_written_slots,
    int* out_written_count,
    uint8_t* out_upvalue_const_idx,
    uint8_t* out_upvalue_idx,
    int* out_upvalue_count,
    uint8_t* out_fail_op) {
    if (trace_instrs == NULL || trace_instr_count <= 0) return 0;
    if (out_instrs == NULL || out_instr_count == NULL || out_consts == NULL || out_const_count == NULL ||
        out_used_slots == NULL || out_used_count == NULL ||
        out_written_slots == NULL || out_written_count == NULL ||
        out_upvalue_const_idx == NULL || out_upvalue_idx == NULL || out_upvalue_count == NULL) {
        return 0;
    }
    if (slot_count <= 0 || slot_count > 255 || trace_instr_count > 256) return 0;
    if (out_fail_op != NULL) *out_fail_op = 255;

#define NTRACE_SLOT_FAIL(opcode_val)                                                                                   \
    do {                                                                                                               \
        if (out_fail_op != NULL) *out_fail_op = (uint8_t)(opcode_val);                                                \
        return 0;                                                                                                      \
    } while (0)

    typedef struct {
        int slot_insn_index;
        int target_trace_index;
    } NativeTraceSlotJumpPatch;

    NativeTraceSlotJumpPatch jump_patches[256];
    int trace_to_slot[256];
    for (int i = 0; i < trace_instr_count; i++) trace_to_slot[i] = -1;

    int insn_count = 0;
    int const_count = 0;
    int used_count = 0;
    int written_count = 0;
    int upvalue_count = 0;
    int jump_patch_count = 0;
    int vsp = 0;
    int upvalue_to_const[256];
    for (int i = 0; i < 256; i++) upvalue_to_const[i] = -1;

    for (int i = 0; i < trace_instr_count; i++) {
        const ToiJitNativeTraceInstr* in = &trace_instrs[i];
        trace_to_slot[i] = insn_count;
        if (insn_count >= 256) NTRACE_SLOT_FAIL(in->op);
        switch (in->op) {
            case TOI_JIT_NATIVE_OP_NOP:
            case TOI_JIT_NATIVE_OP_ADJUST_STACK:
                break;
            case TOI_JIT_NATIVE_OP_PUSH_LOCAL:
                if ((int)in->a >= slot_count) NTRACE_SLOT_FAIL(in->op);
                if (!add_slot_unique(in->a, out_used_slots, &used_count)) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_SLOT, in->a};
                vsp++;
                break;
            case TOI_JIT_NATIVE_OP_PUSH_UPVALUE: {
                int up_idx = (int)in->a;
                if (up_idx < 0 || up_idx > 255) NTRACE_SLOT_FAIL(in->op);
                int idx = upvalue_to_const[up_idx];
                if (idx < 0) {
                    if (const_count >= 32 || upvalue_count >= 16) NTRACE_SLOT_FAIL(in->op);
                    idx = const_count++;
                    out_consts[idx] = 0.0;
                    upvalue_to_const[up_idx] = idx;
                    out_upvalue_const_idx[upvalue_count] = (uint8_t)idx;
                    out_upvalue_idx[upvalue_count] = (uint8_t)up_idx;
                    upvalue_count++;
                }
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                vsp++;
                break;
            }
            case TOI_JIT_NATIVE_OP_PUSH_CONST:
                if (!IS_NUMBER(in->constant)) NTRACE_SLOT_FAIL(in->op);
                {
                    int idx = add_const_index(AS_NUMBER(in->constant), out_consts, &const_count);
                    if (idx < 0 || idx > 255) NTRACE_SLOT_FAIL(in->op);
                    out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                    vsp++;
                }
                break;
            case TOI_JIT_NATIVE_OP_PUSH_TRUE: {
                int idx = add_const_index(1.0, out_consts, &const_count);
                if (idx < 0 || idx > 255) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                vsp++;
                break;
            }
            case TOI_JIT_NATIVE_OP_PUSH_FALSE:
            case TOI_JIT_NATIVE_OP_PUSH_NIL: {
                int idx = add_const_index(0.0, out_consts, &const_count);
                if (idx < 0 || idx > 255) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                vsp++;
                break;
            }
            case TOI_JIT_NATIVE_OP_STORE_LOCAL:
                if ((int)in->a >= slot_count || vsp < 1) NTRACE_SLOT_FAIL(in->op);
                if (in->a == var_slot || in->a == end_slot) NTRACE_SLOT_FAIL(in->op);
                if (!add_slot_unique(in->a, out_written_slots, &written_count)) NTRACE_SLOT_FAIL(in->op);
                if (!add_slot_unique(in->a, out_used_slots, &used_count)) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_STORE_SLOT, in->a};
                break;
            case TOI_JIT_NATIVE_OP_POP:
                if (vsp < 1) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_POP, 0};
                vsp--;
                break;
            case TOI_JIT_NATIVE_OP_BIN_ADD:
                if (vsp < 2) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_ADD, 0};
                vsp--;
                break;
            case TOI_JIT_NATIVE_OP_BIN_SUB:
                if (vsp < 2) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_SUB, 0};
                vsp--;
                break;
            case TOI_JIT_NATIVE_OP_BIN_MUL:
                if (vsp < 2) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_MUL, 0};
                vsp--;
                break;
            case TOI_JIT_NATIVE_OP_BIN_DIV:
                if (vsp < 2) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_DIV, 0};
                vsp--;
                break;
            case TOI_JIT_NATIVE_OP_BIN_MOD:
                if (vsp < 2) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_BIN_MOD, 0};
                vsp--;
                break;
            case TOI_JIT_NATIVE_OP_CMP_LT:
                if (vsp < 2) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_CMP_LT, 0};
                vsp--;
                break;
            case TOI_JIT_NATIVE_OP_CMP_GT:
                if (vsp < 2) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_CMP_GT, 0};
                vsp--;
                break;
            case TOI_JIT_NATIVE_OP_CMP_EQ:
                if (vsp < 2) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_CMP_EQ, 0};
                vsp--;
                break;
            case TOI_JIT_NATIVE_OP_NOT: {
                if (vsp < 1) NTRACE_SLOT_FAIL(in->op);
                if (insn_count + 2 > 256) NTRACE_SLOT_FAIL(in->op);
                int idx = add_const_index(0.0, out_consts, &const_count);
                if (idx < 0 || idx > 255) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_PUSH_CONST, (uint8_t)idx};
                out_instrs[insn_count++] = (ToiJitSlotInstr){TOI_JIT_SLOT_CMP_EQ, 0};
                break;
            }
            case TOI_JIT_NATIVE_OP_JUMP:
            case TOI_JIT_NATIVE_OP_JUMP_IF_FALSE:
            case TOI_JIT_NATIVE_OP_JUMP_IF_TRUE:
                if (jump_patch_count >= 256) NTRACE_SLOT_FAIL(in->op);
                if (in->op != TOI_JIT_NATIVE_OP_JUMP && vsp < 1) NTRACE_SLOT_FAIL(in->op);
                out_instrs[insn_count++] = (ToiJitSlotInstr){
                    (in->op == TOI_JIT_NATIVE_OP_JUMP) ? TOI_JIT_SLOT_JUMP :
                    (in->op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE) ? TOI_JIT_SLOT_JUMP_IF_FALSE : TOI_JIT_SLOT_JUMP_IF_TRUE,
                    0
                };
                jump_patches[jump_patch_count].slot_insn_index = insn_count - 1;
                jump_patches[jump_patch_count].target_trace_index = (int)in->a;
                jump_patch_count++;
                break;
            default:
                NTRACE_SLOT_FAIL(in->op);
        }
    }

    if (vsp != 0 || insn_count <= 0) return 0;

    for (int i = 0; i < jump_patch_count; i++) {
        int trace_target = jump_patches[i].target_trace_index;
        int slot_target = insn_count;
        if (trace_target != trace_instr_count) {
            if (trace_target < 0 || trace_target >= trace_instr_count) return 0;
            slot_target = trace_to_slot[trace_target];
            if (slot_target < 0) return 0;
        }
        if (slot_target < 0 || slot_target > 255) return 0;
        out_instrs[jump_patches[i].slot_insn_index].arg = (uint8_t)slot_target;
    }

    *out_instr_count = insn_count;
    *out_const_count = const_count;
    *out_used_count = used_count;
    *out_written_count = written_count;
    *out_upvalue_count = upvalue_count;
    return 1;
#undef NTRACE_SLOT_FAIL
}

static int native_trace_is_exec_safe(const JitLoopTrace* trace, uint8_t* out_reject_op) {
    if (trace == NULL) return 0;
    if (out_reject_op != NULL) *out_reject_op = 255;
    if (!trace->native_trace_ready || trace->native_trace_fn == NULL) return 0;
    if (trace->native_trace_instr_count == 0 || trace->native_trace_instr_count > 256) return 0;
    if (trace->native_trace_guard_count > 256) return 0;
    if (!is_numeric_for_loop_opcode(trace->opcode) && trace->opcode != OP_LOOP) return 0;

    int instr_count = (int)trace->native_trace_instr_count;
    int call_sites = 0;
    for (int i = 0; i < instr_count; i++) {
        const ToiJitNativeTraceInstr* in = &trace->native_trace_instrs[i];
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
            case TOI_JIT_NATIVE_OP_CMP_EQ:
            case TOI_JIT_NATIVE_OP_CMP_LT:
            case TOI_JIT_NATIVE_OP_CMP_GT:
            case TOI_JIT_NATIVE_OP_NOT:
            case TOI_JIT_NATIVE_OP_APPEND:
            case TOI_JIT_NATIVE_OP_GET_TABLE:
            case TOI_JIT_NATIVE_OP_GET_TABLE_NUM:
            case TOI_JIT_NATIVE_OP_SET_TABLE:
            case TOI_JIT_NATIVE_OP_SET_TABLE_NUM:
                break;
            case TOI_JIT_NATIVE_OP_JUMP_IF_FALSE:
            case TOI_JIT_NATIVE_OP_JUMP_IF_TRUE:
            case TOI_JIT_NATIVE_OP_JUMP:
                // In-trace merges (if/else, and/or short-circuits) need the pending
                // operand stack materialized on both edges. The emitter implements
                // that (write-through + merge fixup), but a spilling trace is still
                // slower than the IR executor for merged loops (measured: app
                // pipeline 0.43 s vs 0.24 s) and one elif shape still diverges, so
                // merges stay on the IR path. Jumps to the end of the body
                // (continue) run in machine code.
                // TODO(jit-native-trace): keep fast paths for unspilled entries and
                // fix the elif merge, then enable merged traces.
                if ((int)in->target != (int)trace->native_trace_instr_count) {
                    if (out_reject_op != NULL) *out_reject_op = in->op;
                    return 0;
                }
                break;
            case TOI_JIT_NATIVE_OP_CALL:
                // Only a call whose callee is a compile-time closure is safe: the
                // emitter then has an identity-guarded entry for it. For a dynamic
                // callee the direct-core path stages arguments/emits deopt state
                // that is not validated yet (it mis-stages operands and crashed the
                // suite in testing), so those loops stay on the IR executor.
                // TODO(jit-native-trace): validate dynamic-call staging + deopt
                // state, then drop this gate.
                if (!IS_CLOSURE(in->constant)) {
                    if (out_reject_op != NULL) *out_reject_op = in->op;
                    return 0;
                }
                if (++call_sites > 1) {
                    if (out_reject_op != NULL) *out_reject_op = in->op;
                    return 0;
                }
                break;
            case TOI_JIT_NATIVE_OP_INVOKE:
                // Method invokes are guarded by an invoke-target guard, so a
                // single invoke site is safe to emit.
                if (++call_sites > 1) {
                    if (out_reject_op != NULL) *out_reject_op = in->op;
                    return 0;
                }
                break;
            case TOI_JIT_NATIVE_OP_BUILD_STRING2:
            case TOI_JIT_NATIVE_OP_BUILD_STRING3:
            case TOI_JIT_NATIVE_OP_BUILD_STRING4:
            case TOI_JIT_NATIVE_OP_BUILD_STRING5:
            case TOI_JIT_NATIVE_OP_ADJUST_STACK:
                break;
            default:
                if (out_reject_op != NULL) *out_reject_op = in->op;
                return 0;
        }
    }
    return 1;
}

static int native_trace_guards_hold(VM* vm, const JitLoopTrace* trace, CallFrame* frame) {
    if (vm == NULL || trace == NULL || frame == NULL || frame->closure == NULL) return 0;
    static int poly_calls_env = -1;
    if (poly_calls_env < 0) {
        const char* v = getenv("TOI_JIT_NATIVE_CALL_POLY");
        poly_calls_env = (v != NULL && v[0] != '\0' && strcmp(v, "0") != 0) ? 1 : 0;
    }
    int slot_count = (int)trace->max_slot + 1;
    for (int i = 0; i < (int)trace->native_trace_guard_count; i++) {
        const ToiJitNativeGuard* g = &trace->native_trace_guards[i];
        switch (g->kind) {
            case TOI_JIT_NATIVE_GUARD_LOCAL_IS_NUMBER:
                if ((int)g->slot_a < 0 || (int)g->slot_a >= slot_count) return 0;
                if (!IS_NUMBER(frame->slots[g->slot_a])) return 0;
                break;
            case TOI_JIT_NATIVE_GUARD_UPVALUE_IS_NUMBER:
                if ((int)g->slot_a < 0 || (int)g->slot_a >= frame->closure->upvalue_count) return 0;
                if (frame->closure->upvalues[g->slot_a] == NULL ||
                    frame->closure->upvalues[g->slot_a]->location == NULL) {
                    return 0;
                }
                if (!IS_NUMBER(*frame->closure->upvalues[g->slot_a]->location)) return 0;
                break;
            case TOI_JIT_NATIVE_GUARD_LOCAL_IS_STRING:
                if ((int)g->slot_a < 0 || (int)g->slot_a >= slot_count) return 0;
                if (!IS_STRING(frame->slots[g->slot_a])) return 0;
                break;
            case TOI_JIT_NATIVE_GUARD_UPVALUE_IS_STRING:
                if ((int)g->slot_a < 0 || (int)g->slot_a >= frame->closure->upvalue_count) return 0;
                if (frame->closure->upvalues[g->slot_a] == NULL ||
                    frame->closure->upvalues[g->slot_a]->location == NULL) {
                    return 0;
                }
                if (!IS_STRING(*frame->closure->upvalues[g->slot_a]->location)) return 0;
                break;
            case TOI_JIT_NATIVE_GUARD_TABLE_SHAPE: {
                Value table_v = NIL_VAL;
                if (g->slot_b == 1) {
                    if ((int)g->slot_a < 0 || (int)g->slot_a >= frame->closure->upvalue_count) return 0;
                    if (frame->closure->upvalues[g->slot_a] == NULL ||
                        frame->closure->upvalues[g->slot_a]->location == NULL) {
                        return 0;
                    }
                    table_v = *frame->closure->upvalues[g->slot_a]->location;
                } else {
                    if ((int)g->slot_a < 0 || (int)g->slot_a >= slot_count) return 0;
                    table_v = frame->slots[g->slot_a];
                }
                if (!IS_TABLE(table_v)) return 0;
                ObjTable* t = AS_TABLE(table_v);
                if ((g->aux & 1u) != 0u && (t == NULL || !t->no_metatable_fast)) return 0;
                break;
            }
            case TOI_JIT_NATIVE_GUARD_CALL_TARGET: {
                if (trace->native_trace_direct_core && poly_calls_env) {
                    break;
                }
                Value callee = NIL_VAL;
                int argc = (int)g->aux;
                if (g->slot_b == 1) {
                    if ((int)g->slot_a < 0 || (int)g->slot_a >= frame->closure->upvalue_count) return 0;
                    if (frame->closure->upvalues[g->slot_a] == NULL ||
                        frame->closure->upvalues[g->slot_a]->location == NULL) {
                        return 0;
                    }
                    callee = *frame->closure->upvalues[g->slot_a]->location;
                } else {
                    if ((int)g->slot_a < 0 || (int)g->slot_a >= slot_count) return 0;
                    callee = frame->slots[g->slot_a];
                }
                if (!IS_CLOSURE(callee) && !IS_NATIVE(callee) && !IS_BOUND_METHOD(callee)) return 0;
                if (IS_CLOSURE(callee)) {
                    ObjClosure* c = AS_CLOSURE(callee);
                    if (c == NULL || c->function == NULL) return 0;
                    if (c->function->arity != argc) return 0;
                } else if (IS_BOUND_METHOD(callee)) {
                    ObjBoundMethod* bm = AS_BOUND_METHOD(callee);
                    if (bm == NULL || bm->method == NULL) return 0;
                    if (argc + 1 > 16) return 0;
                    Value method = OBJ_VAL(bm->method);
                    if (IS_CLOSURE(method)) {
                        ObjClosure* c = AS_CLOSURE(method);
                        if (c == NULL || c->function == NULL) return 0;
                        if (c->function->arity != argc + 1) return 0;
                    }
                }
                break;
            }
            case TOI_JIT_NATIVE_GUARD_INVOKE_TARGET: {
                if (trace->native_trace_direct_core && poly_calls_env) {
                    break;
                }
                Value receiver = NIL_VAL;
                if (g->slot_b == 1) {
                    if ((int)g->slot_a < 0 || (int)g->slot_a >= frame->closure->upvalue_count) return 0;
                    if (frame->closure->upvalues[g->slot_a] == NULL ||
                        frame->closure->upvalues[g->slot_a]->location == NULL) {
                        return 0;
                    }
                    receiver = *frame->closure->upvalues[g->slot_a]->location;
                } else {
                    if ((int)g->slot_a < 0 || (int)g->slot_a >= slot_count) return 0;
                    receiver = frame->slots[g->slot_a];
                }
                if (!IS_STRING(g->constant)) return 0;
                ObjString* method = AS_STRING(g->constant);
                int argc = (int)g->aux;
                if (IS_STRING(receiver)) {
                    if (argc != 0) return 0;
                    if (!jit_objstring_eq_name(method, vm->str_lower_name, "lower") &&
                        !jit_objstring_eq_name(method, vm->str_upper_name, "upper")) {
                        return 0;
                    }
                    break;
                }
                if (!IS_TABLE(receiver)) return 0;
                ObjTable* t = AS_TABLE(receiver);
                if (t == NULL) return 0;
                Value raw_method = NIL_VAL;
                if (!table_get(&t->table, method, &raw_method)) return 0;
                if (jit_value_is_self_callable(raw_method) && argc >= 255) return 0;
                if (!IS_CLOSURE(raw_method) && !IS_NATIVE(raw_method) && !IS_BOUND_METHOD(raw_method)) return 0;
                break;
            }
            default:
                break;
        }
    }
    return 1;
}

static void native_trace_analyze_shape(JitLoopTrace* trace) {
    if (trace == NULL) return;
    trace->native_trace_has_call = 0;
    trace->native_trace_has_invoke = 0;
    trace->native_trace_has_build_string = 0;
    trace->native_trace_has_mutating_table = 0;
    trace->native_trace_has_get_table_generic = 0;
    trace->native_trace_prefix_work = trace->native_trace_instr_count;

    uint16_t first_call_index = trace->native_trace_instr_count;
    int instr_count = (int)trace->native_trace_instr_count;
    for (int i = 0; i < instr_count; i++) {
        switch (trace->native_trace_instrs[i].op) {
            case TOI_JIT_NATIVE_OP_CALL:
                trace->native_trace_has_call = 1;
                if ((uint16_t)i < first_call_index) first_call_index = (uint16_t)i;
                break;
            case TOI_JIT_NATIVE_OP_INVOKE:
                trace->native_trace_has_invoke = 1;
                if ((uint16_t)i < first_call_index) first_call_index = (uint16_t)i;
                break;
            case TOI_JIT_NATIVE_OP_BUILD_STRING2:
            case TOI_JIT_NATIVE_OP_BUILD_STRING3:
            case TOI_JIT_NATIVE_OP_BUILD_STRING4:
            case TOI_JIT_NATIVE_OP_BUILD_STRING5:
                trace->native_trace_has_build_string = 1;
                break;
            case TOI_JIT_NATIVE_OP_APPEND:
            case TOI_JIT_NATIVE_OP_SET_TABLE:
            case TOI_JIT_NATIVE_OP_SET_TABLE_NUM:
                trace->native_trace_has_mutating_table = 1;
                break;
            case TOI_JIT_NATIVE_OP_GET_TABLE:
                trace->native_trace_has_get_table_generic = 1;
                break;
            default:
                break;
        }
    }
    trace->native_trace_prefix_work = first_call_index;
}

static void native_trace_seed_call_program_hints(VM* vm, JitLoopTrace* trace) {
    if (vm == NULL || trace == NULL || !trace->native_trace_ready) return;
    int instr_count = (int)trace->native_trace_instr_count;
    for (int i = 0; i < instr_count; i++) {
        ToiJitNativeTraceInstr* in = &trace->native_trace_instrs[i];
        in->aux_data = 0;
        if (in->op != TOI_JIT_NATIVE_OP_CALL) continue;
        if (!IS_CLOSURE(in->constant)) continue;
        ObjClosure* c = AS_CLOSURE(in->constant);
        if (c == NULL) continue;
        JitProgram* program = NULL;
        if (!jit_get_compiled_closure_program(vm, c, &program) || program == NULL) continue;
        in->aux_data = (uint64_t)(uintptr_t)program;
    }
}

static uint32_t native_trace_exec_stride(const JitLoopTrace* trace) {
    if (trace == NULL) return 16u;
    if (trace->native_trace_direct_core) {
        uint64_t attempts = trace->native_trace_exec_attempts;
        uint64_t exits = trace->native_trace_exit_hits;
        uint64_t soft_bails = trace->native_trace_soft_bails;
        uint16_t prefix_work = trace->native_trace_prefix_work;
        int has_call_or_invoke = trace->native_trace_has_call || trace->native_trace_has_invoke;
        int app_like =
            trace->native_trace_has_build_string ||
            trace->native_trace_has_get_table_generic;

        if (has_call_or_invoke) {
            if (attempts < 8u) return 8u;
            if (soft_bails * 100u > attempts * 95u) {
                if (prefix_work < 4u && !app_like) return 256u;
                return 128u;
            }
            if (soft_bails * 100u > attempts * 60u) return 64u;
            if (exits * 100u >= attempts * 25u) return app_like ? 4u : 8u;
            return app_like ? 16u : 32u;
        }
        if (app_like) {
            if (attempts < 8u) return 8u;
            if (trace->native_trace_continue_hits * 100u > attempts * 90u) return 32u;
            return 8u;
        }
        return 8u;
    }
    if (trace->typed_fast_eligible) {
        if (trace->native_trace_exec_attempts < 8u) return 32u;
        return 128u;
    }
    int has_call = trace->native_trace_has_call != 0;
    int has_build_string = trace->native_trace_has_build_string != 0;
    int has_invoke = trace->native_trace_has_invoke != 0;
    int has_invoke_guard = 0;
    int has_mutating_table = trace->native_trace_has_mutating_table != 0;
    int has_get_table_generic = trace->native_trace_has_get_table_generic != 0;
    for (int i = 0; i < (int)trace->native_trace_guard_count; i++) {
        if (trace->native_trace_guards[i].kind == TOI_JIT_NATIVE_GUARD_INVOKE_TARGET) {
            has_invoke_guard = 1;
            break;
        }
    }
    uint16_t prefix_work = trace->native_trace_prefix_work;
    uint64_t attempts = trace->native_trace_exec_attempts;
    uint64_t exits = trace->native_trace_exit_hits;
    uint64_t soft_bails = trace->native_trace_soft_bails;
    if ((has_call || has_invoke) && prefix_work < 8u && attempts >= 8u) return 64u;
    if (has_invoke) {
        if (!has_invoke_guard) return 128u;
        if (attempts < 4u) return 8u;
        if (prefix_work < 16u && soft_bails * 100u > attempts * 50u) return 128u;
        if (soft_bails * 100u <= attempts * 15u &&
            trace->native_trace_deopts * 100u <= attempts * 10u) return 4u;
        if (exits * 100u >= attempts * 30u &&
            trace->native_trace_deopts * 100u <= attempts * 15u) return 8u;
        if (soft_bails * 100u <= attempts * 30u &&
            trace->native_trace_deopts * 100u <= attempts * 15u) return 8u;
        return 32u;
    }
    if (has_call) {
        if (attempts < 4u) return 8u;
        if (prefix_work < 16u && soft_bails * 100u > attempts * 50u) return 128u;
        if (soft_bails * 100u <= attempts * 15u &&
            trace->native_trace_deopts * 100u <= attempts * 10u) return 4u;
        if (exits * 100u >= attempts * 30u &&
            trace->native_trace_deopts * 100u <= attempts * 15u) return 8u;
        if (soft_bails * 100u <= attempts * 30u &&
            trace->native_trace_deopts * 100u <= attempts * 15u) return 8u;
        return 32u;
    }
    if (has_build_string) return 8u;
    if (has_mutating_table) return 32u;
    if (has_get_table_generic) return 16u;

    uint32_t work = (uint32_t)trace->native_trace_instr_count + ((uint32_t)trace->native_trace_guard_count * 2u);
    if (work < 12u) return 16u;
    if (work < 20u) return 8u;
    if (work < 32u) return 4u;
    return 1u;
}

static int native_trace_is_profitable(const JitLoopTrace* trace) {
    if (trace == NULL) return 0;
    uint64_t attempts = trace->native_trace_exec_attempts;
    if (attempts < 24u) return 1;

    uint64_t hits = trace->native_trace_exec_hits;
    uint64_t deopts = trace->native_trace_deopts;
    uint64_t soft_bails = trace->native_trace_soft_bails;
    uint64_t continues = trace->native_trace_continue_hits;
    int has_call_or_invoke = trace->native_trace_has_call || trace->native_trace_has_invoke;

    /* Require sustained wins after warmup. */
    if ((hits * 100u) < (attempts * 20u)) return 0;
    /* Deopt-heavy traces pay bridge cost but rarely complete natively. */
    if ((deopts * 100u) > (attempts * 35u)) return 0;
    if (trace->native_trace_direct_core &&
        attempts >= 64u &&
        (continues * 100u) > (attempts * 99u) &&
        !has_call_or_invoke &&
        !trace->native_trace_has_build_string &&
        !trace->native_trace_has_get_table_generic &&
        trace->native_trace_prefix_work < 4u) {
        return 0;
    }
    /*
     * CALL/INVOKE-heavy traces can report "hits" via CONTINUE while doing
     * very little native work. Require either meaningful full exits or
     * low soft-bail rates for those traces.
     */
    if (has_call_or_invoke) {
        if (trace->native_trace_direct_core) {
            int app_like =
                trace->native_trace_has_build_string ||
                trace->native_trace_has_get_table_generic;
            if ((soft_bails * 100u) > (attempts * 98u) &&
                (trace->native_trace_prefix_work < 4u || !app_like)) {
                return 0;
            }
        } else {
            if ((soft_bails * 100u) > (attempts * 60u)) return 0;
        }
    }
    return 1;
}

static void commit_numeric_loop_state(
    ToiJitState* st,
    JitLoopTrace* trace,
    CallFrame* frame,
    ObjThread* thread,
    uint8_t** ip,
    const double* work,
    const uint8_t* is_number,
    int frame_window,
    int sp,
    uint32_t next_offset) {
    for (int j = 0; j < frame_window; j++) {
        if (is_number[j]) frame->slots[j] = NUMBER_VAL(work[j]);
    }
    thread->stack_top = frame->slots + sp;
    *ip = frame->closure->function->chunk.code + next_offset;
    st->loop_exec_hits++;
    trace->exec_hits++;
}

static int execute_numeric_trace_fast(
    ToiJitState* st,
    JitLoopTrace* trace,
    CallFrame* frame,
    ObjThread* thread,
    uint8_t** ip) {
    if (st == NULL || trace == NULL || frame == NULL || thread == NULL || ip == NULL) return 0;
    if (!trace->numeric_fast_eligible || !trace->has_program || trace->ops == NULL) return 0;

    ptrdiff_t depth = thread->stack_top - frame->slots;
    if (depth < 0) return 0;
    int sp = (int)depth;
    int frame_window = sp;
    if ((int)trace->max_slot + 1 > frame_window) frame_window = (int)trace->max_slot + 1;
    if (frame_window < 0 || frame_window > 8192) return 0;
    if (trace->max_stack <= 0 || trace->max_stack > 8192) return 0;

    int work_cap = trace->max_stack;
    if (frame_window > work_cap) work_cap = frame_window;
    if (work_cap <= 0) return 0;
    double work[work_cap];
    uint8_t is_number[work_cap];
    for (int j = 0; j < frame_window; j++) {
        Value v = frame->slots[j];
        if (IS_NUMBER(v)) {
            work[j] = AS_NUMBER(v);
            is_number[j] = 1;
        } else {
            work[j] = 0.0;
            is_number[j] = 0;
        }
    }

    int pc = (int)trace->entry_pc;
    uint32_t iter_guard = 0;
    uint32_t loop_chunk_iters = 0;
    const uint32_t loop_chunk_budget = 4096;
    while (pc >= 0 && pc < trace->op_count) {
        if (++iter_guard > 20000000u) {
            // Never drop accumulated work: commit and let the VM resume at the
            // current bytecode offset so every entry makes progress.
            uint32_t resume = trace->ops[pc].byte_offset;
            commit_numeric_loop_state(
                st, trace, frame, thread, ip, work, is_number, frame_window, sp, resume);
            return 1;
        }
        JitOp* op = &trace->ops[pc];
        switch (op->op) {
            case JOP_CONSTANT:
                if (sp >= trace->max_stack) return 0;
                if (!IS_NUMBER(op->constant)) return 0;
                work[sp++] = AS_NUMBER(op->constant);
                is_number[sp - 1] = 1;
                pc++;
                break;
            case JOP_GET_LOCAL:
                if ((int)op->a >= frame_window || sp >= trace->max_stack) return 0;
                if (!is_number[op->a]) return 0;
                work[sp++] = work[op->a];
                is_number[sp - 1] = 1;
                pc++;
                break;
            case JOP_SET_LOCAL:
                if ((int)op->a >= frame_window || sp <= 0) return 0;
                if (!is_number[sp - 1]) return 0;
                work[op->a] = work[sp - 1];
                is_number[op->a] = 1;
                pc++;
                break;
            case JOP_POP:
                if (sp <= 0) return 0;
                sp--;
                pc++;
                break;
            case JOP_ADJUST_STACK:
                if ((int)op->a < 0 || (int)op->a > trace->max_stack) return 0;
                sp = (int)op->a;
                pc++;
                break;
            case JOP_ADD:
            case JOP_ADD_INPLACE:
            case JOP_SUBTRACT:
            case JOP_MULTIPLY:
            case JOP_DIVIDE:
            case JOP_MODULO:
            case JOP_IADD:
            case JOP_ISUB:
            case JOP_IMUL:
            case JOP_IDIV:
            case JOP_IMOD:
            case JOP_FADD:
            case JOP_FSUB:
            case JOP_FMUL:
            case JOP_FDIV:
            case JOP_FMOD:
            case JOP_QFADD:
            case JOP_QFSUB:
            case JOP_QFMUL:
            case JOP_QFDIV:
            case JOP_QFMOD: {
                if (sp < 2) return 0;
                if (!is_number[sp - 1] || !is_number[sp - 2]) return 0;
                double b = work[sp - 1];
                double a = work[sp - 2];
                double out = 0.0;
                if (op->op == JOP_ADD || op->op == JOP_ADD_INPLACE || op->op == JOP_IADD || op->op == JOP_FADD || op->op == JOP_QFADD) out = a + b;
                else if (op->op == JOP_SUBTRACT || op->op == JOP_ISUB || op->op == JOP_FSUB || op->op == JOP_QFSUB) out = a - b;
                else if (op->op == JOP_MULTIPLY || op->op == JOP_IMUL || op->op == JOP_FMUL || op->op == JOP_QFMUL) out = a * b;
                else if (op->op == JOP_DIVIDE || op->op == JOP_IDIV || op->op == JOP_FDIV || op->op == JOP_QFDIV) out = a / b;
                else out = fmod(a, b);
                work[sp - 2] = out;
                is_number[sp - 2] = 1;
                sp--;
                pc++;
                break;
            }
            case JOP_QFOR_PREP_NUM:
                if ((int)op->a >= frame_window || (int)op->b >= frame_window) return 0;
                if (!is_number[op->a] || !is_number[op->b]) return 0;
                if (work[op->a] > work[op->b]) {
                    if (op->target != JIT_TARGET_EXIT) return 0;
                    commit_numeric_loop_state(
                        st, trace, frame, thread, ip, work, is_number, frame_window, sp, trace->after_offset);
                    return 1;
                }
                pc++;
                break;
            case JOP_QFOR_LOOP_NUM: {
                if ((int)op->a >= frame_window || (int)op->b >= frame_window) return 0;
                if (!is_number[op->a] || !is_number[op->b]) return 0;
                double next = work[op->a] + 1.0;
                work[op->a] = next;
                if (next <= work[op->b]) {
                    pc = (int)op->target;
                    if (++loop_chunk_iters >= loop_chunk_budget) {
                        commit_numeric_loop_state(
                            st, trace, frame, thread, ip, work, is_number, frame_window, sp, trace->loop_start_offset);
                        return 1;
                    }
                } else {
                    commit_numeric_loop_state(
                        st, trace, frame, thread, ip, work, is_number, frame_window, sp, trace->after_offset);
                    return 1;
                }
                break;
            }
            default:
                return 0;
        }
    }
    return 0;
}

static int commit_loop_state(
    ToiJitState* st,
    JitLoopTrace* trace,
    CallFrame* frame,
    ObjThread* thread,
    uint8_t** ip,
    const Value* work,
    int sp,
    int frame_window,
    uint32_t next_offset) {
    if (st == NULL || trace == NULL || frame == NULL || thread == NULL || ip == NULL || work == NULL) return 0;
    if (sp < 0 || frame_window < 0) return 0;
    int commit_count = frame_window > sp ? frame_window : sp;
    for (int j = 0; j < commit_count; j++) {
        frame->slots[j] = work[j];
    }
    thread->stack_top = frame->slots + sp;
    *ip = frame->closure->function->chunk.code + next_offset;
    st->loop_exec_hits++;
    trace->exec_hits++;
    return 1;
}

static int execute_typed_trace_fast(
    VM* vm,
    ToiJitState* st,
    ObjFunction* function,
    JitLoopTrace* trace,
    CallFrame* frame,
    ObjThread* thread,
    uint8_t** ip) {
    if (vm == NULL || st == NULL || function == NULL || trace == NULL || frame == NULL || thread == NULL || ip == NULL) return 0;
    if (!trace->typed_fast_eligible || !trace->typed_ir_ready || !trace->has_program || trace->ops == NULL) return 0;

    ptrdiff_t depth = thread->stack_top - frame->slots;
    if (depth < 0) return 0;
    int sp = (int)depth;
    int frame_window = sp;
    if ((int)trace->max_slot + 1 > frame_window) frame_window = (int)trace->max_slot + 1;
    if (frame_window < 0 || frame_window > 8192) return 0;
    if (trace->max_stack <= 0 || trace->max_stack > 8192) return 0;

    int work_cap = trace->max_stack;
    if (frame_window > work_cap) work_cap = frame_window;
    if (work_cap <= 0) return 0;

    Value work[work_cap];
    double num_cache[work_cap];
    uint8_t num_valid[work_cap];
    for (int j = 0; j < frame_window; j++) {
        Value v = (j < sp) ? frame->slots[j] : NIL_VAL;
        work[j] = v;
        if (IS_NUMBER(v)) {
            num_valid[j] = 1;
            num_cache[j] = AS_NUMBER(v);
        } else {
            num_valid[j] = 0;
            num_cache[j] = 0.0;
        }
    }

#define WORK_SET(idx, vexpr)                                                                                           \
    do {                                                                                                                \
        int _idx = (idx);                                                                                               \
        Value _v = (vexpr);                                                                                             \
        work[_idx] = _v;                                                                                                \
        if (IS_NUMBER(_v)) {                                                                                            \
            num_valid[_idx] = 1;                                                                                        \
            num_cache[_idx] = AS_NUMBER(_v);                                                                            \
        } else {                                                                                                        \
            num_valid[_idx] = 0;                                                                                        \
        }                                                                                                               \
    } while (0)
#define WORK_SET_NUM(idx, d)                                                                                            \
    do {                                                                                                                \
        int _idx = (idx);                                                                                               \
        double _d = (d);                                                                                                \
        work[_idx] = NUMBER_VAL(_d);                                                                                    \
        num_valid[_idx] = 1;                                                                                            \
        num_cache[_idx] = _d;                                                                                           \
    } while (0)
#define TYPED_FAIL(reason)                                                                                              \
    do {                                                                                                                \
        jit_record_typed_exit(st, trace, pc, typed_exit_reason_for_op(op->op));                                         \
        if (jit_try_typed_side_exit_to_interp(st, trace, frame, thread, ip, work, sp, frame_window, pc)) {           \
            return 1;                                                                                                   \
        }                                                                                                               \
        if (jit_dump_loops_enabled()) {                                                                                 \
            fprintf(stderr,                                                                                             \
                    "[jit-typed-fail] fn=%s at=%u pc=%d op=%s sp=%d reason=%s\n",                                     \
                    (function->name != NULL) ? function->name->chars : "<script>",                                    \
                    (unsigned)trace->opcode_offset,                                                                     \
                    pc,                                                                                                 \
                    jit_op_name(op->op),                                                                               \
                    sp,                                                                                                 \
                    reason);                                                                                            \
        }                                                                                                               \
        /* Commit accumulated state instead of dropping it. */                                                          \
        return commit_loop_state(                                                                                       \
            st, trace, frame, thread, ip, work, sp, frame_window, trace->ops[pc].byte_offset);                          \
    } while (0)
#define WORK_GET_NUM(idx, out_num)                                                                                      \
    do {                                                                                                                \
        if (!num_valid[idx]) {                                                                                          \
            if (!IS_NUMBER(work[idx])) {                                                                                \
                TYPED_FAIL("non-number");                                                                               \
            }                                                                                                            \
            num_valid[idx] = 1;                                                                                         \
            num_cache[idx] = AS_NUMBER(work[idx]);                                                                      \
        }                                                                                                               \
        out_num = num_cache[idx];                                                                                       \
    } while (0)

    int pc = (int)trace->entry_pc;
    uint32_t iter_guard = 0;
    uint32_t loop_chunk_iters = 0;
    const uint32_t loop_chunk_budget = 4096;
    while (pc >= 0 && pc < trace->op_count) {
        if (++iter_guard > 20000000u) {
            // Commit instead of dropping the work done so far; the VM resumes at
            // the current bytecode offset and the loop can be re-entered later.
            jit_record_typed_exit(st, trace, pc, TYPED_EXIT_REASON_CONTROL);
            return commit_loop_state(
                st, trace, frame, thread, ip, work, sp, frame_window, trace->ops[pc].byte_offset);
        }
        JitOp* op = &trace->ops[pc];
        switch (op->op) {
            case JOP_CONSTANT:
                if (sp >= trace->max_stack) TYPED_FAIL("const-stack");
                WORK_SET(sp++, op->constant);
                pc++;
                break;
            case JOP_NIL:
                if (sp >= trace->max_stack) TYPED_FAIL("nil-stack");
                WORK_SET(sp++, NIL_VAL);
                pc++;
                break;
            case JOP_TRUE:
                if (sp >= trace->max_stack) TYPED_FAIL("true-stack");
                WORK_SET(sp++, BOOL_VAL(1));
                pc++;
                break;
            case JOP_FALSE:
                if (sp >= trace->max_stack) TYPED_FAIL("false-stack");
                WORK_SET(sp++, BOOL_VAL(0));
                pc++;
                break;
            case JOP_POP:
                if (sp <= 0) TYPED_FAIL("pop-stack");
                sp--;
                pc++;
                break;
            case JOP_GET_LOCAL:
                if ((int)op->a >= frame_window || sp >= trace->max_stack) TYPED_FAIL("get-local");
                work[sp] = work[op->a];
                num_valid[sp] = num_valid[op->a];
                num_cache[sp] = num_cache[op->a];
                sp++;
                pc++;
                break;
            case JOP_GET_UPVALUE:
                if ((int)op->a >= frame->closure->upvalue_count || sp >= trace->max_stack) TYPED_FAIL("get-upvalue");
                if (frame->closure->upvalues[op->a] == NULL ||
                    frame->closure->upvalues[op->a]->location == NULL) TYPED_FAIL("get-upvalue-null");
                WORK_SET(sp++, *frame->closure->upvalues[op->a]->location);
                pc++;
                break;
            case JOP_GET2_LOCAL:
                if ((int)op->a >= frame_window || (int)op->b >= frame_window || sp + 1 >= trace->max_stack) {
                    TYPED_FAIL("get2-local");
                }
                work[sp] = work[op->a];
                num_valid[sp] = num_valid[op->a];
                num_cache[sp] = num_cache[op->a];
                sp++;
                work[sp] = work[op->b];
                num_valid[sp] = num_valid[op->b];
                num_cache[sp] = num_cache[op->b];
                sp++;
                pc++;
                break;
            case JOP_SET_LOCAL:
                if ((int)op->a >= frame_window || sp <= 0) TYPED_FAIL("set-local");
                work[op->a] = work[sp - 1];
                num_valid[op->a] = num_valid[sp - 1];
                num_cache[op->a] = num_cache[sp - 1];
                pc++;
                break;
            case JOP_SET_LOCAL_POP:
                if ((int)op->a >= frame_window || sp <= 0) TYPED_FAIL("set-local-pop");
                work[op->a] = work[sp - 1];
                num_valid[op->a] = num_valid[sp - 1];
                num_cache[op->a] = num_cache[sp - 1];
                sp--;
                pc++;
                break;
            case JOP_APPEND: {
                if (sp < 2) TYPED_FAIL("append-stack");
                Value v = work[sp - 1];
                Value tval = work[sp - 2];
                if (!IS_TABLE(tval)) TYPED_FAIL("append-not-table");
                ObjTable* t = AS_TABLE(tval);
                if (!t->no_metatable_fast) TYPED_FAIL("append-metatable");
                int idx = t->table.array_max + 1;
                if (!table_set_array(&t->table, idx, v)) {
                    ObjString* key = number_key_string((double)idx);
                    table_set(&t->table, key, v);
                }
                WORK_SET_NUM(sp - 2, (double)idx);
                sp--;
                pc++;
                break;
            }
            case JOP_ADD:
            case JOP_ADD_INPLACE:
            case JOP_SUBTRACT:
            case JOP_MULTIPLY:
            case JOP_DIVIDE:
            case JOP_MODULO:
            case JOP_IADD:
            case JOP_ISUB:
            case JOP_IMUL:
            case JOP_IDIV:
            case JOP_IMOD:
            case JOP_FADD:
            case JOP_FSUB:
            case JOP_FMUL:
            case JOP_FDIV:
            case JOP_FMOD:
            case JOP_QFADD:
            case JOP_QFSUB:
            case JOP_QFMUL:
            case JOP_QFDIV:
            case JOP_QFMOD: {
                if (sp < 2) TYPED_FAIL("arith-stack");
                double b = 0.0;
                double a = 0.0;
                WORK_GET_NUM(sp - 1, b);
                WORK_GET_NUM(sp - 2, a);
                double out = 0.0;
                if (op->op == JOP_ADD || op->op == JOP_ADD_INPLACE || op->op == JOP_IADD || op->op == JOP_FADD || op->op == JOP_QFADD) out = a + b;
                else if (op->op == JOP_SUBTRACT || op->op == JOP_ISUB || op->op == JOP_FSUB || op->op == JOP_QFSUB) out = a - b;
                else if (op->op == JOP_MULTIPLY || op->op == JOP_IMUL || op->op == JOP_FMUL || op->op == JOP_QFMUL) out = a * b;
                else if (op->op == JOP_DIVIDE || op->op == JOP_IDIV || op->op == JOP_FDIV || op->op == JOP_QFDIV) out = a / b;
                else out = fmod(a, b);
                WORK_SET_NUM(sp - 2, out);
                sp--;
                pc++;
                break;
            }
            case JOP_NOT:
                if (sp <= 0) TYPED_FAIL("not-stack");
                WORK_SET(sp - 1, BOOL_VAL(is_falsey_jit(work[sp - 1])));
                pc++;
                break;
            case JOP_EQUAL: {
                if (sp < 2) TYPED_FAIL("equal-stack");
                Value b = work[sp - 1];
                Value a = work[sp - 2];
                int eq = jit_values_equal(a, b);
                WORK_SET(sp - 2, BOOL_VAL(eq));
                sp--;
                pc++;
                break;
            }
            case JOP_QGREATER_NUM:
            case JOP_QLESS_NUM: {
                if (sp < 2) TYPED_FAIL("cmp-stack");
                double b = 0.0;
                double a = 0.0;
                WORK_GET_NUM(sp - 1, b);
                WORK_GET_NUM(sp - 2, a);
                int cmp = (op->op == JOP_QGREATER_NUM) ? (a > b) : (a < b);
                WORK_SET(sp - 2, BOOL_VAL(cmp));
                sp--;
                pc++;
                break;
            }
            case JOP_JUMP:
                if (op->target == JIT_TARGET_EXIT) {
                    return commit_loop_state(st, trace, frame, thread, ip, work, sp, frame_window, trace->after_offset);
                }
                // Generic backedge (while / iterator loops) has no induction slot
                // to budget against, so chunk on the backedge itself. Without this
                // a very long generic loop can hit the IR guard and livelock.
                if ((int)op->target < pc && ++loop_chunk_iters >= loop_chunk_budget) {
                    return commit_loop_state(
                        st, trace, frame, thread, ip, work, sp, frame_window, trace->loop_start_offset);
                }
                pc = (int)op->target;
                break;
            case JOP_JUMP_IF_FALSE:
                if (sp <= 0) TYPED_FAIL("jif-false-stack");
                if (is_falsey_jit(work[sp - 1])) {
                    if (op->target == JIT_TARGET_EXIT) {
                        return commit_loop_state(st, trace, frame, thread, ip, work, sp, frame_window, trace->after_offset);
                    }
                    pc = (int)op->target;
                } else {
                    pc = pc + 1;
                }
                break;
            case JOP_JUMP_IF_TRUE:
                if (sp <= 0) TYPED_FAIL("jif-true-stack");
                if (is_falsey_jit(work[sp - 1])) {
                    pc = pc + 1;
                } else {
                    if (op->target == JIT_TARGET_EXIT) {
                        return commit_loop_state(st, trace, frame, thread, ip, work, sp, frame_window, trace->after_offset);
                    }
                    pc = (int)op->target;
                }
                break;
            case JOP_CALL: {
                int argc = (int)op->a;
                if (argc < 0 || sp < argc + 1) TYPED_FAIL("call-stack");
                int callee_idx = sp - 1 - argc;
                Value callee = work[callee_idx];
                Value out = NIL_VAL;
                int ic_hit = jit_try_trace_call_ic(vm, function, trace, op, pc, callee, &work[callee_idx + 1], argc, &out);
                if (!ic_hit) {
                    int out_count = 0;
                    if (!jit_call_primitive_multi(
                            vm,
                            callee,
                            &work[callee_idx + 1],
                            argc,
                            &work[callee_idx],
                            trace->max_stack - callee_idx,
                            &out_count)) {
                        TYPED_FAIL("call-primitive");
                    }
                    if (out_count != 1) TYPED_FAIL("call-multi-return");
                    out = work[callee_idx];
                }
                if (!ic_hit) {
                    jit_update_trace_call_ic(vm, trace, pc, callee, argc);
                }
                WORK_SET(callee_idx, out);
                sp = callee_idx + 1;
                pc++;
                break;
            }
            case JOP_QCALL2: {
                if (sp < 3) TYPED_FAIL("qcall2-stack");
                Value callee = work[sp - 3];
                Value arg0 = work[sp - 2];
                Value arg1 = work[sp - 1];
                Value out = NIL_VAL;
                if (IS_CLOSURE(callee)) {
                    ObjClosure* c = AS_CLOSURE(callee);
                    ObjFunction* fn = c->function;
                    if (fn != NULL &&
                        fn->tiny_inline_kind != TINY_INLINE_NONE &&
                        jit_eval_tiny_inline2_local(fn, arg0, arg1, &out)) {
                        WORK_SET(sp - 3, out);
                        sp -= 2;
                        pc++;
                        break;
                    }
                }
                if (!jit_call_primitive(vm, callee, &work[sp - 2], 2, &out)) TYPED_FAIL("qcall2-call");
                WORK_SET(sp - 3, out);
                sp -= 2;
                pc++;
                break;
            }
            case JOP_INVOKE: {
                int argc = (int)op->a;
                if (argc < 0 || sp < argc + 1) TYPED_FAIL("invoke-stack");
                int recv_idx = sp - 1 - argc;
                Value receiver = work[recv_idx];
                Value out = NIL_VAL;
                if (!jit_invoke_primitive(vm, receiver, op->constant, &work[recv_idx + 1], argc, &out)) {
                    TYPED_FAIL("invoke-primitive");
                }
                WORK_SET(recv_idx, out);
                sp = recv_idx + 1;
                pc++;
                break;
            }
            case JOP_LENGTH: {
                if (sp <= 0) TYPED_FAIL("length-stack");
                Value out = NIL_VAL;
                if (!jit_length_primitive(work[sp - 1], &out)) TYPED_FAIL("length-primitive");
                WORK_SET(sp - 1, out);
                pc++;
                break;
            }
            case JOP_SLICE: {
                if (sp < 4) TYPED_FAIL("slice-stack");
                Value out = NIL_VAL;
                if (!jit_slice_primitive(work[sp - 4], work[sp - 3], work[sp - 2], work[sp - 1], &out)) {
                    TYPED_FAIL("slice-primitive");
                }
                WORK_SET(sp - 4, out);
                sp -= 3;
                pc++;
                break;
            }
            case JOP_BUILD_STRING2:
            case JOP_BUILD_STRING3:
            case JOP_BUILD_STRING4:
            case JOP_BUILD_STRING5: {
                int n = 0;
                if (op->op == JOP_BUILD_STRING2) n = 2;
                else if (op->op == JOP_BUILD_STRING3) n = 3;
                else if (op->op == JOP_BUILD_STRING4) n = 4;
                else n = 5;
                if (sp < n) TYPED_FAIL("build-string-stack");
                Value out = NIL_VAL;
                if (!jit_build_string_primitive(vm, &work[sp - n], n, &out)) TYPED_FAIL("build-string");
                WORK_SET(sp - n, out);
                sp = sp - n + 1;
                pc++;
                break;
            }
            case JOP_GET_TABLE_NUM:
            case JOP_GET_TABLE: {
                if (sp < 2) TYPED_FAIL("get-table-stack");
                Value key = work[sp - 1];
                Value table = work[sp - 2];
                if (!IS_TABLE(table)) TYPED_FAIL("get-table-not-table");
                ObjTable* t = AS_TABLE(table);
                Value result = NIL_VAL;
                if (IS_NUMBER(key)) {
                    double num_key = AS_NUMBER(key);
                    int idx = (int)num_key;
                    if (num_key != (double)idx) TYPED_FAIL("get-table-num-nonint");
                    if (idx < 0) idx = t->table.array_max + idx + 1;
                    if (idx >= 1 && idx <= t->table.array_capacity) {
                        Value arr_val = t->table.array[idx - 1];
                        if (!IS_NIL(arr_val)) {
                            WORK_SET(sp - 2, arr_val);
                            sp--;
                            pc++;
                            break;
                        }
                    }
                    if (table_get_array(&t->table, idx, &result)) {
                        WORK_SET(sp - 2, result);
                        sp--;
                        pc++;
                        break;
                    }
                    if (op->op == JOP_GET_TABLE_NUM && t->no_metatable_fast) {
                        WORK_SET(sp - 2, NIL_VAL);
                        sp--;
                        pc++;
                        break;
                    }
                    TYPED_FAIL("get-table-num-miss");
                }
                if (op->op != JOP_GET_TABLE || !IS_STRING(key)) TYPED_FAIL("get-table-key");
                if (!t->no_metatable_fast) TYPED_FAIL("get-table-metatable");
                if (table_get(&t->table, AS_STRING(key), &result)) {
                    WORK_SET(sp - 2, result);
                } else {
                    WORK_SET(sp - 2, NIL_VAL);
                }
                sp--;
                pc++;
                break;
            }
            case JOP_SET_TABLE:
            case JOP_SET_TABLE_NUM: {
                if (sp < 3) TYPED_FAIL("set-table-stack");
                Value value = work[sp - 1];
                Value key = work[sp - 2];
                Value table = work[sp - 3];
                if (!IS_TABLE(table)) TYPED_FAIL("set-table-not-table");
                ObjTable* t = AS_TABLE(table);
                if (!t->no_metatable_fast) TYPED_FAIL("set-table-metatable");
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
                } else if (op->op == JOP_SET_TABLE && IS_STRING(key)) {
                    table_set(&t->table, AS_STRING(key), value);
                } else {
                    TYPED_FAIL("set-table-key");
                }
                WORK_SET(sp - 3, value);
                sp -= 2;
                pc++;
                break;
            }
            case JOP_QFOR_PREP_NUM: {
                if ((int)op->a >= frame_window || (int)op->b >= frame_window) TYPED_FAIL("qfor-prep-slots");
                double v = 0.0;
                double end = 0.0;
                WORK_GET_NUM(op->a, v);
                WORK_GET_NUM(op->b, end);
                if (v > end) {
                    if (op->target == JIT_TARGET_EXIT) {
                        return commit_loop_state(st, trace, frame, thread, ip, work, sp, frame_window, trace->after_offset);
                    }
                    pc = (int)op->target;
                } else {
                    pc = pc + 1;
                }
                break;
            }
            case JOP_QFOR_LOOP_NUM: {
                if ((int)op->a >= frame_window || (int)op->b >= frame_window) TYPED_FAIL("qfor-loop-slots");
                double cur = 0.0;
                double end = 0.0;
                WORK_GET_NUM(op->a, cur);
                WORK_GET_NUM(op->b, end);
                double next = cur + 1.0;
                WORK_SET_NUM(op->a, next);
                if (next <= end) {
                    pc = (int)op->target;
                    if (++loop_chunk_iters >= loop_chunk_budget) {
                        return commit_loop_state(st, trace, frame, thread, ip, work, sp, frame_window, trace->loop_start_offset);
                    }
                } else {
                    return commit_loop_state(st, trace, frame, thread, ip, work, sp, frame_window, trace->after_offset);
                }
                break;
            }
            case JOP_ADJUST_STACK:
                if ((int)op->a < 0 || (int)op->a > trace->max_stack) TYPED_FAIL("adjust-stack");
                sp = (int)op->a;
                pc++;
                break;
            default:
                TYPED_FAIL("unsupported-op");
        }
    }

    return 0;
#undef WORK_GET_NUM
#undef WORK_SET_NUM
#undef WORK_SET
#undef TYPED_FAIL
}

// Structural validation for a built native trace. Checks are derived only from
// build-time data (trace metadata + chunk size), so they run once at compile
// time instead of being re-validated on every loop entry.
static int jit_native_trace_validate(const JitLoopTrace* trace, const ObjFunction* function) {
    if (trace == NULL || function == NULL) return 0;
    int frame_window = (int)trace->max_slot + 1;
    if (frame_window <= 0 || frame_window > 1024) return 0;
    if (trace->native_trace_instr_count == 0 || trace->native_trace_instr_count > 256) return 0;
    if (trace->native_trace_guard_count > 256) return 0;
    if ((int)trace->native_value_var_slot >= frame_window ||
        (int)trace->native_value_end_slot >= frame_window) {
        return 0;
    }
    int chunk_count = function->chunk.count;
    for (int i = 0; i < (int)trace->native_trace_instr_count; i++) {
        const ToiJitNativeTraceInstr* in = &trace->native_trace_instrs[i];
        if (in->deopt_offset >= (uint32_t)chunk_count) return 0;
        if ((in->op == TOI_JIT_NATIVE_OP_JUMP ||
             in->op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE ||
             in->op == TOI_JIT_NATIVE_OP_JUMP_IF_TRUE) &&
            (int)in->a > (int)trace->native_trace_instr_count) {
            return 0;
        }
    }
    return 1;
}

static int compile_loop_trace(VM* vm, ObjFunction* function, JitLoopTrace* trace) {
    if (vm == NULL || function == NULL || trace == NULL) return 0;
    ToiJitState* st = jit_state(vm);
    if (st == NULL) return 0;
    Chunk* chunk = &function->chunk;
    if (chunk->code == NULL) return 0;
    set_loop_fail(trace, LOOP_FAIL_NONE, OP_NIL, 0);
    if (!is_numeric_for_loop_opcode(trace->opcode) && trace->opcode != OP_LOOP) return 0;
    int offset = (int)trace->opcode_offset;
    if (offset < 0 || offset >= chunk->count) return 0;
    if (chunk->code[offset] != trace->opcode) return 0;

    int jump = 0;
    int next = 0;
    if (is_numeric_for_loop_opcode(trace->opcode)) {
        if (offset + 4 >= chunk->count) return 0;
        jump = jit_ir_read_u16(chunk->code, offset + 3);
        next = offset + 5;
    } else if (trace->opcode == OP_LOOP) {
        if (offset + 2 >= chunk->count) return 0;
        jump = jit_ir_read_u16(chunk->code, offset + 1);
        next = offset + 3;
    } else {
        set_loop_fail(trace, LOOP_FAIL_UNSUPPORTED, trace->opcode, offset);
        return 0;
    }

    int loop_start = next - jump;
    if (loop_start < 0 || loop_start >= offset) {
        set_loop_fail(trace, LOOP_FAIL_JUMP, trace->opcode, offset);
        return 0;
    }
    trace->loop_start_offset = (uint32_t)loop_start;
    trace->after_offset = (uint32_t)next;

    if (!compile_qfor_loop_trace(function, trace, loop_start, next)) return 0;
    trace->runtime_disabled = 0;
    trace->native_slot_loop_fn = NULL;
    trace->native_value_loop_fn = NULL;
    trace->native_trace_fn = NULL;
    trace->native_value_instr_count = 0;
    trace->native_trace_ready = 0;
    trace->native_trace_exec_safe = 0;
    trace->native_trace_direct_core = 0;
    trace->native_trace_instr_count = 0;
    trace->native_trace_guard_count = 0;
    trace->native_trace_has_call = 0;
    trace->native_trace_has_invoke = 0;
    trace->native_trace_has_build_string = 0;
    trace->native_trace_has_mutating_table = 0;
    trace->native_trace_has_get_table_generic = 0;
    trace->native_trace_prefix_work = 0;
    trace->native_trace_code_mem = NULL;
    trace->native_trace_code_size = 0;
    trace->native_trace_exec_attempts = 0;
    trace->native_trace_exec_hits = 0;
    trace->native_trace_exit_hits = 0;
    trace->native_trace_continue_hits = 0;
    trace->native_trace_soft_bails = 0;
    trace->native_trace_guard_failures = 0;
    trace->native_trace_deopts = 0;
    trace->native_trace_sandbox_runs = 0;
    trace->native_trace_sandbox_disabled = 0;
    trace->native_trace_sandbox_ok = 0;
    trace->mixed_call_heavy = 0;
    trace->call_stall_runs = 0;
    trace->native_numeric_loop_fn = NULL;
    trace->native_qfor_empty_fn = NULL;
    trace->native_slot_loop_upvalue_const_count = 0;
    trace->native_code_mem = NULL;
    trace->native_code_size = 0;
    trace->numeric_fast_eligible = (uint8_t)trace_is_numeric_fast_eligible(trace);
    trace->typed_fast_eligible = (uint8_t)trace_is_typed_fast_eligible(trace);
    trace->typed_fast_disabled = 0;
    trace->typed_fast_attempts = 0;
    trace->typed_fast_hits = 0;
    if (trace->typed_fast_eligible) {
        if (!build_typed_trace_meta(trace)) {
            trace->typed_fast_eligible = 0;
        }
    } else {
        clear_typed_trace_meta(trace);
    }
    trace->value_fast_eligible = 0;
    trace->value_fast_disabled = 0;
    trace->value_fast_attempts = 0;
    trace->value_fast_hits = 0;
    if (st->backend_enabled && st->backend_host_supported && st->backend_kind != TOI_JIT_BACKEND_NONE) {
        char err_buf[128];
        int compiled_native = 0;

        ToiJitSlotInstr slot_instrs[192];
        int slot_instr_count = 0;
        int slot_const_count = 0;
        int slot_used_count = 0;
        int slot_written_count = 0;
        uint8_t slot_var = 0;
        uint8_t slot_end = 0;
        uint8_t slot_count = 0;
        uint8_t slot_fail_op = 255;
        st->backend_slot_lower_attempts++;
        if (build_slot_loop_ir(
                trace,
                slot_instrs,
                &slot_instr_count,
                &slot_var,
                &slot_end,
                &slot_count,
                trace->native_loop_consts,
                &slot_const_count,
                trace->native_slot_loop_used_slots,
                &slot_used_count,
                trace->native_slot_loop_written_slots,
                &slot_written_count,
                &slot_fail_op)) {
            st->backend_slot_lower_success++;
            st->backend_loop_compile_attempts++;
            if (toi_jit_backend_compile_slot_loop(
                    st->backend_kind,
                    slot_instrs,
                    slot_instr_count,
                    slot_var,
                    slot_end,
                    &trace->native_slot_loop_fn,
                    &trace->native_code_mem,
                    &trace->native_code_size,
                    err_buf,
                    sizeof(err_buf)) &&
                jit_accept_compiled_slot_loop(st, trace)) {
                trace->native_loop_var_slot = slot_var;
                trace->native_loop_end_slot = slot_end;
                trace->native_slot_loop_slot_count = slot_count;
                trace->native_loop_const_count = (uint8_t)slot_const_count;
                trace->native_slot_loop_used_count = (uint8_t)slot_used_count;
                trace->native_slot_loop_written_count = (uint8_t)slot_written_count;
                trace->native_slot_loop_upvalue_const_count = 0;
                st->backend_loop_compile_success++;
                compiled_native = 1;
            } else {
                st->backend_loop_compile_fail++;
            }
        } else {
            st->backend_slot_lower_fail++;
            st->backend_slot_lower_fail_opcode_counts[slot_fail_op]++;
        }

        // Prefer true native numeric kernels over value-loop runner fallback.
        // Value-loop backend is currently a C helper path on arm64, so
        // compiling it first can block a faster numeric-native lowering.
        if (!compiled_native && !trace->numeric_fast_eligible) {
            ToiJitValueInstr value_instrs[256];
            uint32_t value_deopt_offsets[256];
            int value_instr_count = 0;
            uint8_t value_var = 0;
            uint8_t value_end = 0;
            uint8_t value_fail_op = 255;
            if (build_value_loop_ir(
                    trace,
                    value_instrs,
                    value_deopt_offsets,
                    &value_instr_count,
                    &value_var,
                    &value_end,
                    &value_fail_op)) {
                memcpy(trace->native_value_instrs, value_instrs, (size_t)value_instr_count * sizeof(ToiJitValueInstr));
                trace->native_value_instr_count = (uint16_t)value_instr_count;
                trace->native_value_var_slot = value_var;
                trace->native_value_end_slot = value_end;
                trace->value_fast_eligible = (uint8_t)trace_is_value_fast_eligible(trace);

                uint8_t native_trace_fail_op = 255;
                int native_trace_instr_count = 0;
                int native_trace_guard_count = 0;
                st->native_trace_build_attempts++;
                if (toi_jit_native_trace_build_from_value_ir(
                        value_instrs,
                        value_instr_count,
                        value_deopt_offsets,
                        trace->native_trace_instrs,
                        &native_trace_instr_count,
                        trace->native_trace_guards,
                        &native_trace_guard_count,
                        &native_trace_fail_op)) {
                    trace->native_trace_ready = 1;
                    trace->native_trace_instr_count = (uint16_t)native_trace_instr_count;
                    trace->native_trace_guard_count = (uint16_t)native_trace_guard_count;
                    native_trace_analyze_shape(trace);
                    native_trace_seed_call_program_hints(vm, trace);
                    trace->native_trace_sandbox_ok = (uint8_t)jit_native_trace_validate(trace, function);
                    st->native_trace_build_success++;
                } else {
                    trace->native_trace_ready = 0;
                    trace->native_trace_instr_count = 0;
                    trace->native_trace_guard_count = 0;
                    trace->native_trace_sandbox_ok = 0;
                    trace->native_trace_has_call = 0;
                    trace->native_trace_has_invoke = 0;
                    trace->native_trace_has_build_string = 0;
                    trace->native_trace_has_mutating_table = 0;
                    trace->native_trace_has_get_table_generic = 0;
                    trace->native_trace_prefix_work = 0;
                    st->native_trace_build_fail++;
                }
                if (trace->native_trace_ready) {
                    // First try lowering canonicalized native-trace IR directly
                    // into a numeric slot program, then compile that to native code.
                    ToiJitSlotInstr native_trace_slot_instrs[256];
                    int native_trace_slot_instr_count = 0;
                    int native_trace_slot_const_count = 0;
                    int native_trace_slot_used_count = 0;
                    int native_trace_slot_written_count = 0;
                    int native_trace_slot_upvalue_count = 0;
                    int native_trace_slot_count = (int)trace->max_slot + 1;
                    uint8_t native_trace_slot_upvalue_const_idx[16];
                    uint8_t native_trace_slot_upvalue_idx[16];
                    uint8_t native_trace_slot_fail_op = native_trace_fail_op;
                    st->backend_slot_lower_attempts++;
                    if (build_slot_loop_from_native_trace_ir(
                            trace->native_trace_instrs,
                            native_trace_instr_count,
                            value_var,
                            value_end,
                            native_trace_slot_count,
                            native_trace_slot_instrs,
                            &native_trace_slot_instr_count,
                            trace->native_loop_consts,
                            &native_trace_slot_const_count,
                            trace->native_slot_loop_used_slots,
                            &native_trace_slot_used_count,
                            trace->native_slot_loop_written_slots,
                            &native_trace_slot_written_count,
                            native_trace_slot_upvalue_const_idx,
                            native_trace_slot_upvalue_idx,
                            &native_trace_slot_upvalue_count,
                            &native_trace_slot_fail_op)) {
                        st->backend_slot_lower_success++;
                        st->backend_loop_compile_attempts++;
                        if (toi_jit_backend_compile_slot_loop(
                                st->backend_kind,
                                native_trace_slot_instrs,
                                native_trace_slot_instr_count,
                                value_var,
                                value_end,
                                &trace->native_slot_loop_fn,
                                &trace->native_code_mem,
                                &trace->native_code_size,
                                err_buf,
                                sizeof(err_buf)) &&
                            jit_accept_compiled_slot_loop(st, trace)) {
                            trace->native_loop_var_slot = value_var;
                            trace->native_loop_end_slot = value_end;
                            trace->native_slot_loop_slot_count = (uint8_t)native_trace_slot_count;
                            trace->native_loop_const_count = (uint8_t)native_trace_slot_const_count;
                            trace->native_slot_loop_used_count = (uint8_t)native_trace_slot_used_count;
                            trace->native_slot_loop_written_count = (uint8_t)native_trace_slot_written_count;
                            trace->native_slot_loop_upvalue_const_count = (uint8_t)native_trace_slot_upvalue_count;
                            for (int up_i = 0; up_i < native_trace_slot_upvalue_count && up_i < 16; up_i++) {
                                trace->native_slot_loop_upvalue_const_idx[up_i] = native_trace_slot_upvalue_const_idx[up_i];
                                trace->native_slot_loop_upvalue_idx[up_i] = native_trace_slot_upvalue_idx[up_i];
                            }
                            st->backend_loop_compile_success++;
                            compiled_native = 1;
                        } else {
                            st->backend_loop_compile_fail++;
                        }
                    } else {
                        st->backend_slot_lower_fail++;
                        st->backend_slot_lower_fail_opcode_counts[native_trace_slot_fail_op]++;
                    }
                }
                if (trace->native_trace_ready && !compiled_native) {
                    uint8_t native_trace_direct_core = 0;
                    st->native_trace_compile_attempts++;
                    if (toi_jit_backend_compile_native_trace(
                            st->backend_kind,
                            trace->native_trace_instrs,
                            (int)trace->native_trace_instr_count,
                            trace->native_trace_guards,
                            (int)trace->native_trace_guard_count,
                            value_var,
                            value_end,
                            &native_trace_direct_core,
                            &trace->native_trace_fn,
                            &trace->native_trace_code_mem,
                            &trace->native_trace_code_size,
                            err_buf,
                            sizeof(err_buf))) {
                        trace->native_trace_direct_core = native_trace_direct_core;
                        if (native_trace_direct_core) st->native_trace_compile_direct_core++;
                        uint8_t native_trace_reject_op = 255;
                        trace->native_trace_exec_safe = (uint8_t)native_trace_is_exec_safe(trace, &native_trace_reject_op);
                        if (trace->native_trace_exec_safe) {
                            st->native_trace_exec_safe_traces++;
                        } else {
                            st->native_trace_exec_unsafe_traces++;
                            st->native_trace_unsafe_opcode_counts[native_trace_reject_op]++;
                        }
                        st->native_trace_compile_success++;
                    } else {
                        trace->native_trace_fn = NULL;
                        trace->native_trace_code_mem = NULL;
                        trace->native_trace_code_size = 0;
                        trace->native_trace_exec_safe = 0;
                        trace->native_trace_direct_core = 0;
                        st->native_trace_compile_fail++;
                    }
                }
                // Second chance lowering: value IR -> native slot IR.
                // This converts a subset of value traces into real native code
                // instead of relying on the C value-loop runner.
                ToiJitSlotInstr value_slot_instrs[256];
                int value_slot_instr_count = 0;
                int value_slot_const_count = 0;
                int value_slot_used_count = 0;
                int value_slot_written_count = 0;
                int value_slot_upvalue_count = 0;
                uint8_t value_slot_upvalue_const_idx[16];
                uint8_t value_slot_upvalue_idx[16];
                uint8_t value_slot_fail_op = value_fail_op;
                int value_slot_count = (int)trace->max_slot + 1;
                st->backend_slot_lower_attempts++;
                if (build_slot_loop_from_value_ir(
                        value_instrs,
                        value_instr_count,
                        value_var,
                        value_end,
                        value_slot_count,
                        value_slot_instrs,
                        &value_slot_instr_count,
                        trace->native_loop_consts,
                        &value_slot_const_count,
                        trace->native_slot_loop_used_slots,
                        &value_slot_used_count,
                        trace->native_slot_loop_written_slots,
                        &value_slot_written_count,
                        value_slot_upvalue_const_idx,
                        value_slot_upvalue_idx,
                        &value_slot_upvalue_count,
                        &value_slot_fail_op)) {
                    st->backend_slot_lower_success++;
                    st->backend_loop_compile_attempts++;
                    if (toi_jit_backend_compile_slot_loop(
                            st->backend_kind,
                            value_slot_instrs,
                            value_slot_instr_count,
                            value_var,
                            value_end,
                            &trace->native_slot_loop_fn,
                            &trace->native_code_mem,
                            &trace->native_code_size,
                            err_buf,
                            sizeof(err_buf)) &&
                        jit_accept_compiled_slot_loop(st, trace)) {
                        trace->native_loop_var_slot = value_var;
                        trace->native_loop_end_slot = value_end;
                        trace->native_slot_loop_slot_count = (uint8_t)value_slot_count;
                        trace->native_loop_const_count = (uint8_t)value_slot_const_count;
                        trace->native_slot_loop_used_count = (uint8_t)value_slot_used_count;
                        trace->native_slot_loop_written_count = (uint8_t)value_slot_written_count;
                        trace->native_slot_loop_upvalue_const_count = (uint8_t)value_slot_upvalue_count;
                        for (int up_i = 0; up_i < value_slot_upvalue_count && up_i < 16; up_i++) {
                            trace->native_slot_loop_upvalue_const_idx[up_i] = value_slot_upvalue_const_idx[up_i];
                            trace->native_slot_loop_upvalue_idx[up_i] = value_slot_upvalue_idx[up_i];
                        }
                        st->backend_loop_compile_success++;
                        compiled_native = 1;
                    } else {
                        st->backend_loop_compile_fail++;
                    }
                } else {
                    st->backend_slot_lower_fail++;
                    st->backend_slot_lower_fail_opcode_counts[value_slot_fail_op]++;
                }
                if (compiled_native) {
                    goto skip_value_loop_compile;
                }
                st->backend_value_loop_compile_attempts++;
                if (toi_jit_backend_compile_value_loop(
                        st->backend_kind,
                        value_instrs,
                        value_instr_count,
                        value_var,
                        value_end,
                        &trace->native_value_loop_fn,
                        &trace->native_code_mem,
                        &trace->native_code_size,
                        err_buf,
                        sizeof(err_buf))) {
                    st->backend_value_loop_compile_success++;
                    compiled_native = 1;
                } else {
                    st->backend_value_loop_compile_fail++;
                }
            }
skip_value_loop_compile:;
        }

        if (!compiled_native) {
            ToiJitNumInstr num_instrs[128];
            int num_instr_count = 0;
            int num_const_count = 0;
            uint8_t acc_slot = 0;
            uint8_t var_slot = 0;
            uint8_t end_slot = 0;
            if (build_numeric_loop_ir(
                    trace,
                    num_instrs,
                    &num_instr_count,
                    &acc_slot,
                    &var_slot,
                    &end_slot,
                    trace->native_loop_consts,
                    &num_const_count)) {
                st->backend_loop_compile_attempts++;
                if (toi_jit_backend_compile_numeric_loop(
                        st->backend_kind,
                        num_instrs,
                        num_instr_count,
                        &trace->native_numeric_loop_fn,
                        &trace->native_code_mem,
                        &trace->native_code_size,
                        err_buf,
                        sizeof(err_buf))) {
                    trace->native_loop_acc_slot = acc_slot;
                    trace->native_loop_var_slot = var_slot;
                    trace->native_loop_end_slot = end_slot;
                    trace->native_loop_const_count = (uint8_t)num_const_count;
                    st->backend_loop_compile_success++;
                    compiled_native = 1;
                } else {
                    st->backend_loop_compile_fail++;
                }
            }
        }

        if (!compiled_native && trace_is_empty_qfor(trace)) {
            st->backend_loop_compile_attempts++;
            if (toi_jit_backend_compile_qfor_empty(
                    st->backend_kind,
                    &trace->native_qfor_empty_fn,
                    &trace->native_code_mem,
                    &trace->native_code_size,
                    err_buf,
                    sizeof(err_buf))) {
                st->backend_loop_compile_success++;
            } else {
                st->backend_loop_compile_fail++;
            }
        }
    }
    trace->runtime_probe_limit = trace_runtime_probe_limit(trace);
    if (trace->runtime_probe_limit == 0) {
        jit_retire_loop_trace(st, trace);
    }
    // Loops with dynamic call sites are not executed by the loop JIT: the
    // per-site call cache is not reliable when the callee changes between
    // iterations, and a wrong callee silently corrupts results. The VM's
    // quickened call path (with its own inline cache) handles these loops.
    // TODO(jit-call-path): fix the trace call cache, then flip the default.
    if (!jit_call_traces_enabled()) {
        for (int i = 0; i < trace->op_count; i++) {
            JitOpCode op = trace->ops[i].op;
            if (op == JOP_CALL || op == JOP_QCALL2) {
                trace->mixed_call_heavy = 1;
                jit_retire_loop_trace(st, trace);
                break;
            }
        }
    }
    trace->compiled = 1;
    if (!trace->runtime_disabled) st->loop_live_count++;
    return 1;
}

static Value numeric_binop(JitOpCode op, double a, double b) {
    if (op == JOP_ADD || op == JOP_IADD || op == JOP_FADD || op == JOP_QFADD) return NUMBER_VAL(a + b);
    if (op == JOP_SUBTRACT || op == JOP_ISUB || op == JOP_FSUB || op == JOP_QFSUB) return NUMBER_VAL(a - b);
    if (op == JOP_MULTIPLY || op == JOP_IMUL || op == JOP_FMUL || op == JOP_QFMUL) return NUMBER_VAL(a * b);
    if (op == JOP_DIVIDE || op == JOP_IDIV || op == JOP_FDIV || op == JOP_QFDIV) return NUMBER_VAL(a / b);
    if (op == JOP_MODULO || op == JOP_IMOD || op == JOP_FMOD || op == JOP_QFMOD) {
        int64_t ia, ib;
        if (to_int64_local(a, &ia) && to_int64_local(b, &ib) && ib != 0) {
            return NUMBER_VAL((double)(ia % ib));
        }
        return NUMBER_VAL(fmod(a, b));
    }
    return NIL_VAL;
}

int jit_eval_tiny_inline2_local(ObjFunction* function, Value arg0, Value arg1, Value* out) {
    if (function == NULL || out == NULL) return 0;
    switch ((TinyInlineKind)function->tiny_inline_kind) {
        case TINY_INLINE_IADD2:
        case TINY_INLINE_FADD2:
        case TINY_INLINE_ADD2:
            if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
            *out = NUMBER_VAL(AS_NUMBER(arg0) + AS_NUMBER(arg1));
            return 1;
        case TINY_INLINE_ISUB2:
        case TINY_INLINE_FSUB2:
        case TINY_INLINE_SUB2:
            if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
            *out = NUMBER_VAL(AS_NUMBER(arg0) - AS_NUMBER(arg1));
            return 1;
        case TINY_INLINE_IMUL2:
        case TINY_INLINE_FMUL2:
        case TINY_INLINE_MUL2:
            if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
            *out = NUMBER_VAL(AS_NUMBER(arg0) * AS_NUMBER(arg1));
            return 1;
        case TINY_INLINE_IDIV2:
        case TINY_INLINE_FDIV2:
        case TINY_INLINE_DIV2:
            if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
            *out = NUMBER_VAL(AS_NUMBER(arg0) / AS_NUMBER(arg1));
            return 1;
        case TINY_INLINE_GT2:
            if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
            *out = BOOL_VAL(AS_NUMBER(arg0) > AS_NUMBER(arg1));
            return 1;
        case TINY_INLINE_LT2:
            if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
            *out = BOOL_VAL(AS_NUMBER(arg0) < AS_NUMBER(arg1));
            return 1;
        case TINY_INLINE_EQ2:
            if (arg0.type != arg1.type) *out = BOOL_VAL(0);
            else if (IS_NIL(arg0)) *out = BOOL_VAL(1);
            else if (IS_BOOL(arg0)) *out = BOOL_VAL(AS_BOOL(arg0) == AS_BOOL(arg1));
            else if (IS_NUMBER(arg0)) *out = BOOL_VAL(AS_NUMBER(arg0) == AS_NUMBER(arg1));
            else if (IS_OBJ(arg0)) *out = BOOL_VAL(AS_OBJ(arg0) == AS_OBJ(arg1));
            else *out = BOOL_VAL(0);
            return 1;
        case TINY_INLINE_RET0_2:
            *out = arg0;
            return 1;
        case TINY_INLINE_RET1_2:
            *out = arg1;
            return 1;
        case TINY_INLINE_LEN_ADD2:
            if (!IS_NUMBER(arg0) || !IS_STRING(arg1)) return 0;
            *out = NUMBER_VAL(AS_NUMBER(arg0) + (double)AS_STRING(arg1)->length);
            return 1;
        case TINY_INLINE_LEN_ARG0_2:
            if (!IS_STRING(arg0)) return 0;
            *out = NUMBER_VAL((double)AS_STRING(arg0)->length);
            return 1;
        case TINY_INLINE_LEN_ARG1_2:
            if (!IS_STRING(arg1)) return 0;
            *out = NUMBER_VAL((double)AS_STRING(arg1)->length);
            return 1;
        default:
            return 0;
    }
}

// Evaluating a small, straight-line callee body in place is much cheaper than
// re-entering execute_program(): for a loop that calls a function every
// iteration, the nested-interpreter prologue (frame setup, stack probe, argument
// copies, program checks) costs more than the callee body itself. Only opcodes
// whose semantics are shared with execute_program() are modelled here; anything
// else returns 0 so the caller falls back to the normal call path.
static int jit_inline_call_program(
    ObjFunction* fn,
    JitProgram* program,
    const Value* args,
    int argc,
    Value* out) {
    if (fn == NULL || program == NULL || args == NULL || out == NULL) return 0;
    if (program->ops == NULL || program->op_count <= 0 || program->op_count > JIT_INLINE_MAX_OPS) return 0;
    if (program->max_slot < 0 || program->max_slot >= JIT_INLINE_MAX_SLOTS) return 0;
    if (program->max_stack <= 0 || program->max_stack > JIT_INLINE_MAX_STACK) return 0;
    if (argc < 0 || argc + 1 > JIT_INLINE_MAX_SLOTS) return 0;
    if (fn->param_types_count > 0 && fn->param_types != NULL) {
        int check_count = (fn->param_types_count < fn->arity) ? fn->param_types_count : fn->arity;
        if (check_count > argc) return 0;
        for (int i = 0; i < check_count; i++) {
            if (!jit_value_matches_type(args[i], fn->param_types[i])) return 0;
        }
    }

    // A two-argument shim compiles to a single JOP_TINY_INLINE op; evaluate it
    // directly instead of walking a one-op program.
    if (program->op_count == 1 && program->ops[0].op == JOP_TINY_INLINE) {
        if (argc != 2) return 0;
        if (fn->is_variadic || fn->defaults_count != 0) return 0;
        return jit_eval_tiny_inline2_local(fn, args[0], args[1], out);
    }

    Value slots[JIT_INLINE_MAX_SLOTS];
    Value stack[JIT_INLINE_MAX_STACK];
    for (int i = 0; i <= program->max_slot; i++) slots[i] = NIL_VAL;
    for (int i = 0; i < argc; i++) slots[i + 1] = args[i];
    int sp = argc + 1;
    for (int i = 0; i < sp; i++) stack[i] = slots[i];

    for (int pc = 0; pc < program->op_count; pc++) {
        const JitOp* op = &program->ops[pc];
        switch (op->op) {
            case JOP_GET_LOCAL:
                if ((int)op->a > program->max_slot || sp >= program->max_stack) return 0;
                stack[sp++] = slots[op->a];
                break;
            case JOP_GET2_LOCAL:
                if ((int)op->a > program->max_slot || (int)op->b > program->max_slot) return 0;
                if (sp + 1 >= program->max_stack) return 0;
                stack[sp++] = slots[op->a];
                stack[sp++] = slots[op->b];
                break;
            case JOP_CONSTANT:
                if (sp >= program->max_stack) return 0;
                stack[sp++] = op->constant;
                break;
            case JOP_NIL:
            case JOP_TRUE:
            case JOP_FALSE:
                if (sp >= program->max_stack) return 0;
                stack[sp++] = (op->op == JOP_NIL) ? NIL_VAL : ((op->op == JOP_TRUE) ? BOOL_VAL(1) : BOOL_VAL(0));
                break;
            case JOP_SET_LOCAL:
            case JOP_SET_LOCAL_POP:
                if ((int)op->a > program->max_slot || sp <= 0) return 0;
                slots[op->a] = stack[sp - 1];
                if (op->op == JOP_SET_LOCAL_POP) sp--;
                break;
            case JOP_POP:
                if (sp <= 0) return 0;
                sp--;
                break;
            case JOP_ADD:
            case JOP_ADD_INPLACE:
            case JOP_SUBTRACT:
            case JOP_MULTIPLY:
            case JOP_DIVIDE:
            case JOP_MODULO:
            case JOP_IADD:
            case JOP_ISUB:
            case JOP_IMUL:
            case JOP_IDIV:
            case JOP_IMOD:
            case JOP_FADD:
            case JOP_FSUB:
            case JOP_FMUL:
            case JOP_FDIV:
            case JOP_FMOD:
            case JOP_QFADD:
            case JOP_QFSUB:
            case JOP_QFMUL:
            case JOP_QFDIV:
            case JOP_QFMOD:
                if (sp < 2) return 0;
                if (!IS_NUMBER(stack[sp - 1]) || !IS_NUMBER(stack[sp - 2])) return 0;
                stack[sp - 2] = numeric_binop(op->op, AS_NUMBER(stack[sp - 2]), AS_NUMBER(stack[sp - 1]));
                sp--;
                break;
            case JOP_NEGATE:
                if (sp <= 0 || !IS_NUMBER(stack[sp - 1])) return 0;
                stack[sp - 1] = NUMBER_VAL(-AS_NUMBER(stack[sp - 1]));
                break;
            case JOP_NOT:
                if (sp <= 0) return 0;
                stack[sp - 1] = BOOL_VAL(is_falsey_jit(stack[sp - 1]));
                break;
            case JOP_EQUAL:
                if (sp < 2) return 0;
                stack[sp - 2] = BOOL_VAL(jit_values_equal(stack[sp - 2], stack[sp - 1]));
                sp--;
                break;
            case JOP_QGREATER_NUM:
            case JOP_QLESS_NUM:
                if (sp < 2) return 0;
                if (!IS_NUMBER(stack[sp - 1]) || !IS_NUMBER(stack[sp - 2])) return 0;
                stack[sp - 2] = BOOL_VAL((op->op == JOP_QGREATER_NUM)
                                             ? (AS_NUMBER(stack[sp - 2]) > AS_NUMBER(stack[sp - 1]))
                                             : (AS_NUMBER(stack[sp - 2]) < AS_NUMBER(stack[sp - 1])));
                sp--;
                break;
            case JOP_LENGTH: {
                if (sp <= 0) return 0;
                Value len = NIL_VAL;
                if (!jit_length_primitive(stack[sp - 1], &len)) return 0;
                stack[sp - 1] = len;
                break;
            }
            case JOP_RETURN:
                if (sp <= 0) return 0;
                *out = stack[sp - 1];
                return 1;
            default:
                return 0;
        }
    }
    return 0;
}

// Reuses the trace call cache to find this call site's callee program and
// evaluates it in place. Misses fall through to the normal call path, which also
// populates the cache for the next iteration.
static int jit_try_inline_trace_call(
    JitLoopTrace* trace,
    int pc,
    Value callee,
    const Value* args,
    int argc,
    Value* out) {
    if (trace == NULL || args == NULL || out == NULL) return 0;
    if (pc < 0 || pc >= trace->op_count) return 0;
    if (!IS_CLOSURE(callee)) return 0;
    if (trace->call_ic_closures == NULL || trace->call_ic_programs == NULL) return 0;
    ObjClosure* closure = AS_CLOSURE(callee);
    if (closure == NULL || closure->function == NULL) return 0;
    if (closure->function->arity != argc) return 0;

    JitProgram* program = NULL;
    if (trace->call_ic_closures[pc] == closure) {
        program = trace->call_ic_programs[pc];
    } else if (trace->call_ic2_closures != NULL && trace->call_ic2_programs != NULL &&
               trace->call_ic2_closures[pc] == closure) {
        program = trace->call_ic2_programs[pc];
    } else if (trace->call_ic3_closures != NULL && trace->call_ic3_programs != NULL &&
               trace->call_ic3_closures[pc] == closure) {
        program = trace->call_ic3_programs[pc];
    } else if (trace->call_ic4_closures != NULL && trace->call_ic4_programs != NULL &&
               trace->call_ic4_closures[pc] == closure) {
        program = trace->call_ic4_programs[pc];
    } else if (trace->call_ic5_closures != NULL && trace->call_ic5_programs != NULL &&
               trace->call_ic5_closures[pc] == closure) {
        program = trace->call_ic5_programs[pc];
    } else {
        return 0;
    }
    if (program == NULL || program->function != closure->function) return 0;
    return jit_inline_call_program(closure->function, program, args, argc, out);
}

int execute_program(
    VM* vm,
    ObjClosure* closure,
    int arg_count,
    JitProgram* program,
    Value* args,
    Value* out) {
    if (vm == NULL || closure == NULL || program == NULL) return 0;
    ObjFunction* function = closure->function;
    if (function == NULL || function != program->function) return 0;
    if (arg_count != function->arity) return 0;
    int has_direct_args = (args != NULL);
    if (has_direct_args && out == NULL) return 0;

    ObjThread* thread = vm_current_thread(vm);
    if (!has_direct_args && thread == NULL) return 0;
    if (program->op_count == 1 && program->ops[0].op == JOP_TINY_INLINE) {
        Value arg0 = has_direct_args ? args[0] : thread->stack_top[-2];
        Value arg1 = has_direct_args ? args[1] : thread->stack_top[-1];
        Value tiny_out = NIL_VAL;
        switch (program->ops[0].a) {
            case TINY_INLINE_IADD2:
            case TINY_INLINE_FADD2:
            case TINY_INLINE_ADD2:
                if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
                tiny_out = NUMBER_VAL(AS_NUMBER(arg0) + AS_NUMBER(arg1));
                break;
            case TINY_INLINE_ISUB2:
            case TINY_INLINE_FSUB2:
            case TINY_INLINE_SUB2:
                if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
                tiny_out = NUMBER_VAL(AS_NUMBER(arg0) - AS_NUMBER(arg1));
                break;
            case TINY_INLINE_IMUL2:
            case TINY_INLINE_FMUL2:
            case TINY_INLINE_MUL2:
                if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
                tiny_out = NUMBER_VAL(AS_NUMBER(arg0) * AS_NUMBER(arg1));
                break;
            case TINY_INLINE_IDIV2:
            case TINY_INLINE_FDIV2:
            case TINY_INLINE_DIV2:
                if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
                tiny_out = NUMBER_VAL(AS_NUMBER(arg0) / AS_NUMBER(arg1));
                break;
            case TINY_INLINE_GT2:
                if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
                tiny_out = BOOL_VAL(AS_NUMBER(arg0) > AS_NUMBER(arg1));
                break;
            case TINY_INLINE_LT2:
                if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
                tiny_out = BOOL_VAL(AS_NUMBER(arg0) < AS_NUMBER(arg1));
                break;
            case TINY_INLINE_EQ2:
                if (arg0.type != arg1.type) tiny_out = BOOL_VAL(0);
                else if (IS_NIL(arg0)) tiny_out = BOOL_VAL(1);
                else if (IS_BOOL(arg0)) tiny_out = BOOL_VAL(AS_BOOL(arg0) == AS_BOOL(arg1));
                else if (IS_NUMBER(arg0)) tiny_out = BOOL_VAL(AS_NUMBER(arg0) == AS_NUMBER(arg1));
                else if (IS_OBJ(arg0)) tiny_out = BOOL_VAL(AS_OBJ(arg0) == AS_OBJ(arg1));
                else tiny_out = BOOL_VAL(0);
                break;
            case TINY_INLINE_RET0_2:
                tiny_out = arg0;
                break;
            case TINY_INLINE_RET1_2:
                tiny_out = arg1;
                break;
            case TINY_INLINE_LEN_ADD2:
                if (!IS_NUMBER(arg0) || !IS_STRING(arg1)) return 0;
                tiny_out = NUMBER_VAL(AS_NUMBER(arg0) + (double)AS_STRING(arg1)->length);
                break;
            case TINY_INLINE_LEN_ARG0_2:
                if (!IS_STRING(arg0)) return 0;
                tiny_out = NUMBER_VAL((double)AS_STRING(arg0)->length);
                break;
            case TINY_INLINE_LEN_ARG1_2:
                if (!IS_STRING(arg1)) return 0;
                tiny_out = NUMBER_VAL((double)AS_STRING(arg1)->length);
                break;
            default:
                return 0;
        }
        if (has_direct_args) {
            *out = tiny_out;
        } else {
            thread->stack_top -= (arg_count + 1);
            *thread->stack_top++ = tiny_out;
        }
        return 1;
    }
    if (program->max_slot >= 1024 || program->max_stack >= 2048) return 0;

    int locals_cap = program->max_slot + 1;
    int stack_cap = program->max_stack;
    if (stack_cap < arg_count + 1) stack_cap = arg_count + 1;
    if (locals_cap <= 0 || stack_cap <= 0) return 0;
    Value locals[locals_cap];
    Value stack[stack_cap];
    for (int i = 0; i <= program->max_slot; i++) locals[i] = NIL_VAL;
    locals[0] = OBJ_VAL(closure);
    for (int i = 0; i < arg_count; i++) {
        locals[i + 1] = has_direct_args ? args[i] : thread->stack_top[-arg_count + i];
    }

    int sp = arg_count + 1;
    for (int i = 0; i < sp; i++) stack[i] = locals[i];

    uint16_t pc = 0;
    JitOp* op = NULL;
#if defined(__GNUC__) || defined(__clang__)
    static void* hot_dispatch_table[JOP_RETURN + 1] = {
        [JOP_CONSTANT] = &&op_hot_constant,
        [JOP_NIL] = &&op_hot_nil,
        [JOP_TRUE] = &&op_hot_true,
        [JOP_FALSE] = &&op_hot_false,
        [JOP_POP] = &&op_hot_pop,
        [JOP_GET_LOCAL] = &&op_hot_get_local,
        [JOP_GET2_LOCAL] = &&op_hot_get2_local,
        [JOP_SET_LOCAL] = &&op_hot_set_local,
        [JOP_SET_LOCAL_POP] = &&op_hot_set_local_pop,
        [JOP_INC_LOCAL] = &&op_hot_local_arith,
        [JOP_SUB_LOCAL_CONST] = &&op_hot_local_arith,
        [JOP_MUL_LOCAL_CONST] = &&op_hot_local_arith,
        [JOP_DIV_LOCAL_CONST] = &&op_hot_local_arith,
        [JOP_MOD_LOCAL_CONST] = &&op_hot_local_arith,
        [JOP_ADD_CONST] = &&op_hot_const_arith,
        [JOP_SUB_CONST] = &&op_hot_const_arith,
        [JOP_MUL_CONST] = &&op_hot_const_arith,
        [JOP_DIV_CONST] = &&op_hot_const_arith,
        [JOP_MOD_CONST] = &&op_hot_const_arith,
        [JOP_ADD] = &&op_hot_bin_arith,
        [JOP_ADD_INPLACE] = &&op_hot_bin_arith,
        [JOP_SUBTRACT] = &&op_hot_bin_arith,
        [JOP_MULTIPLY] = &&op_hot_bin_arith,
        [JOP_DIVIDE] = &&op_hot_bin_arith,
        [JOP_MODULO] = &&op_hot_bin_arith,
        [JOP_IADD] = &&op_hot_bin_arith,
        [JOP_ISUB] = &&op_hot_bin_arith,
        [JOP_IMUL] = &&op_hot_bin_arith,
        [JOP_IDIV] = &&op_hot_bin_arith,
        [JOP_IMOD] = &&op_hot_bin_arith,
        [JOP_FADD] = &&op_hot_bin_arith,
        [JOP_FSUB] = &&op_hot_bin_arith,
        [JOP_FMUL] = &&op_hot_bin_arith,
        [JOP_FDIV] = &&op_hot_bin_arith,
        [JOP_FMOD] = &&op_hot_bin_arith,
        [JOP_QFADD] = &&op_hot_bin_arith,
        [JOP_QFSUB] = &&op_hot_bin_arith,
        [JOP_QFMUL] = &&op_hot_bin_arith,
        [JOP_QFDIV] = &&op_hot_bin_arith,
        [JOP_QFMOD] = &&op_hot_bin_arith,
        [JOP_NEGATE] = &&op_hot_negate,
        [JOP_NOT] = &&op_hot_not,
        [JOP_EQUAL] = &&op_hot_equal,
        [JOP_QGREATER_NUM] = &&op_hot_cmp_num,
        [JOP_QLESS_NUM] = &&op_hot_cmp_num,
        [JOP_JUMP] = &&op_hot_jump,
        [JOP_JUMP_IF_FALSE] = &&op_hot_jump_if_false,
        [JOP_JUMP_IF_TRUE] = &&op_hot_jump_if_true,
        [JOP_QFOR_PREP_NUM] = &&op_hot_qfor_prep,
        [JOP_QFOR_LOOP_NUM] = &&op_hot_qfor_loop,
        [JOP_ADJUST_STACK] = &&op_hot_adjust_stack,
        [JOP_RETURN] = &&op_hot_return
    };
#define HOT_DISPATCH()                                                                \
    do {                                                                              \
        if (pc >= (uint16_t)program->op_count) return 0;                             \
        op = &program->ops[pc];                                                       \
        void* hot_target = hot_dispatch_table[op->op];                                \
        if (hot_target != NULL) goto *hot_target;                                     \
        goto op_cold;                                                                  \
    } while (0)
    HOT_DISPATCH();
op_hot_constant:
    if (sp >= program->max_stack) return 0;
    stack[sp++] = op->constant;
    pc++;
    HOT_DISPATCH();
op_hot_nil:
    if (sp >= program->max_stack) return 0;
    stack[sp++] = NIL_VAL;
    pc++;
    HOT_DISPATCH();
op_hot_true:
    if (sp >= program->max_stack) return 0;
    stack[sp++] = BOOL_VAL(1);
    pc++;
    HOT_DISPATCH();
op_hot_false:
    if (sp >= program->max_stack) return 0;
    stack[sp++] = BOOL_VAL(0);
    pc++;
    HOT_DISPATCH();
op_hot_pop:
    if (sp <= 0) return 0;
    sp--;
    pc++;
    HOT_DISPATCH();
op_hot_get_local:
    if ((int)op->a > program->max_slot || sp >= program->max_stack) return 0;
    stack[sp++] = locals[op->a];
    pc++;
    HOT_DISPATCH();
op_hot_get2_local:
    if ((int)op->a > program->max_slot || (int)op->b > program->max_slot || sp + 1 >= program->max_stack) {
        return 0;
    }
    stack[sp++] = locals[op->a];
    stack[sp++] = locals[op->b];
    pc++;
    HOT_DISPATCH();
op_hot_set_local:
    if ((int)op->a > program->max_slot || sp <= 0) return 0;
    locals[op->a] = stack[sp - 1];
    pc++;
    HOT_DISPATCH();
op_hot_set_local_pop:
    if ((int)op->a > program->max_slot || sp <= 0) return 0;
    locals[op->a] = stack[sp - 1];
    sp--;
    pc++;
    HOT_DISPATCH();
op_hot_local_arith: {
    if ((int)op->a > program->max_slot || sp >= program->max_stack) return 0;
    Value a = locals[op->a];
    Value b = op->constant;
    if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
    double ad = AS_NUMBER(a);
    double bd = AS_NUMBER(b);
    Value outv = NIL_VAL;
    if (op->op == JOP_INC_LOCAL) outv = NUMBER_VAL(ad + bd);
    else if (op->op == JOP_SUB_LOCAL_CONST) outv = NUMBER_VAL(ad - bd);
    else if (op->op == JOP_MUL_LOCAL_CONST) outv = NUMBER_VAL(ad * bd);
    else if (op->op == JOP_DIV_LOCAL_CONST) outv = NUMBER_VAL(ad / bd);
    else outv = numeric_binop(JOP_MODULO, ad, bd);
    locals[op->a] = outv;
    stack[sp++] = outv;
    pc++;
    HOT_DISPATCH();
}
op_hot_const_arith: {
    if (sp <= 0) return 0;
    Value a = stack[sp - 1];
    Value b = op->constant;
    if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
    double ad = AS_NUMBER(a);
    double bd = AS_NUMBER(b);
    if (op->op == JOP_ADD_CONST) stack[sp - 1] = NUMBER_VAL(ad + bd);
    else if (op->op == JOP_SUB_CONST) stack[sp - 1] = NUMBER_VAL(ad - bd);
    else if (op->op == JOP_MUL_CONST) stack[sp - 1] = NUMBER_VAL(ad * bd);
    else if (op->op == JOP_DIV_CONST) stack[sp - 1] = NUMBER_VAL(ad / bd);
    else stack[sp - 1] = numeric_binop(JOP_MODULO, ad, bd);
    pc++;
    HOT_DISPATCH();
}
op_hot_bin_arith: {
    if (sp < 2) return 0;
    Value b = stack[sp - 1];
    Value a = stack[sp - 2];
    if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
    stack[sp - 2] = numeric_binop(op->op, AS_NUMBER(a), AS_NUMBER(b));
    sp--;
    pc++;
    HOT_DISPATCH();
}
op_hot_negate:
    if (sp <= 0 || !IS_NUMBER(stack[sp - 1])) return 0;
    stack[sp - 1] = NUMBER_VAL(-AS_NUMBER(stack[sp - 1]));
    pc++;
    HOT_DISPATCH();
op_hot_not:
    if (sp <= 0) return 0;
    stack[sp - 1] = BOOL_VAL(is_falsey_jit(stack[sp - 1]));
    pc++;
    HOT_DISPATCH();
op_hot_equal: {
    if (sp < 2) return 0;
    Value b = stack[sp - 1];
    Value a = stack[sp - 2];
    int eq = jit_values_equal(a, b);
    stack[sp - 2] = BOOL_VAL(eq);
    sp--;
    pc++;
    HOT_DISPATCH();
}
op_hot_cmp_num: {
    if (sp < 2) return 0;
    Value b = stack[sp - 1];
    Value a = stack[sp - 2];
    if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
    int cmp = (op->op == JOP_QGREATER_NUM) ? (AS_NUMBER(a) > AS_NUMBER(b))
                                           : (AS_NUMBER(a) < AS_NUMBER(b));
    stack[sp - 2] = BOOL_VAL(cmp);
    sp--;
    pc++;
    HOT_DISPATCH();
}
op_hot_jump:
    pc = op->target;
    HOT_DISPATCH();
op_hot_jump_if_false:
    if (sp <= 0) return 0;
    pc = is_falsey_jit(stack[sp - 1]) ? op->target : (uint16_t)(pc + 1);
    HOT_DISPATCH();
op_hot_jump_if_true:
    if (sp <= 0) return 0;
    pc = is_falsey_jit(stack[sp - 1]) ? (uint16_t)(pc + 1) : op->target;
    HOT_DISPATCH();
op_hot_qfor_prep: {
    if ((int)op->a > program->max_slot || (int)op->b > program->max_slot) return 0;
    Value v = locals[op->a];
    Value end = locals[op->b];
    if (!IS_NUMBER(v) || !IS_NUMBER(end)) return 0;
    if (AS_NUMBER(v) > AS_NUMBER(end)) {
        if (op->target == JIT_TARGET_EXIT) return 0;
        pc = op->target;
    } else {
        pc = (uint16_t)(pc + 1);
    }
    HOT_DISPATCH();
}
op_hot_qfor_loop: {
    if ((int)op->a > program->max_slot || (int)op->b > program->max_slot) return 0;
    Value v = locals[op->a];
    Value end = locals[op->b];
    if (!IS_NUMBER(v) || !IS_NUMBER(end)) return 0;
    double next = AS_NUMBER(v) + 1.0;
    locals[op->a] = NUMBER_VAL(next);
    pc = (next <= AS_NUMBER(end)) ? op->target : (uint16_t)(pc + 1);
    HOT_DISPATCH();
}
op_hot_adjust_stack:
    if ((int)op->a < 0 || (int)op->a > program->max_stack) return 0;
    sp = (int)op->a;
    pc++;
    HOT_DISPATCH();
op_hot_return:
    if (sp <= 0) return 0;
    if (has_direct_args) {
        *out = stack[sp - 1];
    } else {
        thread->stack_top -= (arg_count + 1);
        *thread->stack_top++ = stack[sp - 1];
    }
    return 1;
op_cold:
#undef HOT_DISPATCH
#endif
    while (pc < (uint16_t)program->op_count) {
        op = &program->ops[pc];
        switch (op->op) {
            case JOP_TINY_INLINE:
                return 0;
            case JOP_CONSTANT:
                if (sp >= program->max_stack) return 0;
                stack[sp++] = op->constant;
                pc++;
                break;
            case JOP_NIL:
                if (sp >= program->max_stack) return 0;
                stack[sp++] = NIL_VAL;
                pc++;
                break;
            case JOP_TRUE:
                if (sp >= program->max_stack) return 0;
                stack[sp++] = BOOL_VAL(1);
                pc++;
                break;
            case JOP_FALSE:
                if (sp >= program->max_stack) return 0;
                stack[sp++] = BOOL_VAL(0);
                pc++;
                break;
            case JOP_POP:
                if (sp <= 0) return 0;
                sp--;
                pc++;
                break;
            case JOP_GET_LOCAL:
                if ((int)op->a > program->max_slot || sp >= program->max_stack) return 0;
                stack[sp++] = locals[op->a];
                pc++;
                break;
            case JOP_GET_UPVALUE:
                if ((int)op->a >= closure->upvalue_count || sp >= program->max_stack) return 0;
                if (closure->upvalues[op->a] == NULL || closure->upvalues[op->a]->location == NULL) return 0;
                stack[sp++] = *closure->upvalues[op->a]->location;
                pc++;
                break;
            case JOP_GET2_LOCAL:
                if ((int)op->a > program->max_slot || (int)op->b > program->max_slot || sp + 1 >= program->max_stack) {
                    return 0;
                }
                stack[sp++] = locals[op->a];
                stack[sp++] = locals[op->b];
                pc++;
                break;
            case JOP_SET_LOCAL:
                if ((int)op->a > program->max_slot || sp <= 0) return 0;
                locals[op->a] = stack[sp - 1];
                pc++;
                break;
            case JOP_SET_LOCAL_POP:
                if ((int)op->a > program->max_slot || sp <= 0) return 0;
                locals[op->a] = stack[sp - 1];
                sp--;
                pc++;
                break;
            case JOP_APPEND: {
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
            case JOP_GET_TABLE_NUM:
            case JOP_GET_TABLE: {
                if (sp < 2) return 0;
                Value key = stack[sp - 1];
                Value table = stack[sp - 2];
                if (!IS_TABLE(table)) return 0;
                ObjTable* t = AS_TABLE(table);
                Value result = NIL_VAL;

                if (IS_NUMBER(key)) {
                    double num_key = AS_NUMBER(key);
                    int idx = (int)num_key;
                    if (num_key != (double)idx) return 0;
                    if (idx < 0) idx = t->table.array_max + idx + 1;
                    if (idx >= 1 && idx <= t->table.array_capacity) {
                        Value arr_val = t->table.array[idx - 1];
                        if (!IS_NIL(arr_val)) {
                            stack[sp - 2] = arr_val;
                            sp--;
                            pc++;
                            break;
                        }
                    }
                    if (table_get_array(&t->table, idx, &result)) {
                        stack[sp - 2] = result;
                        sp--;
                        pc++;
                        break;
                    }
                    if (op->op == JOP_GET_TABLE_NUM && t->no_metatable_fast) {
                        stack[sp - 2] = NIL_VAL;
                        sp--;
                        pc++;
                        break;
                    }
                    return 0;
                }

                if (op->op != JOP_GET_TABLE || !IS_STRING(key)) return 0;
                if (!t->no_metatable_fast) return 0;
                if (table_get(&t->table, AS_STRING(key), &result)) {
                    stack[sp - 2] = result;
                } else {
                    stack[sp - 2] = NIL_VAL;
                }
                sp--;
                pc++;
                break;
            }
            case JOP_SET_TABLE:
            case JOP_SET_TABLE_NUM: {
                if (sp < 3) return 0;
                Value value = stack[sp - 1];
                Value key = stack[sp - 2];
                Value table = stack[sp - 3];
                if (!IS_TABLE(table)) return 0;
                ObjTable* t = AS_TABLE(table);
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
                } else if (op->op == JOP_SET_TABLE && IS_STRING(key)) {
                    table_set(&t->table, AS_STRING(key), value);
                } else {
                    return 0;
                }
                stack[sp - 3] = value;
                sp -= 2;
                pc++;
                break;
            }
            case JOP_INC_LOCAL:
            case JOP_SUB_LOCAL_CONST:
            case JOP_MUL_LOCAL_CONST:
            case JOP_DIV_LOCAL_CONST:
            case JOP_MOD_LOCAL_CONST: {
                if ((int)op->a > program->max_slot || sp >= program->max_stack) return 0;
                Value a = locals[op->a];
                Value b = op->constant;
                if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
                double ad = AS_NUMBER(a);
                double bd = AS_NUMBER(b);
                Value out = NIL_VAL;
                if (op->op == JOP_INC_LOCAL) out = NUMBER_VAL(ad + bd);
                else if (op->op == JOP_SUB_LOCAL_CONST) out = NUMBER_VAL(ad - bd);
                else if (op->op == JOP_MUL_LOCAL_CONST) out = NUMBER_VAL(ad * bd);
                else if (op->op == JOP_DIV_LOCAL_CONST) out = NUMBER_VAL(ad / bd);
                else out = numeric_binop(JOP_MODULO, ad, bd);
                locals[op->a] = out;
                stack[sp++] = out;
                pc++;
                break;
            }
            case JOP_ADD_CONST:
            case JOP_SUB_CONST:
            case JOP_MUL_CONST:
            case JOP_DIV_CONST:
            case JOP_MOD_CONST: {
                if (sp <= 0) return 0;
                Value a = stack[sp - 1];
                Value b = op->constant;
                if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
                double ad = AS_NUMBER(a);
                double bd = AS_NUMBER(b);
                if (op->op == JOP_ADD_CONST) stack[sp - 1] = NUMBER_VAL(ad + bd);
                else if (op->op == JOP_SUB_CONST) stack[sp - 1] = NUMBER_VAL(ad - bd);
                else if (op->op == JOP_MUL_CONST) stack[sp - 1] = NUMBER_VAL(ad * bd);
                else if (op->op == JOP_DIV_CONST) stack[sp - 1] = NUMBER_VAL(ad / bd);
                else stack[sp - 1] = numeric_binop(JOP_MODULO, ad, bd);
                pc++;
                break;
            }
            case JOP_ADD:
            case JOP_ADD_INPLACE:
            case JOP_SUBTRACT:
            case JOP_MULTIPLY:
            case JOP_DIVIDE:
            case JOP_MODULO:
            case JOP_IADD:
            case JOP_ISUB:
            case JOP_IMUL:
            case JOP_IDIV:
            case JOP_IMOD:
            case JOP_FADD:
            case JOP_FSUB:
            case JOP_FMUL:
            case JOP_FDIV:
            case JOP_FMOD:
            case JOP_QFADD:
            case JOP_QFSUB:
            case JOP_QFMUL:
            case JOP_QFDIV:
            case JOP_QFMOD: {
                if (sp < 2) return 0;
                Value b = stack[sp - 1];
                Value a = stack[sp - 2];
                if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
                stack[sp - 2] = numeric_binop(op->op, AS_NUMBER(a), AS_NUMBER(b));
                sp--;
                pc++;
                break;
            }
            case JOP_NEGATE:
                if (sp <= 0 || !IS_NUMBER(stack[sp - 1])) return 0;
                stack[sp - 1] = NUMBER_VAL(-AS_NUMBER(stack[sp - 1]));
                pc++;
                break;
            case JOP_NOT:
                if (sp <= 0) return 0;
                stack[sp - 1] = BOOL_VAL(is_falsey_jit(stack[sp - 1]));
                pc++;
                break;
            case JOP_EQUAL: {
                if (sp < 2) return 0;
                Value b = stack[sp - 1];
                Value a = stack[sp - 2];
                int eq = jit_values_equal(a, b);
                stack[sp - 2] = BOOL_VAL(eq);
                sp--;
                pc++;
                break;
            }
            case JOP_QGREATER_NUM:
            case JOP_QLESS_NUM: {
                if (sp < 2) return 0;
                Value b = stack[sp - 1];
                Value a = stack[sp - 2];
                if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
                int cmp = (op->op == JOP_QGREATER_NUM) ? (AS_NUMBER(a) > AS_NUMBER(b))
                                                       : (AS_NUMBER(a) < AS_NUMBER(b));
                stack[sp - 2] = BOOL_VAL(cmp);
                sp--;
                pc++;
                break;
            }
            case JOP_JUMP:
                pc = op->target;
                break;
            case JOP_JUMP_IF_FALSE:
                if (sp <= 0) return 0;
                pc = is_falsey_jit(stack[sp - 1]) ? op->target : (uint16_t)(pc + 1);
                break;
            case JOP_JUMP_IF_TRUE:
                if (sp <= 0) return 0;
                pc = is_falsey_jit(stack[sp - 1]) ? (uint16_t)(pc + 1) : op->target;
                break;
            case JOP_CALL: {
                int argc = (int)op->a;
                if (argc < 0 || sp < argc + 1) return 0;
                int callee_idx = sp - 1 - argc;
                int out_count = 0;
                if (!jit_call_primitive_multi(
                        vm,
                        stack[callee_idx],
                        &stack[callee_idx + 1],
                        argc,
                        &stack[callee_idx],
                        program->max_stack - callee_idx,
                        &out_count)) {
                    return 0;
                }
                sp = callee_idx + out_count;
                pc++;
                break;
            }
            case JOP_INVOKE: {
                int argc = (int)op->a;
                if (argc < 0 || sp < argc + 1) return 0;
                int recv_idx = sp - 1 - argc;
                Value receiver = stack[recv_idx];
                Value out = NIL_VAL;
                if (!jit_invoke_primitive(vm, receiver, op->constant, &stack[recv_idx + 1], argc, &out)) return 0;
                stack[recv_idx] = out;
                sp = recv_idx + 1;
                pc++;
                break;
            }
            case JOP_LENGTH: {
                if (sp <= 0) return 0;
                Value out = NIL_VAL;
                if (!jit_length_primitive(stack[sp - 1], &out)) return 0;
                stack[sp - 1] = out;
                pc++;
                break;
            }
            case JOP_SLICE: {
                if (sp < 4) return 0;
                Value out = NIL_VAL;
                if (!jit_slice_primitive(stack[sp - 4], stack[sp - 3], stack[sp - 2], stack[sp - 1], &out)) return 0;
                stack[sp - 4] = out;
                sp -= 3;
                pc++;
                break;
            }
            case JOP_BUILD_STRING2:
            case JOP_BUILD_STRING3:
            case JOP_BUILD_STRING4:
            case JOP_BUILD_STRING5: {
                int n = 0;
                if (op->op == JOP_BUILD_STRING2) n = 2;
                else if (op->op == JOP_BUILD_STRING3) n = 3;
                else if (op->op == JOP_BUILD_STRING4) n = 4;
                else n = 5;
                if (sp < n) return 0;
                Value out = NIL_VAL;
                if (!jit_build_string_primitive(vm, &stack[sp - n], n, &out)) return 0;
                stack[sp - n] = out;
                sp = sp - n + 1;
                pc++;
                break;
            }
            case JOP_QCALL2: {
                if (sp < 3) return 0;
                Value callee = stack[sp - 3];
                Value arg0 = stack[sp - 2];
                Value arg1 = stack[sp - 1];
                Value out = NIL_VAL;
                if (IS_CLOSURE(callee)) {
                    ObjClosure* c = AS_CLOSURE(callee);
                    ObjFunction* fn = c->function;
                    if (fn != NULL &&
                        fn->tiny_inline_kind != TINY_INLINE_NONE &&
                        jit_eval_tiny_inline2_local(fn, arg0, arg1, &out)) {
                        stack[sp - 3] = out;
                        sp -= 2;
                        pc++;
                        break;
                    }
                }
                if (!jit_call_primitive(vm, callee, &stack[sp - 2], 2, &out)) return 0;
                stack[sp - 3] = out;
                sp -= 2;
                pc++;
                break;
            }
            case JOP_QFOR_PREP_NUM: {
                if ((int)op->a > program->max_slot || (int)op->b > program->max_slot) return 0;
                Value v = locals[op->a];
                Value end = locals[op->b];
                if (!IS_NUMBER(v) || !IS_NUMBER(end)) return 0;
                if (AS_NUMBER(v) > AS_NUMBER(end)) {
                    if (op->target == JIT_TARGET_EXIT) return 0;
                    pc = op->target;
                } else {
                    pc = (uint16_t)(pc + 1);
                }
                break;
            }
            case JOP_QFOR_LOOP_NUM: {
                if ((int)op->a > program->max_slot || (int)op->b > program->max_slot) return 0;
                Value v = locals[op->a];
                Value end = locals[op->b];
                if (!IS_NUMBER(v) || !IS_NUMBER(end)) return 0;
                double next = AS_NUMBER(v) + 1.0;
                locals[op->a] = NUMBER_VAL(next);
                pc = (next <= AS_NUMBER(end)) ? op->target : (uint16_t)(pc + 1);
                break;
            }
            case JOP_ADJUST_STACK:
                if ((int)op->a < 0 || (int)op->a > program->max_stack) return 0;
                sp = (int)op->a;
                pc++;
                break;
            case JOP_RETURN:
                if (sp <= 0) return 0;
                if (has_direct_args) {
                    *out = stack[sp - 1];
                } else {
                    thread->stack_top -= (arg_count + 1);
                    *thread->stack_top++ = stack[sp - 1];
                }
                return 1;
            default:
                return 0;
        }
    }

    return 0;
}

void toi_jit_init(VM* vm) {
    if (vm == NULL) return;
    vm->native_jit = NULL;
    if (!vm->native_jit_enabled) return;

    ToiJitState* st = (ToiJitState*)calloc(1, sizeof(ToiJitState));
    if (st == NULL) {
        vm->native_jit_enabled = 0;
        return;
    }
    st->loop_exec_enabled = 1;
    st->loop_numeric_fast_enabled = 1;
    st->loop_value_fast_enabled = 1;
    st->hot_loop_threshold = jit_loop_hot_threshold();
    st->native_trace_exec_enabled = (uint8_t)TOI_JIT_DEBUG_NATIVE_TRACE_EXEC;
    st->native_trace_sandbox_enabled = (uint8_t)TOI_JIT_DEBUG_NATIVE_TRACE_SANDBOX;
    st->native_trace_compiled_exec_enabled = (uint8_t)jit_pipeline_native_trace_compiled_exec_enabled();
    // Backend selection is overridable for bisecting JIT behaviour:
    //   TOI_JIT_BACKEND=none  -> no native code (IR/typed executors only)
    //   TOI_JIT_BACKEND=arm64 / x64 -> force a specific backend
    const char* backend_env = getenv("TOI_JIT_BACKEND");
    const char* backend_name = (backend_env != NULL && backend_env[0] != '\0') ? backend_env : "auto";
    st->backend_kind = toi_jit_backend_select(backend_name);
    st->backend_enabled = (uint8_t)(st->backend_kind != TOI_JIT_BACKEND_NONE);
    st->backend_host_supported = 0;
    st->backend_host_supported = (uint8_t)toi_jit_backend_host_supported(st->backend_kind);
    st->next_pipeline_enabled = (uint8_t)jit_pipeline_pipeline_enabled();
    if (st->next_pipeline_enabled) {
        st->next_pipeline = jit_pipeline_create();
        if (st->next_pipeline == NULL) st->next_pipeline_enabled = 0;
    }
    vm->native_jit = st;
}

void toi_jit_free(VM* vm) {
    if (vm == NULL) return;
    ToiJitState* st = jit_state(vm);
    if (st == NULL) return;
    for (int i = 0; i < st->count; i++) {
        jit_program_free(st->tracked[i].program);
        st->tracked[i].program = NULL;
    }
    for (int i = 0; i < st->loop_count; i++) {
        toi_jit_backend_free_code(st->backend_kind, st->loops[i].native_code_mem, st->loops[i].native_code_size);
        st->loops[i].native_code_mem = NULL;
        st->loops[i].native_code_size = 0;
        st->loops[i].native_slot_loop_fn = NULL;
        st->loops[i].native_value_loop_fn = NULL;
        st->loops[i].native_trace_fn = NULL;
        st->loops[i].native_value_instr_count = 0;
        if (st->loops[i].native_trace_code_mem != NULL && st->loops[i].native_trace_code_size != 0) {
            toi_jit_backend_free_code(
                st->backend_kind,
                st->loops[i].native_trace_code_mem,
                st->loops[i].native_trace_code_size);
        }
        st->loops[i].native_trace_code_mem = NULL;
        st->loops[i].native_trace_code_size = 0;
        st->loops[i].native_numeric_loop_fn = NULL;
        st->loops[i].native_qfor_empty_fn = NULL;
        st->loops[i].native_math_reduce_fn = NULL;
        free(st->loops[i].ops);
        st->loops[i].ops = NULL;
        free(st->loops[i].call_ic_closures);
        st->loops[i].call_ic_closures = NULL;
        free(st->loops[i].call_ic_programs);
        st->loops[i].call_ic_programs = NULL;
        free(st->loops[i].call_ic2_closures);
        st->loops[i].call_ic2_closures = NULL;
        free(st->loops[i].call_ic2_programs);
        st->loops[i].call_ic2_programs = NULL;
        free(st->loops[i].call_ic3_closures);
        st->loops[i].call_ic3_closures = NULL;
        free(st->loops[i].call_ic3_programs);
        st->loops[i].call_ic3_programs = NULL;
        free(st->loops[i].call_ic4_closures);
        st->loops[i].call_ic4_closures = NULL;
        free(st->loops[i].call_ic4_programs);
        st->loops[i].call_ic4_programs = NULL;
        free(st->loops[i].call_ic5_closures);
        st->loops[i].call_ic5_closures = NULL;
        free(st->loops[i].call_ic5_programs);
        st->loops[i].call_ic5_programs = NULL;
        clear_typed_trace_meta(&st->loops[i]);
    }
    free(st->tracked);
    free(st->loops);
    free(st->side_exits);
    jit_pipeline_free(st->next_pipeline);
    st->next_pipeline = NULL;
    free(st);
    vm->native_jit = NULL;
}

void toi_jit_mark_roots(VM* vm) {
    ToiJitState* st = jit_state(vm);
    if (st == NULL) return;
    jit_pipeline_mark_roots(st->next_pipeline);
    for (int i = 0; i < st->count; i++) {
        if (st->tracked[i].function != NULL) {
            mark_object((struct Obj*)st->tracked[i].function);
        }
    }
    for (int i = 0; i < st->loop_count; i++) {
        if (st->loops[i].function != NULL) {
            mark_object((struct Obj*)st->loops[i].function);
        }
        if (st->loops[i].call_ic_closures != NULL) {
            for (int pc = 0; pc < st->loops[i].op_count; pc++) {
                if (st->loops[i].call_ic_closures[pc] != NULL) {
                    mark_object((struct Obj*)st->loops[i].call_ic_closures[pc]);
                }
            }
        }
        if (st->loops[i].call_ic2_closures != NULL) {
            for (int pc = 0; pc < st->loops[i].op_count; pc++) {
                if (st->loops[i].call_ic2_closures[pc] != NULL) {
                    mark_object((struct Obj*)st->loops[i].call_ic2_closures[pc]);
                }
            }
        }
        if (st->loops[i].call_ic3_closures != NULL) {
            for (int pc = 0; pc < st->loops[i].op_count; pc++) {
                if (st->loops[i].call_ic3_closures[pc] != NULL) {
                    mark_object((struct Obj*)st->loops[i].call_ic3_closures[pc]);
                }
            }
        }
        if (st->loops[i].call_ic4_closures != NULL) {
            for (int pc = 0; pc < st->loops[i].op_count; pc++) {
                if (st->loops[i].call_ic4_closures[pc] != NULL) {
                    mark_object((struct Obj*)st->loops[i].call_ic4_closures[pc]);
                }
            }
        }
        if (st->loops[i].call_ic5_closures != NULL) {
            for (int pc = 0; pc < st->loops[i].op_count; pc++) {
                if (st->loops[i].call_ic5_closures[pc] != NULL) {
                    mark_object((struct Obj*)st->loops[i].call_ic5_closures[pc]);
                }
            }
        }
    }
    for (int i = 0; i < st->side_exit_count; i++) {
        if (st->side_exits[i].function != NULL) {
            mark_object((struct Obj*)st->side_exits[i].function);
        }
    }
}

void toi_jit_notify_hot_function(VM* vm, ObjFunction* function) {
    if (vm == NULL || !vm->native_jit_enabled || function == NULL) return;
    ToiJitState* st = jit_state(vm);
    if (st == NULL) return;

    vm->native_jit_hot_candidates++;
    JitTrackedFunction* entry = jit_find_or_add(vm, function);
    if (entry == NULL) return;
    if (entry->compiled || entry->failed || entry->queued) return;
    if (st->next_pipeline_enabled && st->next_pipeline != NULL) {
        jit_pipeline_note_hot_function(st->next_pipeline, function);
        (void)jit_pipeline_try_compile_stub(st->next_pipeline, function);
    }

    entry->queued = 1;
    entry->compile_attempts++;
    vm->native_jit_compile_attempts++;

    JitProgram* program = jit_program_compile(function);
    entry->queued = 0;
    if (program == NULL) {
        entry->failed = 1;
        return;
    }

    entry->program = program;
    entry->compiled = 1;
    entry->runtime_disabled = 0;
    entry->runtime_probe_limit = program_runtime_probe_limit(program);
    vm->native_jit_compiled_count++;
}

static int jit_try_execute_entry_stack(
    VM* vm,
    ToiJitState* st,
    JitTrackedFunction* entry,
    ObjClosure* closure,
    int arg_count) {
    if (vm == NULL || st == NULL || entry == NULL || closure == NULL) return 0;
    if (!jit_function_jit_enabled()) return 0;
    if (!entry->compiled || entry->program == NULL || entry->runtime_disabled) return 0;
    if (entry->function != closure->function) return 0;
    vm->native_jit_exec_attempts++;
    entry->exec_attempts++;
    uint32_t probe_limit = (entry->runtime_probe_limit != 0) ? (uint32_t)entry->runtime_probe_limit : 128u;
    if (entry->exec_attempts >= probe_limit && entry->exec_hits == 0) {
        entry->runtime_disabled = 1;
        st->func_runtime_disabled++;
        return 0;
    }
    uint32_t low_hit_limit = probe_limit * 4u;
    if (entry->exec_attempts >= low_hit_limit &&
        ((double)entry->exec_hits / (double)entry->exec_attempts) < 0.01) {
        entry->runtime_disabled = 1;
        st->func_runtime_disabled++;
        return 0;
    }
    if (!execute_program(vm, closure, arg_count, entry->program, NULL, NULL)) return 0;
    entry->exec_hits++;
    vm->native_jit_exec_hits++;
    return 1;
}

typedef int (*JitLinkedExecFn)(VM* vm, ObjClosure* closure, int arg_count, Value* args, Value* out);

static int jit_exec_stack_dispatch(VM* vm, ObjClosure* closure, int arg_count, Value* args, Value* out) {
    (void)args;
    (void)out;
    return toi_jit_try_execute(vm, closure, arg_count);
}

static int jit_exec_compiled_with_args_dispatch(
    VM* vm,
    ObjClosure* closure,
    int arg_count,
    Value* args,
    Value* out) {
    if (args == NULL || out == NULL) return 0;
    return toi_jit_try_execute_compiled_with_args(vm, closure, arg_count, args, out);
}

static int jit_linked_pipeline_active(ToiJitState* st) {
    return st != NULL &&
        st->next_pipeline_enabled &&
        st->next_pipeline != NULL &&
        jit_pipeline_link_exec_is_enabled(st->next_pipeline);
}

static void jit_linked_record_hit(JitPipelineState* pipeline, int patch_hit) {
    if (pipeline == NULL) return;
    jit_pipeline_note_link_exec_hit(pipeline);
    if (patch_hit) jit_pipeline_note_link_patch_hit(pipeline);
}

static int jit_try_linked_hot_retry(
    VM* vm,
    JitPipelineState* pipeline,
    ObjFunction* function,
    ObjClosure* closure,
    int arg_count,
    Value* args,
    Value* out,
    JitLinkedExecFn exec_fn,
    int note_attempt,
    int note_patch_attempt,
    int note_patch_hit) {
    if (vm == NULL || pipeline == NULL || function == NULL || closure == NULL || exec_fn == NULL) return 0;
    if (note_attempt) jit_pipeline_note_link_exec_attempt(pipeline);
    if (note_patch_attempt) jit_pipeline_note_link_patch_attempt(pipeline);
    if (exec_fn(vm, closure, arg_count, args, out)) {
        jit_linked_record_hit(pipeline, note_patch_hit);
        return 1;
    }
    toi_jit_notify_hot_function(vm, function);
    if (exec_fn(vm, closure, arg_count, args, out)) {
        jit_linked_record_hit(pipeline, note_patch_hit);
        return 1;
    }
    return 0;
}

int toi_jit_try_execute(VM* vm, ObjClosure* closure, int arg_count) {
    if (!jit_function_jit_enabled()) return 0;
    if (vm == NULL || !vm->native_jit_enabled || closure == NULL) return 0;

    ToiJitState* st = jit_state(vm);
    if (st == NULL) return 0;
    JitTrackedFunction* entry = jit_find_tracked(st, closure->function);
    if (entry == NULL) return 0;
    return jit_try_execute_entry_stack(vm, st, entry, closure, arg_count);
}

int toi_jit_try_execute_linked(VM* vm, ObjClosure* closure, int arg_count) {
    if (vm == NULL || !vm->native_jit_enabled || closure == NULL) return 0;
    ToiJitState* st = jit_state(vm);
    if (!jit_linked_pipeline_active(st)) {
        return toi_jit_try_execute(vm, closure, arg_count);
    }
    if (!jit_pipeline_link_patch_is_enabled(st->next_pipeline)) {
        return toi_jit_try_execute(vm, closure, arg_count);
    }
    ObjFunction* function = closure->function;
    if (function == NULL || !jit_pipeline_can_execute_linked(st->next_pipeline, function)) {
        return toi_jit_try_execute(vm, closure, arg_count);
    }
    return jit_try_linked_hot_retry(
        vm, st->next_pipeline, function, closure, arg_count, NULL, NULL, jit_exec_stack_dispatch, 1, 0, 0);
}

int toi_jit_try_execute_linked_at_site(
    VM* vm,
    ObjFunction* caller_function,
    int opcode_offset,
    ObjClosure* closure,
    int arg_count) {
    if (vm == NULL || !vm->native_jit_enabled || caller_function == NULL || closure == NULL) return 0;
    ToiJitState* st = jit_state(vm);
    if (!jit_linked_pipeline_active(st)) {
        return toi_jit_try_execute(vm, closure, arg_count);
    }
    if (!jit_pipeline_link_patch_is_enabled(st->next_pipeline)) {
        return toi_jit_try_execute(vm, closure, arg_count);
    }
    ObjFunction* function = closure->function;
    if (function == NULL || opcode_offset < 0 ||
        !jit_pipeline_can_execute_linked_at_site(
            st->next_pipeline,
            caller_function,
            (uint32_t)opcode_offset,
            function)) {
        return toi_jit_try_execute(vm, closure, arg_count);
    }
    jit_pipeline_note_link_exec_attempt(st->next_pipeline);
    jit_pipeline_note_link_patch_attempt(st->next_pipeline);
    JitTrackedFunction* entry = jit_link_site_cache_lookup(
        st, caller_function, (uint32_t)opcode_offset, function);
    if (entry != NULL && jit_try_execute_entry_stack(vm, st, entry, closure, arg_count)) {
        jit_linked_record_hit(st->next_pipeline, 1);
        return 1;
    }

    entry = jit_find_tracked(st, function);
    if (entry != NULL && entry->compiled && entry->program != NULL) {
        jit_link_site_cache_store(st, caller_function, (uint32_t)opcode_offset, function, entry);
        if (jit_try_execute_entry_stack(vm, st, entry, closure, arg_count)) {
            jit_linked_record_hit(st->next_pipeline, 1);
            return 1;
        }
    }

    toi_jit_notify_hot_function(vm, function);
    entry = jit_find_tracked(st, function);
    if (entry != NULL && entry->compiled && entry->program != NULL) {
        jit_link_site_cache_store(st, caller_function, (uint32_t)opcode_offset, function, entry);
    }
    if (entry != NULL && jit_try_execute_entry_stack(vm, st, entry, closure, arg_count)) {
        jit_linked_record_hit(st->next_pipeline, 1);
        return 1;
    }
    return 0;
}

static int jit_validate_closure_call_args(ObjClosure* closure, int arg_count, Value* args) {
    if (closure == NULL || args == NULL) return 0;
    ObjFunction* function = closure->function;
    if (function == NULL) return 0;
    if (function->is_generator || function->is_variadic || function->defaults_count != 0) return 0;
    if (function->arity != arg_count) return 0;
    if (function->param_types_count > 0 && function->param_types != NULL) {
        int check_count = function->param_types_count < function->arity
            ? function->param_types_count
            : function->arity;
        for (int i = 0; i < check_count; i++) {
            uint8_t type = function->param_types[i];
            if (type == TYPEHINT_ANY) continue;
            if (!jit_value_matches_type(args[i], type)) return 0;
        }
    }
    return 1;
}

int toi_jit_execute_program_direct_with_args(
    VM* vm,
    ObjClosure* closure,
    void* program_ptr,
    int arg_count,
    Value* args,
    Value* out) {
    if (!jit_function_jit_enabled()) return 0;
    if (vm == NULL || !vm->native_jit_enabled || closure == NULL || program_ptr == NULL ||
        args == NULL || out == NULL) {
        return 0;
    }
    if (!jit_validate_closure_call_args(closure, arg_count, args)) return 0;
    JitProgram* program = (JitProgram*)program_ptr;
    return execute_program(vm, closure, arg_count, program, args, out);
}

static int jit_try_execute_entry_with_args(
    VM* vm,
    ToiJitState* st,
    JitTrackedFunction* entry,
    ObjClosure* closure,
    int arg_count,
    Value* args,
    Value* out) {
    if (vm == NULL || st == NULL || entry == NULL || closure == NULL || args == NULL || out == NULL) return 0;
    if (entry->program == NULL || entry->runtime_disabled) return 0;
    if (!jit_validate_closure_call_args(closure, arg_count, args)) return 0;
    ObjFunction* function = closure->function;
    if (function == NULL || entry->function != function) return 0;
    vm->native_jit_exec_attempts++;
    entry->exec_attempts++;
    uint32_t probe_limit = (entry->runtime_probe_limit != 0) ? (uint32_t)entry->runtime_probe_limit : 128u;
    if (entry->exec_attempts >= probe_limit && entry->exec_hits == 0) {
        entry->runtime_disabled = 1;
        st->func_runtime_disabled++;
        return 0;
    }
    uint32_t low_hit_limit = probe_limit * 4u;
    if (entry->exec_attempts >= low_hit_limit &&
        ((double)entry->exec_hits / (double)entry->exec_attempts) < 0.01) {
        entry->runtime_disabled = 1;
        st->func_runtime_disabled++;
        return 0;
    }
    if (!execute_program(vm, closure, arg_count, entry->program, args, out)) return 0;

    entry->exec_hits++;
    vm->native_jit_exec_hits++;
    return 1;
}

int toi_jit_try_execute_with_args(VM* vm, ObjClosure* closure, int arg_count, Value* args, Value* out) {
    if (!jit_function_jit_enabled()) return 0;
    if (vm == NULL || !vm->native_jit_enabled || closure == NULL || args == NULL || out == NULL) return 0;
    ToiJitState* st = jit_state(vm);
    if (st == NULL) return 0;
    if (!jit_validate_closure_call_args(closure, arg_count, args)) return 0;
    ObjFunction* function = closure->function;
    if (function == NULL) return 0;
    JitProgram* program = NULL;
    if (!jit_get_compiled_closure_program(vm, closure, &program) || program == NULL) return 0;
    JitTrackedFunction* entry = jit_find_tracked(st, function);
    if (entry == NULL) return 0;
    if (entry->program == NULL) entry->program = program;
    return jit_try_execute_entry_with_args(vm, st, entry, closure, arg_count, args, out);
}

int toi_jit_try_execute_compiled_with_args(VM* vm, ObjClosure* closure, int arg_count, Value* args, Value* out) {
    if (!jit_function_jit_enabled()) return 0;
    if (vm == NULL || !vm->native_jit_enabled || closure == NULL || args == NULL || out == NULL) return 0;
    ToiJitState* st = jit_state(vm);
    if (st == NULL) return 0;
    ObjFunction* function = closure->function;
    if (function == NULL) return 0;
    JitTrackedFunction* entry = jit_find_tracked(st, function);
    if (entry == NULL || !entry->compiled || entry->program == NULL) return 0;
    return jit_try_execute_entry_with_args(vm, st, entry, closure, arg_count, args, out);
}

int toi_jit_try_execute_compiled_at_callsite_with_args(
    VM* vm,
    ObjClosure* caller,
    uint32_t opcode_offset,
    ObjClosure* callee,
    int arg_count,
    Value* args,
    Value* out) {
    if (!jit_function_jit_enabled()) return 0;
    if (vm == NULL || !vm->native_jit_enabled || caller == NULL || callee == NULL ||
        args == NULL || out == NULL) {
        return 0;
    }
    ToiJitState* st = jit_state(vm);
    if (st == NULL) return 0;
    if (!jit_validate_closure_call_args(callee, arg_count, args)) return 0;
    ObjFunction* caller_function = caller->function;
    ObjFunction* callee_function = callee->function;
    if (caller_function == NULL || callee_function == NULL) return 0;

    JitTrackedFunction* entry = jit_link_site_cache_lookup(st, caller_function, opcode_offset, callee_function);
    if (entry != NULL &&
        jit_try_execute_entry_with_args(vm, st, entry, callee, arg_count, args, out)) {
        return 1;
    }

    entry = jit_find_tracked(st, callee_function);
    if (entry != NULL && entry->compiled && entry->program != NULL) {
        jit_link_site_cache_store(st, caller_function, opcode_offset, callee_function, entry);
        return jit_try_execute_entry_with_args(vm, st, entry, callee, arg_count, args, out);
    }

    JitProgram* program = NULL;
    if (!jit_get_compiled_closure_program(vm, callee, &program) || program == NULL) return 0;
    entry = jit_find_tracked(st, callee_function);
    if (entry == NULL) return 0;
    if (entry->program == NULL) entry->program = program;
    if (!entry->compiled || entry->program == NULL) return 0;
    jit_link_site_cache_store(st, caller_function, opcode_offset, callee_function, entry);
    return jit_try_execute_entry_with_args(vm, st, entry, callee, arg_count, args, out);
}

int toi_jit_get_compiled_program_ptr_at_callsite(
    VM* vm,
    ObjClosure* caller,
    uint32_t opcode_offset,
    ObjClosure* callee,
    void** out_program_ptr) {
    if (!jit_function_jit_enabled()) return 0;
    if (out_program_ptr == NULL) return 0;
    *out_program_ptr = NULL;
    if (vm == NULL || !vm->native_jit_enabled || caller == NULL || callee == NULL) return 0;
    ToiJitState* st = jit_state(vm);
    if (st == NULL) return 0;
    ObjFunction* caller_function = caller->function;
    ObjFunction* callee_function = callee->function;
    if (caller_function == NULL || callee_function == NULL) return 0;

    JitTrackedFunction* entry = jit_link_site_cache_lookup(st, caller_function, opcode_offset, callee_function);
    if (entry != NULL && entry->compiled && entry->program != NULL) {
        *out_program_ptr = (void*)entry->program;
        return 1;
    }

    entry = jit_find_tracked(st, callee_function);
    if (entry != NULL && entry->compiled && entry->program != NULL) {
        jit_link_site_cache_store(st, caller_function, opcode_offset, callee_function, entry);
        *out_program_ptr = (void*)entry->program;
        return 1;
    }

    JitProgram* program = NULL;
    if (!jit_get_compiled_closure_program(vm, callee, &program) || program == NULL) return 0;
    entry = jit_find_tracked(st, callee_function);
    if (entry == NULL || !entry->compiled || entry->program == NULL) return 0;
    jit_link_site_cache_store(st, caller_function, opcode_offset, callee_function, entry);
    *out_program_ptr = (void*)entry->program;
    return 1;
}

int toi_jit_try_execute_linked_with_args(VM* vm, ObjClosure* closure, int arg_count, Value* args, Value* out) {
    if (!jit_function_jit_enabled()) return 0;
    if (vm == NULL || !vm->native_jit_enabled || closure == NULL || args == NULL || out == NULL) return 0;
    ToiJitState* st = jit_state(vm);
    if (!jit_linked_pipeline_active(st)) return 0;
    if (!jit_pipeline_link_patch_is_enabled(st->next_pipeline)) {
        return toi_jit_try_execute_compiled_with_args(vm, closure, arg_count, args, out);
    }
    ObjFunction* function = closure->function;
    if (function == NULL) return 0;
    if (!jit_pipeline_can_execute_linked(st->next_pipeline, function)) return 0;
    return jit_try_linked_hot_retry(
        vm, st->next_pipeline, function, closure, arg_count, args, out, jit_exec_compiled_with_args_dispatch, 1, 0, 0);
}

// Shared tail of loop compilation: attempt compile_loop_trace once and record
// the outcome. Invoked from toi_jit_notify_loop_backedge when a loop crosses its
// hot threshold; afterwards the loop is assessed (compiled or failed) and the
// per-backedge fast path skips straight past this.
static void jit_compile_loop_trace_full(ToiJitState* st, VM* vm, ObjFunction* function, JitLoopTrace* trace) {
    trace->compile_attempts++;
    st->loop_compile_attempts++;
    if (!compile_loop_trace(vm, function, trace)) {
        trace->failed = 1;
        st->loop_compile_fail_count++;
        st->loop_last_fail_opcode = trace->fail_opcode;
        st->loop_last_fail_offset = trace->fail_offset;
        st->loop_last_fail_reason = trace->fail_reason;
        st->loop_fail_opcode_counts[trace->fail_opcode]++;
        if (trace->fail_reason == LOOP_FAIL_UNSUPPORTED) st->loop_fail_reason_unsupported++;
        else if (trace->fail_reason == LOOP_FAIL_DECODE) st->loop_fail_reason_decode++;
        else if (trace->fail_reason == LOOP_FAIL_JUMP) st->loop_fail_reason_jump++;
        else if (trace->fail_reason == LOOP_FAIL_STACK) st->loop_fail_reason_stack++;
        if (jit_dump_loops_enabled()) {
            fprintf(stderr, "[jit-loop-fail] fn=%s at=%d reason=%s op=%u off=%u\n",
                    (function->name != NULL) ? function->name->chars : "<script>",
                    trace->opcode_offset,
                    loop_fail_reason_name(trace->fail_reason),
                    (unsigned)trace->fail_opcode,
                    (unsigned)trace->fail_offset);
        }
        return;
    }
    st->loop_compiled_count++;
    if (jit_dump_loops_enabled()) {
        fprintf(stderr, "[jit-loop-compiled] fn=%s at=%d ops=%d entry=%u typed=%u numeric=%u value=%u\n",
                (function->name != NULL) ? function->name->chars : "<script>",
                trace->opcode_offset,
                trace->op_count,
                (unsigned)trace->entry_pc,
                (unsigned)trace->typed_fast_eligible,
                (unsigned)trace->numeric_fast_eligible,
                (unsigned)trace->value_fast_eligible);
        for (int i = 0; i < trace->op_count; i++) {
            JitOp* op = &trace->ops[i];
            fprintf(stderr, "  %03d: %s a=%u b=%u t=%u\n",
                    i, jit_op_name(op->op), (unsigned)op->a, (unsigned)op->b, (unsigned)op->target);
        }
    }
}

void toi_jit_notify_loop_backedge(VM* vm, CallFrame* frame, int opcode_offset, uint8_t opcode) {
    if (vm == NULL || !vm->native_jit_enabled || frame == NULL || frame->closure == NULL) return;
    if (!is_numeric_for_loop_opcode(opcode) && opcode != OP_LOOP) return;

    ToiJitState* st = jit_state(vm);
    if (st == NULL) return;
    if (!st->loop_exec_enabled) return;
    st->loop_backedge_events++;

    ObjFunction* function = frame->closure->function;

    // Fast path: this backedge was already tracked. Assessed loops (compiled,
    // failed, or runtime-disabled) never need re-probing; unassessed loops only
    // need to accumulate heat toward the compile threshold.
    JitLoopTrace* trace = jit_loop_from_offset(st, function, opcode_offset);
    if (trace != NULL) {
        if (trace->compiled || trace->failed || trace->runtime_disabled) return;
        trace->hot_count++;
        if (trace->hot_count < st->hot_loop_threshold) return;
        jit_compile_loop_trace_full(st, vm, function, trace);
        return;
    }

    // Slow path: first sighting of this backedge. Warm up the whole-function
    // pipeline once and register the trace in the per-chunk backedge table so
    // every later iteration skips the hash-based lookup entirely.
    if (st->next_pipeline_enabled && st->next_pipeline != NULL) {
        jit_pipeline_note_hot_function(st->next_pipeline, function);
        (void)jit_pipeline_try_compile_stub(st->next_pipeline, function);
    }
    trace = jit_find_or_add_loop(vm, function, opcode_offset, opcode);
    if (trace == NULL) return;
    jit_loop_store_offset(st, function, opcode_offset, trace);
    if (trace->compiled || trace->failed || trace->runtime_disabled) return;

    trace->hot_count++;
    if (trace->hot_count < st->hot_loop_threshold) return;
    jit_compile_loop_trace_full(st, vm, function, trace);
}

int toi_jit_has_compiled_loops(VM* vm) {
    if (vm == NULL || !vm->native_jit_enabled) return 0;
    ToiJitState* st = jit_state(vm);
    if (st == NULL) return 0;
    return st->loop_live_count != 0;
}

int toi_jit_try_execute_loop(VM* vm, CallFrame* frame, uint8_t** ip, int opcode_offset, uint8_t opcode) {
    if (vm == NULL || !vm->native_jit_enabled || frame == NULL || frame->closure == NULL) return 0;
    if (!is_numeric_for_loop_opcode(opcode) && opcode != OP_LOOP) return 0;

    ToiJitState* st = jit_state(vm);
    if (st == NULL) return 0;
    if (!st->loop_exec_enabled) return 0;
    if (st->loop_compiled_count == 0) return 0;

    ObjFunction* function = frame->closure->function;
    JitLoopTrace* trace = jit_loop_from_offset(st, function, opcode_offset);
    if (trace == NULL) {
        // Fallback for backedges not yet in the per-chunk table (e.g. the table
        // could not be allocated, or the loop compiled on an earlier path).
        trace = jit_find_loop(st, function, opcode_offset, opcode);
        if (trace == NULL) return 0;
    }
    if (!trace->compiled || trace->runtime_disabled) return 0;
    // Traces that were retired because their call sites are polymorphic must not
    // be re-probed on every backedge; the VM handles the loop from here on.
    if (trace->mixed_call_heavy) {
        jit_retire_loop_trace(st, trace);
        return 0;
    }
    // A trace whose in-trace call bails every iteration never gets past the call:
    // each backedge pays a trace entry plus a state commit, which is slower than
    // the plain VM (measured 2.7x on a call loop with a non-inlinable callee).
    // Retire it and let the VM run the loop with its own call path.
    if (trace->call_stall_runs >= 32u) {
        jit_retire_loop_trace(st, trace);
        if (jit_dump_loops_enabled()) {
            fprintf(stderr, "[jit-loop-runtime-disabled] fn=%s at=%d stalls=%u reason=call-stall\n",
                    (function->name != NULL) ? function->name->chars : "<script>",
                    opcode_offset,
                    (unsigned)trace->call_stall_runs);
        }
        return 0;
    }

    st->loop_exec_attempts++;
    trace->exec_attempts++;
    uint32_t probe_limit = (trace->runtime_probe_limit != 0) ? (uint32_t)trace->runtime_probe_limit : 128u;
    if (trace->exec_attempts >= probe_limit && trace->exec_hits == 0) {
        jit_retire_loop_trace(st, trace);
        if (jit_dump_loops_enabled()) {
            fprintf(stderr, "[jit-loop-runtime-disabled] fn=%s at=%d attempts=%u hits=%u reason=no-hits\n",
                    (function->name != NULL) ? function->name->chars : "<script>",
                    opcode_offset,
                    (unsigned)trace->exec_attempts,
                    (unsigned)trace->exec_hits);
        }
        return 0;
    }
    uint32_t low_hit_limit = probe_limit * 4u;
    if (trace->exec_attempts >= low_hit_limit &&
        ((double)trace->exec_hits / (double)trace->exec_attempts) < 0.01) {
        jit_retire_loop_trace(st, trace);
        if (jit_dump_loops_enabled()) {
            fprintf(stderr, "[jit-loop-runtime-disabled] fn=%s at=%d attempts=%u hits=%u reason=low-hit-rate\n",
                    (function->name != NULL) ? function->name->chars : "<script>",
                    opcode_offset,
                    (unsigned)trace->exec_attempts,
                    (unsigned)trace->exec_hits);
        }
        return 0;
    }
    ObjThread* thread = vm_current_thread(vm);
    if (thread == NULL) return 0;
    if (st->native_trace_sandbox_enabled &&
        trace->native_trace_fn != NULL &&
        trace->native_trace_ready &&
        !trace->native_trace_sandbox_disabled) {
        st->native_trace_sandbox_attempts++;
        trace->native_trace_sandbox_runs++;
        // Trace structural validity is verified once at build time
        // (jit_native_trace_validate sets native_trace_sandbox_ok); the per-entry
        // cost here is a single flag test instead of an O(n) instruction scan.
        if (trace->native_trace_sandbox_ok) {
            st->native_trace_sandbox_success++;
        } else {
            st->native_trace_sandbox_fail++;
            trace->native_trace_sandbox_disabled = 1;
        }
        if (trace->native_trace_sandbox_runs >= 8) {
            trace->native_trace_sandbox_disabled = 1;
        }
    }
    int native_trace_candidate =
        st->native_trace_exec_enabled &&
        !st->native_trace_exec_globally_disabled &&
        trace->native_slot_loop_fn == NULL &&
        (is_numeric_for_loop_opcode(trace->opcode) || trace->opcode == OP_LOOP) &&
        trace->native_trace_fn != NULL &&
        trace->native_trace_ready &&
        trace->native_trace_exec_safe;

    if (native_trace_candidate) {
        int direct_core = trace->native_trace_direct_core != 0;
        int has_call_or_invoke = trace->native_trace_has_call || trace->native_trace_has_invoke;
        int app_like_direct_core =
            direct_core &&
            (trace->native_trace_has_build_string ||
             trace->native_trace_has_get_table_generic ||
             trace->native_trace_has_mutating_table);

        uint32_t native_probe_limit = direct_core ? 8u : 8u;
        if (trace->native_trace_exec_attempts >= native_probe_limit &&
            trace->native_trace_exec_hits == 0) {
            trace->native_trace_exec_safe = 0;
            st->native_trace_runtime_disabled++;
        }
        uint32_t native_low_hit_limit = direct_core ? 256u : 32u;
        double native_low_hit_ratio = direct_core ? 0.01 : 0.05;
        if (trace->native_trace_exec_safe &&
            trace->native_trace_exec_attempts >= native_low_hit_limit &&
            ((double)trace->native_trace_exec_hits / (double)trace->native_trace_exec_attempts) < native_low_hit_ratio) {
            trace->native_trace_exec_safe = 0;
            st->native_trace_runtime_disabled++;
        }
        if (trace->native_trace_exec_safe &&
            trace->native_trace_exec_attempts >= (direct_core ? 128u : 16u) &&
            has_call_or_invoke &&
            trace->native_trace_soft_bails * 100u >
                trace->native_trace_exec_attempts * (direct_core ? 98u : 60u) &&
            (!direct_core || trace->native_trace_prefix_work < 4u || !app_like_direct_core)) {
            trace->native_trace_exec_safe = 0;
            st->native_trace_runtime_disabled++;
        }
        if (trace->native_trace_exec_safe &&
            (!direct_core || trace->native_trace_exec_attempts >= 128u) &&
            !native_trace_is_profitable(trace)) {
            trace->native_trace_exec_safe = 0;
            st->native_trace_runtime_disabled++;
        }
    }
    native_trace_candidate =
        st->native_trace_exec_enabled &&
        !st->native_trace_exec_globally_disabled &&
        trace->native_slot_loop_fn == NULL &&
        (is_numeric_for_loop_opcode(trace->opcode) || trace->opcode == OP_LOOP) &&
        trace->native_trace_fn != NULL &&
        trace->native_trace_ready &&
        trace->native_trace_exec_safe;

    if (native_trace_candidate) {
        if (st->native_trace_exec_attempts >= 32 &&
            st->native_trace_exec_hits == 0) {
            st->native_trace_exec_globally_disabled = 1;
            st->native_trace_exec_disable_reason = 1;
        }
        if (!st->native_trace_exec_globally_disabled &&
            st->native_trace_exec_attempts >= 128 &&
            ((double)st->native_trace_exec_hits / (double)st->native_trace_exec_attempts) < 0.03) {
            st->native_trace_exec_globally_disabled = 1;
            st->native_trace_exec_disable_reason = 2;
        }
    }
    native_trace_candidate =
        st->native_trace_exec_enabled &&
        !st->native_trace_exec_globally_disabled &&
        trace->native_slot_loop_fn == NULL &&
        (is_numeric_for_loop_opcode(trace->opcode) || trace->opcode == OP_LOOP) &&
        trace->native_trace_fn != NULL &&
        trace->native_trace_ready &&
        trace->native_trace_exec_safe;
    uint32_t native_stride = native_trace_exec_stride(trace);
    int native_trace_sampled =
        native_trace_candidate &&
        native_stride != 0u &&
        ((trace->exec_attempts & (native_stride - 1u)) == 0u);

    if (native_trace_sampled) {
        if (!native_trace_guards_hold(vm, trace, frame)) {
            st->native_trace_guard_failures++;
            trace->native_trace_guard_failures++;
            native_trace_sampled = 0;
        }
    }

    if (native_trace_sampled) {
        st->native_trace_exec_attempts++;
        trace->native_trace_exec_attempts++;
        if (trace->native_trace_direct_core) st->native_trace_exec_attempts_direct_core++;
        st->backend_loop_exec_attempts++;
        int frame_window = (int)trace->max_slot + 1;
        if (frame_window > 0 && frame_window <= 8192) {
            int needs_buffered_slots =
                trace->native_trace_has_call ||
                trace->native_trace_has_invoke ||
                trace->native_trace_has_build_string ||
                trace->native_trace_has_get_table_generic ||
                trace->native_trace_has_mutating_table;
            Value work[frame_window];
            Value* run_slots = frame->slots;
            if (needs_buffered_slots) {
                for (int i = 0; i < frame_window; i++) work[i] = frame->slots[i];
                run_slots = work;
            }
            ToiJitNativeExecState exec_state;
            exec_state.resume_offset = 0xffffffffu;
            exec_state.op = 0;
            exec_state.reason = TOI_JIT_NATIVE_DEOPT_NONE;
            exec_state.stack_count = 0;
            ToiJitNativeExecResult r = TOI_JIT_NATIVE_EXEC_DEOPT;
            if (st->native_trace_compiled_exec_enabled &&
                trace->native_trace_fn != NULL) {
                // Prefer compiled native-trace entry whenever available.
                st->native_trace_active_depth++;
                int rr = trace->native_trace_fn(
                    vm,
                    frame->closure,
                    run_slots,
                    frame_window,
                    &exec_state);
                st->native_trace_active_depth--;
                if (rr == (int)TOI_JIT_NATIVE_EXEC_EXIT) r = TOI_JIT_NATIVE_EXEC_EXIT;
                else if (rr == (int)TOI_JIT_NATIVE_EXEC_CONTINUE) r = TOI_JIT_NATIVE_EXEC_CONTINUE;
                else r = TOI_JIT_NATIVE_EXEC_DEOPT;
            } else {
                st->native_trace_active_depth++;
                r = toi_jit_backend_run_native_trace(
                    st->backend_kind,
                    vm,
                    frame->closure,
                    run_slots,
                    frame_window,
                    trace->native_trace_instrs,
                    (int)trace->native_trace_instr_count,
                    trace->native_value_var_slot,
                    trace->native_value_end_slot,
                    &exec_state);
                st->native_trace_active_depth--;
            }
            if (r == TOI_JIT_NATIVE_EXEC_EXIT) {
                if (needs_buffered_slots) {
                    for (int i = 0; i < frame_window; i++) frame->slots[i] = work[i];
                }
                *ip = frame->closure->function->chunk.code + trace->after_offset;
                st->native_trace_exec_hits++;
                trace->native_trace_exec_hits++;
                if (trace->native_trace_direct_core) st->native_trace_exec_hits_direct_core++;
                trace->native_trace_exit_hits++;
                st->backend_loop_exec_hits++;
                st->loop_exec_hits++;
                trace->exec_hits++;
                return 1;
            } else if (r == TOI_JIT_NATIVE_EXEC_CONTINUE) {
                uint32_t off = (exec_state.resume_offset == 0xffffffffu)
                    ? trace->loop_start_offset
                    : exec_state.resume_offset;
                if ((exec_state.reason == TOI_JIT_NATIVE_DEOPT_CALL_TARGET ||
                     exec_state.reason == TOI_JIT_NATIVE_DEOPT_INVOKE_TARGET) &&
                    jit_try_stitch_soft_continue_calls(vm, frame, &exec_state, &off)) {
                    exec_state.reason = TOI_JIT_NATIVE_DEOPT_NONE;
                }
                if (off < (uint32_t)frame->closure->function->chunk.count) {
                    if (needs_buffered_slots) {
                        for (int i = 0; i < frame_window; i++) frame->slots[i] = work[i];
                    }
                    if (exec_state.stack_count > 0u) {
                        int stack_count = (int)exec_state.stack_count;
                        if (frame_window + stack_count >= thread->stack_capacity) {
                            st->native_trace_guard_failures++;
                            trace->native_trace_guard_failures++;
                            st->native_trace_deopts++;
                            trace->native_trace_deopts++;
                            st->native_trace_deopt_opcode_counts[exec_state.op]++;
                            if (exec_state.reason < 32u) st->native_trace_deopt_reason_counts[exec_state.reason]++;
                            return 0;
                        }
                        for (int i = 0; i < stack_count; i++) {
                            frame->slots[frame_window + i] = exec_state.stack[i];
                        }
                        thread->stack_top = frame->slots + frame_window + stack_count;
                    }
                    *ip = frame->closure->function->chunk.code + off;
                    st->native_trace_exec_hits++;
                    trace->native_trace_exec_hits++;
                    if (trace->native_trace_direct_core) st->native_trace_exec_hits_direct_core++;
                    trace->native_trace_continue_hits++;
                    if (exec_state.reason == TOI_JIT_NATIVE_DEOPT_CALL_TARGET ||
                        exec_state.reason == TOI_JIT_NATIVE_DEOPT_INVOKE_TARGET) {
                        trace->native_trace_soft_bails++;
                    }
                    st->backend_loop_exec_hits++;
                    st->loop_exec_hits++;
                    trace->exec_hits++;
                    return 1;
                }
                st->native_trace_guard_failures++;
                trace->native_trace_guard_failures++;
                st->native_trace_deopts++;
                trace->native_trace_deopts++;
                st->native_trace_deopt_opcode_counts[exec_state.op]++;
                if (exec_state.reason < 32u) st->native_trace_deopt_reason_counts[exec_state.reason]++;
                if (trace->native_trace_exec_safe &&
                    trace->native_trace_direct_core &&
                    trace->native_trace_exec_hits == 0 &&
                    trace->native_trace_deopts >= 2) {
                    trace->native_trace_exec_safe = 0;
                    st->native_trace_runtime_disabled++;
                }
            } else {
                st->native_trace_guard_failures++;
                trace->native_trace_guard_failures++;
                st->native_trace_deopts++;
                trace->native_trace_deopts++;
                st->native_trace_deopt_opcode_counts[exec_state.op]++;
                if (exec_state.reason < 32u) st->native_trace_deopt_reason_counts[exec_state.reason]++;
                if (trace->native_trace_exec_safe &&
                    trace->native_trace_direct_core &&
                    trace->native_trace_exec_hits == 0 &&
                    trace->native_trace_deopts >= 2) {
                    trace->native_trace_exec_safe = 0;
                    st->native_trace_runtime_disabled++;
                }
            }
        } else {
            st->native_trace_guard_failures++;
            trace->native_trace_guard_failures++;
        }
    }
    if (st->native_trace_exec_enabled &&
        !st->native_trace_exec_globally_disabled &&
        st->native_trace_exec_attempts >= 64 &&
        ((double)st->native_trace_deopts / (double)st->native_trace_exec_attempts) > 0.90) {
        st->native_trace_exec_globally_disabled = 1;
        st->native_trace_exec_disable_reason = 3;
    }
    if (st->loop_value_fast_enabled &&
        !trace->native_trace_direct_core &&
        trace->native_value_loop_fn != NULL &&
        trace->value_fast_eligible &&
        !trace->value_fast_disabled) {
        st->backend_loop_exec_attempts++;
        st->backend_value_loop_exec_attempts++;
        trace->value_fast_attempts++;
        int frame_window = (int)trace->max_slot + 1;
        if (frame_window > 0 && frame_window <= 8192 &&
            trace->native_value_instr_count > 0 &&
            trace->native_value_instr_count <= 256) {
            Value work[frame_window];
            for (int i = 0; i < frame_window; i++) work[i] = frame->slots[i];
            if (trace->native_value_loop_fn(
                    vm,
                    frame->closure,
                    work,
                    frame_window,
                    trace->native_value_instrs,
                    (int)trace->native_value_instr_count,
                    trace->native_value_var_slot,
                    trace->native_value_end_slot)) {
                for (int i = 0; i < frame_window; i++) frame->slots[i] = work[i];
                *ip = frame->closure->function->chunk.code + trace->after_offset;
                st->backend_value_loop_exec_hits++;
                st->backend_loop_exec_hits++;
                trace->value_fast_hits++;
                st->loop_exec_hits++;
                trace->exec_hits++;
                return 1;
            }
        }
        if (trace->value_fast_attempts >= 8 && trace->value_fast_hits == 0) {
            trace->value_fast_disabled = 1;
            st->backend_value_loop_runtime_disabled++;
        } else if (trace->value_fast_attempts >= 16 &&
                   ((double)trace->value_fast_hits / (double)trace->value_fast_attempts) < 0.10) {
            trace->value_fast_disabled = 1;
            st->backend_value_loop_runtime_disabled++;
        } else if (trace->value_fast_attempts >= 64 &&
                   ((double)trace->value_fast_hits / (double)trace->value_fast_attempts) < 0.25) {
            trace->value_fast_disabled = 1;
            st->backend_value_loop_runtime_disabled++;
        }
    }
    if (trace->native_slot_loop_fn != NULL) {
        st->backend_loop_exec_attempts++;
        int slot_count = (int)trace->native_slot_loop_slot_count;
        int max_local = (int)trace->max_slot + 1;
        int var_slot = (int)trace->native_loop_var_slot;
        int end_slot = (int)trace->native_loop_end_slot;
        if (slot_count > 0 && slot_count <= 255 &&
            slot_count <= max_local &&
            var_slot >= 0 && var_slot < slot_count &&
            end_slot >= 0 && end_slot < slot_count) {
            double slots[256];
            for (int i = 0; i < slot_count; i++) {
                Value v = frame->slots[i];
                slots[i] = IS_NUMBER(v) ? AS_NUMBER(v) : 0.0;
            }
            int ok = 1;
            for (int i = 0; i < (int)trace->native_slot_loop_used_count; i++) {
                int s = (int)trace->native_slot_loop_used_slots[i];
                if (s < 0 || s >= slot_count || !IS_NUMBER(frame->slots[s])) {
                    ok = 0;
                    break;
                }
            }
            if (ok && trace->native_slot_loop_upvalue_const_count != 0) {
                for (int i = 0; i < (int)trace->native_slot_loop_upvalue_const_count; i++) {
                    int const_idx = (int)trace->native_slot_loop_upvalue_const_idx[i];
                    int up_idx = (int)trace->native_slot_loop_upvalue_idx[i];
                    if (const_idx < 0 || const_idx >= 32 ||
                        up_idx < 0 || up_idx >= frame->closure->upvalue_count ||
                        frame->closure->upvalues[up_idx] == NULL ||
                        frame->closure->upvalues[up_idx]->location == NULL) {
                        ok = 0;
                        break;
                    }
                    Value uv = *frame->closure->upvalues[up_idx]->location;
                    if (!IS_NUMBER(uv)) {
                        ok = 0;
                        break;
                    }
                    trace->native_loop_consts[const_idx] = AS_NUMBER(uv);
                }
            }
            if (ok && (!IS_NUMBER(frame->slots[var_slot]) || !IS_NUMBER(frame->slots[end_slot]))) {
                ok = 0;
            }
            if (ok) {
                trace->native_slot_loop_fn(slots, trace->native_loop_consts);
                frame->slots[var_slot] = NUMBER_VAL(slots[var_slot]);
                for (int i = 0; i < (int)trace->native_slot_loop_written_count; i++) {
                    int s = (int)trace->native_slot_loop_written_slots[i];
                    if (s >= 0 && s < slot_count) {
                        frame->slots[s] = NUMBER_VAL(slots[s]);
                    }
                }
                *ip = frame->closure->function->chunk.code + trace->after_offset;
                st->backend_loop_exec_hits++;
                st->loop_exec_hits++;
                trace->exec_hits++;
                return 1;
            }
        }
    }
    if (trace->native_numeric_loop_fn != NULL) {
        st->backend_loop_exec_attempts++;
        int acc_slot = (int)trace->native_loop_acc_slot;
        int var_slot = (int)trace->native_loop_var_slot;
        int end_slot = (int)trace->native_loop_end_slot;
        int max_local = (int)trace->max_slot + 1;
        if (acc_slot >= 0 && acc_slot < max_local &&
            var_slot >= 0 && var_slot < max_local &&
            end_slot >= 0 && end_slot < max_local) {
            Value acc_v = frame->slots[acc_slot];
            Value var_v = frame->slots[var_slot];
            Value end_v = frame->slots[end_slot];
            if (IS_NUMBER(acc_v) && IS_NUMBER(var_v) && IS_NUMBER(end_v)) {
                double acc = AS_NUMBER(acc_v);
                double var = AS_NUMBER(var_v);
                double end = AS_NUMBER(end_v);
                trace->native_numeric_loop_fn(&acc, &var, &end, trace->native_loop_consts);
                frame->slots[acc_slot] = NUMBER_VAL(acc);
                frame->slots[var_slot] = NUMBER_VAL(var);
                *ip = frame->closure->function->chunk.code + trace->after_offset;
                st->backend_loop_exec_hits++;
                st->loop_exec_hits++;
                trace->exec_hits++;
                return 1;
            }
        }
    }
    if (st->loop_numeric_fast_enabled && trace->numeric_fast_eligible) {
        st->loop_numeric_fast_attempts++;
        if (execute_numeric_trace_fast(st, trace, frame, thread, ip)) {
            st->loop_numeric_fast_hits++;
            return 1;
        }
    }
    if (trace->typed_fast_eligible && !trace->typed_fast_disabled && !trace->mixed_call_heavy) {
        trace->typed_fast_attempts++;
        st->loop_typed_fast_attempts++;
        if (execute_typed_trace_fast(vm, st, function, trace, frame, thread, ip)) {
            trace->typed_fast_hits++;
            st->loop_typed_fast_hits++;
            return 1;
        }
        if (trace->typed_fast_attempts >= 32 && trace->typed_fast_hits == 0) {
            trace->typed_fast_disabled = 1;
        }
    }
    if (trace->native_qfor_empty_fn != NULL && trace_is_empty_qfor(trace)) {
        st->backend_loop_exec_attempts++;
        Chunk* chunk = &frame->closure->function->chunk;
        if (thread != NULL && opcode_offset + 2 < chunk->count) {
            int var_slot = (int)chunk->code[opcode_offset + 1];
            int end_slot = (int)chunk->code[opcode_offset + 2];
            ptrdiff_t depth = thread->stack_top - frame->slots;
            int frame_window = (depth < 0) ? 0 : (int)depth;
            if (var_slot >= 0 && var_slot < frame_window && end_slot >= 0 && end_slot < frame_window) {
                Value v = frame->slots[var_slot];
                Value end = frame->slots[end_slot];
                int64_t var_i = 0;
                int64_t end_i = 0;
                if (IS_NUMBER(v) && IS_NUMBER(end) &&
                    to_int64_local(AS_NUMBER(v), &var_i) &&
                    to_int64_local(AS_NUMBER(end), &end_i) &&
                    var_i < INT64_MAX) {
                    uint64_t out_u = trace->native_qfor_empty_fn((uint64_t)var_i, (uint64_t)end_i);
                    int64_t out_i = (int64_t)out_u;
                    frame->slots[var_slot] = NUMBER_VAL((double)out_i);
                    *ip = frame->closure->function->chunk.code + trace->after_offset;
                    st->backend_loop_exec_hits++;
                    st->loop_exec_hits++;
                    trace->exec_hits++;
                    return 1;
                }
            }
        }
    }
    if (!trace->has_program || trace->ops == NULL || trace->op_count <= 0) return 0;
    // Loops whose bodies perform an interpreter-executed (IR) call every iteration
    // are much slower inside this mixed executor than in the plain VM, where the
    // call can use the quickened/OP_CALL1 path. Once detected, fall back for good.
    if (trace->mixed_call_heavy) return 0;

    ptrdiff_t depth = thread->stack_top - frame->slots;
    if (depth < 0) return 0;
    int sp = (int)depth;
    int frame_window = sp;
    if ((int)trace->max_slot + 1 > frame_window) frame_window = (int)trace->max_slot + 1;
    if (frame_window < 0 || frame_window > 8192) return 0;
    if (trace->max_stack <= 0 || trace->max_stack > 8192) return 0;
    int work_cap = trace->max_stack;
    if (frame_window > work_cap) work_cap = frame_window;
    if (work_cap <= 0 || work_cap > 8192) return 0;
    Chunk* chunk = &frame->closure->function->chunk;
    Value work[work_cap];
    for (int j = 0; j < frame_window; j++) {
        if (j < sp) work[j] = frame->slots[j];
        else work[j] = NIL_VAL;
    }

    int pc = (int)trace->entry_pc;
    uint32_t iter_guard = 0;
    uint32_t loop_chunk_iters = 0;
    const uint32_t loop_chunk_budget = 4096;
    const int debug_mixed_loop = jit_debug_mixed_loop_matches_fn(function);
    int debug_mixed_left = debug_mixed_loop ? jit_debug_mixed_loop_limit() : 0;
#define DEBUG_MIXED_LOOP(...)                                                                                           \
    do {                                                                                                                \
        if (debug_mixed_loop && debug_mixed_left > 0) {                                                                \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            debug_mixed_left--;                                                                                         \
        }                                                                                                               \
    } while (0)
#define LOOP_FAIL(reason)                                                                                               \
    do {                                                                                                                \
        if (jit_dump_loops_enabled() && trace->exec_hits == 0 && trace->exec_attempts <= 1) {                         \
            fprintf(stderr,                                                                                             \
                    "[jit-loop-exec-fail] fn=%s at=%d pc=%d op=%s sp=%d reason=%s\n",                                 \
                    (function->name != NULL) ? function->name->chars : "<script>",                                    \
                    opcode_offset,                                                                                      \
                    pc,                                                                                                 \
                    jit_op_name(op->op),                                                                               \
                    sp,                                                                                                 \
                    reason);                                                                                            \
        }                                                                                                               \
        /* Never drop accumulated locals: commit them and resume in the VM at */                                        \
        /* the current operation. Silently discarding `work` loses loop state. */                                       \
        return commit_loop_state(                                                                                       \
            st, trace, frame, thread, ip, work, sp, frame_window, trace->ops[pc].byte_offset);                          \
    } while (0)
#define LOOP_FAIL_CALL(reason)                                                                                          \
    do {                                                                                                                \
        if (trace->call_stall_runs < 0xFFFFu) trace->call_stall_runs++;                                                 \
        LOOP_FAIL(reason);                                                                                              \
    } while (0)
    while (pc >= 0 && pc < trace->op_count) {
        // Too many interpreter-executed (IR) calls in this loop body: hand the
        // loop back to the VM. The switch happens at the next loop backedge so a
        // partially executed iteration is never replayed (which would duplicate
        // its side effects).
        if (++iter_guard > 20000000u) {
            // Never discard work: commit what we have and let the VM continue at
            // the current bytecode offset. Guarantees one iteration of progress
            // per JIT entry instead of a full-loop livelock.
            if (jit_dump_loops_enabled()) {
                fprintf(stderr,
                        "[jit-loop-exec-fail] fn=%s at=%d pc=%d op=<guard> sp=%d reason=iter-guard\n",
                        (function->name != NULL) ? function->name->chars : "<script>",
                        opcode_offset,
                        pc,
                        sp);
            }
            return commit_loop_state(
                st, trace, frame, thread, ip, work, sp, frame_window, trace->ops[pc].byte_offset);
        }
        JitOp* op = &trace->ops[pc];
        switch (op->op) {
                    case JOP_CONSTANT:
                        if (sp >= trace->max_stack) return 0;
                        work[sp++] = op->constant;
                        pc++;
                        break;
                    case JOP_NIL:
                        if (sp >= trace->max_stack) return 0;
                        work[sp++] = NIL_VAL;
                        pc++;
                        break;
                    case JOP_TRUE:
                        if (sp >= trace->max_stack) return 0;
                        work[sp++] = BOOL_VAL(1);
                        pc++;
                        break;
                    case JOP_FALSE:
                        if (sp >= trace->max_stack) return 0;
                        work[sp++] = BOOL_VAL(0);
                        pc++;
                        break;
                    case JOP_POP:
                        if (sp <= 0) return 0;
                        sp--;
                        pc++;
                        break;
                    case JOP_GET_LOCAL:
                        if ((int)op->a >= frame_window || sp >= trace->max_stack) return 0;
                        work[sp++] = work[op->a];
                        pc++;
                        break;
                    case JOP_GET_UPVALUE:
                        if ((int)op->a >= frame->closure->upvalue_count || sp >= trace->max_stack) return 0;
                        if (frame->closure->upvalues[op->a] == NULL ||
                            frame->closure->upvalues[op->a]->location == NULL) return 0;
                        work[sp++] = *frame->closure->upvalues[op->a]->location;
                        pc++;
                        break;
                    case JOP_GET2_LOCAL:
                        if ((int)op->a >= frame_window || (int)op->b >= frame_window || sp + 1 >= trace->max_stack) return 0;
                        work[sp++] = work[op->a];
                        work[sp++] = work[op->b];
                        pc++;
                        break;
                    case JOP_SET_LOCAL:
                        if ((int)op->a >= frame_window || sp <= 0) return 0;
                        work[op->a] = work[sp - 1];
                        pc++;
                        break;
                    case JOP_SET_LOCAL_POP:
                        if ((int)op->a >= frame_window || sp <= 0) return 0;
                        work[op->a] = work[sp - 1];
                        sp--;
                        pc++;
                        break;
                    case JOP_APPEND: {
                        if (sp < 2) return 0;
                        Value v = work[sp - 1];
                        Value tval = work[sp - 2];
                        if (!IS_TABLE(tval)) return 0;
                        ObjTable* t = AS_TABLE(tval);
                        if (!t->no_metatable_fast) return 0;
                        int idx = t->table.array_max + 1;
                        if (!table_set_array(&t->table, idx, v)) {
                            ObjString* key = number_key_string((double)idx);
                            table_set(&t->table, key, v);
                        }
                        work[sp - 2] = NUMBER_VAL((double)idx);
                        sp--;
                        pc++;
                        break;
                    }
                    case JOP_ADD:
                    case JOP_ADD_INPLACE:
                    case JOP_SUBTRACT:
                    case JOP_MULTIPLY:
                    case JOP_DIVIDE:
                    case JOP_MODULO:
                    case JOP_IADD:
                    case JOP_ISUB:
                    case JOP_IMUL:
                    case JOP_IDIV:
                    case JOP_IMOD:
                    case JOP_FADD:
                    case JOP_FSUB:
                    case JOP_FMUL:
                    case JOP_FDIV:
                    case JOP_FMOD:
                    case JOP_QFADD:
                    case JOP_QFSUB:
                    case JOP_QFMUL:
                    case JOP_QFDIV:
                    case JOP_QFMOD: {
                        if (sp < 2) return 0;
                        Value b = work[sp - 1];
                        Value a = work[sp - 2];
                        if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
                        work[sp - 2] = numeric_binop(op->op, AS_NUMBER(a), AS_NUMBER(b));
                        sp--;
                        pc++;
                        break;
                    }
                    case JOP_NEGATE:
                        if (sp <= 0 || !IS_NUMBER(work[sp - 1])) return 0;
                        work[sp - 1] = NUMBER_VAL(-AS_NUMBER(work[sp - 1]));
                        pc++;
                        break;
                    case JOP_NOT:
                        if (sp <= 0) return 0;
                        work[sp - 1] = BOOL_VAL(is_falsey_jit(work[sp - 1]));
                        pc++;
                        break;
                    case JOP_EQUAL: {
                        if (sp < 2) return 0;
                        Value b = work[sp - 1];
                        Value a = work[sp - 2];
                        int eq = jit_values_equal(a, b);
                        work[sp - 2] = BOOL_VAL(eq);
                        sp--;
                        pc++;
                        break;
                    }
                    case JOP_QGREATER_NUM:
                    case JOP_QLESS_NUM: {
                        if (sp < 2) return 0;
                        Value b = work[sp - 1];
                        Value a = work[sp - 2];
                        if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
                        int cmp = (op->op == JOP_QGREATER_NUM) ? (AS_NUMBER(a) > AS_NUMBER(b))
                                                               : (AS_NUMBER(a) < AS_NUMBER(b));
                        work[sp - 2] = BOOL_VAL(cmp);
                        sp--;
                        pc++;
                        break;
                    }
                    case JOP_JUMP:
                        if (op->target == JIT_TARGET_EXIT) {
                            return commit_loop_state(
                                st,
                                trace,
                                frame,
                                thread,
                                ip,
                                work,
                                sp,
                                frame_window,
                                trace->after_offset);
                        }
                        // Generic backedge (while / iterator loops): chunk already
                        // executed iterations so a long loop cannot hit the IR
                        // guard and livelock against the VM.
                        if ((int)op->target < pc &&
                            (trace->mixed_call_heavy || ++loop_chunk_iters >= loop_chunk_budget)) {
                            trace->call_stall_runs = 0;
                            if (jit_dump_loops_enabled() && trace->exec_hits == 0 && trace->exec_attempts <= 1) {
                                fprintf(stderr,
                                        "[jit-loop-chunk] fn=%s at=%d iters=%u reason=backedge-budget\n",
                                        (function->name != NULL) ? function->name->chars : "<script>",
                                        opcode_offset,
                                        (unsigned)loop_chunk_iters);
                            }
                            return commit_loop_state(
                                st,
                                trace,
                                frame,
                                thread,
                                ip,
                                work,
                                sp,
                                frame_window,
                                trace->loop_start_offset);
                        }
                        pc = (int)op->target;
                        break;
                    case JOP_JUMP_IF_FALSE:
                        if (sp <= 0) return 0;
                        if (is_falsey_jit(work[sp - 1])) {
                            if (op->target == JIT_TARGET_EXIT) {
                                return commit_loop_state(
                                    st,
                                    trace,
                                    frame,
                                    thread,
                                    ip,
                                    work,
                                    sp,
                                    frame_window,
                                    trace->after_offset);
                            }
                            pc = (int)op->target;
                        } else {
                            pc = pc + 1;
                        }
                        break;
                    case JOP_JUMP_IF_TRUE:
                        if (sp <= 0) return 0;
                        if (is_falsey_jit(work[sp - 1])) {
                            pc = pc + 1;
                        } else {
                            if (op->target == JIT_TARGET_EXIT) {
                                return commit_loop_state(
                                    st,
                                    trace,
                                    frame,
                                    thread,
                                    ip,
                                    work,
                                    sp,
                                    frame_window,
                                    trace->after_offset);
                            }
                            pc = (int)op->target;
                        }
                        break;
                    case JOP_CALL: {
                        int argc = (int)op->a;
                        if (argc < 0 || sp < argc + 1) LOOP_FAIL("call-stack-shape");
                        int callee_idx = sp - 1 - argc;
                        Value callee = work[callee_idx];
                        DEBUG_MIXED_LOOP(
                            "[jit-mixed-loop] fn=%s at=%d pc=%d op=CALL pre sp=%d argc=%d callee=%s arg0=%s arg1=%s\n",
                            (function->name != NULL) ? function->name->chars : "<script>",
                            opcode_offset,
                            pc,
                            sp,
                            argc,
                            jit_value_kind_name(callee),
                            (argc > 0) ? jit_value_kind_name(work[callee_idx + 1]) : "-",
                            (argc > 1) ? jit_value_kind_name(work[callee_idx + 2]) : "-");
                        Value out = NIL_VAL;
                        // Closure calls inside a trace go through the IR call
                        // cache, which is not yet reliable for every shape.
                        // Finish this iteration, then hand the loop back to the
                        // VM's (correct) quickened call path.
                        // TODO(jit-call-path): drop this once the call cache is
                        // validated by tests/ with the JIT forced hot.
                                int ic_hit = jit_try_trace_call_ic(
                                vm,
                                function,
                                trace,
                                op,
                                pc,
                                callee,
                                &work[callee_idx + 1],
                                argc,
                                &out);
                        if (!ic_hit &&
                            IS_CLOSURE(callee) &&
                            jit_try_inline_trace_call(trace, pc, callee, &work[callee_idx + 1], argc, &out)) {
                            work[callee_idx] = out;
                            sp = callee_idx + 1;
                        } else if (!ic_hit &&
                            IS_CLOSURE(callee)) {
                            // A closure executed through the whole-function IR
                            // interpreter: track it so we can bail to the VM.
                            if (!jit_try_safe_loop_closure_call(
                                    vm,
                                    AS_CLOSURE(callee),
                                    &work[callee_idx + 1],
                                    argc,
                                    &out,
                                    NULL)) {
                                LOOP_FAIL_CALL("call-closure-unsupported");
                            }
                            // The call consumed the callee and its arguments;
                            // leave exactly one result on the stack like every
                            // other call path does.
                            work[callee_idx] = out;
                            sp = callee_idx + 1;
                        } else if (!ic_hit &&
                                   1) {
                            int out_count = 0;
                            if (!jit_call_primitive_multi(
                                    vm,
                                    callee,
                                    &work[callee_idx + 1],
                                    argc,
                                    &work[callee_idx],
                                    trace->max_stack - callee_idx,
                                    &out_count)) {
                                LOOP_FAIL_CALL("call-primitive-miss");
                            }
                            if (out_count <= 0) {
                                LOOP_FAIL_CALL("call-zero-return");
                            }
                            out = work[callee_idx];
                            sp = callee_idx + out_count;
                        }
                        if (!ic_hit) {
                            jit_update_trace_call_ic(vm, trace, pc, callee, argc);
                        }
                        if (ic_hit) {
                            // IC hit means the callee ran via execute_program (IR).
                            work[callee_idx] = out;
                            sp = callee_idx + 1;
                        }
                        DEBUG_MIXED_LOOP(
                            "[jit-mixed-loop] fn=%s at=%d pc=%d op=CALL post sp=%d out=%s ic=%d\n",
                            (function->name != NULL) ? function->name->chars : "<script>",
                            opcode_offset,
                            pc,
                            sp,
                            jit_value_kind_name(out),
                            ic_hit);
                        pc++;
                        break;
                    }
                    case JOP_INVOKE: {
                        int argc = (int)op->a;
                        if (argc < 0 || sp < argc + 1) LOOP_FAIL("invoke-stack-shape");
                        int recv_idx = sp - 1 - argc;
                        Value receiver = work[recv_idx];
                        DEBUG_MIXED_LOOP(
                            "[jit-mixed-loop] fn=%s at=%d pc=%d op=INVOKE pre sp=%d argc=%d recv=%s\n",
                            (function->name != NULL) ? function->name->chars : "<script>",
                            opcode_offset,
                            pc,
                            sp,
                            argc,
                            jit_value_kind_name(receiver));
                        Value out = NIL_VAL;
                        if (!jit_invoke_primitive(vm, receiver, op->constant, &work[recv_idx + 1], argc, &out)) {
                            LOOP_FAIL_CALL("invoke-unsupported");
                        }
                        work[recv_idx] = out;
                        sp = recv_idx + 1;
                        DEBUG_MIXED_LOOP(
                            "[jit-mixed-loop] fn=%s at=%d pc=%d op=INVOKE post sp=%d out=%s method=%s\n",
                            (function->name != NULL) ? function->name->chars : "<script>",
                            opcode_offset,
                            pc,
                            sp,
                            jit_value_kind_name(out),
                            IS_STRING(op->constant)
                                ? AS_STRING(op->constant)->chars
                                : "<non-string>");
                        pc++;
                        break;
                    }
                    case JOP_LENGTH: {
                        if (sp <= 0) return 0;
                        Value out = NIL_VAL;
                        if (!jit_length_primitive(work[sp - 1], &out)) return 0;
                        work[sp - 1] = out;
                        pc++;
                        break;
                    }
                    case JOP_SLICE: {
                        if (sp < 4) return 0;
                        Value out = NIL_VAL;
                        if (!jit_slice_primitive(work[sp - 4], work[sp - 3], work[sp - 2], work[sp - 1], &out)) return 0;
                        work[sp - 4] = out;
                        sp -= 3;
                        pc++;
                        break;
                    }
                    case JOP_BUILD_STRING2:
                    case JOP_BUILD_STRING3:
                    case JOP_BUILD_STRING4:
                    case JOP_BUILD_STRING5: {
                        int n = 0;
                        if (op->op == JOP_BUILD_STRING2) n = 2;
                        else if (op->op == JOP_BUILD_STRING3) n = 3;
                        else if (op->op == JOP_BUILD_STRING4) n = 4;
                        else n = 5;
                        if (sp < n) return 0;
                        Value out = NIL_VAL;
                        if (!jit_build_string_primitive(vm, &work[sp - n], n, &out)) return 0;
                        work[sp - n] = out;
                        sp = sp - n + 1;
                        pc++;
                        break;
                    }
                    case JOP_QCALL2: {
                        if (sp < 3) return 0;
                        Value callee = work[sp - 3];
                        Value arg0 = work[sp - 2];
                        Value arg1 = work[sp - 1];
                        Value out = NIL_VAL;
                                if (IS_CLOSURE(callee)) {
                            ObjClosure* c = AS_CLOSURE(callee);
                            ObjFunction* fn = c->function;
                            if (fn != NULL &&
                                fn->tiny_inline_kind != TINY_INLINE_NONE &&
                                jit_eval_tiny_inline2_local(fn, arg0, arg1, &out)) {
                                work[sp - 3] = out;
                                sp -= 2;
                                pc++;
                                break;
                            }
                            // Small straight-line callees run in place; otherwise
                            // fall back to the per-site call cache and finally to
                            // the whole-program IR interpreter.
                            if (jit_try_inline_trace_call(trace, pc, callee, &work[sp - 2], 2, &out)) {
                                work[sp - 3] = out;
                                sp -= 2;
                                pc++;
                                break;
                            }
                            int ic_hit = jit_try_trace_call_ic(
                                vm, function, trace, op, pc, callee, &work[sp - 2], 2, &out);
                            if (!ic_hit) {
                                if (!jit_try_safe_loop_closure_call(vm, c, &work[sp - 2], 2, &out, NULL)) {
                                    LOOP_FAIL_CALL("qcall2-closure-unsupported");
                                }
                                jit_update_trace_call_ic(vm, trace, pc, callee, 2);
                            }
                            work[sp - 3] = out;
                            sp -= 2;
                            pc++;
                            break;
                        }
                        if (!jit_call_primitive(vm, callee, &work[sp - 2], 2, &out)) {
                            LOOP_FAIL_CALL("qcall2-call-miss");
                        }
                        work[sp - 3] = out;
                        sp -= 2;
                        pc++;
                        break;
                    }
                    case JOP_QFOR_LOOP_NUM: {
                        if ((int)op->a >= frame_window || (int)op->b >= frame_window) return 0;
                        Value v = work[op->a];
                        Value end = work[op->b];
                        if (!IS_NUMBER(v) || !IS_NUMBER(end)) return 0;
                        double next = AS_NUMBER(v) + 1.0;
                        work[op->a] = NUMBER_VAL(next);
                        int is_trace_backedge = (pc == trace->op_count - 1);
                        if (next <= AS_NUMBER(end)) {
                            pc = (int)op->target;
                            if (is_trace_backedge &&
                                (trace->mixed_call_heavy || ++loop_chunk_iters >= loop_chunk_budget)) {
                                trace->call_stall_runs = 0;
                                return commit_loop_state(
                                    st,
                                    trace,
                                    frame,
                                    thread,
                                    ip,
                                    work,
                                    sp,
                                    frame_window,
                                    trace->loop_start_offset);
                            }
                        } else if (is_trace_backedge) {
                            return commit_loop_state(
                                st,
                                trace,
                                frame,
                                thread,
                                ip,
                                work,
                                sp,
                                frame_window,
                                trace->after_offset);
                        } else {
                            // Inner loop of a nested trace: fall through to the
                            // rest of the body instead of leaving the trace.
                            pc++;
                        }
                        break;
                    }
                    case JOP_QFOR_PREP_NUM: {
                        if ((int)op->a >= frame_window || (int)op->b >= frame_window) return 0;
                        Value v = work[op->a];
                        Value end = work[op->b];
                        if (!IS_NUMBER(v) || !IS_NUMBER(end)) return 0;
                        if (AS_NUMBER(v) > AS_NUMBER(end)) {
                            if (op->target == JIT_TARGET_EXIT) {
                                return commit_loop_state(
                                    st,
                                    trace,
                                    frame,
                                    thread,
                                    ip,
                                    work,
                                    sp,
                                    frame_window,
                                    trace->after_offset);
                            }
                            pc = (int)op->target;
                        } else {
                            pc = pc + 1;
                        }
                        break;
                    }
                    case JOP_GET_TABLE_NUM:
                    case JOP_GET_TABLE: {
                        if (sp < 2) LOOP_FAIL("get-table-stack-shape");
                        Value key = work[sp - 1];
                        Value table = work[sp - 2];
                        if (!IS_TABLE(table)) LOOP_FAIL("get-table-not-table");
                        ObjTable* t = AS_TABLE(table);
                        Value result = NIL_VAL;
                        if (IS_NUMBER(key)) {
                            double num_key = AS_NUMBER(key);
                            int idx = (int)num_key;
                            if (num_key != (double)idx) LOOP_FAIL("get-table-non-int-num-key");
                            if (idx < 0) idx = t->table.array_max + idx + 1;
                            if (idx >= 1 && idx <= t->table.array_capacity) {
                                Value arr_val = t->table.array[idx - 1];
                                if (!IS_NIL(arr_val)) {
                                    work[sp - 2] = arr_val;
                                    sp--;
                                    pc++;
                                    break;
                                }
                            }
                            if (table_get_array(&t->table, idx, &result)) {
                                work[sp - 2] = result;
                                sp--;
                                pc++;
                                break;
                            }
                            if (op->op == JOP_GET_TABLE_NUM && t->no_metatable_fast) {
                                work[sp - 2] = NIL_VAL;
                                sp--;
                                pc++;
                                break;
                            }
                            LOOP_FAIL("get-table-num-fallback");
                        }
                        if (op->op != JOP_GET_TABLE || !IS_STRING(key)) LOOP_FAIL("get-table-key-type");
                        if (!t->no_metatable_fast) LOOP_FAIL("get-table-metatable");
                        ObjString* key_str = AS_STRING(key);
                        int site = (int)op->byte_offset;
                        int site_ok = (site >= 0 && site < chunk->capacity);
                        if (site_ok &&
                            chunk->get_table_ic_tables != NULL &&
                            chunk->get_table_ic_slots != NULL &&
                            chunk->get_table_ic_versions != NULL &&
                            chunk->get_table_ic_keys != NULL) {
                            int exact_hit = 0;
                            int ic_hit = 0;
                            ic_hit = jit_get_table_ic_try_lane(
                                t, key_str, t->table.shape_version,
                                chunk->get_table_ic_tables,
                                chunk->get_table_ic_keys,
                                chunk->get_table_ic_slots,
                                chunk->get_table_ic_versions,
                                site,
                                &result,
                                &exact_hit);
                            if (ic_hit) {
                                work[sp - 2] = result;
                                sp--;
                                pc++;
                                break;
                            }
                        }
                        int slot = -1;
                        if (table_get_with_slot(&t->table, key_str, &result, &slot)) {
                            if (site_ok) {
                                jit_update_get_table_ic(chunk, site, t, key_str, slot);
                            }
                            work[sp - 2] = result;
                        } else {
                            if (site_ok) {
                                jit_update_get_table_ic(chunk, site, t, key_str, -2);
                            }
                            work[sp - 2] = NIL_VAL;
                        }
                        sp--;
                        pc++;
                        break;
                    }
                    case JOP_SET_TABLE:
                    case JOP_SET_TABLE_NUM: {
                        if (sp < 3) LOOP_FAIL("set-table-stack-shape");
                        Value value = work[sp - 1];
                        Value key = work[sp - 2];
                        Value table = work[sp - 3];
                        DEBUG_MIXED_LOOP(
                            "[jit-mixed-loop] fn=%s at=%d pc=%d op=%s pre sp=%d table=%s key=%s value=%s\n",
                            (function->name != NULL) ? function->name->chars : "<script>",
                            opcode_offset,
                            pc,
                            jit_op_name(op->op),
                            sp,
                            jit_value_kind_name(table),
                            jit_value_kind_name(key),
                            jit_value_kind_name(value));
                        if (!IS_TABLE(table)) LOOP_FAIL("set-table-not-table");
                        ObjTable* t = AS_TABLE(table);
                        if (!t->no_metatable_fast) LOOP_FAIL("set-table-metatable");
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
                        } else if (op->op == JOP_SET_TABLE && IS_STRING(key)) {
                            ObjString* key_str = AS_STRING(key);
                            int site = (int)op->byte_offset;
                            int site_ok = (site >= 0 && site < chunk->capacity);
                            int done = 0;
                            if (site_ok &&
                                chunk->set_table_ic_tables != NULL &&
                                chunk->set_table_ic_versions != NULL &&
                                chunk->set_table_ic_slots != NULL &&
                                chunk->set_table_ic_tables[site] == t &&
                                objstring_equals_jit(chunk->set_table_ic_keys[site], key_str) &&
                                chunk->set_table_ic_versions[site] == t->table.shape_version) {
                                int slot = chunk->set_table_ic_slots[site];
                                if (table_set_at_slot_or_empty(&t->table, slot, key_str, value)) {
                                    chunk->set_table_ic_versions[site] = t->table.shape_version;
                                    done = 1;
                                }
                            }
                            if (!done &&
                                site_ok &&
                                chunk->set_table_ic_slots != NULL &&
                                chunk->set_table_ic_keys != NULL &&
                                objstring_equals_jit(chunk->set_table_ic_keys[site], key_str)) {
                                int slot = chunk->set_table_ic_slots[site];
                                if (table_set_at_slot_or_empty(&t->table, slot, key_str, value)) {
                                    if (chunk->set_table_ic_versions != NULL) {
                                        chunk->set_table_ic_versions[site] = t->table.shape_version;
                                    }
                                    done = 1;
                                }
                            }
                            if (!done) {
                                int slot = -1;
                                Value dummy = NIL_VAL;
                                if (table_get_with_slot(&t->table, key_str, &dummy, &slot)) {
                                    if (!table_set_at_slot(&t->table, slot, key_str, value)) {
                                        table_set_with_slot(&t->table, key_str, value, &slot);
                                    }
                                } else {
                                    table_set_with_slot(&t->table, key_str, value, &slot);
                                }
                                if (site_ok &&
                                    chunk->set_table_ic_tables != NULL &&
                                    chunk->set_table_ic_versions != NULL) {
                                    chunk->set_table_ic_tables[site] = t;
                                    chunk->set_table_ic_keys[site] = key_str;
                                    if (chunk->set_table_ic_slots != NULL) {
                                        chunk->set_table_ic_slots[site] = slot;
                                    }
                                    chunk->set_table_ic_versions[site] = t->table.shape_version;
                                }
                            }
                        } else {
                            LOOP_FAIL("set-table-key-type");
                        }
                        work[sp - 3] = value;
                        sp -= 2;
                        DEBUG_MIXED_LOOP(
                            "[jit-mixed-loop] fn=%s at=%d pc=%d op=%s post sp=%d out=%s\n",
                            (function->name != NULL) ? function->name->chars : "<script>",
                            opcode_offset,
                            pc,
                            jit_op_name(op->op),
                            sp,
                            jit_value_kind_name(work[sp - 1]));
                        pc++;
                        break;
                    }
                    case JOP_INC_LOCAL:
                    case JOP_SUB_LOCAL_CONST:
                    case JOP_MUL_LOCAL_CONST:
                    case JOP_DIV_LOCAL_CONST:
                    case JOP_MOD_LOCAL_CONST: {
                        if ((int)op->a >= frame_window || sp >= trace->max_stack) return 0;
                        Value a = work[op->a];
                        Value b = op->constant;
                        if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
                        double ad = AS_NUMBER(a);
                        double bd = AS_NUMBER(b);
                        Value out = NIL_VAL;
                        if (op->op == JOP_INC_LOCAL) out = NUMBER_VAL(ad + bd);
                        else if (op->op == JOP_SUB_LOCAL_CONST) out = NUMBER_VAL(ad - bd);
                        else if (op->op == JOP_MUL_LOCAL_CONST) out = NUMBER_VAL(ad * bd);
                        else if (op->op == JOP_DIV_LOCAL_CONST) out = NUMBER_VAL(ad / bd);
                        else out = numeric_binop(JOP_MODULO, ad, bd);
                        work[op->a] = out;
                        work[sp++] = out;
                        pc++;
                        break;
                    }
                    case JOP_ADD_CONST:
                    case JOP_SUB_CONST:
                    case JOP_MUL_CONST:
                    case JOP_DIV_CONST:
                    case JOP_MOD_CONST: {
                        if (sp <= 0) return 0;
                        Value a = work[sp - 1];
                        Value b = op->constant;
                        if (!IS_NUMBER(a) || !IS_NUMBER(b)) return 0;
                        double ad = AS_NUMBER(a);
                        double bd = AS_NUMBER(b);
                        if (op->op == JOP_ADD_CONST) work[sp - 1] = NUMBER_VAL(ad + bd);
                        else if (op->op == JOP_SUB_CONST) work[sp - 1] = NUMBER_VAL(ad - bd);
                        else if (op->op == JOP_MUL_CONST) work[sp - 1] = NUMBER_VAL(ad * bd);
                        else if (op->op == JOP_DIV_CONST) work[sp - 1] = NUMBER_VAL(ad / bd);
                        else work[sp - 1] = numeric_binop(JOP_MODULO, ad, bd);
                        pc++;
                        break;
                    }
                    case JOP_ADJUST_STACK:
                        if ((int)op->a < 0 || (int)op->a > trace->max_stack) return 0;
                        sp = (int)op->a;
                        pc++;
                        break;
                    case JOP_TINY_INLINE:
                    case JOP_RETURN:
                    default:
                        LOOP_FAIL("unsupported-op");
        }
    }
#undef DEBUG_MIXED_LOOP
#undef LOOP_FAIL
    return 0;
}

void toi_jit_print_stats(VM* vm) {
    if (vm == NULL || !vm->native_jit_enabled) return;
    ToiJitState* st = jit_state(vm);
    if (st == NULL) return;
    if (st->next_pipeline_enabled && st->next_pipeline != NULL) {
        JitPipelineStats ns;
        jit_pipeline_collect_stats(st->next_pipeline, &ns);
        fprintf(stderr,
                "[vm-native-jit-pipeline] enabled=1 hot_functions=%llu compile_attempts=%llu "
                "compile_success=%llu compile_fail=%llu exec_attempts=%llu exec_hits=%llu "
                "functions_built=%llu ir_ops=%llu ir_unsupported=%llu ir_calls=%llu ir_invokes=%llu ir_loops=%llu "
                "lowered_traces=%llu lowered_ops=%llu lowered_calls=%llu lowered_invokes=%llu lowered_unsupported=%llu "
                "lowered_callsites=%llu lowered_guard_hints=%llu "
                "link_edges=%llu link_resolved=%llu link_unresolved=%llu "
                "link_exec_enabled=%d link_patch_enabled=%d "
                "link_exec_attempts=%llu link_exec_hits=%llu "
                "link_patch_attempts=%llu link_patch_hits=%llu\n",
                (unsigned long long)ns.hot_functions,
                (unsigned long long)ns.compile_attempts,
                (unsigned long long)ns.compile_success,
                (unsigned long long)ns.compile_fail,
                (unsigned long long)ns.exec_attempts,
                (unsigned long long)ns.exec_hits,
                (unsigned long long)ns.functions_built,
                (unsigned long long)ns.ir_ops_total,
                (unsigned long long)ns.ir_unsupported_total,
                (unsigned long long)ns.ir_call_sites_total,
                (unsigned long long)ns.ir_invoke_sites_total,
                (unsigned long long)ns.ir_loop_sites_total,
                (unsigned long long)ns.lowered_trace_count,
                (unsigned long long)ns.lowered_trace_ops_total,
                (unsigned long long)ns.lowered_trace_calls_total,
                (unsigned long long)ns.lowered_trace_invokes_total,
                (unsigned long long)ns.lowered_trace_unsupported_total,
                (unsigned long long)ns.lowered_callsite_count,
                (unsigned long long)ns.lowered_callsite_guard_hints,
                (unsigned long long)ns.link_edge_total,
                (unsigned long long)ns.link_edge_resolved,
                (unsigned long long)ns.link_edge_unresolved,
                (int)ns.link_exec_enabled,
                (int)ns.link_patch_enabled,
                (unsigned long long)ns.link_exec_attempts,
                (unsigned long long)ns.link_exec_hits,
                (unsigned long long)ns.link_patch_attempts,
                (unsigned long long)ns.link_patch_hits);
    }
    int top_fail_opcode = -1;
    uint64_t top_fail_count = 0;
    for (int i = 0; i < 256; i++) {
        uint64_t c = st->loop_fail_opcode_counts[i];
        if (c > top_fail_count) {
            top_fail_count = c;
            top_fail_opcode = i;
        }
    }
    int top_slot_lower_fail_opcode = -1;
    uint64_t top_slot_lower_fail_count = 0;
    for (int i = 0; i < 256; i++) {
        uint64_t c = st->backend_slot_lower_fail_opcode_counts[i];
        if (c > top_slot_lower_fail_count) {
            top_slot_lower_fail_count = c;
            top_slot_lower_fail_opcode = i;
        }
    }
    int top_native_trace_unsafe_opcode = -1;
    uint64_t top_native_trace_unsafe_count = 0;
    for (int i = 0; i < 256; i++) {
        uint64_t c = st->native_trace_unsafe_opcode_counts[i];
        if (c > top_native_trace_unsafe_count) {
            top_native_trace_unsafe_count = c;
            top_native_trace_unsafe_opcode = i;
        }
    }
    int top_native_trace_deopt_opcode = -1;
    uint64_t top_native_trace_deopt_count = 0;
    for (int i = 0; i < 256; i++) {
        uint64_t c = st->native_trace_deopt_opcode_counts[i];
        if (c > top_native_trace_deopt_count) {
            top_native_trace_deopt_count = c;
            top_native_trace_deopt_opcode = i;
        }
    }
    int top_native_trace_deopt_reason = -1;
    uint64_t top_native_trace_deopt_reason_count = 0;
    for (int i = 0; i < 32; i++) {
        uint64_t c = st->native_trace_deopt_reason_counts[i];
        if (c > top_native_trace_deopt_reason_count) {
            top_native_trace_deopt_reason_count = c;
            top_native_trace_deopt_reason = i;
        }
    }
    uint64_t typed_exit_total = 0;
    uint64_t typed_exit_type = 0;
    uint64_t typed_exit_stack = 0;
    uint64_t typed_exit_call = 0;
    uint64_t typed_exit_invoke = 0;
    uint64_t typed_exit_table = 0;
    uint64_t typed_exit_control = 0;
    uint64_t typed_exit_unsupported = 0;
    uint64_t typed_hot_exit_count = 0;
    for (int i = 0; i < st->loop_count; i++) {
        JitLoopTrace* trace = &st->loops[i];
        typed_exit_total += (uint64_t)trace->typed_exit_total;
        typed_hot_exit_count += (uint64_t)trace->typed_hot_exit_count;
        if (trace->typed_exit_reasons == NULL || trace->typed_exit_hits == NULL) continue;
        for (uint16_t j = 0; j < trace->typed_exit_count; j++) {
            uint64_t hits = (uint64_t)trace->typed_exit_hits[j];
            switch (trace->typed_exit_reasons[j]) {
                case TYPED_EXIT_REASON_TYPE: typed_exit_type += hits; break;
                case TYPED_EXIT_REASON_STACK: typed_exit_stack += hits; break;
                case TYPED_EXIT_REASON_CALL: typed_exit_call += hits; break;
                case TYPED_EXIT_REASON_INVOKE: typed_exit_invoke += hits; break;
                case TYPED_EXIT_REASON_TABLE: typed_exit_table += hits; break;
                case TYPED_EXIT_REASON_CONTROL: typed_exit_control += hits; break;
                case TYPED_EXIT_REASON_UNSUPPORTED: typed_exit_unsupported += hits; break;
                default: break;
            }
        }
    }
    fprintf(stderr,
            "\n[vm-native-jit] enabled=%d hot_candidates=%llu compile_attempts=%llu compiled=%llu "
            "exec_attempts=%llu exec_hits=%llu loop_events=%llu loop_compile_attempts=%llu "
            "loop_compiled=%llu loop_compile_fail=%llu loop_fail_unsupported=%llu "
            "loop_fail_decode=%llu loop_fail_jump=%llu loop_fail_stack=%llu "
            "loop_last_fail_reason=%s loop_last_fail_opcode=%u loop_last_fail_offset=%u "
            "loop_top_fail_opcode=%d loop_top_fail_count=%llu "
            "func_runtime_disabled=%llu loop_exec_attempts=%llu loop_exec_hits=%llu loop_runtime_disabled=%llu "
            "loop_numeric_fast_enabled=%d loop_value_fast_enabled=%d "
            "loop_numeric_fast_attempts=%llu loop_numeric_fast_hits=%llu "
            "loop_typed_fast_attempts=%llu loop_typed_fast_hits=%llu "
            "typed_exit_total=%llu typed_exit_type=%llu typed_exit_stack=%llu "
            "typed_exit_call=%llu typed_exit_invoke=%llu typed_exit_table=%llu "
            "typed_exit_control=%llu typed_exit_unsupported=%llu "
            "typed_hot_exit_count=%llu typed_exit_promotions=%llu typed_side_exit_stitches=%llu "
            "side_exit_candidates=%llu side_exit_hits=%llu "
            "backend=%s backend_host=%d backend_enabled=%d "
            "backend_loop_attempts=%llu backend_loop_success=%llu backend_loop_fail=%llu "
            "backend_value_loop_attempts=%llu backend_value_loop_success=%llu backend_value_loop_fail=%llu "
            "backend_value_loop_exec_attempts=%llu backend_value_loop_exec_hits=%llu "
            "backend_loop_exec_attempts=%llu backend_loop_exec_hits=%llu "
            "backend_slot_lower_attempts=%llu backend_slot_lower_success=%llu backend_slot_lower_fail=%llu "
            "backend_slot_lower_top_fail_opcode=%d backend_slot_lower_top_fail_count=%llu "
            "native_trace_build_attempts=%llu native_trace_build_success=%llu native_trace_build_fail=%llu "
            "native_trace_compile_attempts=%llu native_trace_compile_success=%llu native_trace_compile_fail=%llu "
            "native_trace_compile_direct_core=%llu "
            "native_trace_exec_safe_traces=%llu native_trace_exec_unsafe_traces=%llu "
            "native_trace_unsafe_top_opcode=%d native_trace_unsafe_top_count=%llu "
            "native_trace_runtime_disabled=%llu "
            "native_trace_exec_globally_disabled=%d native_trace_exec_disable_reason=%s "
            "native_trace_exec_attempts=%llu native_trace_exec_hits=%llu "
            "native_trace_exec_attempts_direct_core=%llu native_trace_exec_hits_direct_core=%llu "
            "native_trace_guard_failures=%llu native_trace_deopts=%llu "
            "native_trace_deopt_top_opcode=%d native_trace_deopt_top_count=%llu "
            "native_trace_deopt_top_reason=%s native_trace_deopt_top_reason_count=%llu "
            "native_trace_sandbox_attempts=%llu "
            "native_trace_sandbox_success=%llu native_trace_sandbox_fail=%llu\n",
            vm->native_jit_enabled,
            (unsigned long long)vm->native_jit_hot_candidates,
            (unsigned long long)vm->native_jit_compile_attempts,
            (unsigned long long)vm->native_jit_compiled_count,
            (unsigned long long)vm->native_jit_exec_attempts,
            (unsigned long long)vm->native_jit_exec_hits,
            (unsigned long long)st->loop_backedge_events,
            (unsigned long long)st->loop_compile_attempts,
            (unsigned long long)st->loop_compiled_count,
            (unsigned long long)st->loop_compile_fail_count,
            (unsigned long long)st->loop_fail_reason_unsupported,
            (unsigned long long)st->loop_fail_reason_decode,
            (unsigned long long)st->loop_fail_reason_jump,
            (unsigned long long)st->loop_fail_reason_stack,
            loop_fail_reason_name(st->loop_last_fail_reason),
            (unsigned int)st->loop_last_fail_opcode,
            (unsigned int)st->loop_last_fail_offset,
            top_fail_opcode,
            (unsigned long long)top_fail_count,
            (unsigned long long)st->func_runtime_disabled,
            (unsigned long long)st->loop_exec_attempts,
            (unsigned long long)st->loop_exec_hits,
            (unsigned long long)st->loop_runtime_disabled,
            (int)st->loop_numeric_fast_enabled,
            (int)st->loop_value_fast_enabled,
            (unsigned long long)st->loop_numeric_fast_attempts,
            (unsigned long long)st->loop_numeric_fast_hits,
            (unsigned long long)st->loop_typed_fast_attempts,
            (unsigned long long)st->loop_typed_fast_hits,
            (unsigned long long)typed_exit_total,
            (unsigned long long)typed_exit_type,
            (unsigned long long)typed_exit_stack,
            (unsigned long long)typed_exit_call,
            (unsigned long long)typed_exit_invoke,
            (unsigned long long)typed_exit_table,
            (unsigned long long)typed_exit_control,
            (unsigned long long)typed_exit_unsupported,
            (unsigned long long)typed_hot_exit_count,
            (unsigned long long)st->loop_typed_exit_promotions,
            (unsigned long long)st->loop_typed_side_exit_stitches,
            (unsigned long long)st->loop_side_exit_candidates,
            (unsigned long long)st->loop_side_exit_hits,
            toi_jit_backend_name(st->backend_kind),
            (int)st->backend_host_supported,
            (int)st->backend_enabled,
            (unsigned long long)st->backend_loop_compile_attempts,
            (unsigned long long)st->backend_loop_compile_success,
            (unsigned long long)st->backend_loop_compile_fail,
            (unsigned long long)st->backend_value_loop_compile_attempts,
            (unsigned long long)st->backend_value_loop_compile_success,
            (unsigned long long)st->backend_value_loop_compile_fail,
            (unsigned long long)st->backend_value_loop_exec_attempts,
            (unsigned long long)st->backend_value_loop_exec_hits,
            (unsigned long long)st->backend_loop_exec_attempts,
            (unsigned long long)st->backend_loop_exec_hits,
            (unsigned long long)st->backend_slot_lower_attempts,
            (unsigned long long)st->backend_slot_lower_success,
            (unsigned long long)st->backend_slot_lower_fail,
            top_slot_lower_fail_opcode,
            (unsigned long long)top_slot_lower_fail_count,
            (unsigned long long)st->native_trace_build_attempts,
            (unsigned long long)st->native_trace_build_success,
            (unsigned long long)st->native_trace_build_fail,
            (unsigned long long)st->native_trace_compile_attempts,
            (unsigned long long)st->native_trace_compile_success,
            (unsigned long long)st->native_trace_compile_fail,
            (unsigned long long)st->native_trace_compile_direct_core,
            (unsigned long long)st->native_trace_exec_safe_traces,
            (unsigned long long)st->native_trace_exec_unsafe_traces,
            top_native_trace_unsafe_opcode,
            (unsigned long long)top_native_trace_unsafe_count,
            (unsigned long long)st->native_trace_runtime_disabled,
            (int)st->native_trace_exec_globally_disabled,
            native_trace_disable_reason_name(st->native_trace_exec_disable_reason),
            (unsigned long long)st->native_trace_exec_attempts,
            (unsigned long long)st->native_trace_exec_hits,
            (unsigned long long)st->native_trace_exec_attempts_direct_core,
            (unsigned long long)st->native_trace_exec_hits_direct_core,
            (unsigned long long)st->native_trace_guard_failures,
            (unsigned long long)st->native_trace_deopts,
            top_native_trace_deopt_opcode,
            (unsigned long long)top_native_trace_deopt_count,
            (top_native_trace_deopt_reason >= 0)
                ? native_trace_deopt_reason_name((uint8_t)top_native_trace_deopt_reason)
                : "none",
            (unsigned long long)top_native_trace_deopt_reason_count,
            (unsigned long long)st->native_trace_sandbox_attempts,
            (unsigned long long)st->native_trace_sandbox_success,
            (unsigned long long)st->native_trace_sandbox_fail);
    if (st->backend_kind == TOI_JIT_BACKEND_ARM64) {
        uint64_t chain_attempts = 0;
        uint64_t chain_hits = 0;
        uint64_t chain_misses = 0;
        toi_jit_arm64_get_chain_stats(&chain_attempts, &chain_hits, &chain_misses);
        fprintf(stderr,
                "[vm-native-jit-chain] attempts=%llu hits=%llu misses=%llu\n",
                (unsigned long long)chain_attempts,
                (unsigned long long)chain_hits,
                (unsigned long long)chain_misses);
    }
    if (st->loop_compile_fail_count != 0) {
        fprintf(stderr, "[vm-native-jit-loop-fail-opcodes]");
        for (int i = 0; i < 256; i++) {
            uint64_t c = st->loop_fail_opcode_counts[i];
            if (c != 0) fprintf(stderr, " %d=%llu", i, (unsigned long long)c);
        }
        fprintf(stderr, "\n");
    }
    if (st->backend_slot_lower_fail != 0) {
        fprintf(stderr, "[vm-native-jit-slot-lower-fail-opcodes]");
        for (int i = 0; i < 256; i++) {
            uint64_t c = st->backend_slot_lower_fail_opcode_counts[i];
            if (c != 0) fprintf(stderr, " %d=%llu", i, (unsigned long long)c);
        }
        fprintf(stderr, "\n");
    }
    if (st->native_trace_exec_unsafe_traces != 0) {
        fprintf(stderr, "[vm-native-jit-native-trace-unsafe-opcodes]");
        for (int i = 0; i < 256; i++) {
            uint64_t c = st->native_trace_unsafe_opcode_counts[i];
            if (c != 0) fprintf(stderr, " %d=%llu", i, (unsigned long long)c);
        }
        fprintf(stderr, "\n");
    }
    if (st->native_trace_deopts != 0) {
        fprintf(stderr, "[vm-native-jit-native-trace-deopt-opcodes]");
        for (int i = 0; i < 256; i++) {
            uint64_t c = st->native_trace_deopt_opcode_counts[i];
            if (c != 0) fprintf(stderr, " %d=%llu", i, (unsigned long long)c);
        }
        fprintf(stderr, "\n");
        fprintf(stderr, "[vm-native-jit-native-trace-deopt-reasons]");
        for (int i = 0; i < 32; i++) {
            uint64_t c = st->native_trace_deopt_reason_counts[i];
            if (c != 0) fprintf(stderr, " %s=%llu", native_trace_deopt_reason_name((uint8_t)i), (unsigned long long)c);
        }
        fprintf(stderr, "\n");
    }
    if (st->loop_count > 0) {
        fprintf(stderr, "[vm-native-jit-native-trace-loops]");
        int printed = 0;
        int picked[8];
        for (int i = 0; i < 8; i++) picked[i] = -1;
        for (int rank = 0; rank < 8; rank++) {
            int best_i = -1;
            uint64_t best_attempts = 0;
            uint64_t best_hits = 0;
            for (int i = 0; i < st->loop_count; i++) {
                JitLoopTrace* t = &st->loops[i];
                if (!t->compiled || !t->native_trace_ready) continue;
                int already = 0;
                for (int j = 0; j < rank; j++) {
                    if (picked[j] == i) {
                        already = 1;
                        break;
                    }
                }
                if (already) continue;
                uint64_t attempts = t->native_trace_exec_attempts;
                uint64_t hits = t->native_trace_exec_hits;
                if (attempts == 0 && hits == 0 && t->exec_attempts == 0) continue;
                if (best_i < 0 ||
                    attempts > best_attempts ||
                    (attempts == best_attempts && hits > best_hits)) {
                    best_i = i;
                    best_attempts = attempts;
                    best_hits = hits;
                }
            }
            if (best_i < 0) break;
            picked[rank] = best_i;
            printed++;
        }
        if (printed == 0) {
            fprintf(stderr, " none");
        } else {
            for (int i = 0; i < printed; i++) {
                JitLoopTrace* t = &st->loops[picked[i]];
                const char* fn_name =
                    (t->function != NULL && t->function->name != NULL) ? t->function->name->chars : "<script>";
                fprintf(stderr,
                        " fn=%s@%u op=%u exec=%llu native=%llu/%llu exit=%llu cont=%llu soft=%llu guard=%llu deopt=%llu safe=%u dc=%u hc=%u hi=%u hb=%u hm=%u hg=%u",
                        fn_name,
                        (unsigned)t->opcode_offset,
                        (unsigned)t->opcode,
                        (unsigned long long)t->exec_attempts,
                        (unsigned long long)t->native_trace_exec_hits,
                        (unsigned long long)t->native_trace_exec_attempts,
                        (unsigned long long)t->native_trace_exit_hits,
                        (unsigned long long)t->native_trace_continue_hits,
                        (unsigned long long)t->native_trace_soft_bails,
                        (unsigned long long)t->native_trace_guard_failures,
                        (unsigned long long)t->native_trace_deopts,
                        (unsigned)t->native_trace_exec_safe,
                        (unsigned)t->native_trace_direct_core,
                        (unsigned)t->native_trace_has_call,
                        (unsigned)t->native_trace_has_invoke,
                        (unsigned)t->native_trace_has_build_string,
                        (unsigned)t->native_trace_has_mutating_table,
                        (unsigned)t->native_trace_has_get_table_generic);
            }
        }
        fprintf(stderr, "\n");
    }
}
