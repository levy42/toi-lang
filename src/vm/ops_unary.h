#ifndef VM_OPS_UNARY_H
#define VM_OPS_UNARY_H

#include "../vm.h"

void vm_handle_op_negate(VM* vm);
void vm_handle_op_not(VM* vm);
int vm_handle_op_length(VM* vm);

#endif
