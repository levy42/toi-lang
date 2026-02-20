#ifndef VM_OPS_LOCAL_SET_H
#define VM_OPS_LOCAL_SET_H

#include "../vm.h"

int vm_handle_op_add_set_local(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_sub_set_local(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_mul_set_local(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_div_set_local(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_mod_set_local(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
