#include <stdio.h>
#include <string.h>

#include "jit_x64.h"

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
    if (err_buf != NULL && err_buf_size > 0) {
        snprintf(
            err_buf,
            err_buf_size,
            "x64 qfor trace backend scaffold: codegen not implemented");
    }
    (void)memchr(code, 0, (size_t)code_count);
    return 0;
}

int toi_jit_x64_compile_qfor_empty(
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn != NULL) *out_fn = NULL;
    if (out_code_mem != NULL) *out_code_mem = NULL;
    if (out_code_size != NULL) *out_code_size = 0;
    if (err_buf != NULL && err_buf_size > 0) {
        snprintf(err_buf, err_buf_size, "x64 qfor-empty backend not implemented");
    }
    return 0;
}

int toi_jit_x64_compile_math_reduce(
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn != NULL) *out_fn = NULL;
    if (out_code_mem != NULL) *out_code_mem = NULL;
    if (out_code_size != NULL) *out_code_size = 0;
    if (err_buf != NULL && err_buf_size > 0) {
        snprintf(err_buf, err_buf_size, "x64 math-reduce backend not implemented");
    }
    return 0;
}

int toi_jit_x64_compile_numeric_loop(
    const ToiJitNumInstr* instrs,
    int instr_count,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    (void)instrs;
    (void)instr_count;
    if (out_fn != NULL) *out_fn = NULL;
    if (out_code_mem != NULL) *out_code_mem = NULL;
    if (out_code_size != NULL) *out_code_size = 0;
    if (err_buf != NULL && err_buf_size > 0) {
        snprintf(err_buf, err_buf_size, "x64 numeric-loop backend not implemented");
    }
    return 0;
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
    (void)instrs;
    (void)instr_count;
    (void)var_slot;
    (void)end_slot;
    if (out_fn != NULL) *out_fn = NULL;
    if (out_code_mem != NULL) *out_code_mem = NULL;
    if (out_code_size != NULL) *out_code_size = 0;
    if (err_buf != NULL && err_buf_size > 0) {
        snprintf(err_buf, err_buf_size, "x64 slot-loop backend not implemented");
    }
    return 0;
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
    (void)instrs;
    (void)instr_count;
    (void)var_slot;
    (void)end_slot;
    if (out_fn != NULL) *out_fn = NULL;
    if (out_code_mem != NULL) *out_code_mem = NULL;
    if (out_code_size != NULL) *out_code_size = 0;
    if (err_buf != NULL && err_buf_size > 0) {
        snprintf(err_buf, err_buf_size, "x64 value-loop backend not implemented");
    }
    return 0;
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
    (void)instrs;
    (void)instr_count;
    (void)guards;
    (void)guard_count;
    (void)var_slot;
    (void)end_slot;
    if (out_is_direct_core != NULL) *out_is_direct_core = 0;
    if (out_fn != NULL) *out_fn = NULL;
    if (out_code_mem != NULL) *out_code_mem = NULL;
    if (out_code_size != NULL) *out_code_size = 0;
    if (err_buf != NULL && err_buf_size > 0) {
        snprintf(err_buf, err_buf_size, "x64 native-trace backend not implemented");
    }
    return 0;
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
    (void)vm;
    (void)closure;
    (void)slots;
    (void)slot_count;
    (void)instrs;
    (void)instr_count;
    (void)var_slot;
    (void)end_slot;
    if (out_state != NULL) {
        out_state->resume_offset = 0xffffffffu;
        out_state->op = 0;
        out_state->reason = TOI_JIT_NATIVE_DEOPT_UNSUPPORTED;
        out_state->stack_count = 0;
    }
    return TOI_JIT_NATIVE_EXEC_DEOPT;
}

void toi_jit_x64_free_code(void* code_mem, size_t code_size) {
    (void)code_mem;
    (void)code_size;
}
