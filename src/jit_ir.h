#ifndef TOI_JIT_IR_H
#define TOI_JIT_IR_H

#include <stdint.h>

#include "object.h"

typedef enum {
    JOP_TINY_INLINE,
    JOP_CONSTANT,
    JOP_NIL,
    JOP_TRUE,
    JOP_FALSE,
    JOP_POP,
    JOP_GET_LOCAL,
    JOP_GET2_LOCAL,
    JOP_GET_UPVALUE,
    JOP_SET_LOCAL,
    JOP_SET_LOCAL_POP,
    JOP_APPEND,
    JOP_GET_TABLE,
    JOP_GET_TABLE_NUM,
    JOP_SET_TABLE,
    JOP_SET_TABLE_NUM,
    JOP_INC_LOCAL,
    JOP_SUB_LOCAL_CONST,
    JOP_MUL_LOCAL_CONST,
    JOP_DIV_LOCAL_CONST,
    JOP_MOD_LOCAL_CONST,
    JOP_ADD_CONST,
    JOP_SUB_CONST,
    JOP_MUL_CONST,
    JOP_DIV_CONST,
    JOP_MOD_CONST,
    JOP_ADD,
    JOP_ADD_INPLACE,
    JOP_SUBTRACT,
    JOP_MULTIPLY,
    JOP_DIVIDE,
    JOP_MODULO,
    JOP_NEGATE,
    JOP_NOT,
    JOP_IADD,
    JOP_ISUB,
    JOP_IMUL,
    JOP_IDIV,
    JOP_IMOD,
    JOP_FADD,
    JOP_FSUB,
    JOP_FMUL,
    JOP_FDIV,
    JOP_FMOD,
    JOP_QFADD,
    JOP_QFSUB,
    JOP_QFMUL,
    JOP_QFDIV,
    JOP_QFMOD,
    JOP_EQUAL,
    JOP_QGREATER_NUM,
    JOP_QLESS_NUM,
    JOP_JUMP,
    JOP_JUMP_IF_FALSE,
    JOP_JUMP_IF_TRUE,
    JOP_CALL,
    JOP_INVOKE,
    JOP_LENGTH,
    JOP_SLICE,
    JOP_BUILD_STRING2,
    JOP_BUILD_STRING3,
    JOP_BUILD_STRING4,
    JOP_BUILD_STRING5,
    JOP_QCALL2,
    JOP_QFOR_PREP_NUM,
    JOP_QFOR_LOOP_NUM,
    JOP_ADJUST_STACK,
    JOP_RETURN
} JitOpCode;

typedef struct {
    JitOpCode op;
    uint8_t a;
    uint8_t b;
    uint16_t target;
    uint32_t byte_offset;
    Value constant;
} JitOp;

typedef struct {
    ObjFunction* function;
    int op_count;
    int max_slot;
    int max_stack;
    JitOp* ops;
} JitProgram;

int jit_ir_read_u16(const uint8_t* code, int offset);
int jit_ir_decode_instr_len(const uint8_t* code, int offset, int count);
int jit_ir_map_supported_opcode(uint8_t op, JitOpCode* out);

void jit_program_free(JitProgram* program);
JitProgram* jit_program_compile(ObjFunction* function);

#endif
