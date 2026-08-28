#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "jit_call_runtime.h"
#include "vm/build_string.h"

extern ToiJitState* jit_state(VM* vm);
extern JitTrackedFunction* jit_find_or_add(VM* vm, ObjFunction* function);
extern uint16_t program_runtime_probe_limit(const JitProgram* program);
extern int execute_program(
    VM* vm,
    ObjClosure* closure,
    int arg_count,
    JitProgram* program,
    Value* args,
    Value* out);
extern int jit_eval_tiny_inline2_local(ObjFunction* function, Value arg0, Value arg1, Value* out);

static int to_int64_local_runtime(double x, int64_t* out) {
    if (out == NULL) return 0;
    int64_t i = (int64_t)x;
    if ((double)i != x) return 0;
    *out = i;
    return 1;
}
static int jit_ascii_case_transform(ObjString* src, int to_upper, Value* out) {
    if (src == NULL || out == NULL) return 0;
    int len = src->length;
    char* buf = (char*)malloc((size_t)len + 1);
    if (buf == NULL) return 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src->chars[i];
        buf[i] = (char)(to_upper ? toupper(c) : tolower(c));
    }
    buf[len] = '\0';
    ObjString* s = take_string(buf, len);
    *out = OBJ_VAL(s);
    return 1;
}

int jit_build_string_primitive(VM* vm, Value* parts, int count, Value* out) {
    (void)vm;
    if (parts == NULL || out == NULL || count < 2 || count > 5) return 0;
    if (vm != NULL) {
        int primitive_only = 1;
        for (int i = 0; i < count; i++) {
            Value p = parts[i];
            if (!IS_STRING(p) && !IS_NUMBER(p) && !IS_BOOL(p) && !IS_NIL(p)) {
                primitive_only = 0;
                break;
            }
        }
        if (primitive_only) {
            ObjThread* thread = vm_current_thread(vm);
            if (thread != NULL && thread->stack_top + count <= thread->stack + thread->stack_capacity) {
                Value* saved_top = thread->stack_top;
                for (int i = 0; i < count; i++) {
                    *thread->stack_top++ = parts[i];
                }
                if (vm_build_string(vm, (uint8_t)count)) {
                    *out = pop(vm);
                    thread->stack_top = saved_top;
                    return 1;
                }
                thread->stack_top = saved_top;
                return 0;
            }
        }
    }
    int total_len = 0;
    for (int i = 0; i < count; i++) {
        Value p = parts[i];
        if (IS_STRING(p)) {
            total_len += AS_STRING(p)->length;
            continue;
        }
        if (IS_NUMBER(p)) {
            char num_buf[32];
            int n = snprintf(num_buf, sizeof(num_buf), "%.14g", AS_NUMBER(p));
            if (n < 0) return 0;
            total_len += n;
            continue;
        }
        if (IS_BOOL(p)) {
            total_len += AS_BOOL(p) ? 4 : 5;
            continue;
        }
        if (IS_NIL(p)) {
            total_len += 3;
            continue;
        }
        return 0;
    }
    char* buf = (char*)malloc((size_t)total_len + 1);
    if (buf == NULL) return 0;
    char* cur = buf;
    for (int i = 0; i < count; i++) {
        Value p = parts[i];
        if (IS_STRING(p)) {
            ObjString* s = AS_STRING(p);
            memcpy(cur, s->chars, (size_t)s->length);
            cur += s->length;
        } else if (IS_NUMBER(p)) {
            int n = snprintf(cur, 32, "%.14g", AS_NUMBER(p));
            if (n < 0) {
                free(buf);
                return 0;
            }
            cur += n;
        } else if (IS_BOOL(p)) {
            if (AS_BOOL(p)) {
                memcpy(cur, "true", 4);
                cur += 4;
            } else {
                memcpy(cur, "false", 5);
                cur += 5;
            }
        } else {
            memcpy(cur, "nil", 3);
            cur += 3;
        }
    }
    *cur = '\0';
    ObjString* s = take_string(buf, total_len);
    *out = OBJ_VAL(s);
    return 1;
}

static int jit_value_to_string_primitive(Value in, Value* out) {
    if (out == NULL) return 0;
    if (IS_STRING(in)) {
        *out = in;
        return 1;
    }
    if (IS_NUMBER(in)) {
        char num_buf[32];
        int n = snprintf(num_buf, sizeof(num_buf), "%.14g", AS_NUMBER(in));
        if (n < 0) return 0;
        *out = OBJ_VAL(copy_string(num_buf, n));
        return 1;
    }
    if (IS_BOOL(in)) {
        *out = OBJ_VAL(copy_string(AS_BOOL(in) ? "true" : "false", AS_BOOL(in) ? 4 : 5));
        return 1;
    }
    if (IS_NIL(in)) {
        *out = OBJ_VAL(copy_string("nil", 3));
        return 1;
    }
    return 0;
}

int jit_value_matches_type(Value v, uint8_t type) {
    switch (type) {
        case TYPEHINT_ANY:
            return 1;
        case TYPEHINT_INT: {
            if (!IS_NUMBER(v)) return 0;
            int64_t tmp = 0;
            return to_int64_local_runtime(AS_NUMBER(v), &tmp);
        }
        case TYPEHINT_FLOAT:
            return IS_NUMBER(v);
        case TYPEHINT_BOOL:
            return IS_BOOL(v);
        case TYPEHINT_STR:
            return IS_STRING(v);
        case TYPEHINT_TABLE:
            return IS_TABLE(v);
        default:
            return 0;
    }
}

int jit_length_primitive(Value in, Value* out) {
    if (out == NULL) return 0;
    if (IS_STRING(in)) {
        *out = NUMBER_VAL((double)AS_STRING(in)->length);
        return 1;
    }
    if (IS_TABLE(in)) {
        ObjTable* t = AS_TABLE(in);
        int count = t->table.count;
        for (int i = 0; i < t->table.array_capacity; i++) {
            if (!IS_NIL(t->table.array[i])) count++;
        }
        *out = NUMBER_VAL((double)count);
        return 1;
    }
    return 0;
}

int jit_slice_primitive(Value obj, Value start, Value end, Value step, Value* out) {
    if (out == NULL) return 0;
    if (!IS_STRING(obj)) return 0;
    ObjString* s = AS_STRING(obj);
    int len = s->length;

    if (IS_NIL(step)) step = NUMBER_VAL(1.0);
    if (!IS_NUMBER(step)) return 0;
    double step_num = AS_NUMBER(step);
    if (step_num == 0.0) return 0;

    if (IS_NIL(start)) start = NUMBER_VAL(step_num < 0.0 ? (double)len : 1.0);
    if (IS_NIL(end)) end = NUMBER_VAL(step_num < 0.0 ? 1.0 : (double)len);
    if (!IS_NUMBER(start) || !IS_NUMBER(end)) return 0;

    double raw_start = AS_NUMBER(start);
    double raw_end = AS_NUMBER(end);
    int start_int = (int)raw_start;
    int end_int = (int)raw_end;
    if ((double)start_int != raw_start || (double)end_int != raw_end) return 0;

    if (start_int < 0) start_int = len + start_int + 1;
    if (end_int < 0) end_int = len + end_int;

    int s0 = start_int;
    int e0 = end_int;
    if (s0 < 1) s0 = 1;
    if (e0 > s->length) e0 = s->length;

    if (step_num > 0.0) {
        if (s0 > e0) {
            *out = OBJ_VAL(copy_string("", 0));
            return 1;
        }
        if (step_num == 1.0 && s0 == 1 && e0 == s->length) {
            *out = obj;
            return 1;
        }
        if (step_num == 1.0) {
            int out_len = e0 - s0 + 1;
            *out = OBJ_VAL(copy_string_unhashed(s->chars + (s0 - 1), out_len));
            return 1;
        }
        int step_i = (int)step_num;
        int out_len = 0;
        for (int i = s0; i <= e0; i += step_i) out_len++;
        char* buf = (char*)malloc((size_t)out_len + 1);
        if (buf == NULL) return 0;
        int w = 0;
        for (int i = s0; i <= e0; i += step_i) buf[w++] = s->chars[i - 1];
        buf[w] = '\0';
        *out = OBJ_VAL(take_string_unhashed(buf, w));
        return 1;
    }

    if (s0 < e0) {
        *out = OBJ_VAL(copy_string("", 0));
        return 1;
    }
    int step_i = (int)step_num;
    int out_len = 0;
    for (int i = s0; i >= e0; i += step_i) out_len++;
    char* buf = (char*)malloc((size_t)out_len + 1);
    if (buf == NULL) return 0;
    int w = 0;
    for (int i = s0; i >= e0; i += step_i) buf[w++] = s->chars[i - 1];
    buf[w] = '\0';
    *out = OBJ_VAL(take_string_unhashed(buf, w));
    return 1;
}

static int jit_execute_closure_program(VM* vm, ObjClosure* closure, Value* args, int argc, Value* out) {
    if (vm == NULL || closure == NULL || out == NULL) return 0;
    ObjFunction* fn = closure->function;
    if (fn == NULL) return 0;
    if (fn->is_variadic || fn->defaults_count != 0) return 0;
    if (argc != fn->arity || argc < 0) return 0;
    if (fn->param_types_count > 0 && fn->param_types != NULL) {
        int check_count = (fn->param_types_count < fn->arity) ? fn->param_types_count : fn->arity;
        for (int i = 0; i < check_count; i++) {
            if (!jit_value_matches_type(args[i], fn->param_types[i])) return 0;
        }
    }

    ToiJitState* st = jit_state(vm);
    if (st == NULL) return 0;
    JitTrackedFunction* entry = jit_find_or_add(vm, fn);
    if (entry == NULL) return 0;
    if (!entry->compiled && !entry->failed && !entry->queued) {
        entry->queued = 1;
        entry->compile_attempts++;
        vm->native_jit_compile_attempts++;
        JitProgram* program = jit_program_compile(fn);
        entry->queued = 0;
        if (program == NULL) {
            entry->failed = 1;
            return 0;
        }
        entry->program = program;
        entry->compiled = 1;
        entry->runtime_disabled = 0;
        entry->runtime_probe_limit = program_runtime_probe_limit(program);
        vm->native_jit_compiled_count++;
    }
    if (!entry->compiled || entry->program == NULL || entry->runtime_disabled) return 0;

    return execute_program(vm, closure, argc, entry->program, args, out);
}

int jit_get_compiled_closure_program(VM* vm, ObjClosure* closure, JitProgram** out_program) {
    if (out_program == NULL) return 0;
    *out_program = NULL;
    if (vm == NULL || closure == NULL) return 0;
    ObjFunction* fn = closure->function;
    if (fn == NULL) return 0;
    if (fn->is_variadic || fn->defaults_count != 0) return 0;

    ToiJitState* st = jit_state(vm);
    if (st == NULL) return 0;
    JitTrackedFunction* entry = jit_find_or_add(vm, fn);
    if (entry == NULL) return 0;
    if (!entry->compiled && !entry->failed && !entry->queued) {
        entry->queued = 1;
        entry->compile_attempts++;
        vm->native_jit_compile_attempts++;
        JitProgram* program = jit_program_compile(fn);
        entry->queued = 0;
        if (program == NULL) {
            entry->failed = 1;
            return 0;
        }
        entry->program = program;
        entry->compiled = 1;
        entry->runtime_disabled = 0;
        entry->runtime_probe_limit = program_runtime_probe_limit(program);
        vm->native_jit_compiled_count++;
    }
    if (!entry->compiled || entry->program == NULL || entry->runtime_disabled) return 0;
    *out_program = entry->program;
    return 1;
}

static int jit_execute_compiled_program_with_args(
    VM* vm,
    ObjClosure* closure,
    JitProgram* program,
    Value* args,
    int argc,
    Value* out) {
    if (vm == NULL || closure == NULL || program == NULL || out == NULL) return 0;
    ObjFunction* fn = closure->function;
    if (fn != NULL && fn->param_types_count > 0 && fn->param_types != NULL) {
        int check_count = (fn->param_types_count < fn->arity) ? fn->param_types_count : fn->arity;
        for (int i = 0; i < check_count; i++) {
            if (!jit_value_matches_type(args[i], fn->param_types[i])) return 0;
        }
    }
    return execute_program(vm, closure, argc, program, args, out);
}

static int jit_seed_trace_call_ic(
    ObjFunction* function,
    const JitOp* op,
    int argc,
    ObjClosure** out_closure,
    JitProgram** out_program) {
    if (out_closure == NULL || out_program == NULL) return 0;
    *out_closure = NULL;
    *out_program = NULL;
    if (function == NULL || op == NULL) return 0;
    Chunk* chunk = &function->chunk;
    int off = (int)op->byte_offset;
    if (off < 0 || off >= chunk->capacity) return 0;
    if (chunk->call_ic_closures == NULL || chunk->call_ic_argc == NULL) return 0;
    ObjClosure* c = chunk->call_ic_closures[off];
    if (c != NULL && (int)chunk->call_ic_argc[off] == argc) {
        *out_closure = c;
        return 1;
    }
    if (chunk->call_ic2_closures == NULL || chunk->call_ic2_argc == NULL) return 0;
    c = chunk->call_ic2_closures[off];
    if (c != NULL && (int)chunk->call_ic2_argc[off] == argc) {
        *out_closure = c;
        return 1;
    }
    if (chunk->call_ic3_closures != NULL && chunk->call_ic3_argc != NULL) {
        c = chunk->call_ic3_closures[off];
        if (c != NULL && (int)chunk->call_ic3_argc[off] == argc) {
            *out_closure = c;
            return 1;
        }
    }
    if (chunk->call_ic4_closures != NULL && chunk->call_ic4_argc != NULL) {
        c = chunk->call_ic4_closures[off];
        if (c != NULL && (int)chunk->call_ic4_argc[off] == argc) {
            *out_closure = c;
            return 1;
        }
    }
    if (chunk->call_ic5_closures != NULL && chunk->call_ic5_argc != NULL) {
        c = chunk->call_ic5_closures[off];
        if (c != NULL && (int)chunk->call_ic5_argc[off] == argc) {
            *out_closure = c;
            return 1;
        }
    }
    return 0;
}

static int jit_program_loop_call_safe(const JitProgram* program) {
    if (program == NULL || program->ops == NULL || program->op_count <= 0) return 0;
    for (int i = 0; i < program->op_count; i++) {
        switch (program->ops[i].op) {
            case JOP_CONSTANT:
            case JOP_NIL:
            case JOP_TRUE:
            case JOP_FALSE:
            case JOP_POP:
            case JOP_GET_LOCAL:
            case JOP_GET2_LOCAL:
            case JOP_GET_UPVALUE:
            case JOP_SET_LOCAL:
            case JOP_SET_LOCAL_POP:
            case JOP_INC_LOCAL:
            case JOP_SUB_LOCAL_CONST:
            case JOP_MUL_LOCAL_CONST:
            case JOP_DIV_LOCAL_CONST:
            case JOP_MOD_LOCAL_CONST:
            case JOP_ADD_CONST:
            case JOP_SUB_CONST:
            case JOP_MUL_CONST:
            case JOP_DIV_CONST:
            case JOP_MOD_CONST:
            case JOP_ADD:
            case JOP_ADD_INPLACE:
            case JOP_SUBTRACT:
            case JOP_MULTIPLY:
            case JOP_DIVIDE:
            case JOP_MODULO:
            case JOP_NEGATE:
            case JOP_NOT:
            case JOP_IADD:
            case JOP_ISUB:
            case JOP_IMUL:
            case JOP_IDIV:
            case JOP_IMOD:
            case JOP_FADD:
            case JOP_FSUB:
            case JOP_FMUL:
            case JOP_FDIV:
            case JOP_FMOD:
            case JOP_QFADD:
            case JOP_QFSUB:
            case JOP_QFMUL:
            case JOP_QFDIV:
            case JOP_QFMOD:
            case JOP_EQUAL:
            case JOP_QGREATER_NUM:
            case JOP_QLESS_NUM:
            case JOP_JUMP:
            case JOP_JUMP_IF_FALSE:
            case JOP_JUMP_IF_TRUE:
            case JOP_ADJUST_STACK:
            case JOP_RETURN:
            case JOP_TINY_INLINE:
                break;
            default:
                return 0;
        }
    }
    return 1;
}

int jit_try_safe_loop_closure_call(
    VM* vm,
    ObjClosure* closure,
    Value* args,
    int argc,
    Value* out,
    JitProgram** out_program) {
    if (out_program != NULL) *out_program = NULL;
    if (vm == NULL || closure == NULL || out == NULL) return 0;
    ObjFunction* fn = closure->function;
    if (fn == NULL) return 0;
    if (fn->arity != argc || fn->is_variadic || fn->defaults_count != 0) return 0;
    JitProgram* program = NULL;
    if (!jit_get_compiled_closure_program(vm, closure, &program)) return 0;
    if (!jit_program_loop_call_safe(program)) return 0;
    if (!jit_execute_compiled_program_with_args(vm, closure, program, args, argc, out)) return 0;
    if (out_program != NULL) *out_program = program;
    return 1;
}

void jit_update_trace_call_ic(VM* vm, JitLoopTrace* trace, int pc, Value callee, int argc) {
    if (vm == NULL || trace == NULL) return;
    if (pc < 0 || pc >= trace->op_count) return;
    if (trace->call_ic_closures == NULL || trace->call_ic_programs == NULL ||
        trace->call_ic2_closures == NULL || trace->call_ic2_programs == NULL ||
        trace->call_ic3_closures == NULL || trace->call_ic3_programs == NULL ||
        trace->call_ic4_closures == NULL || trace->call_ic4_programs == NULL ||
        trace->call_ic5_closures == NULL || trace->call_ic5_programs == NULL) {
        return;
    }
    if (!IS_CLOSURE(callee)) return;
    ObjClosure* closure = AS_CLOSURE(callee);
    if (closure == NULL || closure->function == NULL) return;
    if (closure->function->arity != argc) return;
    JitProgram* program = NULL;
    if (!jit_get_compiled_closure_program(vm, closure, &program)) return;
    if (!jit_program_loop_call_safe(program)) return;

    if (trace->call_ic_closures[pc] == closure) {
        trace->call_ic_programs[pc] = program;
        return;
    }
    if (trace->call_ic2_closures[pc] == closure) {
        trace->call_ic2_programs[pc] = program;
        return;
    }
    if (trace->call_ic3_closures[pc] == closure) {
        trace->call_ic3_programs[pc] = program;
        return;
    }
    if (trace->call_ic4_closures[pc] == closure) {
        trace->call_ic4_programs[pc] = program;
        return;
    }
    if (trace->call_ic5_closures[pc] == closure) {
        trace->call_ic5_programs[pc] = program;
        return;
    }

    if (trace->call_ic_closures[pc] == NULL) {
        trace->call_ic_closures[pc] = closure;
        trace->call_ic_programs[pc] = program;
        return;
    }

    if (trace->call_ic2_closures[pc] == NULL) {
        trace->call_ic2_closures[pc] = closure;
        trace->call_ic2_programs[pc] = program;
        return;
    }
    if (trace->call_ic3_closures[pc] == NULL) {
        trace->call_ic3_closures[pc] = closure;
        trace->call_ic3_programs[pc] = program;
        return;
    }
    if (trace->call_ic4_closures[pc] == NULL) {
        trace->call_ic4_closures[pc] = closure;
        trace->call_ic4_programs[pc] = program;
        return;
    }
    if (trace->call_ic5_closures[pc] == NULL) {
        trace->call_ic5_closures[pc] = closure;
        trace->call_ic5_programs[pc] = program;
        return;
    }

    // Keep slot1 as most stable target, rotate replacement across 2..5.
    ObjClosure* old2 = trace->call_ic2_closures[pc];
    JitProgram* old2p = trace->call_ic2_programs[pc];
    trace->call_ic2_closures[pc] = trace->call_ic3_closures[pc];
    trace->call_ic2_programs[pc] = trace->call_ic3_programs[pc];
    trace->call_ic3_closures[pc] = trace->call_ic4_closures[pc];
    trace->call_ic3_programs[pc] = trace->call_ic4_programs[pc];
    trace->call_ic4_closures[pc] = trace->call_ic5_closures[pc];
    trace->call_ic4_programs[pc] = trace->call_ic5_programs[pc];
    trace->call_ic5_closures[pc] = closure;
    trace->call_ic5_programs[pc] = program;
    (void)old2;
    (void)old2p;
}

int jit_try_trace_call_ic(
    VM* vm,
    ObjFunction* function,
    JitLoopTrace* trace,
    const JitOp* op,
    int pc,
    Value callee,
    Value* args,
    int argc,
    Value* out) {
    if (vm == NULL || trace == NULL || op == NULL || out == NULL) return 0;
    if (pc < 0 || pc >= trace->op_count) return 0;
    if (trace->call_ic_closures == NULL || trace->call_ic_programs == NULL ||
        trace->call_ic2_closures == NULL || trace->call_ic2_programs == NULL ||
        trace->call_ic3_closures == NULL || trace->call_ic3_programs == NULL ||
        trace->call_ic4_closures == NULL || trace->call_ic4_programs == NULL ||
        trace->call_ic5_closures == NULL || trace->call_ic5_programs == NULL) {
        return 0;
    }
    ObjClosure* closure1 = trace->call_ic_closures[pc];
    JitProgram* program1 = trace->call_ic_programs[pc];
    ObjClosure* closure2 = trace->call_ic2_closures[pc];
    JitProgram* program2 = trace->call_ic2_programs[pc];
    ObjClosure* closure3 = trace->call_ic3_closures[pc];
    JitProgram* program3 = trace->call_ic3_programs[pc];
    ObjClosure* closure4 = trace->call_ic4_closures[pc];
    JitProgram* program4 = trace->call_ic4_programs[pc];
    ObjClosure* closure5 = trace->call_ic5_closures[pc];
    JitProgram* program5 = trace->call_ic5_programs[pc];
    if (closure1 == NULL && closure2 == NULL && closure3 == NULL && closure4 == NULL && closure5 == NULL) {
        ObjClosure* seed_closure = NULL;
        JitProgram* seed_program = NULL;
        if (!jit_seed_trace_call_ic(function, op, argc, &seed_closure, &seed_program)) return 0;
        trace->call_ic_closures[pc] = seed_closure;
        trace->call_ic_programs[pc] = seed_program;
        closure1 = seed_closure;
        program1 = seed_program;
    }
    if (!IS_CLOSURE(callee)) return 0;
    ObjClosure* callee_closure = AS_CLOSURE(callee);
    int slot = 0;
    ObjClosure* closure = NULL;
    JitProgram* program = NULL;
    if (callee_closure == closure1) {
        slot = 1;
        closure = closure1;
        program = program1;
    } else if (callee_closure == closure2) {
        slot = 2;
        closure = closure2;
        program = program2;
    } else if (callee_closure == closure3) {
        slot = 3;
        closure = closure3;
        program = program3;
    } else if (callee_closure == closure4) {
        slot = 4;
        closure = closure4;
        program = program4;
    } else if (callee_closure == closure5) {
        slot = 5;
        closure = closure5;
        program = program5;
    } else {
        return 0;
    }
    if (program == NULL) {
        if (!jit_get_compiled_closure_program(vm, closure, &program)) {
            if (slot == 1) {
                trace->call_ic_closures[pc] = NULL;
                trace->call_ic_programs[pc] = NULL;
            } else if (slot == 2) {
                trace->call_ic2_closures[pc] = NULL;
                trace->call_ic2_programs[pc] = NULL;
            } else if (slot == 3) {
                trace->call_ic3_closures[pc] = NULL;
                trace->call_ic3_programs[pc] = NULL;
            } else if (slot == 4) {
                trace->call_ic4_closures[pc] = NULL;
                trace->call_ic4_programs[pc] = NULL;
            } else {
                trace->call_ic5_closures[pc] = NULL;
                trace->call_ic5_programs[pc] = NULL;
            }
            return 0;
        }
        if (!jit_program_loop_call_safe(program)) {
            if (slot == 1) {
                trace->call_ic_closures[pc] = NULL;
                trace->call_ic_programs[pc] = NULL;
            } else if (slot == 2) {
                trace->call_ic2_closures[pc] = NULL;
                trace->call_ic2_programs[pc] = NULL;
            } else if (slot == 3) {
                trace->call_ic3_closures[pc] = NULL;
                trace->call_ic3_programs[pc] = NULL;
            } else if (slot == 4) {
                trace->call_ic4_closures[pc] = NULL;
                trace->call_ic4_programs[pc] = NULL;
            } else {
                trace->call_ic5_closures[pc] = NULL;
                trace->call_ic5_programs[pc] = NULL;
            }
            return 0;
        }
        if (slot == 1) {
            trace->call_ic_programs[pc] = program;
        } else if (slot == 2) {
            trace->call_ic2_programs[pc] = program;
        } else if (slot == 3) {
            trace->call_ic3_programs[pc] = program;
        } else if (slot == 4) {
            trace->call_ic4_programs[pc] = program;
        } else {
            trace->call_ic5_programs[pc] = program;
        }
    }
    if (!jit_execute_compiled_program_with_args(vm, closure, program, args, argc, out)) {
        // Program became ineligible at runtime; drop this IC entry.
        if (slot == 1) {
            trace->call_ic_closures[pc] = NULL;
            trace->call_ic_programs[pc] = NULL;
        } else if (slot == 2) {
            trace->call_ic2_closures[pc] = NULL;
            trace->call_ic2_programs[pc] = NULL;
        } else if (slot == 3) {
            trace->call_ic3_closures[pc] = NULL;
            trace->call_ic3_programs[pc] = NULL;
        } else if (slot == 4) {
            trace->call_ic4_closures[pc] = NULL;
            trace->call_ic4_programs[pc] = NULL;
        } else {
            trace->call_ic5_closures[pc] = NULL;
            trace->call_ic5_programs[pc] = NULL;
        }
        return 0;
    }
    return 1;
}

int jit_call_primitive_multi(
    VM* vm,
    Value callee,
    Value* args,
    int argc,
    Value* out_values,
    int out_cap,
    int* out_count) {
    if (vm == NULL || out_values == NULL || out_count == NULL) return 0;
    if (out_cap <= 0) return 0;
    if (IS_BOUND_METHOD(callee)) {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        if (bound == NULL || bound->method == NULL) return 0;
        Value method_val = OBJ_VAL(bound->method);
        Value shifted_args[argc + 1];
        shifted_args[0] = bound->receiver;
        for (int i = 0; i < argc; i++) shifted_args[i + 1] = args[i];
        return jit_call_primitive_multi(vm, method_val, shifted_args, argc + 1, out_values, out_cap, out_count);
    }
    if (IS_CLOSURE(callee)) {
        ObjClosure* c = AS_CLOSURE(callee);
        ObjFunction* fn = (c != NULL) ? c->function : NULL;
        if (fn == NULL) return 0;
        if (fn->tiny_inline_kind != TINY_INLINE_NONE && argc == 2) {
            if (jit_eval_tiny_inline2_local(fn, args[0], args[1], &out_values[0])) {
                *out_count = 1;
                return 1;
            }
        }
        if (jit_execute_closure_program(vm, c, args, argc, &out_values[0])) {
            *out_count = 1;
            return 1;
        }
        return 0;
    }
    if (IS_NATIVE(callee)) {
        ObjNative* n = AS_NATIVE_OBJ(callee);
        if (n == NULL || n->name == NULL) return 0;
        if (argc == 1 && n->name->length == 3 && memcmp(n->name->chars, "str", 3) == 0) {
            if (!jit_value_to_string_primitive(args[0], &out_values[0])) return 0;
            *out_count = 1;
            return 1;
        }
        if (argc == 1 && n->name->length == 8 && memcmp(n->name->chars, "tostring", 8) == 0) {
            if (!jit_value_to_string_primitive(args[0], &out_values[0])) return 0;
            *out_count = 1;
            return 1;
        }

        ObjThread* thread = vm_current_thread(vm);
        if (thread == NULL || thread->stack_top + argc > thread->stack + thread->stack_capacity) return 0;
        Value* saved_top = thread->stack_top;
        for (int i = 0; i < argc; i++) {
            *thread->stack_top++ = args[i];
        }
        Value* native_args = saved_top;
        int retc = n->function(vm, argc, native_args);
        if (retc <= 0) {
            thread->stack_top = saved_top;
            return 0;
        }
        ObjThread* after = vm_current_thread(vm);
        if (after != thread) {
            thread->stack_top = saved_top;
            return 0;
        }
        if (thread->stack_top - saved_top < retc) {
            thread->stack_top = saved_top;
            return 0;
        }
        if (retc > out_cap) {
            thread->stack_top = saved_top;
            return 0;
        }
        Value* ret_base = thread->stack_top - retc;
        for (int i = 0; i < retc; i++) out_values[i] = ret_base[i];
        *out_count = retc;
        thread->stack_top = saved_top;
        return 1;
    }
    return 0;
}

int jit_call_primitive(VM* vm, Value callee, Value* args, int argc, Value* out) {
    if (out == NULL) return 0;
    int out_count = 0;
    if (!jit_call_primitive_multi(vm, callee, args, argc, out, 1, &out_count)) return 0;
    return out_count == 1;
}

int jit_value_is_self_callable(Value v) {
    if (IS_CLOSURE(v)) return AS_CLOSURE(v)->function->is_self;
    if (IS_NATIVE(v)) return AS_NATIVE_OBJ(v)->is_self;
    return 0;
}

int jit_objstring_eq_name(ObjString* a, ObjString* b, const char* cstr) {
    if (a == NULL) return 0;
    if (b != NULL && a == b) return 1;
    if (cstr == NULL) return 0;
    size_t n = strlen(cstr);
    if ((size_t)a->length != n) return 0;
    return memcmp(a->chars, cstr, n) == 0;
}

int jit_invoke_primitive(VM* vm, Value receiver, Value method_name, Value* args, int argc, Value* out) {
    if (vm == NULL || out == NULL) return 0;
    if (!IS_STRING(method_name) || argc < 0) return 0;
    ObjString* method = AS_STRING(method_name);

    if (IS_STRING(receiver) && argc == 0) {
        if (jit_objstring_eq_name(method, vm->str_lower_name, "lower")) {
            return jit_ascii_case_transform(AS_STRING(receiver), 0, out);
        }
        if (jit_objstring_eq_name(method, vm->str_upper_name, "upper")) {
            return jit_ascii_case_transform(AS_STRING(receiver), 1, out);
        }
    }

    if (IS_TABLE(receiver)) {
        ObjTable* t = AS_TABLE(receiver);
        if (t != NULL) {
            Value raw_method = NIL_VAL;
            if (table_get(&t->table, method, &raw_method)) {
                if (!t->is_module && jit_value_is_self_callable(raw_method)) {
                    Value shifted_args[argc + 1];
                    shifted_args[0] = receiver;
                    for (int i = 0; i < argc; i++) shifted_args[i + 1] = args[i];
                    return jit_call_primitive(vm, raw_method, shifted_args, argc + 1, out);
                }
                return jit_call_primitive(vm, raw_method, args, argc, out);
            }
        }
    }

    return 0;
}
