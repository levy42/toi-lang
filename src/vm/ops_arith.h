#ifndef VM_OPS_ARITH_H
#define VM_OPS_ARITH_H

#include "../vm.h"

int vm_handle_op_add_const(VM* vm, CallFrame** frame, uint8_t** ip, Value b);
int vm_handle_op_add(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_add_inplace(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_subtract(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_multiply(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_divide(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_modulo(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
