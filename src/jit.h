#ifndef TOI_JIT_H
#define TOI_JIT_H

#include "vm.h"

void toi_jit_init(VM* vm);
void toi_jit_free(VM* vm);
void toi_jit_mark_roots(VM* vm);
void toi_jit_notify_hot_function(VM* vm, ObjFunction* function);
int toi_jit_try_execute(VM* vm, ObjClosure* closure, int arg_count);
int toi_jit_try_execute_linked(VM* vm, ObjClosure* closure, int arg_count);
int toi_jit_try_execute_linked_at_site(
    VM* vm,
    ObjFunction* caller_function,
    int opcode_offset,
    ObjClosure* closure,
    int arg_count);
int toi_jit_try_execute_with_args(VM* vm, ObjClosure* closure, int arg_count, Value* args, Value* out);
int toi_jit_try_execute_compiled_with_args(VM* vm, ObjClosure* closure, int arg_count, Value* args, Value* out);
int toi_jit_try_execute_linked_with_args(VM* vm, ObjClosure* closure, int arg_count, Value* args, Value* out);
int toi_jit_try_execute_compiled_at_callsite_with_args(
    VM* vm,
    ObjClosure* caller,
    uint32_t opcode_offset,
    ObjClosure* callee,
    int arg_count,
    Value* args,
    Value* out);
int toi_jit_get_compiled_program_ptr_at_callsite(
    VM* vm,
    ObjClosure* caller,
    uint32_t opcode_offset,
    ObjClosure* callee,
    void** out_program_ptr);
int toi_jit_program_chain_safe(void* program_ptr);
int toi_jit_execute_program_direct_with_args(VM* vm, ObjClosure* closure, void* program_ptr, int arg_count, Value* args, Value* out);
int toi_jit_in_native_trace(VM* vm);
void toi_jit_notify_loop_backedge(VM* vm, CallFrame* frame, int opcode_offset, uint8_t opcode);
int toi_jit_has_compiled_loops(VM* vm);
int toi_jit_try_execute_loop(VM* vm, CallFrame* frame, uint8_t** ip, int opcode_offset, uint8_t opcode);
void toi_jit_print_stats(VM* vm);

#endif
