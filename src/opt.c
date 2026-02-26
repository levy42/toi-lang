#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "opt.h"
#include "value.h"
#include "object.h"

static int instr_length(Chunk* chunk, int offset) {
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
        case OP_CALL_NAMED:
        case OP_CALL_EXPAND:
        case OP_IMPORT:
        case OP_BUILD_STRING:
            return 2;
        case OP_CALL0:
        case OP_CALL1:
        case OP_CALL2:
            return 1;
        case OP_TRY:
            return 7;
        case OP_INC_LOCAL:
        case OP_SUB_LOCAL_CONST:
        case OP_MUL_LOCAL_CONST:
        case OP_DIV_LOCAL_CONST:
        case OP_MOD_LOCAL_CONST:
        case OP_UNPACK:
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
            return 2 + fn->upvalue_count * 2;
        }
        default:
            return 1;
    }
}

static void emit_byte(uint8_t** code, int* count, int* capacity, int line, int** lines, uint8_t byte) {
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

static void emit_bytes(uint8_t** code, int* count, int* capacity, int line, int** lines, uint8_t a, uint8_t b) {
    emit_byte(code, count, capacity, line, lines, a);
    emit_byte(code, count, capacity, line, lines, b);
}

static int fold_binary_numbers(Chunk* chunk, int offset, int* out_const) {
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

    *out_const = add_constant(chunk, NUMBER_VAL(result));
    return 1;
}

static int fold_unary_number(Chunk* chunk, int offset, int* out_const) {
    uint8_t op = chunk->code[offset + 2];
    if (op != OP_NEGATE) return 0;
    uint8_t c = chunk->code[offset + 1];
    Value v = chunk->constants.values[c];
    if (!IS_NUMBER(v)) return 0;
    double result = -AS_NUMBER(v);
    *out_const = add_constant(chunk, NUMBER_VAL(result));
    return 1;
}

static int is_number_constant(Chunk* chunk, int constant_index, double* out_value) {
    Value v = chunk->constants.values[constant_index];
    if (!IS_NUMBER(v)) return 0;
    *out_value = AS_NUMBER(v);
    return 1;
}

static int is_safe_single_producer(uint8_t op) {
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

void optimize_chunk(Chunk* chunk) {
    if (chunk->count == 0) return;

    uint8_t* new_code = NULL;
    int* new_lines = NULL;
    int new_count = 0;
    int new_capacity = 0;
    int old_count = chunk->count;

    int* is_jump_target = (int*)calloc(old_count, sizeof(int));
    for (int i = 0; i < old_count; ) {
        uint8_t op = chunk->code[i];
        if (op == OP_JUMP || op == OP_JUMP_IF_FALSE || op == OP_JUMP_IF_TRUE || op == OP_LOOP) {
            int sign = (op == OP_LOOP) ? -1 : 1;
            uint16_t jump = (uint16_t)(chunk->code[i + 1] << 8);
            jump |= chunk->code[i + 2];
            int target = i + 3 + sign * (int)jump;
            if (target >= 0 && target < old_count) {
                is_jump_target[target] = 1;
            }
        } else if (op == OP_TRY) {
            uint16_t ex_jump = (uint16_t)(chunk->code[i + 3] << 8);
            ex_jump |= chunk->code[i + 4];
            uint16_t fin_jump = (uint16_t)(chunk->code[i + 5] << 8);
            fin_jump |= chunk->code[i + 6];
            int ex_target = i + 7 + (int)ex_jump;
            int fin_target = i + 7 + (int)fin_jump;
            if (ex_jump && ex_target >= 0 && ex_target < old_count) {
                is_jump_target[ex_target] = 1;
            }
            if (fin_jump && fin_target >= 0 && fin_target < old_count) {
                is_jump_target[fin_target] = 1;
            }
        } else if (op == OP_FOR_PREP || op == OP_FOR_LOOP) {
            uint16_t jump = (uint16_t)(chunk->code[i + 3] << 8);
            jump |= chunk->code[i + 4];
            int target = (op == OP_FOR_PREP) ? (i + 5 + (int)jump) : (i + 5 - (int)jump);
            if (target >= 0 && target < old_count) {
                is_jump_target[target] = 1;
            }
        }
        i += instr_length(chunk, i);
    }

    int* old_to_new = (int*)malloc(sizeof(int) * old_count);
    for (int i = 0; i < old_count; i++) old_to_new[i] = -1;

    typedef struct {
        int old_offset;
        int new_offset;
        int sign;
        uint16_t old_jump;
        int write_offset;
    } JumpPatch;

    JumpPatch* patches = NULL;
    int patch_count = 0;
    int patch_capacity = 0;

    for (int i = 0; i < old_count; ) {
        old_to_new[i] = new_count;

        // Drop redundant OP_ADJUST_STACK after simple expression results:
        //   <single-producer> OP_POP OP_ADJUST_STACK
        {
            uint8_t op = chunk->code[i];
            if (is_safe_single_producer(op)) {
                int len1 = instr_length(chunk, i);
                int next = i + len1;
                if (next < old_count && chunk->code[next] == OP_POP) {
                    int next2 = next + 1;
                    if (next2 < old_count && chunk->code[next2] == OP_ADJUST_STACK) {
                        if (!is_jump_target[i] && !is_jump_target[next] && !is_jump_target[next2]) {
                            int line = chunk->lines[i];
                            for (int j = 0; j < len1; j++) {
                                emit_byte(&new_code, &new_count, &new_capacity, line, &new_lines, chunk->code[i + j]);
                            }
                            emit_byte(&new_code, &new_count, &new_capacity, line, &new_lines, OP_POP);
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
                if (is_number_constant(chunk, c, &num)) {
                    int drop = 0;
                    if ((op == OP_ADD || op == OP_SUBTRACT) && num == 0) {
                        drop = 1;
                    } else if ((op == OP_MULTIPLY || op == OP_DIVIDE) && num == 1) {
                        drop = 1;
                    }
                    if (drop && !is_jump_target[i] && !is_jump_target[i + 2]) {
                        i += 3;
                        continue;
                    }
                }
            }
        }

        // Local increment: GET_LOCAL s, CONST c, ADD, SET_LOCAL s
        if (i + 6 < old_count &&
            chunk->code[i] == OP_GET_LOCAL &&
            chunk->code[i + 2] == OP_CONSTANT &&
            chunk->code[i + 4] == OP_ADD &&
            chunk->code[i + 5] == OP_SET_LOCAL) {
            uint8_t slot_get = chunk->code[i + 1];
            uint8_t constant = chunk->code[i + 3];
            uint8_t slot_set = chunk->code[i + 6];
            if (slot_get == slot_set) {
                double num;
                if (is_number_constant(chunk, constant, &num)) {
                    if (!is_jump_target[i + 2] && !is_jump_target[i + 4] && !is_jump_target[i + 5]) {
                        int line = chunk->lines[i];
                        emit_byte(&new_code, &new_count, &new_capacity, line, &new_lines, OP_INC_LOCAL);
                        emit_byte(&new_code, &new_count, &new_capacity, line, &new_lines, slot_get);
                        emit_byte(&new_code, &new_count, &new_capacity, line, &new_lines, constant);
                        i += 7;
                        continue;
                    }
                }
            }
        }

        // Local op with const: GET_LOCAL s, CONST c, OP, SET_LOCAL s
        if (i + 6 < old_count &&
            chunk->code[i] == OP_GET_LOCAL &&
            chunk->code[i + 2] == OP_CONSTANT &&
            chunk->code[i + 5] == OP_SET_LOCAL) {
            uint8_t op = chunk->code[i + 4];
            if (op == OP_SUBTRACT || op == OP_MULTIPLY || op == OP_DIVIDE || op == OP_MODULO) {
                uint8_t slot_get = chunk->code[i + 1];
                uint8_t constant = chunk->code[i + 3];
                uint8_t slot_set = chunk->code[i + 6];
                if (slot_get == slot_set &&
                    !is_jump_target[i + 2] && !is_jump_target[i + 4] && !is_jump_target[i + 5]) {
                    int line = chunk->lines[i];
                    uint8_t new_op = op;
                    switch (op) {
                        case OP_SUBTRACT: new_op = OP_SUB_LOCAL_CONST; break;
                        case OP_MULTIPLY: new_op = OP_MUL_LOCAL_CONST; break;
                        case OP_DIVIDE: new_op = OP_DIV_LOCAL_CONST; break;
                        case OP_MODULO: new_op = OP_MOD_LOCAL_CONST; break;
                        default: break;
                    }
                    emit_byte(&new_code, &new_count, &new_capacity, line, &new_lines, new_op);
                    emit_byte(&new_code, &new_count, &new_capacity, line, &new_lines, slot_get);
                    emit_byte(&new_code, &new_count, &new_capacity, line, &new_lines, constant);
                    i += 7;
                    continue;
                }
            }
        }

        // Fold constant binary ops: CONST a, CONST b, OP
        if (i + 4 < chunk->count &&
            chunk->code[i] == OP_CONSTANT &&
            chunk->code[i + 2] == OP_CONSTANT) {
            int new_const = -1;
            if (fold_binary_numbers(chunk, i, &new_const) &&
                !is_jump_target[i + 2] && !is_jump_target[i + 4]) {
                int line = chunk->lines[i];
                emit_bytes(&new_code, &new_count, &new_capacity, line, &new_lines,
                          OP_CONSTANT, (uint8_t)new_const);
                i += 5;
                continue;
            }
        }

        // Fold constant unary negate: CONST a, NEGATE
        if (i + 2 < chunk->count &&
            chunk->code[i] == OP_CONSTANT) {
            int new_const = -1;
            if (fold_unary_number(chunk, i, &new_const) && !is_jump_target[i + 2]) {
                int line = chunk->lines[i];
                emit_bytes(&new_code, &new_count, &new_capacity, line, &new_lines,
                          OP_CONSTANT, (uint8_t)new_const);
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
                !is_jump_target[i] && !is_jump_target[i + 2]) {
                uint8_t constant = chunk->code[i + 1];
                int line = chunk->lines[i];
                uint8_t new_op = op;
                switch (op) {
                    case OP_ADD: new_op = OP_ADD_CONST; break;
                    case OP_SUBTRACT: new_op = OP_SUB_CONST; break;
                    case OP_MULTIPLY: new_op = OP_MUL_CONST; break;
                    case OP_DIVIDE: new_op = OP_DIV_CONST; break;
                    case OP_MODULO: new_op = OP_MOD_CONST; break;
                    default: break;
                }
                emit_bytes(&new_code, &new_count, &new_capacity, line, &new_lines, new_op, constant);
                i += 3;
                continue;
            }
        }

        // Fuse OP_* + OP_SET_LOCAL -> OP_*_SET_LOCAL
        if (i + 2 < old_count &&
            (chunk->code[i] == OP_ADD || chunk->code[i] == OP_SUBTRACT ||
             chunk->code[i] == OP_MULTIPLY || chunk->code[i] == OP_DIVIDE ||
             chunk->code[i] == OP_MODULO) &&
            chunk->code[i + 1] == OP_SET_LOCAL &&
            !is_jump_target[i] && !is_jump_target[i + 1]) {
            int line = chunk->lines[i];
            uint8_t slot = chunk->code[i + 2];
            uint8_t new_op = chunk->code[i];
            switch (chunk->code[i]) {
                case OP_ADD: new_op = OP_ADD_SET_LOCAL; break;
                case OP_SUBTRACT: new_op = OP_SUB_SET_LOCAL; break;
                case OP_MULTIPLY: new_op = OP_MUL_SET_LOCAL; break;
                case OP_DIVIDE: new_op = OP_DIV_SET_LOCAL; break;
                case OP_MODULO: new_op = OP_MOD_SET_LOCAL; break;
                default: break;
            }
            emit_byte(&new_code, &new_count, &new_capacity, line, &new_lines, new_op);
            emit_byte(&new_code, &new_count, &new_capacity, line, &new_lines, slot);
            i += 3;
            continue;
        }

        int len = instr_length(chunk, i);
        int line = chunk->lines[i];
        if (chunk->code[i] == OP_JUMP || chunk->code[i] == OP_JUMP_IF_FALSE ||
            chunk->code[i] == OP_JUMP_IF_TRUE || chunk->code[i] == OP_LOOP) {
            if (patch_count + 1 > patch_capacity) {
                patch_capacity = patch_capacity < 8 ? 8 : patch_capacity * 2;
                patches = (JumpPatch*)realloc(patches, sizeof(JumpPatch) * patch_capacity);
            }
            int sign = (chunk->code[i] == OP_LOOP) ? -1 : 1;
            uint16_t jump = (uint16_t)(chunk->code[i + 1] << 8);
            jump |= chunk->code[i + 2];
            patches[patch_count++] = (JumpPatch){i, new_count, sign, jump, 1};
        } else if (chunk->code[i] == OP_TRY) {
            if (patch_count + 2 > patch_capacity) {
                patch_capacity = patch_capacity < 8 ? 8 : patch_capacity * 2;
                patches = (JumpPatch*)realloc(patches, sizeof(JumpPatch) * patch_capacity);
            }
            uint16_t ex_jump = (uint16_t)(chunk->code[i + 3] << 8);
            ex_jump |= chunk->code[i + 4];
            uint16_t fin_jump = (uint16_t)(chunk->code[i + 5] << 8);
            fin_jump |= chunk->code[i + 6];
            patches[patch_count++] = (JumpPatch){i, new_count, 1, ex_jump, 3};
            patches[patch_count++] = (JumpPatch){i, new_count, 1, fin_jump, 5};
        } else if (chunk->code[i] == OP_FOR_PREP || chunk->code[i] == OP_FOR_LOOP) {
            if (patch_count + 1 > patch_capacity) {
                patch_capacity = patch_capacity < 8 ? 8 : patch_capacity * 2;
                patches = (JumpPatch*)realloc(patches, sizeof(JumpPatch) * patch_capacity);
            }
            int sign = (chunk->code[i] == OP_FOR_LOOP) ? -1 : 1;
            uint16_t jump = (uint16_t)(chunk->code[i + 3] << 8);
            jump |= chunk->code[i + 4];
            patches[patch_count++] = (JumpPatch){i, new_count, sign, jump, 3};
        }
        for (int j = 0; j < len; j++) {
            emit_byte(&new_code, &new_count, &new_capacity, line, &new_lines, chunk->code[i + j]);
        }
        i += len;
    }

    for (int i = 0; i < patch_count; i++) {
        JumpPatch p = patches[i];
        int instr_len = instr_length(chunk, p.old_offset);
        int old_target = p.old_offset + instr_len + p.sign * (int)p.old_jump;
        if (old_target < 0 || old_target >= old_count) continue;
        int new_target = old_to_new[old_target];
        if (new_target < 0) continue;
        int new_instr_len = instr_length(chunk, p.old_offset);
        int new_jump = p.sign * (new_target - (p.new_offset + new_instr_len));
        if (chunk->code[p.old_offset] == OP_TRY) {
            new_code[p.new_offset + p.write_offset] = (uint8_t)((new_jump >> 8) & 0xff);
            new_code[p.new_offset + p.write_offset + 1] = (uint8_t)(new_jump & 0xff);
        } else if (chunk->code[p.old_offset] == OP_FOR_PREP || chunk->code[p.old_offset] == OP_FOR_LOOP) {
            new_code[p.new_offset + 3] = (uint8_t)((new_jump >> 8) & 0xff);
            new_code[p.new_offset + 4] = (uint8_t)(new_jump & 0xff);
        } else {
            new_code[p.new_offset + 1] = (uint8_t)((new_jump >> 8) & 0xff);
            new_code[p.new_offset + 2] = (uint8_t)(new_jump & 0xff);
        }
    }

    free(old_to_new);
    free(is_jump_target);
    free(patches);

    free(chunk->code);
    free(chunk->lines);
    free(chunk->global_ic_versions);
    free(chunk->global_ic_names);
    free(chunk->global_ic_values);
    free(chunk->get_table_ic_versions);
    free(chunk->get_table_ic_tables);
    free(chunk->get_table_ic_keys);
    free(chunk->get_table_ic_values);
    chunk->code = new_code;
    chunk->lines = new_lines;
    chunk->count = new_count;
    chunk->capacity = new_capacity;
    chunk->global_ic_versions = NULL;
    chunk->global_ic_names = NULL;
    chunk->global_ic_values = NULL;
    chunk->get_table_ic_versions = NULL;
    chunk->get_table_ic_tables = NULL;
    chunk->get_table_ic_keys = NULL;
    chunk->get_table_ic_values = NULL;
    if (new_capacity > 0) {
        chunk->global_ic_versions = (uint32_t*)malloc(sizeof(uint32_t) * new_capacity);
        chunk->global_ic_names = (ObjString**)malloc(sizeof(ObjString*) * new_capacity);
        chunk->global_ic_values = (Value*)malloc(sizeof(Value) * new_capacity);
        chunk->get_table_ic_versions = (uint32_t*)malloc(sizeof(uint32_t) * new_capacity);
        chunk->get_table_ic_tables = (ObjTable**)malloc(sizeof(ObjTable*) * new_capacity);
        chunk->get_table_ic_keys = (ObjString**)malloc(sizeof(ObjString*) * new_capacity);
        chunk->get_table_ic_values = (Value*)malloc(sizeof(Value) * new_capacity);
        for (int i = 0; i < new_capacity; i++) {
            chunk->global_ic_versions[i] = 0;
            chunk->global_ic_names[i] = NULL;
            chunk->global_ic_values[i] = NIL_VAL;
            chunk->get_table_ic_versions[i] = 0;
            chunk->get_table_ic_tables[i] = NULL;
            chunk->get_table_ic_keys[i] = NULL;
            chunk->get_table_ic_values[i] = NIL_VAL;
        }
    }
}
