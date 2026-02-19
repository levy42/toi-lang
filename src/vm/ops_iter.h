#ifndef VM_OPS_ITER_H
#define VM_OPS_ITER_H

#include "../vm.h"

int vmHandleOpIterPrep(VM* vm);
int vmHandleOpIterPrepIPairs(VM* vm);
int vmHandleOpRange(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpSlice(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
