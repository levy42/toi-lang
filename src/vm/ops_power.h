#ifndef VM_OPS_POWER_H
#define VM_OPS_POWER_H

#include "../vm.h"

int vmHandleOpPower(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpIntDiv(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
