#ifndef VM_OPS_CONTROL_H
#define VM_OPS_CONTROL_H

#include "../vm.h"

void vm_handle_op_jump(uint8_t** ip);
void vm_handle_op_jump_if_false(VM* vm, uint8_t** ip);
void vm_handle_op_jump_if_true(VM* vm, uint8_t** ip);
void vm_handle_op_loop(uint8_t** ip);
int vm_handle_op_for_prep(VM* vm, CallFrame* frame, uint8_t** ip);
int vm_handle_op_for_loop(VM* vm, CallFrame* frame, uint8_t** ip);

#endif
