#ifndef VM_OPS_HAS_H
#define VM_OPS_HAS_H

#include "../vm.h"

int vm_handle_op_has(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_in(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
