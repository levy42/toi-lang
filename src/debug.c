#include <stdio.h>

#include "debug.h"
#include "value.h"
#include "object.h"

void disassemble_chunk(Chunk* chunk, const char* name) {
    printf("== %s ==\n", name);
    
    for (int offset = 0; offset < chunk->count;) {
        offset = disassemble_instruction(chunk, offset);
    }
}

static int simple_instruction(const char* name, int offset) {
    printf("%s\n", name);
    return offset + 1;
}

static int constant_instruction(const char* name, Chunk* chunk, int offset) {
    uint8_t constant = chunk->code[offset + 1];
    printf("%-16s %4d '", name, constant);
    print_value(chunk->constants.values[constant]);
    printf("'\n");
    return offset + 2;
}

static int jump_instruction(const char* name, int sign, Chunk* chunk, int offset) {
    uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
    jump |= chunk->code[offset + 2];
    printf("%-16s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
    return offset + 3;
}

static int byte_instruction(const char* name, Chunk* chunk, int offset) {
    uint8_t slot = chunk->code[offset + 1];
    printf("%-16s %4d\n", name, slot);
    return offset + 2;
}

static int double_byte_instruction(const char* name, Chunk* chunk, int offset) {
    uint8_t a = chunk->code[offset + 1];
    uint8_t b = chunk->code[offset + 2];
    printf("%-16s %4d %4d\n", name, a, b);
    return offset + 3;
}

static int try_instruction(const char* name, Chunk* chunk, int offset) {
    uint8_t depth = chunk->code[offset + 1];
    uint8_t flags = chunk->code[offset + 2];
    uint16_t ex_jump = (uint16_t)(chunk->code[offset + 3] << 8);
    ex_jump |= chunk->code[offset + 4];
    uint16_t fin_jump = (uint16_t)(chunk->code[offset + 5] << 8);
    fin_jump |= chunk->code[offset + 6];
    int ex_target = offset + 7 + ex_jump;
    int fin_target = offset + 7 + fin_jump;
    printf("%-16s %4d ex:%d fin:%d flags:%d\n", name, depth, ex_target, fin_target, flags);
    return offset + 7;
}

int disassemble_instruction(Chunk* chunk, int offset) {
    printf("%04d ", offset);
    
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        printf("   | ");
    } else {
        printf("%4d ", chunk->lines[offset]);
    }
    
    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
        case OP_CONSTANT:
            return constant_instruction("OP_CONSTANT", chunk, offset);
        case OP_APPEND:
            return simple_instruction("OP_APPEND", offset);
        case OP_RETURN:
            return simple_instruction("OP_RETURN", offset);
        case OP_RETURN_N:
            return byte_instruction("OP_RETURN_N", chunk, offset);
        case OP_ADJUST_STACK:
            return byte_instruction("OP_ADJUST_STACK", chunk, offset);
        case OP_UNPACK:
            return double_byte_instruction("OP_UNPACK", chunk, offset);
        case OP_TRY:
            return try_instruction("OP_TRY", chunk, offset);
        case OP_END_TRY:
            return simple_instruction("OP_END_TRY", offset);
        case OP_END_FINALLY:
            return simple_instruction("OP_END_FINALLY", offset);
        case OP_IMPORT:
            return constant_instruction("OP_IMPORT", chunk, offset);
        case OP_IMPORT_STAR:
            return simple_instruction("OP_IMPORT_STAR", offset);
        case OP_THROW:
            return simple_instruction("OP_THROW", offset);
        case OP_BUILD_STRING:
            return byte_instruction("OP_BUILD_STRING", chunk, offset);
        case OP_FOR_PREP: {
            uint8_t var_slot = chunk->code[offset + 1];
            uint8_t end_slot = chunk->code[offset + 2];
            uint16_t jump = (uint16_t)(chunk->code[offset + 3] << 8);
            jump |= chunk->code[offset + 4];
            printf("%-16s %4d %4d -> %d\n", "OP_FOR_PREP", var_slot, end_slot, offset + 5 + jump);
            return offset + 5;
        }
        case OP_FOR_LOOP: {
            uint8_t var_slot = chunk->code[offset + 1];
            uint8_t end_slot = chunk->code[offset + 2];
            uint16_t jump = (uint16_t)(chunk->code[offset + 3] << 8);
            jump |= chunk->code[offset + 4];
            printf("%-16s %4d %4d -> %d\n", "OP_FOR_LOOP", var_slot, end_slot, offset + 5 - jump);
            return offset + 5;
        }
        case OP_GET_LOCAL:
            return byte_instruction("OP_GET_LOCAL", chunk, offset);
        case OP_SET_LOCAL:
            return byte_instruction("OP_SET_LOCAL", chunk, offset);
        case OP_ADD_SET_LOCAL:
            return byte_instruction("OP_ADD_SET_LOCAL", chunk, offset);
        case OP_SUB_SET_LOCAL:
            return byte_instruction("OP_SUB_SET_LOCAL", chunk, offset);
        case OP_MUL_SET_LOCAL:
            return byte_instruction("OP_MUL_SET_LOCAL", chunk, offset);
        case OP_DIV_SET_LOCAL:
            return byte_instruction("OP_DIV_SET_LOCAL", chunk, offset);
        case OP_MOD_SET_LOCAL:
            return byte_instruction("OP_MOD_SET_LOCAL", chunk, offset);
        case OP_INC_LOCAL:
            return double_byte_instruction("OP_INC_LOCAL", chunk, offset);
        case OP_SUB_LOCAL_CONST:
            return double_byte_instruction("OP_SUB_LOCAL_CONST", chunk, offset);
        case OP_MUL_LOCAL_CONST:
            return double_byte_instruction("OP_MUL_LOCAL_CONST", chunk, offset);
        case OP_DIV_LOCAL_CONST:
            return double_byte_instruction("OP_DIV_LOCAL_CONST", chunk, offset);
        case OP_MOD_LOCAL_CONST:
            return double_byte_instruction("OP_MOD_LOCAL_CONST", chunk, offset);
        case OP_GET_UPVALUE:
            return byte_instruction("OP_GET_UPVALUE", chunk, offset);
        case OP_SET_UPVALUE:
            return byte_instruction("OP_SET_UPVALUE", chunk, offset);
        case OP_CLOSE_UPVALUE:
            return simple_instruction("OP_CLOSE_UPVALUE", offset);
        case OP_GET_TABLE:
            return simple_instruction("OP_GET_TABLE", offset);
        case OP_GET_META_TABLE:
            return simple_instruction("OP_GET_META_TABLE", offset);
        case OP_SET_TABLE:
            return simple_instruction("OP_SET_TABLE", offset);
        case OP_DELETE_TABLE:
            return simple_instruction("OP_DELETE_TABLE", offset);
        case OP_NEW_TABLE:
            return simple_instruction("OP_NEW_TABLE", offset);
        case OP_DUP:
            return simple_instruction("OP_DUP", offset);
        case OP_ADD:
            return simple_instruction("OP_ADD", offset);
        case OP_ADD_CONST:
            return constant_instruction("OP_ADD_CONST", chunk, offset);
        case OP_SUBTRACT:
            return simple_instruction("OP_SUBTRACT", offset);
        case OP_SUB_CONST:
            return constant_instruction("OP_SUB_CONST", chunk, offset);
        case OP_MULTIPLY:
            return simple_instruction("OP_MULTIPLY", offset);
        case OP_MUL_CONST:
            return constant_instruction("OP_MUL_CONST", chunk, offset);
        case OP_DIVIDE:
            return simple_instruction("OP_DIVIDE", offset);
        case OP_DIV_CONST:
            return constant_instruction("OP_DIV_CONST", chunk, offset);
        case OP_NOT:
            return simple_instruction("OP_NOT", offset);
        case OP_NEGATE:
            return simple_instruction("OP_NEGATE", offset);
        case OP_LENGTH:
            return simple_instruction("OP_LENGTH", offset);
        case OP_PRINT:
            return byte_instruction("OP_PRINT", chunk, offset);
        case OP_JUMP:
            return jump_instruction("OP_JUMP", 1, chunk, offset);
        case OP_JUMP_IF_FALSE:
            return jump_instruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
        case OP_JUMP_IF_TRUE:
            return jump_instruction("OP_JUMP_IF_TRUE", 1, chunk, offset);
        case OP_LOOP:
            return jump_instruction("OP_LOOP", -1, chunk, offset);
        case OP_CALL:
            return byte_instruction("OP_CALL", chunk, offset);
        case OP_CALL0:
            return simple_instruction("OP_CALL0", offset);
        case OP_CALL1:
            return simple_instruction("OP_CALL1", offset);
        case OP_CALL2:
            return simple_instruction("OP_CALL2", offset);
        case OP_CALL_NAMED:
            return byte_instruction("OP_CALL_NAMED", chunk, offset);
        case OP_CALL_EXPAND:
            return byte_instruction("OP_CALL_EXPAND", chunk, offset);
        case OP_CLOSURE: {
            offset++;
            uint8_t constant = chunk->code[offset++];
            printf("%-16s %4d ", "OP_CLOSURE", constant);
            print_value(chunk->constants.values[constant]);
            printf("\n");

            // Print upvalue information
            ObjFunction* function = AS_FUNCTION(chunk->constants.values[constant]);
            for (int j = 0; j < function->upvalue_count; j++) {
                int is_local = chunk->code[offset++];
                int index = chunk->code[offset++];
                printf("%04d    |                     %s %d\n",
                       offset - 2, is_local ? "local" : "upvalue", index);
            }

            return offset;
        }
        case OP_POP:
            return simple_instruction("OP_POP", offset);
        case OP_GET_GLOBAL:
            return constant_instruction("OP_GET_GLOBAL", chunk, offset);
        case OP_DEFINE_GLOBAL:
            return constant_instruction("OP_DEFINE_GLOBAL", chunk, offset);
        case OP_SET_GLOBAL:
            return constant_instruction("OP_SET_GLOBAL", chunk, offset);
        case OP_DELETE_GLOBAL:
            return constant_instruction("OP_DELETE_GLOBAL", chunk, offset);
        case OP_NIL:
            return simple_instruction("OP_NIL", offset);
        case OP_TRUE:
            return simple_instruction("OP_TRUE", offset);
        case OP_FALSE:
            return simple_instruction("OP_FALSE", offset);
        case OP_EQUAL:
            return simple_instruction("OP_EQUAL", offset);
        case OP_GREATER:
            return simple_instruction("OP_GREATER", offset);
        case OP_LESS:
            return simple_instruction("OP_LESS", offset);
        case OP_HAS:
            return simple_instruction("OP_HAS", offset);
        case OP_IN:
            return simple_instruction("OP_IN", offset);
        case OP_POWER:
            return simple_instruction("OP_POWER", offset);
        case OP_INT_DIV:
            return simple_instruction("OP_INT_DIV", offset);
        case OP_MODULO:
            return simple_instruction("OP_MODULO", offset);
        case OP_IADD:
            return simple_instruction("OP_IADD", offset);
        case OP_ISUB:
            return simple_instruction("OP_ISUB", offset);
        case OP_IMUL:
            return simple_instruction("OP_IMUL", offset);
        case OP_IDIV:
            return simple_instruction("OP_IDIV", offset);
        case OP_IMOD:
            return simple_instruction("OP_IMOD", offset);
        case OP_FADD:
            return simple_instruction("OP_FADD", offset);
        case OP_FSUB:
            return simple_instruction("OP_FSUB", offset);
        case OP_FMUL:
            return simple_instruction("OP_FMUL", offset);
        case OP_FDIV:
            return simple_instruction("OP_FDIV", offset);
        case OP_FMOD:
            return simple_instruction("OP_FMOD", offset);
        case OP_MOD_CONST:
            return constant_instruction("OP_MOD_CONST", chunk, offset);
        case OP_GC:
            return simple_instruction("OP_GC", offset);
        case OP_SET_METATABLE:
            return simple_instruction("OP_SET_METATABLE", offset);
        case OP_ITER_PREP:
            return simple_instruction("OP_ITER_PREP", offset);
        case OP_ITER_PREP_IPAIRS:
            return simple_instruction("OP_ITER_PREP_IPAIRS", offset);
        case OP_RANGE:
            return simple_instruction("OP_RANGE", offset);
        case OP_SLICE:
            return simple_instruction("OP_SLICE", offset);
        default:
            printf("Unknown opcode %d\n", instruction);
            return offset + 1;
            
    }
}
