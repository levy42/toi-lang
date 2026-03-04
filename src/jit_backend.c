#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "jit_arm64.h"
#include "jit_backend.h"
#include "jit_x64.h"

static int str_eq_nocase(const char* a, const char* b) {
    if (a == NULL || b == NULL) return 0;
    while (*a != '\0' && *b != '\0') {
        char ca = (char)tolower((unsigned char)*a++);
        char cb = (char)tolower((unsigned char)*b++);
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

ToiJitBackendKind toi_jit_backend_select(const char* backend_name) {
    if (backend_name == NULL || backend_name[0] == '\0') return TOI_JIT_BACKEND_NONE;
    if (str_eq_nocase(backend_name, "none") || str_eq_nocase(backend_name, "off")) {
        return TOI_JIT_BACKEND_NONE;
    }
    if (str_eq_nocase(backend_name, "x64") || str_eq_nocase(backend_name, "amd64")) {
        return TOI_JIT_BACKEND_X64;
    }
    if (str_eq_nocase(backend_name, "arm64") || str_eq_nocase(backend_name, "aarch64")) {
        return TOI_JIT_BACKEND_ARM64;
    }
    if (str_eq_nocase(backend_name, "auto")) {
        if (toi_jit_arm64_host_supported()) return TOI_JIT_BACKEND_ARM64;
        if (toi_jit_x64_host_supported()) return TOI_JIT_BACKEND_X64;
        return TOI_JIT_BACKEND_NONE;
    }
    return TOI_JIT_BACKEND_NONE;
}

int toi_jit_backend_host_supported(ToiJitBackendKind kind) {
    switch (kind) {
        case TOI_JIT_BACKEND_X64:
            return toi_jit_x64_host_supported();
        case TOI_JIT_BACKEND_ARM64:
            return toi_jit_arm64_host_supported();
        default:
            return 0;
    }
}

const char* toi_jit_backend_name(ToiJitBackendKind kind) {
    switch (kind) {
        case TOI_JIT_BACKEND_X64:
            return "x64";
        case TOI_JIT_BACKEND_ARM64:
            return "arm64";
        default:
            return "none";
    }
}

int toi_jit_backend_compile_qfor_num_stub(
    ToiJitBackendKind kind,
    const uint8_t* code,
    int code_count,
    int loop_offset,
    char* err_buf,
    size_t err_buf_size) {
    switch (kind) {
        case TOI_JIT_BACKEND_X64:
            return toi_jit_x64_compile_qfor_num_stub(code, code_count, loop_offset, err_buf, err_buf_size);
        case TOI_JIT_BACKEND_ARM64:
            return toi_jit_arm64_compile_qfor_num_stub(code, code_count, loop_offset, err_buf, err_buf_size);
        default:
            return 0;
    }
}

int toi_jit_backend_compile_qfor_empty(
    ToiJitBackendKind kind,
    ToiJitQforEmptyFn* out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "invalid output pointers");
        }
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;

    void* fn_ptr = NULL;
    switch (kind) {
        case TOI_JIT_BACKEND_X64:
            if (!toi_jit_x64_compile_qfor_empty(&fn_ptr, out_code_mem, out_code_size, err_buf, err_buf_size)) {
                return 0;
            }
            break;
        case TOI_JIT_BACKEND_ARM64:
            if (!toi_jit_arm64_compile_qfor_empty(&fn_ptr, out_code_mem, out_code_size, err_buf, err_buf_size)) {
                return 0;
            }
            break;
        default:
            if (err_buf != NULL && err_buf_size > 0) {
                snprintf(err_buf, err_buf_size, "backend disabled");
            }
            return 0;
    }
    *out_fn = (ToiJitQforEmptyFn)fn_ptr;
    return 1;
}

int toi_jit_backend_compile_math_reduce(
    ToiJitBackendKind kind,
    ToiJitMathReduceFn* out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "invalid output pointers");
        }
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;

    void* fn_ptr = NULL;
    switch (kind) {
        case TOI_JIT_BACKEND_X64:
            if (!toi_jit_x64_compile_math_reduce(&fn_ptr, out_code_mem, out_code_size, err_buf, err_buf_size)) {
                return 0;
            }
            break;
        case TOI_JIT_BACKEND_ARM64:
            if (!toi_jit_arm64_compile_math_reduce(&fn_ptr, out_code_mem, out_code_size, err_buf, err_buf_size)) {
                return 0;
            }
            break;
        default:
            if (err_buf != NULL && err_buf_size > 0) {
                snprintf(err_buf, err_buf_size, "backend disabled");
            }
            return 0;
    }
    *out_fn = (ToiJitMathReduceFn)fn_ptr;
    return 1;
}

int toi_jit_backend_compile_numeric_loop(
    ToiJitBackendKind kind,
    const ToiJitNumInstr* instrs,
    int instr_count,
    ToiJitNumericLoopFn* out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "invalid output pointers");
        }
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;

    void* fn_ptr = NULL;
    switch (kind) {
        case TOI_JIT_BACKEND_X64:
            if (!toi_jit_x64_compile_numeric_loop(
                    instrs, instr_count, &fn_ptr, out_code_mem, out_code_size, err_buf, err_buf_size)) {
                return 0;
            }
            break;
        case TOI_JIT_BACKEND_ARM64:
            if (!toi_jit_arm64_compile_numeric_loop(
                    instrs, instr_count, &fn_ptr, out_code_mem, out_code_size, err_buf, err_buf_size)) {
                return 0;
            }
            break;
        default:
            if (err_buf != NULL && err_buf_size > 0) {
                snprintf(err_buf, err_buf_size, "backend disabled");
            }
            return 0;
    }
    *out_fn = (ToiJitNumericLoopFn)fn_ptr;
    return 1;
}

int toi_jit_backend_compile_slot_loop(
    ToiJitBackendKind kind,
    const ToiJitSlotInstr* instrs,
    int instr_count,
    uint8_t var_slot,
    uint8_t end_slot,
    ToiJitSlotLoopFn* out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "invalid output pointers");
        }
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;

    void* fn_ptr = NULL;
    switch (kind) {
        case TOI_JIT_BACKEND_X64:
            if (!toi_jit_x64_compile_slot_loop(
                    instrs, instr_count, var_slot, end_slot, &fn_ptr, out_code_mem, out_code_size, err_buf, err_buf_size)) {
                return 0;
            }
            break;
        case TOI_JIT_BACKEND_ARM64:
            if (!toi_jit_arm64_compile_slot_loop(
                    instrs, instr_count, var_slot, end_slot, &fn_ptr, out_code_mem, out_code_size, err_buf, err_buf_size)) {
                return 0;
            }
            break;
        default:
            if (err_buf != NULL && err_buf_size > 0) {
                snprintf(err_buf, err_buf_size, "backend disabled");
            }
            return 0;
    }
    *out_fn = (ToiJitSlotLoopFn)fn_ptr;
    return 1;
}

int toi_jit_backend_compile_value_loop(
    ToiJitBackendKind kind,
    const ToiJitValueInstr* instrs,
    int instr_count,
    uint8_t var_slot,
    uint8_t end_slot,
    ToiJitValueLoopFn* out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "invalid output pointers");
        }
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;

    void* fn_ptr = NULL;
    switch (kind) {
        case TOI_JIT_BACKEND_X64:
            if (!toi_jit_x64_compile_value_loop(
                    instrs, instr_count, var_slot, end_slot, &fn_ptr, out_code_mem, out_code_size, err_buf, err_buf_size)) {
                return 0;
            }
            break;
        case TOI_JIT_BACKEND_ARM64:
            if (!toi_jit_arm64_compile_value_loop(
                    instrs, instr_count, var_slot, end_slot, &fn_ptr, out_code_mem, out_code_size, err_buf, err_buf_size)) {
                return 0;
            }
            break;
        default:
            if (err_buf != NULL && err_buf_size > 0) {
                snprintf(err_buf, err_buf_size, "backend disabled");
            }
            return 0;
    }
    *out_fn = (ToiJitValueLoopFn)fn_ptr;
    return 1;
}

int toi_jit_backend_compile_native_trace(
    ToiJitBackendKind kind,
    const struct ToiJitNativeTraceInstr* instrs,
    int instr_count,
    const struct ToiJitNativeGuard* guards,
    int guard_count,
    uint8_t var_slot,
    uint8_t end_slot,
    uint8_t* out_is_direct_core,
    ToiJitNativeTraceFn* out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size) {
    if (out_fn == NULL || out_code_mem == NULL || out_code_size == NULL) {
        if (err_buf != NULL && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "invalid output pointers");
        }
        return 0;
    }
    *out_fn = NULL;
    *out_code_mem = NULL;
    *out_code_size = 0;
    if (out_is_direct_core != NULL) *out_is_direct_core = 0;

    void* fn_ptr = NULL;
    switch (kind) {
        case TOI_JIT_BACKEND_X64:
            if (!toi_jit_x64_compile_native_trace(
                    instrs,
                    instr_count,
                    guards,
                    guard_count,
                    var_slot,
                    end_slot,
                    out_is_direct_core,
                    &fn_ptr,
                    out_code_mem,
                    out_code_size,
                    err_buf,
                    err_buf_size)) {
                return 0;
            }
            break;
        case TOI_JIT_BACKEND_ARM64:
            if (!toi_jit_arm64_compile_native_trace(
                    instrs,
                    instr_count,
                    guards,
                    guard_count,
                    var_slot,
                    end_slot,
                    out_is_direct_core,
                    &fn_ptr,
                    out_code_mem,
                    out_code_size,
                    err_buf,
                    err_buf_size)) {
                return 0;
            }
            break;
        default:
            if (err_buf != NULL && err_buf_size > 0) {
                snprintf(err_buf, err_buf_size, "backend disabled");
            }
            return 0;
    }
    *out_fn = (ToiJitNativeTraceFn)fn_ptr;
    return 1;
}

ToiJitNativeExecResult toi_jit_backend_run_native_trace(
    ToiJitBackendKind kind,
    struct VM* vm,
    struct ObjClosure* closure,
    Value* slots,
    int slot_count,
    const struct ToiJitNativeTraceInstr* instrs,
    int instr_count,
    uint8_t var_slot,
    uint8_t end_slot,
    ToiJitNativeExecState* out_state) {
    switch (kind) {
        case TOI_JIT_BACKEND_ARM64:
            return toi_jit_arm64_run_native_trace(
                vm, closure, slots, slot_count, instrs, instr_count, var_slot, end_slot, out_state);
        case TOI_JIT_BACKEND_X64:
            return toi_jit_x64_run_native_trace(
                vm, closure, slots, slot_count, instrs, instr_count, var_slot, end_slot, out_state);
        default:
            if (out_state != NULL) {
                out_state->resume_offset = 0xffffffffu;
                out_state->op = 0;
                out_state->reason = TOI_JIT_NATIVE_DEOPT_UNSUPPORTED;
            }
            return TOI_JIT_NATIVE_EXEC_DEOPT;
    }
}

void toi_jit_backend_free_code(ToiJitBackendKind kind, void* code_mem, size_t code_size) {
    switch (kind) {
        case TOI_JIT_BACKEND_X64:
            toi_jit_x64_free_code(code_mem, code_size);
            break;
        case TOI_JIT_BACKEND_ARM64:
            toi_jit_arm64_free_code(code_mem, code_size);
            break;
        default:
            break;
    }
}
