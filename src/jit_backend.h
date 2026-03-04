#ifndef TOI_JIT_BACKEND_H
#define TOI_JIT_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include "value.h"
struct VM;
struct ObjClosure;
struct ToiJitValueInstr;
struct ToiJitNativeTraceInstr;
struct ToiJitNativeGuard;

typedef enum {
    TOI_JIT_BACKEND_NONE = 0,
    TOI_JIT_BACKEND_X64 = 1,
    TOI_JIT_BACKEND_ARM64 = 2
} ToiJitBackendKind;

typedef uint64_t (*ToiJitQforEmptyFn)(uint64_t var, uint64_t end);
typedef void (*ToiJitMathReduceFn)(
    double* acc_ptr,
    double* var_ptr,
    double* end_ptr,
    double c1,
    double c2,
    double c3);
typedef void (*ToiJitNumericLoopFn)(
    double* acc_ptr,
    double* var_ptr,
    double* end_ptr,
    const double* consts);
typedef void (*ToiJitSlotLoopFn)(double* slots, const double* consts);
typedef int (*ToiJitValueLoopFn)(
    struct VM* vm,
    struct ObjClosure* closure,
    Value* slots,
    int slot_count,
    const struct ToiJitValueInstr* instrs,
    int instr_count,
    uint8_t var_slot,
    uint8_t end_slot);
typedef enum {
    TOI_JIT_NATIVE_EXEC_DEOPT = 0,
    TOI_JIT_NATIVE_EXEC_EXIT = 1,
    TOI_JIT_NATIVE_EXEC_CONTINUE = 2
} ToiJitNativeExecResult;

typedef enum {
    TOI_JIT_NATIVE_DEOPT_NONE = 0,
    TOI_JIT_NATIVE_DEOPT_UNSPEC = 1,
    TOI_JIT_NATIVE_DEOPT_STACK_BOUNDS = 2,
    TOI_JIT_NATIVE_DEOPT_TYPE_GUARD = 3,
    TOI_JIT_NATIVE_DEOPT_GUARD = 4,
    TOI_JIT_NATIVE_DEOPT_CALL_TARGET = 5,
    TOI_JIT_NATIVE_DEOPT_INVOKE_TARGET = 6,
    TOI_JIT_NATIVE_DEOPT_TABLE_GUARD = 7,
    TOI_JIT_NATIVE_DEOPT_CONTROL_FLOW = 8,
    TOI_JIT_NATIVE_DEOPT_RUNTIME = 9,
    TOI_JIT_NATIVE_DEOPT_UNSUPPORTED = 10
} ToiJitNativeDeoptReason;

#ifndef TOI_JIT_NATIVE_EXEC_STACK_CAP
#define TOI_JIT_NATIVE_EXEC_STACK_CAP 256
#endif

typedef struct {
    uint32_t resume_offset;
    uint8_t op;
    uint8_t reason;
    uint16_t stack_count;
    Value stack[TOI_JIT_NATIVE_EXEC_STACK_CAP];
} ToiJitNativeExecState;
typedef int (*ToiJitNativeTraceFn)(
    struct VM* vm,
    struct ObjClosure* closure,
    Value* slots,
    int slot_count,
    ToiJitNativeExecState* out_state);

typedef enum {
    TOI_JIT_NUM_PUSH_ACC = 1,
    TOI_JIT_NUM_PUSH_VAR = 2,
    TOI_JIT_NUM_PUSH_END = 3,
    TOI_JIT_NUM_PUSH_CONST = 4,
    TOI_JIT_NUM_BIN_ADD = 5,
    TOI_JIT_NUM_BIN_SUB = 6,
    TOI_JIT_NUM_BIN_MUL = 7,
    TOI_JIT_NUM_BIN_DIV = 8,
    TOI_JIT_NUM_BIN_MOD = 9,
    TOI_JIT_NUM_STORE_ACC = 10,
    TOI_JIT_NUM_POP = 11
} ToiJitNumOpCode;

typedef struct {
    uint8_t op;
    uint8_t arg;
} ToiJitNumInstr;

typedef enum {
    TOI_JIT_SLOT_PUSH_SLOT = 1,
    TOI_JIT_SLOT_PUSH_CONST = 2,
    TOI_JIT_SLOT_BIN_ADD = 3,
    TOI_JIT_SLOT_BIN_SUB = 4,
    TOI_JIT_SLOT_BIN_MUL = 5,
    TOI_JIT_SLOT_BIN_DIV = 6,
    TOI_JIT_SLOT_BIN_MOD = 7,
    TOI_JIT_SLOT_STORE_SLOT = 8,
    TOI_JIT_SLOT_POP = 9,
    TOI_JIT_SLOT_CMP_LT = 10,
    TOI_JIT_SLOT_CMP_GT = 11,
    TOI_JIT_SLOT_CMP_EQ = 12,
    TOI_JIT_SLOT_JUMP = 13,
    TOI_JIT_SLOT_JUMP_IF_FALSE = 14,
    TOI_JIT_SLOT_JUMP_IF_TRUE = 15
} ToiJitSlotOpCode;

typedef struct {
    uint8_t op;
    uint8_t arg;
} ToiJitSlotInstr;

typedef enum {
    TOI_JIT_VAL_PUSH_LOCAL = 1,
    TOI_JIT_VAL_PUSH_UPVALUE = 2,
    TOI_JIT_VAL_PUSH_CONST = 3,
    TOI_JIT_VAL_PUSH_NIL = 4,
    TOI_JIT_VAL_PUSH_TRUE = 5,
    TOI_JIT_VAL_PUSH_FALSE = 6,
    TOI_JIT_VAL_STORE_LOCAL = 7,
    TOI_JIT_VAL_POP = 8,
    TOI_JIT_VAL_BIN_ADD = 9,
    TOI_JIT_VAL_BIN_SUB = 10,
    TOI_JIT_VAL_BIN_MUL = 11,
    TOI_JIT_VAL_BIN_DIV = 12,
    TOI_JIT_VAL_BIN_MOD = 13,
    TOI_JIT_VAL_CMP_EQ = 14,
    TOI_JIT_VAL_CMP_LT = 15,
    TOI_JIT_VAL_CMP_GT = 16,
    TOI_JIT_VAL_NOT = 17,
    TOI_JIT_VAL_JUMP = 18,
    TOI_JIT_VAL_JUMP_IF_FALSE = 19,
    TOI_JIT_VAL_JUMP_IF_TRUE = 20,
    TOI_JIT_VAL_APPEND = 21,
    TOI_JIT_VAL_GET_TABLE = 22,
    TOI_JIT_VAL_GET_TABLE_NUM = 23,
    TOI_JIT_VAL_SET_TABLE = 24,
    TOI_JIT_VAL_SET_TABLE_NUM = 25,
    TOI_JIT_VAL_CALL = 26,
    TOI_JIT_VAL_INVOKE = 27,
    TOI_JIT_VAL_BUILD_STRING2 = 28,
    TOI_JIT_VAL_BUILD_STRING3 = 29,
    TOI_JIT_VAL_BUILD_STRING4 = 30,
    TOI_JIT_VAL_BUILD_STRING5 = 31,
    TOI_JIT_VAL_ADJUST_STACK = 32
} ToiJitValueOpCode;

typedef struct ToiJitValueInstr {
    uint8_t op;
    uint8_t a;
    uint8_t b;
    uint16_t target;
    Value constant;
} ToiJitValueInstr;

ToiJitBackendKind toi_jit_backend_select(const char* backend_name);
int toi_jit_backend_host_supported(ToiJitBackendKind kind);
const char* toi_jit_backend_name(ToiJitBackendKind kind);
int toi_jit_backend_compile_qfor_num_stub(
    ToiJitBackendKind kind,
    const uint8_t* code,
    int code_count,
    int loop_offset,
    char* err_buf,
    size_t err_buf_size);
int toi_jit_backend_compile_qfor_empty(
    ToiJitBackendKind kind,
    ToiJitQforEmptyFn* out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size);
int toi_jit_backend_compile_math_reduce(
    ToiJitBackendKind kind,
    ToiJitMathReduceFn* out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size);
int toi_jit_backend_compile_numeric_loop(
    ToiJitBackendKind kind,
    const ToiJitNumInstr* instrs,
    int instr_count,
    ToiJitNumericLoopFn* out_fn,
    void** out_code_mem,
    size_t* out_code_size,
    char* err_buf,
    size_t err_buf_size);
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
    size_t err_buf_size);
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
    size_t err_buf_size);
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
    size_t err_buf_size);
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
    ToiJitNativeExecState* out_state);
void toi_jit_backend_free_code(ToiJitBackendKind kind, void* code_mem, size_t code_size);

#endif
