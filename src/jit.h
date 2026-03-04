#ifndef TOI_JIT_H
#define TOI_JIT_H

#include "vm.h"

void toi_jit_init(VM* vm);
void toi_jit_free(VM* vm);
void toi_jit_mark_roots(VM* vm);
void toi_jit_notify_hot_function(VM* vm, ObjFunction* function);
int toi_jit_try_execute(VM* vm, ObjClosure* closure, int arg_count);
int toi_jit_try_execute_with_args(VM* vm, ObjClosure* closure, int arg_count, Value* args, Value* out);
int toi_jit_try_execute_compiled_with_args(VM* vm, ObjClosure* closure, int arg_count, Value* args, Value* out);
int toi_jit_execute_program_direct_with_args(VM* vm, ObjClosure* closure, void* program_ptr, int arg_count, Value* args, Value* out);
void toi_jit_notify_loop_backedge(VM* vm, CallFrame* frame, int opcode_offset, uint8_t opcode);
int toi_jit_has_compiled_loops(VM* vm);
int toi_jit_try_execute_loop(VM* vm, CallFrame* frame, uint8_t** ip, int opcode_offset, uint8_t opcode);
void toi_jit_print_stats(VM* vm);

#endif
