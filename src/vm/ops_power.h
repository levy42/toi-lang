#ifndef VM_OPS_POWER_H
#define VM_OPS_POWER_H

#include "../vm.h"

int vm_handle_op_power(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_int_div(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
