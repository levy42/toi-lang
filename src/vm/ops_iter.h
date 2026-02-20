#ifndef VM_OPS_ITER_H
#define VM_OPS_ITER_H

#include "../vm.h"

int vm_handle_op_iter_prep(VM* vm);
int vm_handle_op_iter_prep_i_pairs(VM* vm);
int vm_handle_op_range(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_slice(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
