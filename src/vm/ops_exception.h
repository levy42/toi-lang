#ifndef VM_OPS_EXCEPTION_H
#define VM_OPS_EXCEPTION_H

#include "../vm.h"

int vm_handle_op_try(VM* vm, CallFrame* frame, uint8_t** ip);
void vm_handle_op_end_try(VM* vm);
int vm_handle_op_end_finally(VM* vm);
void vm_handle_op_throw(VM* vm);

#endif
