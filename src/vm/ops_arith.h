#ifndef VM_OPS_ARITH_H
#define VM_OPS_ARITH_H

#include "../vm.h"

int vmHandleOpAddConst(VM* vm, CallFrame** frame, uint8_t** ip, Value b);
int vmHandleOpAdd(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpSubtract(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpMultiply(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpDivide(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpModulo(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
