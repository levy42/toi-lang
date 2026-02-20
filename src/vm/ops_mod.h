#ifndef VM_OPS_MOD_H
#define VM_OPS_MOD_H

#include "../vm.h"

void vm_handle_op_i_mod(VM* vm);
int vm_handle_op_mod_const(VM* vm, CallFrame** frame, uint8_t** ip, Value b);

#endif
