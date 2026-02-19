#ifndef VM_OPS_COMPARE_H
#define VM_OPS_COMPARE_H

#include "../vm.h"

int vmHandleOpEqual(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpGreater(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpLess(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
