#ifndef VM_OPS_COMPARE_H
#define VM_OPS_COMPARE_H

#include "../vm.h"

int vm_handle_op_equal(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_greater(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_less(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
