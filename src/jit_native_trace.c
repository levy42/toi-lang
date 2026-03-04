#include "jit_native_trace.h"
#include "object.h"

typedef enum {
    TRACE_ORIGIN_UNKNOWN = 0,
    TRACE_ORIGIN_LOCAL,
    TRACE_ORIGIN_UPVALUE
} TraceOriginKind;

typedef struct {
    uint8_t kind;
    uint8_t slot;
} TraceOrigin;

static void trace_origin_set(TraceOrigin* origin, TraceOriginKind kind, uint8_t slot) {
    if (origin == NULL) return;
    origin->kind = (uint8_t)kind;
    origin->slot = slot;
}

static void trace_push(TraceOrigin* stack, int* sp, TraceOrigin origin) {
    if (stack == NULL || sp == NULL) return;
    if (*sp < 0 || *sp >= 1024) return;
    stack[*sp] = origin;
    (*sp)++;
}

static TraceOrigin trace_pop(TraceOrigin* stack, int* sp) {
    TraceOrigin unknown;
    trace_origin_set(&unknown, TRACE_ORIGIN_UNKNOWN, 0);
    if (stack == NULL || sp == NULL || *sp <= 0 || *sp > 1024) return unknown;
    (*sp)--;
    return stack[*sp];
}

static int trace_add_number_guard(
    ToiJitNativeGuard* guards,
    int* guard_count,
    int guard_cap,
    TraceOrigin origin,
    uint32_t deopt_pc) {
    if (guards == NULL || guard_count == NULL || *guard_count < 0 || *guard_count > guard_cap) return 0;

    ToiJitNativeGuardKind kind = TOI_JIT_NATIVE_GUARD_NONE;
    if (origin.kind == TRACE_ORIGIN_LOCAL) {
        kind = TOI_JIT_NATIVE_GUARD_LOCAL_IS_NUMBER;
    } else if (origin.kind == TRACE_ORIGIN_UPVALUE) {
        kind = TOI_JIT_NATIVE_GUARD_UPVALUE_IS_NUMBER;
    } else {
        return 1;
    }

    for (int i = 0; i < *guard_count; i++) {
        if (guards[i].kind == (uint8_t)kind && guards[i].slot_a == origin.slot) return 1;
    }
    if (*guard_count >= guard_cap) return 0;

    ToiJitNativeGuard g;
    g.kind = (uint8_t)kind;
    g.slot_a = origin.slot;
    g.slot_b = 0;
    g.aux = 0;
    g.deopt_pc = (deopt_pc <= 0xffffu) ? (uint16_t)deopt_pc : 0xffffu;
    g.constant = NIL_VAL;
    guards[*guard_count] = g;
    (*guard_count)++;
    return 1;
}

static int trace_add_string_guard(
    ToiJitNativeGuard* guards,
    int* guard_count,
    int guard_cap,
    TraceOrigin origin,
    uint32_t deopt_pc) {
    if (guards == NULL || guard_count == NULL || *guard_count < 0 || *guard_count > guard_cap) return 0;

    ToiJitNativeGuardKind kind = TOI_JIT_NATIVE_GUARD_NONE;
    if (origin.kind == TRACE_ORIGIN_LOCAL) {
        kind = TOI_JIT_NATIVE_GUARD_LOCAL_IS_STRING;
    } else if (origin.kind == TRACE_ORIGIN_UPVALUE) {
        kind = TOI_JIT_NATIVE_GUARD_UPVALUE_IS_STRING;
    } else {
        return 1;
    }

    for (int i = 0; i < *guard_count; i++) {
        if (guards[i].kind == (uint8_t)kind && guards[i].slot_a == origin.slot) return 1;
    }
    if (*guard_count >= guard_cap) return 0;

    ToiJitNativeGuard g;
    g.kind = (uint8_t)kind;
    g.slot_a = origin.slot;
    g.slot_b = 0;
    g.aux = 0;
    g.deopt_pc = (deopt_pc <= 0xffffu) ? (uint16_t)deopt_pc : 0xffffu;
    g.constant = NIL_VAL;
    guards[*guard_count] = g;
    (*guard_count)++;
    return 1;
}

static int trace_add_call_target_guard(
    ToiJitNativeGuard* guards,
    int* guard_count,
    int guard_cap,
    TraceOrigin origin,
    uint8_t argc,
    uint32_t deopt_pc) {
    if (guards == NULL || guard_count == NULL || *guard_count < 0 || *guard_count > guard_cap) return 0;
    if (origin.kind != TRACE_ORIGIN_LOCAL && origin.kind != TRACE_ORIGIN_UPVALUE) return 1;

    for (int i = 0; i < *guard_count; i++) {
        if (guards[i].kind == (uint8_t)TOI_JIT_NATIVE_GUARD_CALL_TARGET &&
            guards[i].slot_a == origin.slot &&
            guards[i].slot_b == (uint8_t)((origin.kind == TRACE_ORIGIN_UPVALUE) ? 1 : 0) &&
            guards[i].aux == argc) {
            return 1;
        }
    }
    if (*guard_count >= guard_cap) return 0;

    ToiJitNativeGuard g;
    g.kind = (uint8_t)TOI_JIT_NATIVE_GUARD_CALL_TARGET;
    g.slot_a = origin.slot;
    g.slot_b = (uint8_t)((origin.kind == TRACE_ORIGIN_UPVALUE) ? 1 : 0);
    g.aux = argc;
    g.deopt_pc = (deopt_pc <= 0xffffu) ? (uint16_t)deopt_pc : 0xffffu;
    g.constant = NIL_VAL;
    guards[*guard_count] = g;
    (*guard_count)++;
    return 1;
}

static int trace_add_invoke_target_guard(
    ToiJitNativeGuard* guards,
    int* guard_count,
    int guard_cap,
    TraceOrigin origin,
    Value method_name,
    uint8_t argc,
    uint32_t deopt_pc) {
    if (guards == NULL || guard_count == NULL || *guard_count < 0 || *guard_count > guard_cap) return 0;
    if (origin.kind != TRACE_ORIGIN_LOCAL && origin.kind != TRACE_ORIGIN_UPVALUE) return 1;
    if (!IS_STRING(method_name)) return 1;

    ObjString* method = AS_STRING(method_name);
    for (int i = 0; i < *guard_count; i++) {
        if (guards[i].kind == (uint8_t)TOI_JIT_NATIVE_GUARD_INVOKE_TARGET &&
            guards[i].slot_a == origin.slot &&
            guards[i].slot_b == (uint8_t)((origin.kind == TRACE_ORIGIN_UPVALUE) ? 1 : 0) &&
            guards[i].aux == argc &&
            IS_STRING(guards[i].constant) &&
            AS_STRING(guards[i].constant) == method) {
            return 1;
        }
    }
    if (*guard_count >= guard_cap) return 0;

    ToiJitNativeGuard g;
    g.kind = (uint8_t)TOI_JIT_NATIVE_GUARD_INVOKE_TARGET;
    g.slot_a = origin.slot;
    g.slot_b = (uint8_t)((origin.kind == TRACE_ORIGIN_UPVALUE) ? 1 : 0);
    g.aux = argc;
    g.deopt_pc = (deopt_pc <= 0xffffu) ? (uint16_t)deopt_pc : 0xffffu;
    g.constant = method_name;
    guards[*guard_count] = g;
    (*guard_count)++;
    return 1;
}

static int trace_add_table_shape_guard(
    ToiJitNativeGuard* guards,
    int* guard_count,
    int guard_cap,
    TraceOrigin origin,
    uint8_t require_no_metatable_fast,
    uint32_t deopt_pc) {
    if (guards == NULL || guard_count == NULL || *guard_count < 0 || *guard_count > guard_cap) return 0;
    if (origin.kind != TRACE_ORIGIN_LOCAL && origin.kind != TRACE_ORIGIN_UPVALUE) return 1;

    uint8_t slot_b = (uint8_t)((origin.kind == TRACE_ORIGIN_UPVALUE) ? 1 : 0);
    uint8_t aux = (uint8_t)(require_no_metatable_fast ? 1 : 0);
    for (int i = 0; i < *guard_count; i++) {
        if (guards[i].kind == (uint8_t)TOI_JIT_NATIVE_GUARD_TABLE_SHAPE &&
            guards[i].slot_a == origin.slot &&
            guards[i].slot_b == slot_b &&
            guards[i].aux == aux) {
            return 1;
        }
    }
    if (*guard_count >= guard_cap) return 0;

    ToiJitNativeGuard g;
    g.kind = (uint8_t)TOI_JIT_NATIVE_GUARD_TABLE_SHAPE;
    g.slot_a = origin.slot;
    g.slot_b = slot_b;
    g.aux = aux;
    g.deopt_pc = (deopt_pc <= 0xffffu) ? (uint16_t)deopt_pc : 0xffffu;
    g.constant = NIL_VAL;
    guards[*guard_count] = g;
    (*guard_count)++;
    return 1;
}

int toi_jit_native_trace_build_from_value_ir(
    const ToiJitValueInstr* value_instrs,
    int value_instr_count,
    const uint32_t* value_deopt_offsets,
    ToiJitNativeTraceInstr* out_instrs,
    int* out_instr_count,
    ToiJitNativeGuard* out_guards,
    int* out_guard_count,
    uint8_t* out_fail_op) {
    if (value_instrs == NULL || value_instr_count <= 0) return 0;
    if (out_instrs == NULL || out_instr_count == NULL) return 0;
    if (out_guards == NULL || out_guard_count == NULL) return 0;
    if (value_instr_count > 256) return 0;
    if (out_fail_op != NULL) *out_fail_op = 255;

    int instr_count = 0;
    int guard_count = 0;
    TraceOrigin stack[1024];
    int sp = 0;

    for (int i = 0; i < value_instr_count; i++) {
        const ToiJitValueInstr* in = &value_instrs[i];
        ToiJitNativeTraceInstr out;
        out.a = in->a;
        out.b = in->b;
        out.target = in->target;
        out.deopt_offset = (value_deopt_offsets != NULL) ? value_deopt_offsets[i] : 0xffffffffu;
        out.aux_data = 0;
        out.constant = in->constant;
        switch (in->op) {
            case TOI_JIT_VAL_PUSH_LOCAL:
                out.op = TOI_JIT_NATIVE_OP_PUSH_LOCAL;
                if (sp < 1024) trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_LOCAL, in->a});
                break;
            case TOI_JIT_VAL_PUSH_UPVALUE:
                out.op = TOI_JIT_NATIVE_OP_PUSH_UPVALUE;
                if (sp < 1024) trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UPVALUE, in->a});
                break;
            case TOI_JIT_VAL_PUSH_CONST:
                out.op = TOI_JIT_NATIVE_OP_PUSH_CONST;
                if (sp < 1024) trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                break;
            case TOI_JIT_VAL_PUSH_NIL:
                out.op = TOI_JIT_NATIVE_OP_PUSH_NIL;
                if (sp < 1024) trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                break;
            case TOI_JIT_VAL_PUSH_TRUE:
                out.op = TOI_JIT_NATIVE_OP_PUSH_TRUE;
                if (sp < 1024) trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                break;
            case TOI_JIT_VAL_PUSH_FALSE:
                out.op = TOI_JIT_NATIVE_OP_PUSH_FALSE;
                if (sp < 1024) trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                break;
            case TOI_JIT_VAL_STORE_LOCAL:
                out.op = TOI_JIT_NATIVE_OP_STORE_LOCAL;
                break;
            case TOI_JIT_VAL_POP:
                out.op = TOI_JIT_NATIVE_OP_POP;
                (void)trace_pop(stack, &sp);
                break;
            case TOI_JIT_VAL_BIN_ADD:
                out.op = TOI_JIT_NATIVE_OP_BIN_ADD;
                if (sp >= 2) {
                    (void)trace_pop(stack, &sp);
                    (void)trace_pop(stack, &sp);
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            case TOI_JIT_VAL_BIN_SUB:
                out.op = TOI_JIT_NATIVE_OP_BIN_SUB;
                if (sp >= 2) {
                    TraceOrigin rhs = trace_pop(stack, &sp);
                    TraceOrigin lhs = trace_pop(stack, &sp);
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, lhs, out.deopt_offset)) return 0;
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, rhs, out.deopt_offset)) return 0;
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            case TOI_JIT_VAL_BIN_MUL:
                out.op = TOI_JIT_NATIVE_OP_BIN_MUL;
                if (sp >= 2) {
                    TraceOrigin rhs = trace_pop(stack, &sp);
                    TraceOrigin lhs = trace_pop(stack, &sp);
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, lhs, out.deopt_offset)) return 0;
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, rhs, out.deopt_offset)) return 0;
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            case TOI_JIT_VAL_BIN_DIV:
                out.op = TOI_JIT_NATIVE_OP_BIN_DIV;
                if (sp >= 2) {
                    TraceOrigin rhs = trace_pop(stack, &sp);
                    TraceOrigin lhs = trace_pop(stack, &sp);
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, lhs, out.deopt_offset)) return 0;
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, rhs, out.deopt_offset)) return 0;
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            case TOI_JIT_VAL_BIN_MOD:
                out.op = TOI_JIT_NATIVE_OP_BIN_MOD;
                if (sp >= 2) {
                    TraceOrigin rhs = trace_pop(stack, &sp);
                    TraceOrigin lhs = trace_pop(stack, &sp);
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, lhs, out.deopt_offset)) return 0;
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, rhs, out.deopt_offset)) return 0;
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            case TOI_JIT_VAL_CMP_EQ:
                out.op = TOI_JIT_NATIVE_OP_CMP_EQ;
                if (sp >= 2) {
                    (void)trace_pop(stack, &sp);
                    (void)trace_pop(stack, &sp);
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            case TOI_JIT_VAL_CMP_LT:
                out.op = TOI_JIT_NATIVE_OP_CMP_LT;
                if (sp >= 2) {
                    TraceOrigin rhs = trace_pop(stack, &sp);
                    TraceOrigin lhs = trace_pop(stack, &sp);
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, lhs, out.deopt_offset)) return 0;
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, rhs, out.deopt_offset)) return 0;
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            case TOI_JIT_VAL_CMP_GT:
                out.op = TOI_JIT_NATIVE_OP_CMP_GT;
                if (sp >= 2) {
                    TraceOrigin rhs = trace_pop(stack, &sp);
                    TraceOrigin lhs = trace_pop(stack, &sp);
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, lhs, out.deopt_offset)) return 0;
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, rhs, out.deopt_offset)) return 0;
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            case TOI_JIT_VAL_NOT:
                out.op = TOI_JIT_NATIVE_OP_NOT;
                break;
            case TOI_JIT_VAL_JUMP:
                out.op = TOI_JIT_NATIVE_OP_JUMP;
                break;
            case TOI_JIT_VAL_JUMP_IF_FALSE:
                out.op = TOI_JIT_NATIVE_OP_JUMP_IF_FALSE;
                break;
            case TOI_JIT_VAL_JUMP_IF_TRUE:
                out.op = TOI_JIT_NATIVE_OP_JUMP_IF_TRUE;
                break;
            case TOI_JIT_VAL_APPEND:
                out.op = TOI_JIT_NATIVE_OP_APPEND;
                if (sp >= 2) {
                    (void)trace_pop(stack, &sp);
                    (void)trace_pop(stack, &sp);
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            case TOI_JIT_VAL_GET_TABLE:
                out.op = TOI_JIT_NATIVE_OP_GET_TABLE;
                if (sp >= 2) {
                    TraceOrigin key = trace_pop(stack, &sp);
                    TraceOrigin table = trace_pop(stack, &sp);
                    if (!trace_add_table_shape_guard(out_guards, &guard_count, 256, table, 1, out.deopt_offset)) return 0;
                    if (!trace_add_string_guard(out_guards, &guard_count, 256, key, out.deopt_offset)) return 0;
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            case TOI_JIT_VAL_GET_TABLE_NUM:
                out.op = TOI_JIT_NATIVE_OP_GET_TABLE_NUM;
                if (sp >= 2) {
                    TraceOrigin key = trace_pop(stack, &sp);
                    TraceOrigin table = trace_pop(stack, &sp);
                    if (!trace_add_table_shape_guard(out_guards, &guard_count, 256, table, 1, out.deopt_offset)) return 0;
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, key, out.deopt_offset)) return 0;
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            case TOI_JIT_VAL_SET_TABLE:
                out.op = TOI_JIT_NATIVE_OP_SET_TABLE;
                if (sp >= 3) {
                    (void)trace_pop(stack, &sp);
                    TraceOrigin key = trace_pop(stack, &sp);
                    TraceOrigin table = trace_pop(stack, &sp);
                    if (!trace_add_table_shape_guard(out_guards, &guard_count, 256, table, 1, out.deopt_offset)) return 0;
                    if (!trace_add_string_guard(out_guards, &guard_count, 256, key, out.deopt_offset)) return 0;
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            case TOI_JIT_VAL_SET_TABLE_NUM:
                out.op = TOI_JIT_NATIVE_OP_SET_TABLE_NUM;
                if (sp >= 3) {
                    (void)trace_pop(stack, &sp);
                    TraceOrigin key = trace_pop(stack, &sp);
                    TraceOrigin table = trace_pop(stack, &sp);
                    if (!trace_add_table_shape_guard(out_guards, &guard_count, 256, table, 1, out.deopt_offset)) return 0;
                    if (!trace_add_number_guard(out_guards, &guard_count, 256, key, out.deopt_offset)) return 0;
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            case TOI_JIT_VAL_CALL: {
                out.op = TOI_JIT_NATIVE_OP_CALL;
                int argc = (int)in->a;
                if (argc >= 0 && sp >= argc + 1) {
                    TraceOrigin callee = stack[sp - 1 - argc];
                    if (!trace_add_call_target_guard(
                            out_guards,
                            &guard_count,
                            256,
                            callee,
                            (uint8_t)argc,
                            out.deopt_offset)) return 0;
                    for (int j = 0; j < argc + 1; j++) (void)trace_pop(stack, &sp);
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            }
            case TOI_JIT_VAL_INVOKE: {
                out.op = TOI_JIT_NATIVE_OP_INVOKE;
                int argc = (int)in->a;
                if (argc >= 0 && sp >= argc + 1) {
                    TraceOrigin receiver = stack[sp - 1 - argc];
                    if (!trace_add_invoke_target_guard(
                            out_guards,
                            &guard_count,
                            256,
                            receiver,
                            in->constant,
                            (uint8_t)argc,
                            out.deopt_offset)) return 0;
                    for (int j = 0; j < argc + 1; j++) (void)trace_pop(stack, &sp);
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            }
            case TOI_JIT_VAL_BUILD_STRING2:
            case TOI_JIT_VAL_BUILD_STRING3:
            case TOI_JIT_VAL_BUILD_STRING4:
            case TOI_JIT_VAL_BUILD_STRING5: {
                out.op = (in->op == TOI_JIT_VAL_BUILD_STRING2) ? TOI_JIT_NATIVE_OP_BUILD_STRING2 :
                    (in->op == TOI_JIT_VAL_BUILD_STRING3) ? TOI_JIT_NATIVE_OP_BUILD_STRING3 :
                    (in->op == TOI_JIT_VAL_BUILD_STRING4) ? TOI_JIT_NATIVE_OP_BUILD_STRING4 :
                    TOI_JIT_NATIVE_OP_BUILD_STRING5;
                int n = (in->op == TOI_JIT_VAL_BUILD_STRING2) ? 2 :
                    (in->op == TOI_JIT_VAL_BUILD_STRING3) ? 3 :
                    (in->op == TOI_JIT_VAL_BUILD_STRING4) ? 4 : 5;
                if (sp >= n) {
                    for (int j = 0; j < n; j++) (void)trace_pop(stack, &sp);
                    trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            }
            case TOI_JIT_VAL_ADJUST_STACK:
                out.op = TOI_JIT_NATIVE_OP_ADJUST_STACK;
                if ((int)in->a >= 0 && (int)in->a <= 1024) {
                    int target = (int)in->a;
                    while (sp > target) (void)trace_pop(stack, &sp);
                    while (sp < target) trace_push(stack, &sp, (TraceOrigin){TRACE_ORIGIN_UNKNOWN, 0});
                }
                break;
            default:
                if (out_fail_op != NULL) *out_fail_op = in->op;
                return 0;
        }
        out_instrs[instr_count++] = out;
    }

    *out_instr_count = instr_count;
    *out_guard_count = guard_count;
    return instr_count > 0;
}
