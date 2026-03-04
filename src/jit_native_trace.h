#ifndef TOI_JIT_NATIVE_TRACE_H
#define TOI_JIT_NATIVE_TRACE_H

#include <stdint.h>
#include "jit_backend.h"

typedef enum {
    TOI_JIT_NATIVE_OP_NOP = 0,
    TOI_JIT_NATIVE_OP_PUSH_LOCAL,
    TOI_JIT_NATIVE_OP_PUSH_UPVALUE,
    TOI_JIT_NATIVE_OP_PUSH_CONST,
    TOI_JIT_NATIVE_OP_PUSH_NIL,
    TOI_JIT_NATIVE_OP_PUSH_TRUE,
    TOI_JIT_NATIVE_OP_PUSH_FALSE,
    TOI_JIT_NATIVE_OP_STORE_LOCAL,
    TOI_JIT_NATIVE_OP_POP,
    TOI_JIT_NATIVE_OP_BIN_ADD,
    TOI_JIT_NATIVE_OP_BIN_SUB,
    TOI_JIT_NATIVE_OP_BIN_MUL,
    TOI_JIT_NATIVE_OP_BIN_DIV,
    TOI_JIT_NATIVE_OP_BIN_MOD,
    TOI_JIT_NATIVE_OP_CMP_EQ,
    TOI_JIT_NATIVE_OP_CMP_LT,
    TOI_JIT_NATIVE_OP_CMP_GT,
    TOI_JIT_NATIVE_OP_NOT,
    TOI_JIT_NATIVE_OP_JUMP,
    TOI_JIT_NATIVE_OP_JUMP_IF_FALSE,
    TOI_JIT_NATIVE_OP_JUMP_IF_TRUE,
    TOI_JIT_NATIVE_OP_APPEND,
    TOI_JIT_NATIVE_OP_GET_TABLE,
    TOI_JIT_NATIVE_OP_GET_TABLE_NUM,
    TOI_JIT_NATIVE_OP_SET_TABLE,
    TOI_JIT_NATIVE_OP_SET_TABLE_NUM,
    TOI_JIT_NATIVE_OP_CALL,
    TOI_JIT_NATIVE_OP_INVOKE,
    TOI_JIT_NATIVE_OP_BUILD_STRING2,
    TOI_JIT_NATIVE_OP_BUILD_STRING3,
    TOI_JIT_NATIVE_OP_BUILD_STRING4,
    TOI_JIT_NATIVE_OP_BUILD_STRING5,
    TOI_JIT_NATIVE_OP_ADJUST_STACK
} ToiJitNativeOpCode;

typedef enum {
    TOI_JIT_NATIVE_GUARD_NONE = 0,
    TOI_JIT_NATIVE_GUARD_LOCAL_IS_NUMBER,
    TOI_JIT_NATIVE_GUARD_UPVALUE_IS_NUMBER,
    TOI_JIT_NATIVE_GUARD_LOCAL_IS_STRING,
    TOI_JIT_NATIVE_GUARD_UPVALUE_IS_STRING,
    TOI_JIT_NATIVE_GUARD_TABLE_SHAPE,
    TOI_JIT_NATIVE_GUARD_CALL_TARGET,
    TOI_JIT_NATIVE_GUARD_INVOKE_TARGET
} ToiJitNativeGuardKind;

typedef struct ToiJitNativeTraceInstr {
    uint8_t op;
    uint8_t a;
    uint8_t b;
    uint16_t target;
    uint32_t deopt_offset;
    uint64_t aux_data;
    Value constant;
} ToiJitNativeTraceInstr;

typedef struct ToiJitNativeGuard {
    uint8_t kind;
    uint8_t slot_a;
    uint8_t slot_b;
    uint8_t aux;
    uint16_t deopt_pc;
    Value constant;
} ToiJitNativeGuard;

int toi_jit_native_trace_build_from_value_ir(
    const ToiJitValueInstr* value_instrs,
    int value_instr_count,
    const uint32_t* value_deopt_offsets,
    ToiJitNativeTraceInstr* out_instrs,
    int* out_instr_count,
    ToiJitNativeGuard* out_guards,
    int* out_guard_count,
    uint8_t* out_fail_op);

#endif
