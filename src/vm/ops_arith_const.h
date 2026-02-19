#ifndef VM_OPS_ARITH_CONST_H
#define VM_OPS_ARITH_CONST_H

#include "../vm.h"

int vmHandleOpSubConst(VM* vm, CallFrame** frame, uint8_t** ip, Value b);
int vmHandleOpMulConst(VM* vm, CallFrame** frame, uint8_t** ip, Value b);
int vmHandleOpDivConst(VM* vm, CallFrame** frame, uint8_t** ip, Value b);
void vmHandleOpIAdd(VM* vm);
void vmHandleOpISub(VM* vm);
void vmHandleOpIMul(VM* vm);
void vmHandleOpIDiv(VM* vm);

#endif
