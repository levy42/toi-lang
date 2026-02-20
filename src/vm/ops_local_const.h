#ifndef VM_OPS_LOCAL_CONST_H
#define VM_OPS_LOCAL_CONST_H

#include "../vm.h"

int vm_handle_op_inc_local(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_sub_local_const(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_mul_local_const(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_div_local_const(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_mod_local_const(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
