#ifndef TOI_JIT_CALL_RUNTIME_H
#define TOI_JIT_CALL_RUNTIME_H

#include "jit_internal.h"

int jit_build_string_primitive(VM* vm, Value* parts, int count, Value* out);
int jit_length_primitive(Value in, Value* out);
int jit_slice_primitive(Value obj, Value start, Value end, Value step, Value* out);
int jit_value_matches_type(Value v, uint8_t type);
int jit_value_is_self_callable(Value v);
int jit_objstring_eq_name(ObjString* a, ObjString* b, const char* cstr);

void jit_update_trace_call_ic(VM* vm, JitLoopTrace* trace, int pc, Value callee, int argc);
int jit_try_trace_call_ic(
    VM* vm,
    ObjFunction* function,
    JitLoopTrace* trace,
    const JitOp* op,
    int pc,
    Value callee,
    Value* args,
    int argc,
    Value* out);

int jit_call_primitive(VM* vm, Value callee, Value* args, int argc, Value* out);
int jit_call_primitive_multi(
    VM* vm,
    Value callee,
    Value* args,
    int argc,
    Value* out_values,
    int out_cap,
    int* out_count);
int jit_invoke_primitive(VM* vm, Value receiver, Value method_name, Value* args, int argc, Value* out);

int jit_get_compiled_closure_program(VM* vm, ObjClosure* closure, JitProgram** out_program);
int jit_try_safe_loop_closure_call(
    VM* vm,
    ObjClosure* closure,
    Value* args,
    int argc,
    Value* out,
    JitProgram** out_program);

#endif
