#ifndef VM_OPS_LOCAL_SET_H
#define VM_OPS_LOCAL_SET_H

#include "../vm.h"

int vmHandleOpAddSetLocal(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpSubSetLocal(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpMulSetLocal(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpDivSetLocal(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpModSetLocal(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
