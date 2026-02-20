#ifndef VM_OPS_PRINT_H
#define VM_OPS_PRINT_H

#include "../vm.h"

// Returns:
//  1: handled successfully
//  0: caller should return *out_result
// -1: caller should jump to runtime_error
int vm_handle_op_print(VM* vm, CallFrame** frame, uint8_t** ip, InterpretResult* out_result);

#endif
