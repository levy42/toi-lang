#ifndef TOI_JIT_ARM64_H
#define TOI_JIT_ARM64_H

#include <stddef.h>
#include <stdint.h>

#include "jit_backend.h"

int toi_jit_arm64_host_supported(void);
int toi_jit_arm64_compile_qfor_num_stub(
    const uint8_t* code, int code_count, int loop_offset, char* err_buf, size_t err_buf_size);
int toi_jit_arm64_compile_qfor_empty(
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size);
int toi_jit_arm64_compile_math_reduce(
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size);
int toi_jit_arm64_compile_numeric_loop(
    const ToiJitNumInstr* instrs,
    int instr_count,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size);
int toi_jit_arm64_compile_slot_loop(
    const ToiJitSlotInstr* instrs,
    int instr_count,
    uint8_t var_slot,
    uint8_t end_slot,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size);
int toi_jit_arm64_compile_value_loop(
    const ToiJitValueInstr* instrs,
    int instr_count,
    uint8_t var_slot,
    uint8_t end_slot,
    void** out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size);
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
    size_t err_buf_size);
ToiJitNativeExecResult toi_jit_arm64_run_native_trace(
    struct VM* vm,
    struct ObjClosure* closure,
    Value* slots,
    int slot_count,
    const struct ToiJitNativeTraceInstr* instrs,
    int instr_count,
    uint8_t var_slot,
    uint8_t end_slot,
    ToiJitNativeExecState* out_state);
void toi_jit_arm64_free_code(void* code_mem, size_t code_size);

#endif
