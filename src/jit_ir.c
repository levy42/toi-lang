#include <stdlib.h>

#include "chunk.h"
#include "jit_ir.h"

int jit_ir_read_u16(const uint8_t* code, int offset) {
    return ((int)code[offset] << 8) | (int)code[offset + 1];
}

int jit_ir_decode_instr_len(const uint8_t* code, int offset, int count) {
    if (offset >= count) return 0;
    switch (code[offset]) {
        case OP_CONSTANT:
        case OP_GET_GLOBAL:
        case OP_DEFINE_GLOBAL:
        case OP_SET_GLOBAL:
        case OP_SET_GLOBAL_POP:
        case OP_DELETE_GLOBAL:
        case OP_GET_LOCAL:
        case OP_GET_UPVALUE:
        case OP_SET_UPVALUE:
        case OP_SET_UPVALUE_POP:
        case OP_SET_LOCAL:
        case OP_SET_LOCAL_POP:
        case OP_GET_FIELD_CONST:
        case OP_SET_FIELD_CONST:
        case OP_SET_FIELD_CONST_KEEP:
        case OP_PRINT:
        case OP_CALL:
        case OP_CALL_NAMED:
        case OP_CALL_EXPAND:
        case OP_IMPORT:
        case OP_BUILD_STRING:
        case OP_RETURN_N:
        case OP_ADJUST_STACK:
        case OP_ADD_CONST:
        case OP_SUB_CONST:
        case OP_MUL_CONST:
        case OP_DIV_CONST:
        case OP_MOD_CONST:
            return 2;
        case OP_GET2_LOCAL:
        case OP_INVOKE:
        case OP_QINVOKE0_STR_LOWER:
        case OP_UNPACK:
        case OP_INC_LOCAL:
        case OP_SUB_LOCAL_CONST:
        case OP_MUL_LOCAL_CONST:
        case OP_DIV_LOCAL_CONST:
        case OP_MOD_LOCAL_CONST:
            return 3;
        case OP_TRY:
            return 7;
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_TRUE:
        case OP_LOOP:
            return 3;
        case OP_QFOR_PREP_NUM:
        case OP_QFOR_LOOP_NUM:
        case OP_FOR_PREP:
        case OP_FOR_LOOP:
            return 5;
        case OP_NIL:
        case OP_TRUE:
        case OP_FALSE:
        case OP_APPEND:
        case OP_POP:
        case OP_CLOSE_UPVALUE:
        case OP_GET_META_TABLE:
        case OP_GET_TABLE:
        case OP_GET_TABLE_NUM:
        case OP_SET_TABLE:
        case OP_SET_TABLE_KEEP:
        case OP_SET_TABLE_NUM:
        case OP_DELETE_TABLE:
        case OP_NEW_TABLE:
        case OP_DUP:
        case OP_ADD_INPLACE:
        case OP_ADD:
        case OP_SUBTRACT:
        case OP_MULTIPLY:
        case OP_DIVIDE:
        case OP_MODULO:
        case OP_LENGTH:
        case OP_NEGATE:
        case OP_NOT:
        case OP_CALL0:
        case OP_CALL1:
        case OP_QCALL0:
        case OP_QCALL1:
        case OP_CALL2:
        case OP_IADD:
        case OP_ISUB:
        case OP_IMUL:
        case OP_IDIV:
        case OP_IMOD:
        case OP_FADD:
        case OP_FSUB:
        case OP_FMUL:
        case OP_FDIV:
        case OP_FMOD:
        case OP_QFADD:
        case OP_QFSUB:
        case OP_QFMUL:
        case OP_QFDIV:
        case OP_QFMOD:
        case OP_QGREATER_NUM:
        case OP_QLESS_NUM:
        case OP_QCALL2:
        case OP_QCALL1_STR_FIND:
        case OP_QINVOKE1_STR_FIND:
        case OP_BUILD_STRING2:
        case OP_BUILD_STRING3:
        case OP_BUILD_STRING4:
        case OP_BUILD_STRING5:
        case OP_END_TRY:
        case OP_END_FINALLY:
        case OP_IMPORT_STAR:
        case OP_THROW:
        case OP_EQUAL:
        case OP_GREATER:
        case OP_LESS:
        case OP_HAS:
        case OP_IN:
        case OP_POWER:
        case OP_INT_DIV:
        case OP_GC:
        case OP_SET_METATABLE:
        case OP_ITER_PREP:
        case OP_ITER_PREP_IPAIRS:
        case OP_RANGE:
        case OP_SLICE:
        case OP_RETURN:
            return 1;
        default:
            return 0;
    }
}

int jit_ir_map_supported_opcode(uint8_t op, JitOpCode* out) {
    switch (op) {
        case OP_CONSTANT: *out = JOP_CONSTANT; return 1;
        case OP_NIL: *out = JOP_NIL; return 1;
        case OP_TRUE: *out = JOP_TRUE; return 1;
        case OP_FALSE: *out = JOP_FALSE; return 1;
        case OP_POP: *out = JOP_POP; return 1;
        case OP_APPEND: *out = JOP_APPEND; return 1;
        case OP_GET_LOCAL: *out = JOP_GET_LOCAL; return 1;
        case OP_GET2_LOCAL: *out = JOP_GET2_LOCAL; return 1;
        case OP_GET_UPVALUE: *out = JOP_GET_UPVALUE; return 1;
        case OP_SET_LOCAL: *out = JOP_SET_LOCAL; return 1;
        case OP_SET_LOCAL_POP: *out = JOP_SET_LOCAL_POP; return 1;
        case OP_GET_TABLE: *out = JOP_GET_TABLE; return 1;
        case OP_GET_TABLE_NUM: *out = JOP_GET_TABLE_NUM; return 1;
        case OP_SET_TABLE: *out = JOP_SET_TABLE; return 1;
        case OP_SET_TABLE_NUM: *out = JOP_SET_TABLE_NUM; return 1;
        case OP_INC_LOCAL: *out = JOP_INC_LOCAL; return 1;
        case OP_SUB_LOCAL_CONST: *out = JOP_SUB_LOCAL_CONST; return 1;
        case OP_MUL_LOCAL_CONST: *out = JOP_MUL_LOCAL_CONST; return 1;
        case OP_DIV_LOCAL_CONST: *out = JOP_DIV_LOCAL_CONST; return 1;
        case OP_MOD_LOCAL_CONST: *out = JOP_MOD_LOCAL_CONST; return 1;
        case OP_ADD_CONST: *out = JOP_ADD_CONST; return 1;
        case OP_SUB_CONST: *out = JOP_SUB_CONST; return 1;
        case OP_MUL_CONST: *out = JOP_MUL_CONST; return 1;
        case OP_DIV_CONST: *out = JOP_DIV_CONST; return 1;
        case OP_MOD_CONST: *out = JOP_MOD_CONST; return 1;
        case OP_ADD: *out = JOP_ADD; return 1;
        case OP_ADD_INPLACE: *out = JOP_ADD_INPLACE; return 1;
        case OP_SUBTRACT: *out = JOP_SUBTRACT; return 1;
        case OP_MULTIPLY: *out = JOP_MULTIPLY; return 1;
        case OP_DIVIDE: *out = JOP_DIVIDE; return 1;
        case OP_MODULO: *out = JOP_MODULO; return 1;
        case OP_NEGATE: *out = JOP_NEGATE; return 1;
        case OP_NOT: *out = JOP_NOT; return 1;
        case OP_IADD: *out = JOP_IADD; return 1;
        case OP_ISUB: *out = JOP_ISUB; return 1;
        case OP_IMUL: *out = JOP_IMUL; return 1;
        case OP_IDIV: *out = JOP_IDIV; return 1;
        case OP_IMOD: *out = JOP_IMOD; return 1;
        case OP_FADD: *out = JOP_FADD; return 1;
        case OP_FSUB: *out = JOP_FSUB; return 1;
        case OP_FMUL: *out = JOP_FMUL; return 1;
        case OP_FDIV: *out = JOP_FDIV; return 1;
        case OP_FMOD: *out = JOP_FMOD; return 1;
        case OP_QFADD: *out = JOP_QFADD; return 1;
        case OP_QFSUB: *out = JOP_QFSUB; return 1;
        case OP_QFMUL: *out = JOP_QFMUL; return 1;
        case OP_QFDIV: *out = JOP_QFDIV; return 1;
        case OP_QFMOD: *out = JOP_QFMOD; return 1;
        case OP_EQUAL: *out = JOP_EQUAL; return 1;
        case OP_QGREATER_NUM: *out = JOP_QGREATER_NUM; return 1;
        case OP_QLESS_NUM: *out = JOP_QLESS_NUM; return 1;
        case OP_JUMP: *out = JOP_JUMP; return 1;
        case OP_JUMP_IF_FALSE: *out = JOP_JUMP_IF_FALSE; return 1;
        case OP_JUMP_IF_TRUE: *out = JOP_JUMP_IF_TRUE; return 1;
        case OP_LOOP: *out = JOP_JUMP; return 1;
        case OP_CALL:
        case OP_CALL0:
        case OP_CALL1:
        case OP_CALL2:
            *out = JOP_CALL; return 1;
        case OP_INVOKE: *out = JOP_INVOKE; return 1;
        case OP_LENGTH: *out = JOP_LENGTH; return 1;
        case OP_SLICE: *out = JOP_SLICE; return 1;
        case OP_BUILD_STRING2: *out = JOP_BUILD_STRING2; return 1;
        case OP_BUILD_STRING3: *out = JOP_BUILD_STRING3; return 1;
        case OP_BUILD_STRING4: *out = JOP_BUILD_STRING4; return 1;
        case OP_BUILD_STRING5: *out = JOP_BUILD_STRING5; return 1;
        case OP_QCALL2: *out = JOP_QCALL2; return 1;
        case OP_QFOR_PREP_NUM:
        case OP_FOR_PREP:
            *out = JOP_QFOR_PREP_NUM;
            return 1;
        case OP_QFOR_LOOP_NUM:
        case OP_FOR_LOOP:
            *out = JOP_QFOR_LOOP_NUM;
            return 1;
        case OP_ADJUST_STACK: *out = JOP_ADJUST_STACK; return 1;
        case OP_RETURN: *out = JOP_RETURN; return 1;
        default: return 0;
    }
}

static int is_numeric_const_ok(Chunk* chunk, int idx) {
    if (idx < 0 || idx >= chunk->constants.count) return 0;
    Value c = chunk->constants.values[idx];
    return IS_NUMBER(c) || IS_BOOL(c) || IS_NIL(c);
}

void jit_program_free(JitProgram* program) {
    if (program == NULL) return;
    free(program->ops);
    free(program);
}

JitProgram* jit_program_compile(ObjFunction* function) {
    if (function == NULL) return NULL;
    if (function->is_variadic || function->defaults_count != 0) return NULL;
    if (function->tiny_inline_kind != TINY_INLINE_NONE &&
        function->arity == 2 &&
        !function->is_variadic &&
        function->defaults_count == 0) {
        JitProgram* tiny = (JitProgram*)calloc(1, sizeof(JitProgram));
        if (tiny == NULL) return NULL;
        tiny->ops = (JitOp*)calloc(1, sizeof(JitOp));
        if (tiny->ops == NULL) {
            free(tiny);
            return NULL;
        }
        tiny->function = function;
        tiny->op_count = 1;
        tiny->max_slot = 2;
        tiny->max_stack = 4;
        tiny->ops[0].op = JOP_TINY_INLINE;
        tiny->ops[0].a = function->tiny_inline_kind;
        return tiny;
    }

    Chunk* chunk = &function->chunk;
    int count = chunk->count;
    const uint8_t* code = chunk->code;
    if (count <= 0 || code == NULL) return NULL;

    int* byte_to_jit = (int*)malloc(sizeof(int) * (size_t)count);
    if (byte_to_jit == NULL) return NULL;
    for (int i = 0; i < count; i++) byte_to_jit[i] = -1;

    int op_count = 0;
    int max_slot = function->arity;
    int max_stack = 0;
    int stack_depth = function->arity + 1;

    for (int offset = 0; offset < count;) {
        int len = jit_ir_decode_instr_len(code, offset, count);
        if (len <= 0 || offset + len > count) {
            free(byte_to_jit);
            return NULL;
        }

        JitOpCode jit_op = JOP_POP;
        if (!jit_ir_map_supported_opcode(code[offset], &jit_op)) {
            free(byte_to_jit);
            return NULL;
        }
        byte_to_jit[offset] = op_count++;

        switch (code[offset]) {
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
                stack_depth -= (int)code[offset + 1];
                break;
            case OP_CALL1:
                stack_depth -= 1;
                break;
            case OP_CALL2:
                stack_depth -= 2;
                break;
            case OP_INVOKE:
                stack_depth -= (int)code[offset + 2];
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
            case OP_SET_LOCAL:
            case OP_ADD_CONST:
            case OP_SUB_CONST:
            case OP_MUL_CONST:
            case OP_DIV_CONST:
            case OP_MOD_CONST:
            case OP_NEGATE:
            case OP_NOT:
            case OP_JUMP:
            case OP_JUMP_IF_FALSE:
            case OP_JUMP_IF_TRUE:
            case OP_QFOR_PREP_NUM:
            case OP_QFOR_LOOP_NUM:
            case OP_ADJUST_STACK:
                break;
            case OP_INC_LOCAL:
            case OP_SUB_LOCAL_CONST:
            case OP_MUL_LOCAL_CONST:
            case OP_DIV_LOCAL_CONST:
            case OP_MOD_LOCAL_CONST:
                stack_depth += 1;
                break;
            case OP_RETURN:
                stack_depth -= 1;
                break;
            default:
                stack_depth -= 1;
                break;
        }
        if (stack_depth < 0) {
            free(byte_to_jit);
            return NULL;
        }
        if (stack_depth > max_stack) max_stack = stack_depth;

        uint8_t op = code[offset];
        if (op == OP_GET_LOCAL || op == OP_SET_LOCAL || op == OP_SET_LOCAL_POP ||
            op == OP_INC_LOCAL || op == OP_SUB_LOCAL_CONST || op == OP_MUL_LOCAL_CONST ||
            op == OP_DIV_LOCAL_CONST || op == OP_MOD_LOCAL_CONST) {
            int slot = (int)code[offset + 1];
            if (slot > max_slot) max_slot = slot;
        } else if (op == OP_GET2_LOCAL) {
            int a = (int)code[offset + 1];
            int b = (int)code[offset + 2];
            if (a > max_slot) max_slot = a;
            if (b > max_slot) max_slot = b;
        }

        if (op == OP_CONSTANT && !is_numeric_const_ok(chunk, (int)code[offset + 1])) {
            free(byte_to_jit);
            return NULL;
        }
        if ((op == OP_ADD_CONST || op == OP_SUB_CONST || op == OP_MUL_CONST || op == OP_DIV_CONST || op == OP_MOD_CONST) &&
            !is_numeric_const_ok(chunk, (int)code[offset + 1])) {
            free(byte_to_jit);
            return NULL;
        }
        if ((op == OP_INC_LOCAL || op == OP_SUB_LOCAL_CONST || op == OP_MUL_LOCAL_CONST ||
             op == OP_DIV_LOCAL_CONST || op == OP_MOD_LOCAL_CONST) &&
            !is_numeric_const_ok(chunk, (int)code[offset + 2])) {
            free(byte_to_jit);
            return NULL;
        }

        offset += len;
    }

    JitProgram* program = (JitProgram*)calloc(1, sizeof(JitProgram));
    if (program == NULL) {
        free(byte_to_jit);
        return NULL;
    }
    program->ops = (JitOp*)calloc((size_t)op_count, sizeof(JitOp));
    if (program->ops == NULL) {
        free(byte_to_jit);
        free(program);
        return NULL;
    }
    program->function = function;
    program->op_count = op_count;
    program->max_slot = max_slot;
    program->max_stack = max_stack + 16;

    int jit_i = 0;
    for (int offset = 0; offset < count;) {
        uint8_t op = code[offset];
        JitOp* out = &program->ops[jit_i++];
        out->byte_offset = (uint32_t)offset;
        if (!jit_ir_map_supported_opcode(op, &out->op)) {
            free(byte_to_jit);
            jit_program_free(program);
            return NULL;
        }

        switch (op) {
            case OP_CONSTANT:
                out->constant = chunk->constants.values[(int)code[offset + 1]];
                break;
            case OP_GET_LOCAL:
            case OP_GET_UPVALUE:
            case OP_SET_LOCAL:
            case OP_SET_LOCAL_POP:
            case OP_ADJUST_STACK:
                out->a = code[offset + 1];
                break;
            case OP_CALL:
                out->a = code[offset + 1];
                break;
            case OP_CALL0:
                out->a = 0;
                break;
            case OP_CALL1:
                out->a = 1;
                break;
            case OP_CALL2:
                out->a = 2;
                break;
            case OP_INVOKE:
                out->a = code[offset + 2];
                out->constant = chunk->constants.values[(int)code[offset + 1]];
                break;
            case OP_GET2_LOCAL:
                out->a = code[offset + 1];
                out->b = code[offset + 2];
                break;
            case OP_INC_LOCAL:
            case OP_SUB_LOCAL_CONST:
            case OP_MUL_LOCAL_CONST:
            case OP_DIV_LOCAL_CONST:
            case OP_MOD_LOCAL_CONST:
                out->a = code[offset + 1];
                out->constant = chunk->constants.values[(int)code[offset + 2]];
                break;
            case OP_ADD_CONST:
            case OP_SUB_CONST:
            case OP_MUL_CONST:
            case OP_DIV_CONST:
            case OP_MOD_CONST:
                out->constant = chunk->constants.values[(int)code[offset + 1]];
                break;
            case OP_JUMP:
            case OP_JUMP_IF_FALSE:
            case OP_JUMP_IF_TRUE:
            case OP_LOOP: {
                int sign = (op == OP_LOOP) ? -1 : 1;
                int jump = jit_ir_read_u16(code, offset + 1);
                int target_byte = offset + 3 + sign * jump;
                if (target_byte < 0 || target_byte >= count) {
                    free(byte_to_jit);
                    jit_program_free(program);
                    return NULL;
                }
                int target = byte_to_jit[target_byte];
                if (target < 0 || target >= op_count) {
                    free(byte_to_jit);
                    jit_program_free(program);
                    return NULL;
                }
                out->target = (uint16_t)target;
                break;
            }
            case OP_QFOR_PREP_NUM:
            case OP_QFOR_LOOP_NUM:
            case OP_FOR_PREP:
            case OP_FOR_LOOP: {
                out->a = code[offset + 1];
                out->b = code[offset + 2];
                int sign = (op == OP_QFOR_LOOP_NUM || op == OP_FOR_LOOP) ? -1 : 1;
                int jump = jit_ir_read_u16(code, offset + 3);
                int target_byte = offset + 5 + sign * jump;
                if (target_byte < 0 || target_byte >= count) {
                    free(byte_to_jit);
                    jit_program_free(program);
                    return NULL;
                }
                int target = byte_to_jit[target_byte];
                if (target < 0 || target >= op_count) {
                    free(byte_to_jit);
                    jit_program_free(program);
                    return NULL;
                }
                out->target = (uint16_t)target;
                break;
            }
            default:
                break;
        }

        offset += jit_ir_decode_instr_len(code, offset, count);
    }

    free(byte_to_jit);
    return program;
}
