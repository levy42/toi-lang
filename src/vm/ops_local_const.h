#ifndef VM_OPS_LOCAL_CONST_H
#define VM_OPS_LOCAL_CONST_H

#include "../vm.h"

int vmHandleOpIncLocal(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpSubLocalConst(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpMulLocalConst(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpDivLocalConst(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpModLocalConst(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
