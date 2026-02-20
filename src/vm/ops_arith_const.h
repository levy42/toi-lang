#ifndef VM_OPS_ARITH_CONST_H
#define VM_OPS_ARITH_CONST_H

#include "../vm.h"

int vm_handle_op_sub_const(VM* vm, CallFrame** frame, uint8_t** ip, Value b);
int vm_handle_op_mul_const(VM* vm, CallFrame** frame, uint8_t** ip, Value b);
int vm_handle_op_div_const(VM* vm, CallFrame** frame, uint8_t** ip, Value b);
void vm_handle_op_i_add(VM* vm);
void vm_handle_op_i_sub(VM* vm);
void vm_handle_op_i_mul(VM* vm);
void vm_handle_op_i_div(VM* vm);

#endif
