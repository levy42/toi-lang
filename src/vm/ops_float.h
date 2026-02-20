#ifndef VM_OPS_FLOAT_H
#define VM_OPS_FLOAT_H

#include "../vm.h"

void vm_handle_op_f_add(VM* vm);
void vm_handle_op_f_sub(VM* vm);
void vm_handle_op_f_mul(VM* vm);
void vm_handle_op_f_div(VM* vm);
void vm_handle_op_f_mod(VM* vm);

#endif
