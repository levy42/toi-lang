/* Shared lowering of a compiled function into a one-shot native trace.
 * Private implementation included by the architecture backends. */
typedef struct {
    int patch_insn;
    uint16_t target_op;
} JitNativeLowerPatch;

#define JIT_NATIVE_TARGET_EXIT 0xFFFFu

static int jit_native_emit_instr(
    ToiJitNativeTraceInstr* out_instrs,
    int* io_count,
    uint8_t op,
    uint8_t a,
    uint8_t b,
    uint16_t target,
    uint32_t deopt_offset,
    uint64_t aux_data,
    Value constant) {
    if (out_instrs == NULL || io_count == NULL) return 0;
    int idx = *io_count;
    if (idx < 0 || idx >= 256) return 0;
    ToiJitNativeTraceInstr* in = &out_instrs[idx];
    in->op = op;
    in->a = a;
    in->b = b;
    in->target = target;
    in->deopt_offset = deopt_offset;
    in->aux_data = aux_data;
    in->constant = constant;
    *io_count = idx + 1;
    return 1;
}

static int jit_native_lower_chain_safe_program(
    const JitProgram* program,
    int argc,
    ToiJitNativeTraceInstr* out_instrs,
    int* out_instr_count,
    uint8_t* out_ret_slot,
    uint8_t* out_var_slot,
    uint8_t* out_end_slot,
    uint8_t* out_slot_count) {
    if (program == NULL || program->ops == NULL || program->op_count <= 0 ||
        out_instrs == NULL || out_instr_count == NULL ||
        out_ret_slot == NULL || out_var_slot == NULL ||
        out_end_slot == NULL || out_slot_count == NULL) {
        return 0;
    }
    if (argc < 0) return 0;
    if (program->max_slot < argc - 1) return 0;
    if (program->max_slot > 252) return 0;
    // Keep lowering bounded by instruction buffer capacity; do not impose a tiny
    // program-length gate here because it blocks real hot handlers.
    if (program->op_count > 1024) return 0;
    uint8_t ret_slot = (uint8_t)(program->max_slot + 1);
    uint8_t var_slot = (uint8_t)(program->max_slot + 2);
    uint8_t end_slot = (uint8_t)(program->max_slot + 3);
    uint8_t slot_count = (uint8_t)(program->max_slot + 4);

    int op_to_insn[1024];
    for (int i = 0; i < program->op_count; i++) op_to_insn[i] = -1;

    JitNativeLowerPatch patches[256];
    int patch_count = 0;
    int insn_count = 0;

    for (int i = 0; i < program->op_count; i++) {
        const JitOp* op = &program->ops[i];
        op_to_insn[i] = insn_count;
        switch (op->op) {
            case JOP_CONSTANT:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_CONST, 0, 0, 0, op->byte_offset, 0, op->constant)) return 0;
                break;
            case JOP_NIL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_NIL, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_TRUE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_TRUE, 0, 0, 0, op->byte_offset, 0, BOOL_VAL(1))) return 0;
                break;
            case JOP_FALSE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_FALSE, 0, 0, 0, op->byte_offset, 0, BOOL_VAL(0))) return 0;
                break;
            case JOP_POP:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_POP, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_GET_LOCAL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_LOCAL, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_GET2_LOCAL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_LOCAL, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_LOCAL, op->b, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_GET_UPVALUE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_UPVALUE, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_SET_LOCAL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_STORE_LOCAL, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_SET_LOCAL_POP:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_STORE_LOCAL, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_POP, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_INC_LOCAL:
            case JOP_SUB_LOCAL_CONST:
            case JOP_MUL_LOCAL_CONST:
            case JOP_DIV_LOCAL_CONST:
            case JOP_MOD_LOCAL_CONST: {
                uint8_t bin_op = TOI_JIT_NATIVE_OP_BIN_ADD;
                if (op->op == JOP_SUB_LOCAL_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_SUB;
                else if (op->op == JOP_MUL_LOCAL_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_MUL;
                else if (op->op == JOP_DIV_LOCAL_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_DIV;
                else if (op->op == JOP_MOD_LOCAL_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_MOD;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_LOCAL, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_CONST, 0, 0, 0, op->byte_offset, 0, op->constant)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, bin_op, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_STORE_LOCAL, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            }
            case JOP_ADD_CONST:
            case JOP_SUB_CONST:
            case JOP_MUL_CONST:
            case JOP_DIV_CONST:
            case JOP_MOD_CONST: {
                uint8_t bin_op = TOI_JIT_NATIVE_OP_BIN_ADD;
                if (op->op == JOP_SUB_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_SUB;
                else if (op->op == JOP_MUL_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_MUL;
                else if (op->op == JOP_DIV_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_DIV;
                else if (op->op == JOP_MOD_CONST) bin_op = TOI_JIT_NATIVE_OP_BIN_MOD;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_CONST, 0, 0, 0, op->byte_offset, 0, op->constant)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, bin_op, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            }
            case JOP_NEGATE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_PUSH_CONST, 0, 0, 0, op->byte_offset, 0, NUMBER_VAL(-1.0))) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BIN_MUL, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_ADD:
            case JOP_ADD_INPLACE:
            case JOP_IADD:
            case JOP_FADD:
            case JOP_QFADD:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BIN_ADD, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_SUBTRACT:
            case JOP_ISUB:
            case JOP_FSUB:
            case JOP_QFSUB:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BIN_SUB, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_MULTIPLY:
            case JOP_IMUL:
            case JOP_FMUL:
            case JOP_QFMUL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BIN_MUL, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_DIVIDE:
            case JOP_IDIV:
            case JOP_FDIV:
            case JOP_QFDIV:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BIN_DIV, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_MODULO:
            case JOP_IMOD:
            case JOP_FMOD:
            case JOP_QFMOD:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BIN_MOD, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_NOT:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_NOT, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_EQUAL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_CMP_EQ, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_QGREATER_NUM:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_CMP_GT, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_QLESS_NUM:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_CMP_LT, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_APPEND:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_APPEND, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_GET_TABLE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_GET_TABLE, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_GET_TABLE_NUM:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_GET_TABLE_NUM, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_SET_TABLE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_SET_TABLE, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_SET_TABLE_NUM:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_SET_TABLE_NUM, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_CALL:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_CALL, op->a, 0, 0, op->byte_offset, op->target, op->constant)) return 0;
                break;
            case JOP_QCALL2:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_CALL, 2, 0, 0, op->byte_offset, op->target, op->constant)) return 0;
                break;
            case JOP_INVOKE:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_INVOKE, op->a, 0, 0, op->byte_offset, op->target, op->constant)) return 0;
                break;
            case JOP_BUILD_STRING2:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BUILD_STRING2, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_BUILD_STRING3:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BUILD_STRING3, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_BUILD_STRING4:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BUILD_STRING4, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_BUILD_STRING5:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_BUILD_STRING5, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_JUMP:
            case JOP_JUMP_IF_FALSE:
            case JOP_JUMP_IF_TRUE: {
                uint8_t nop = TOI_JIT_NATIVE_OP_JUMP;
                if (op->op == JOP_JUMP_IF_FALSE) nop = TOI_JIT_NATIVE_OP_JUMP_IF_FALSE;
                else if (op->op == JOP_JUMP_IF_TRUE) nop = TOI_JIT_NATIVE_OP_JUMP_IF_TRUE;
                if (!jit_native_emit_instr(out_instrs, &insn_count, nop, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (patch_count >= 256) return 0;
                patches[patch_count++] = (JitNativeLowerPatch){insn_count - 1, op->target};
                break;
            }
            case JOP_ADJUST_STACK:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_ADJUST_STACK, op->a, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                break;
            case JOP_RETURN:
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_STORE_LOCAL, ret_slot, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (!jit_native_emit_instr(out_instrs, &insn_count, TOI_JIT_NATIVE_OP_JUMP, 0, 0, 0, op->byte_offset, 0, NIL_VAL)) return 0;
                if (patch_count >= 256) return 0;
                patches[patch_count++] = (JitNativeLowerPatch){insn_count - 1, JIT_NATIVE_TARGET_EXIT};
                break;
            case JOP_TINY_INLINE:
            case JOP_LENGTH:
            case JOP_SLICE:
            case JOP_QFOR_PREP_NUM:
            case JOP_QFOR_LOOP_NUM:
                return 0;
            default:
                return 0;
        }
    }

    if (insn_count <= 0 || insn_count >= 256) return 0;
    int exit_target = insn_count;
    for (int i = 0; i < patch_count; i++) {
        int target_insn = -1;
        uint16_t target_op = patches[i].target_op;
        if (target_op == JIT_NATIVE_TARGET_EXIT) {
            target_insn = exit_target;
        } else if ((int)target_op >= 0 && (int)target_op < program->op_count) {
            target_insn = op_to_insn[target_op];
        }
        if (target_insn < 0 || target_insn > exit_target) return 0;
        out_instrs[patches[i].patch_insn].target = (uint16_t)target_insn;
    }

    *out_instr_count = insn_count;
    *out_ret_slot = ret_slot;
    *out_var_slot = var_slot;
    *out_end_slot = end_slot;
    *out_slot_count = slot_count;
    return 1;
}

