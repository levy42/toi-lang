#include <stdio.h>

#include "debug.h"
#include "value.h"
#include "object.h"

void disassembleChunk(Chunk* chunk, const char* name) {
    printf("== %s ==\n", name);
    
    for (int offset = 0; offset < chunk->count;) {
        offset = disassembleInstruction(chunk, offset);
    }
}

static int simpleInstruction(const char* name, int offset) {
    printf("%s\n", name);
    return offset + 1;
}

static int constantInstruction(const char* name, Chunk* chunk, int offset) {
    uint8_t constant = chunk->code[offset + 1];
    printf("%-16s %4d '", name, constant);
    printValue(chunk->constants.values[constant]);
    printf("'\n");
    return offset + 2;
}

static int jumpInstruction(const char* name, int sign, Chunk* chunk, int offset) {
    uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
    jump |= chunk->code[offset + 2];
    printf("%-16s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
    return offset + 3;
}

static int byteInstruction(const char* name, Chunk* chunk, int offset) {
    uint8_t slot = chunk->code[offset + 1];
    printf("%-16s %4d\n", name, slot);
    return offset + 2;
}

static int doubleByteInstruction(const char* name, Chunk* chunk, int offset) {
    uint8_t a = chunk->code[offset + 1];
    uint8_t b = chunk->code[offset + 2];
    printf("%-16s %4d %4d\n", name, a, b);
    return offset + 3;
}

static int tryInstruction(const char* name, Chunk* chunk, int offset) {
    uint8_t depth = chunk->code[offset + 1];
    uint8_t flags = chunk->code[offset + 2];
    uint16_t exJump = (uint16_t)(chunk->code[offset + 3] << 8);
    exJump |= chunk->code[offset + 4];
    uint16_t finJump = (uint16_t)(chunk->code[offset + 5] << 8);
    finJump |= chunk->code[offset + 6];
    int exTarget = offset + 7 + exJump;
    int finTarget = offset + 7 + finJump;
    printf("%-16s %4d ex:%d fin:%d flags:%d\n", name, depth, exTarget, finTarget, flags);
    return offset + 7;
}

int disassembleInstruction(Chunk* chunk, int offset) {
    printf("%04d ", offset);
    
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        printf("   | ");
    } else {
        printf("%4d ", chunk->lines[offset]);
    }
    
    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
        case OP_CONSTANT:
            return constantInstruction("OP_CONSTANT", chunk, offset);
        case OP_RETURN:
            return simpleInstruction("OP_RETURN", offset);
        case OP_RETURN_N:
            return byteInstruction("OP_RETURN_N", chunk, offset);
        case OP_ADJUST_STACK:
            return byteInstruction("OP_ADJUST_STACK", chunk, offset);
        case OP_TRY:
            return tryInstruction("OP_TRY", chunk, offset);
        case OP_END_TRY:
            return simpleInstruction("OP_END_TRY", offset);
        case OP_END_FINALLY:
            return simpleInstruction("OP_END_FINALLY", offset);
        case OP_IMPORT:
            return constantInstruction("OP_IMPORT", chunk, offset);
        case OP_IMPORT_STAR:
            return simpleInstruction("OP_IMPORT_STAR", offset);
        case OP_THROW:
            return simpleInstruction("OP_THROW", offset);
        case OP_FOR_PREP: {
            uint8_t varSlot = chunk->code[offset + 1];
            uint8_t endSlot = chunk->code[offset + 2];
            uint16_t jump = (uint16_t)(chunk->code[offset + 3] << 8);
            jump |= chunk->code[offset + 4];
            printf("%-16s %4d %4d -> %d\n", "OP_FOR_PREP", varSlot, endSlot, offset + 5 + jump);
            return offset + 5;
        }
        case OP_FOR_LOOP: {
            uint8_t varSlot = chunk->code[offset + 1];
            uint8_t endSlot = chunk->code[offset + 2];
            uint16_t jump = (uint16_t)(chunk->code[offset + 3] << 8);
            jump |= chunk->code[offset + 4];
            printf("%-16s %4d %4d -> %d\n", "OP_FOR_LOOP", varSlot, endSlot, offset + 5 - jump);
            return offset + 5;
        }
        case OP_GET_LOCAL:
            return byteInstruction("OP_GET_LOCAL", chunk, offset);
        case OP_SET_LOCAL:
            return byteInstruction("OP_SET_LOCAL", chunk, offset);
        case OP_ADD_SET_LOCAL:
            return byteInstruction("OP_ADD_SET_LOCAL", chunk, offset);
        case OP_SUB_SET_LOCAL:
            return byteInstruction("OP_SUB_SET_LOCAL", chunk, offset);
        case OP_MUL_SET_LOCAL:
            return byteInstruction("OP_MUL_SET_LOCAL", chunk, offset);
        case OP_DIV_SET_LOCAL:
            return byteInstruction("OP_DIV_SET_LOCAL", chunk, offset);
        case OP_MOD_SET_LOCAL:
            return byteInstruction("OP_MOD_SET_LOCAL", chunk, offset);
        case OP_INC_LOCAL:
            return doubleByteInstruction("OP_INC_LOCAL", chunk, offset);
        case OP_SUB_LOCAL_CONST:
            return doubleByteInstruction("OP_SUB_LOCAL_CONST", chunk, offset);
        case OP_MUL_LOCAL_CONST:
            return doubleByteInstruction("OP_MUL_LOCAL_CONST", chunk, offset);
        case OP_DIV_LOCAL_CONST:
            return doubleByteInstruction("OP_DIV_LOCAL_CONST", chunk, offset);
        case OP_MOD_LOCAL_CONST:
            return doubleByteInstruction("OP_MOD_LOCAL_CONST", chunk, offset);
        case OP_GET_UPVALUE:
            return byteInstruction("OP_GET_UPVALUE", chunk, offset);
        case OP_SET_UPVALUE:
            return byteInstruction("OP_SET_UPVALUE", chunk, offset);
        case OP_CLOSE_UPVALUE:
            return simpleInstruction("OP_CLOSE_UPVALUE", offset);
        case OP_GET_TABLE:
            return simpleInstruction("OP_GET_TABLE", offset);
        case OP_SET_TABLE:
            return simpleInstruction("OP_SET_TABLE", offset);
        case OP_DELETE_TABLE:
            return simpleInstruction("OP_DELETE_TABLE", offset);
        case OP_NEW_TABLE:
            return simpleInstruction("OP_NEW_TABLE", offset);
        case OP_DUP:
            return simpleInstruction("OP_DUP", offset);
        case OP_ADD:
            return simpleInstruction("OP_ADD", offset);
        case OP_ADD_CONST:
            return constantInstruction("OP_ADD_CONST", chunk, offset);
        case OP_SUBTRACT:
            return simpleInstruction("OP_SUBTRACT", offset);
        case OP_SUB_CONST:
            return constantInstruction("OP_SUB_CONST", chunk, offset);
        case OP_MULTIPLY:
            return simpleInstruction("OP_MULTIPLY", offset);
        case OP_MUL_CONST:
            return constantInstruction("OP_MUL_CONST", chunk, offset);
        case OP_DIVIDE:
            return simpleInstruction("OP_DIVIDE", offset);
        case OP_DIV_CONST:
            return constantInstruction("OP_DIV_CONST", chunk, offset);
        case OP_NOT:
            return simpleInstruction("OP_NOT", offset);
        case OP_NEGATE:
            return simpleInstruction("OP_NEGATE", offset);
        case OP_LENGTH:
            return simpleInstruction("OP_LENGTH", offset);
        case OP_PRINT:
            return simpleInstruction("OP_PRINT", offset);
        case OP_JUMP:
            return jumpInstruction("OP_JUMP", 1, chunk, offset);
        case OP_JUMP_IF_FALSE:
            return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
        case OP_JUMP_IF_TRUE:
            return jumpInstruction("OP_JUMP_IF_TRUE", 1, chunk, offset);
        case OP_LOOP:
            return jumpInstruction("OP_LOOP", -1, chunk, offset);
        case OP_CALL:
            return byteInstruction("OP_CALL", chunk, offset);
        case OP_CALL_EXPAND:
            return byteInstruction("OP_CALL_EXPAND", chunk, offset);
        case OP_CLOSURE: {
            offset++;
            uint8_t constant = chunk->code[offset++];
            printf("%-16s %4d ", "OP_CLOSURE", constant);
            printValue(chunk->constants.values[constant]);
            printf("\n");

            // Print upvalue information
            ObjFunction* function = AS_FUNCTION(chunk->constants.values[constant]);
            for (int j = 0; j < function->upvalueCount; j++) {
                int isLocal = chunk->code[offset++];
                int index = chunk->code[offset++];
                printf("%04d    |                     %s %d\n",
                       offset - 2, isLocal ? "local" : "upvalue", index);
            }

            return offset;
        }
        case OP_POP:
            return simpleInstruction("OP_POP", offset);
        case OP_GET_GLOBAL:
            return constantInstruction("OP_GET_GLOBAL", chunk, offset);
        case OP_DEFINE_GLOBAL:
            return constantInstruction("OP_DEFINE_GLOBAL", chunk, offset);
        case OP_SET_GLOBAL:
            return constantInstruction("OP_SET_GLOBAL", chunk, offset);
        case OP_DELETE_GLOBAL:
            return constantInstruction("OP_DELETE_GLOBAL", chunk, offset);
        case OP_NIL:
            return simpleInstruction("OP_NIL", offset);
        case OP_TRUE:
            return simpleInstruction("OP_TRUE", offset);
        case OP_FALSE:
            return simpleInstruction("OP_FALSE", offset);
        case OP_EQUAL:
            return simpleInstruction("OP_EQUAL", offset);
        case OP_GREATER:
            return simpleInstruction("OP_GREATER", offset);
        case OP_LESS:
            return simpleInstruction("OP_LESS", offset);
        case OP_HAS:
            return simpleInstruction("OP_HAS", offset);
        case OP_POWER:
            return simpleInstruction("OP_POWER", offset);
        case OP_INT_DIV:
            return simpleInstruction("OP_INT_DIV", offset);
        case OP_MODULO:
            return simpleInstruction("OP_MODULO", offset);
        case OP_IADD:
            return simpleInstruction("OP_IADD", offset);
        case OP_ISUB:
            return simpleInstruction("OP_ISUB", offset);
        case OP_IMUL:
            return simpleInstruction("OP_IMUL", offset);
        case OP_IDIV:
            return simpleInstruction("OP_IDIV", offset);
        case OP_IMOD:
            return simpleInstruction("OP_IMOD", offset);
        case OP_FADD:
            return simpleInstruction("OP_FADD", offset);
        case OP_FSUB:
            return simpleInstruction("OP_FSUB", offset);
        case OP_FMUL:
            return simpleInstruction("OP_FMUL", offset);
        case OP_FDIV:
            return simpleInstruction("OP_FDIV", offset);
        case OP_FMOD:
            return simpleInstruction("OP_FMOD", offset);
        case OP_MOD_CONST:
            return constantInstruction("OP_MOD_CONST", chunk, offset);
        case OP_GC:
            return simpleInstruction("OP_GC", offset);
        case OP_SET_METATABLE:
            return simpleInstruction("OP_SET_METATABLE", offset);
        case OP_ITER_PREP:
            return simpleInstruction("OP_ITER_PREP", offset);
        case OP_ITER_PREP_IPAIRS:
            return simpleInstruction("OP_ITER_PREP_IPAIRS", offset);
        case OP_RANGE:
            return simpleInstruction("OP_RANGE", offset);
        case OP_SLICE:
            return simpleInstruction("OP_SLICE", offset);
        default:
            printf("Unknown opcode %d\n", instruction);
            return offset + 1;
            
    }
}
