#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "opt.h"
#include "value.h"
#include "object.h"

static int instrLength(Chunk* chunk, int offset) {
    uint8_t op = chunk->code[offset];
    switch (op) {
        case OP_CONSTANT:
        case OP_ADD_CONST:
        case OP_SUB_CONST:
        case OP_MUL_CONST:
        case OP_DIV_CONST:
        case OP_MOD_CONST:
        case OP_GET_GLOBAL:
        case OP_DEFINE_GLOBAL:
        case OP_SET_GLOBAL:
        case OP_DELETE_GLOBAL:
        case OP_GET_LOCAL:
        case OP_SET_LOCAL:
        case OP_ADD_SET_LOCAL:
        case OP_SUB_SET_LOCAL:
        case OP_MUL_SET_LOCAL:
        case OP_DIV_SET_LOCAL:
        case OP_MOD_SET_LOCAL:
        case OP_GET_UPVALUE:
        case OP_SET_UPVALUE:
        case OP_RETURN_N:
        case OP_ADJUST_STACK:
        case OP_CALL:
        case OP_IMPORT:
            return 2;
        case OP_TRY:
            return 7;
        case OP_INC_LOCAL:
        case OP_SUB_LOCAL_CONST:
        case OP_MUL_LOCAL_CONST:
        case OP_DIV_LOCAL_CONST:
        case OP_MOD_LOCAL_CONST:
            return 3;
        case OP_FOR_PREP:
        case OP_FOR_LOOP:
            return 5;
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_TRUE:
        case OP_LOOP:
            return 3;
        case OP_CLOSURE: {
            uint8_t constant = chunk->code[offset + 1];
            Value val = chunk->constants.values[constant];
            if (!IS_FUNCTION(val)) {
                return 2;
            }
            ObjFunction* fn = AS_FUNCTION(val);
            return 2 + fn->upvalueCount * 2;
        }
        default:
            return 1;
    }
}

static void emitByte(uint8_t** code, int* count, int* capacity, int line, int** lines, uint8_t byte) {
    if (*count + 1 > *capacity) {
        int old = *capacity;
        *capacity = old < 8 ? 8 : old * 2;
        *code = (uint8_t*)realloc(*code, *capacity);
        *lines = (int*)realloc(*lines, sizeof(int) * (*capacity));
    }
    (*code)[*count] = byte;
    (*lines)[*count] = line;
    (*count)++;
}

static void emitBytes(uint8_t** code, int* count, int* capacity, int line, int** lines, uint8_t a, uint8_t b) {
    emitByte(code, count, capacity, line, lines, a);
    emitByte(code, count, capacity, line, lines, b);
}

static int foldBinaryNumbers(Chunk* chunk, int offset, int* outConst) {
    uint8_t op = chunk->code[offset + 4];
    if (op != OP_ADD && op != OP_SUBTRACT && op != OP_MULTIPLY &&
        op != OP_DIVIDE && op != OP_POWER && op != OP_INT_DIV &&
        op != OP_MODULO) {
        return 0;
    }

    uint8_t c1 = chunk->code[offset + 1];
    uint8_t c2 = chunk->code[offset + 3];
    Value v1 = chunk->constants.values[c1];
    Value v2 = chunk->constants.values[c2];
    if (!IS_NUMBER(v1) || !IS_NUMBER(v2)) return 0;

    double a = AS_NUMBER(v1);
    double b = AS_NUMBER(v2);
    double result;
    switch (op) {
        case OP_ADD: result = a + b; break;
        case OP_SUBTRACT: result = a - b; break;
        case OP_MULTIPLY: result = a * b; break;
        case OP_DIVIDE: result = a / b; break;
        case OP_POWER: result = pow(a, b); break;
        case OP_INT_DIV: result = (double)((long)(a / b)); break;
        case OP_MODULO: result = fmod(a, b); break;
        default: return 0;
    }

    *outConst = addConstant(chunk, NUMBER_VAL(result));
    return 1;
}

static int foldUnaryNumber(Chunk* chunk, int offset, int* outConst) {
    uint8_t op = chunk->code[offset + 2];
    if (op != OP_NEGATE) return 0;
    uint8_t c = chunk->code[offset + 1];
    Value v = chunk->constants.values[c];
    if (!IS_NUMBER(v)) return 0;
    double result = -AS_NUMBER(v);
    *outConst = addConstant(chunk, NUMBER_VAL(result));
    return 1;
}

static int isNumberConstant(Chunk* chunk, int constantIndex, double* outValue) {
    Value v = chunk->constants.values[constantIndex];
    if (!IS_NUMBER(v)) return 0;
    *outValue = AS_NUMBER(v);
    return 1;
}

static int isSafeSingleProducer(uint8_t op) {
    switch (op) {
        case OP_CONSTANT:
        case OP_NIL:
        case OP_TRUE:
        case OP_FALSE:
        case OP_GET_LOCAL:
        case OP_GET_GLOBAL:
        case OP_GET_UPVALUE:
        case OP_DUP:
            return 1;
        default:
            return 0;
    }
}

void optimizeChunk(Chunk* chunk) {
    if (chunk->count == 0) return;

    uint8_t* newCode = NULL;
    int* newLines = NULL;
    int newCount = 0;
    int newCapacity = 0;
    int oldCount = chunk->count;

    int* isJumpTarget = (int*)calloc(oldCount, sizeof(int));
    for (int i = 0; i < oldCount; ) {
        uint8_t op = chunk->code[i];
        if (op == OP_JUMP || op == OP_JUMP_IF_FALSE || op == OP_JUMP_IF_TRUE || op == OP_LOOP) {
            int sign = (op == OP_LOOP) ? -1 : 1;
            uint16_t jump = (uint16_t)(chunk->code[i + 1] << 8);
            jump |= chunk->code[i + 2];
            int target = i + 3 + sign * (int)jump;
            if (target >= 0 && target < oldCount) {
                isJumpTarget[target] = 1;
            }
        } else if (op == OP_TRY) {
            uint16_t exJump = (uint16_t)(chunk->code[i + 3] << 8);
            exJump |= chunk->code[i + 4];
            uint16_t finJump = (uint16_t)(chunk->code[i + 5] << 8);
            finJump |= chunk->code[i + 6];
            int exTarget = i + 7 + (int)exJump;
            int finTarget = i + 7 + (int)finJump;
            if (exJump && exTarget >= 0 && exTarget < oldCount) {
                isJumpTarget[exTarget] = 1;
            }
            if (finJump && finTarget >= 0 && finTarget < oldCount) {
                isJumpTarget[finTarget] = 1;
            }
        } else if (op == OP_FOR_PREP || op == OP_FOR_LOOP) {
            uint16_t jump = (uint16_t)(chunk->code[i + 3] << 8);
            jump |= chunk->code[i + 4];
            int target = (op == OP_FOR_PREP) ? (i + 5 + (int)jump) : (i + 5 - (int)jump);
            if (target >= 0 && target < oldCount) {
                isJumpTarget[target] = 1;
            }
        }
        i += instrLength(chunk, i);
    }

    int* oldToNew = (int*)malloc(sizeof(int) * oldCount);
    for (int i = 0; i < oldCount; i++) oldToNew[i] = -1;

    typedef struct {
        int oldOffset;
        int newOffset;
        int sign;
        uint16_t oldJump;
        int writeOffset;
    } JumpPatch;

    JumpPatch* patches = NULL;
    int patchCount = 0;
    int patchCapacity = 0;

    for (int i = 0; i < oldCount; ) {
        oldToNew[i] = newCount;

        // Drop redundant OP_ADJUST_STACK after simple expression results:
        //   <single-producer> OP_POP OP_ADJUST_STACK
        {
            uint8_t op = chunk->code[i];
            if (isSafeSingleProducer(op)) {
                int len1 = instrLength(chunk, i);
                int next = i + len1;
                if (next < oldCount && chunk->code[next] == OP_POP) {
                    int next2 = next + 1;
                    if (next2 < oldCount && chunk->code[next2] == OP_ADJUST_STACK) {
                        if (!isJumpTarget[i] && !isJumpTarget[next] && !isJumpTarget[next2]) {
                            int line = chunk->lines[i];
                            for (int j = 0; j < len1; j++) {
                                emitByte(&newCode, &newCount, &newCapacity, line, &newLines, chunk->code[i + j]);
                            }
                            emitByte(&newCode, &newCount, &newCapacity, line, &newLines, OP_POP);
                            i = next2 + 2;
                            continue;
                        }
                    }
                }
            }
        }

        // Right-hand identity simplifications:
        //   x + 0 => x
        //   x - 0 => x
        //   x * 1 => x
        //   x / 1 => x
        if (i + 2 < chunk->count &&
            chunk->code[i] == OP_CONSTANT) {
            uint8_t op = chunk->code[i + 2];
            if (op == OP_ADD || op == OP_SUBTRACT || op == OP_MULTIPLY || op == OP_DIVIDE) {
                uint8_t c = chunk->code[i + 1];
                double num;
                if (isNumberConstant(chunk, c, &num)) {
                    int drop = 0;
                    if ((op == OP_ADD || op == OP_SUBTRACT) && num == 0) {
                        drop = 1;
                    } else if ((op == OP_MULTIPLY || op == OP_DIVIDE) && num == 1) {
                        drop = 1;
                    }
                    if (drop && !isJumpTarget[i] && !isJumpTarget[i + 2]) {
                        i += 3;
                        continue;
                    }
                }
            }
        }

        // Local increment: GET_LOCAL s, CONST c, ADD, SET_LOCAL s
        if (i + 6 < oldCount &&
            chunk->code[i] == OP_GET_LOCAL &&
            chunk->code[i + 2] == OP_CONSTANT &&
            chunk->code[i + 4] == OP_ADD &&
            chunk->code[i + 5] == OP_SET_LOCAL) {
            uint8_t slotGet = chunk->code[i + 1];
            uint8_t constant = chunk->code[i + 3];
            uint8_t slotSet = chunk->code[i + 6];
            if (slotGet == slotSet) {
                double num;
                if (isNumberConstant(chunk, constant, &num)) {
                    if (!isJumpTarget[i + 2] && !isJumpTarget[i + 4] && !isJumpTarget[i + 5]) {
                        int line = chunk->lines[i];
                        emitByte(&newCode, &newCount, &newCapacity, line, &newLines, OP_INC_LOCAL);
                        emitByte(&newCode, &newCount, &newCapacity, line, &newLines, slotGet);
                        emitByte(&newCode, &newCount, &newCapacity, line, &newLines, constant);
                        i += 7;
                        continue;
                    }
                }
            }
        }

        // Local op with const: GET_LOCAL s, CONST c, OP, SET_LOCAL s
        if (i + 6 < oldCount &&
            chunk->code[i] == OP_GET_LOCAL &&
            chunk->code[i + 2] == OP_CONSTANT &&
            chunk->code[i + 5] == OP_SET_LOCAL) {
            uint8_t op = chunk->code[i + 4];
            if (op == OP_SUBTRACT || op == OP_MULTIPLY || op == OP_DIVIDE || op == OP_MODULO) {
                uint8_t slotGet = chunk->code[i + 1];
                uint8_t constant = chunk->code[i + 3];
                uint8_t slotSet = chunk->code[i + 6];
                if (slotGet == slotSet &&
                    !isJumpTarget[i + 2] && !isJumpTarget[i + 4] && !isJumpTarget[i + 5]) {
                    int line = chunk->lines[i];
                    uint8_t newOp = op;
                    switch (op) {
                        case OP_SUBTRACT: newOp = OP_SUB_LOCAL_CONST; break;
                        case OP_MULTIPLY: newOp = OP_MUL_LOCAL_CONST; break;
                        case OP_DIVIDE: newOp = OP_DIV_LOCAL_CONST; break;
                        case OP_MODULO: newOp = OP_MOD_LOCAL_CONST; break;
                        default: break;
                    }
                    emitByte(&newCode, &newCount, &newCapacity, line, &newLines, newOp);
                    emitByte(&newCode, &newCount, &newCapacity, line, &newLines, slotGet);
                    emitByte(&newCode, &newCount, &newCapacity, line, &newLines, constant);
                    i += 7;
                    continue;
                }
            }
        }

        // Fold constant binary ops: CONST a, CONST b, OP
        if (i + 4 < chunk->count &&
            chunk->code[i] == OP_CONSTANT &&
            chunk->code[i + 2] == OP_CONSTANT) {
            int newConst = -1;
            if (foldBinaryNumbers(chunk, i, &newConst) &&
                !isJumpTarget[i + 2] && !isJumpTarget[i + 4]) {
                int line = chunk->lines[i];
                emitBytes(&newCode, &newCount, &newCapacity, line, &newLines,
                          OP_CONSTANT, (uint8_t)newConst);
                i += 5;
                continue;
            }
        }

        // Fold constant unary negate: CONST a, NEGATE
        if (i + 2 < chunk->count &&
            chunk->code[i] == OP_CONSTANT) {
            int newConst = -1;
            if (foldUnaryNumber(chunk, i, &newConst) && !isJumpTarget[i + 2]) {
                int line = chunk->lines[i];
                emitBytes(&newCode, &newCount, &newCapacity, line, &newLines,
                          OP_CONSTANT, (uint8_t)newConst);
                i += 3;
                continue;
            }
        }

        // Fold constant RHS into op: <value> CONST c OP_* => OP_*_CONST c
        if (i + 2 < chunk->count &&
            chunk->code[i] == OP_CONSTANT) {
            uint8_t op = chunk->code[i + 2];
            if ((op == OP_ADD || op == OP_SUBTRACT || op == OP_MULTIPLY ||
                 op == OP_DIVIDE || op == OP_MODULO) &&
                !isJumpTarget[i] && !isJumpTarget[i + 2]) {
                uint8_t constant = chunk->code[i + 1];
                int line = chunk->lines[i];
                uint8_t newOp = op;
                switch (op) {
                    case OP_ADD: newOp = OP_ADD_CONST; break;
                    case OP_SUBTRACT: newOp = OP_SUB_CONST; break;
                    case OP_MULTIPLY: newOp = OP_MUL_CONST; break;
                    case OP_DIVIDE: newOp = OP_DIV_CONST; break;
                    case OP_MODULO: newOp = OP_MOD_CONST; break;
                    default: break;
                }
                emitBytes(&newCode, &newCount, &newCapacity, line, &newLines, newOp, constant);
                i += 3;
                continue;
            }
        }

        // Fuse OP_* + OP_SET_LOCAL -> OP_*_SET_LOCAL
        if (i + 2 < oldCount &&
            (chunk->code[i] == OP_ADD || chunk->code[i] == OP_SUBTRACT ||
             chunk->code[i] == OP_MULTIPLY || chunk->code[i] == OP_DIVIDE ||
             chunk->code[i] == OP_MODULO) &&
            chunk->code[i + 1] == OP_SET_LOCAL &&
            !isJumpTarget[i] && !isJumpTarget[i + 1]) {
            int line = chunk->lines[i];
            uint8_t slot = chunk->code[i + 2];
            uint8_t newOp = chunk->code[i];
            switch (chunk->code[i]) {
                case OP_ADD: newOp = OP_ADD_SET_LOCAL; break;
                case OP_SUBTRACT: newOp = OP_SUB_SET_LOCAL; break;
                case OP_MULTIPLY: newOp = OP_MUL_SET_LOCAL; break;
                case OP_DIVIDE: newOp = OP_DIV_SET_LOCAL; break;
                case OP_MODULO: newOp = OP_MOD_SET_LOCAL; break;
                default: break;
            }
            emitByte(&newCode, &newCount, &newCapacity, line, &newLines, newOp);
            emitByte(&newCode, &newCount, &newCapacity, line, &newLines, slot);
            i += 3;
            continue;
        }

        int len = instrLength(chunk, i);
        int line = chunk->lines[i];
        if (chunk->code[i] == OP_JUMP || chunk->code[i] == OP_JUMP_IF_FALSE ||
            chunk->code[i] == OP_JUMP_IF_TRUE || chunk->code[i] == OP_LOOP) {
            if (patchCount + 1 > patchCapacity) {
                patchCapacity = patchCapacity < 8 ? 8 : patchCapacity * 2;
                patches = (JumpPatch*)realloc(patches, sizeof(JumpPatch) * patchCapacity);
            }
            int sign = (chunk->code[i] == OP_LOOP) ? -1 : 1;
            uint16_t jump = (uint16_t)(chunk->code[i + 1] << 8);
            jump |= chunk->code[i + 2];
            patches[patchCount++] = (JumpPatch){i, newCount, sign, jump, 1};
        } else if (chunk->code[i] == OP_TRY) {
            if (patchCount + 2 > patchCapacity) {
                patchCapacity = patchCapacity < 8 ? 8 : patchCapacity * 2;
                patches = (JumpPatch*)realloc(patches, sizeof(JumpPatch) * patchCapacity);
            }
            uint16_t exJump = (uint16_t)(chunk->code[i + 3] << 8);
            exJump |= chunk->code[i + 4];
            uint16_t finJump = (uint16_t)(chunk->code[i + 5] << 8);
            finJump |= chunk->code[i + 6];
            patches[patchCount++] = (JumpPatch){i, newCount, 1, exJump, 3};
            patches[patchCount++] = (JumpPatch){i, newCount, 1, finJump, 5};
        } else if (chunk->code[i] == OP_FOR_PREP || chunk->code[i] == OP_FOR_LOOP) {
            if (patchCount + 1 > patchCapacity) {
                patchCapacity = patchCapacity < 8 ? 8 : patchCapacity * 2;
                patches = (JumpPatch*)realloc(patches, sizeof(JumpPatch) * patchCapacity);
            }
            int sign = (chunk->code[i] == OP_FOR_LOOP) ? -1 : 1;
            uint16_t jump = (uint16_t)(chunk->code[i + 3] << 8);
            jump |= chunk->code[i + 4];
            patches[patchCount++] = (JumpPatch){i, newCount, sign, jump, 3};
        }
        for (int j = 0; j < len; j++) {
            emitByte(&newCode, &newCount, &newCapacity, line, &newLines, chunk->code[i + j]);
        }
        i += len;
    }

    for (int i = 0; i < patchCount; i++) {
        JumpPatch p = patches[i];
        int instrLen = instrLength(chunk, p.oldOffset);
        int oldTarget = p.oldOffset + instrLen + p.sign * (int)p.oldJump;
        if (oldTarget < 0 || oldTarget >= oldCount) continue;
        int newTarget = oldToNew[oldTarget];
        if (newTarget < 0) continue;
        int newInstrLen = instrLength(chunk, p.oldOffset);
        int newJump = p.sign * (newTarget - (p.newOffset + newInstrLen));
        if (chunk->code[p.oldOffset] == OP_TRY) {
            newCode[p.newOffset + p.writeOffset] = (uint8_t)((newJump >> 8) & 0xff);
            newCode[p.newOffset + p.writeOffset + 1] = (uint8_t)(newJump & 0xff);
        } else if (chunk->code[p.oldOffset] == OP_FOR_PREP || chunk->code[p.oldOffset] == OP_FOR_LOOP) {
            newCode[p.newOffset + 3] = (uint8_t)((newJump >> 8) & 0xff);
            newCode[p.newOffset + 4] = (uint8_t)(newJump & 0xff);
        } else {
            newCode[p.newOffset + 1] = (uint8_t)((newJump >> 8) & 0xff);
            newCode[p.newOffset + 2] = (uint8_t)(newJump & 0xff);
        }
    }

    free(oldToNew);
    free(isJumpTarget);
    free(patches);

    free(chunk->code);
    free(chunk->lines);
    chunk->code = newCode;
    chunk->lines = newLines;
    chunk->count = newCount;
    chunk->capacity = newCapacity;
}
