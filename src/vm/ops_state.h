#ifndef VM_OPS_STATE_H
#define VM_OPS_STATE_H

#include "../vm.h"

void vm_handle_op_constant(VM* vm, CallFrame* frame, uint8_t** ip);
int vm_handle_op_get_global(VM* vm, CallFrame* frame, uint8_t** ip);
void vm_handle_op_define_global(VM* vm, CallFrame* frame, uint8_t** ip);
void vm_handle_op_set_global(VM* vm, CallFrame* frame, uint8_t** ip);
int vm_handle_op_delete_global(VM* vm, CallFrame* frame, uint8_t** ip);
void vm_handle_op_get_local(VM* vm, CallFrame* frame, uint8_t** ip);
void vm_handle_op_set_local(VM* vm, CallFrame* frame, uint8_t** ip);
void vm_handle_op_get_upvalue(VM* vm, CallFrame* frame, uint8_t** ip);
void vm_handle_op_set_upvalue(VM* vm, CallFrame* frame, uint8_t** ip);

#endif
