#ifndef VM_OPS_MOD_H
#define VM_OPS_MOD_H

#include "../vm.h"

void vmHandleOpIMod(VM* vm);
int vmHandleOpModConst(VM* vm, CallFrame** frame, uint8_t** ip, Value b);

#endif
