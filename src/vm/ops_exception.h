#ifndef VM_OPS_EXCEPTION_H
#define VM_OPS_EXCEPTION_H

#include "../vm.h"

int vmHandleOpTry(VM* vm, CallFrame* frame, uint8_t** ip);
void vmHandleOpEndTry(VM* vm);
int vmHandleOpEndFinally(VM* vm);
void vmHandleOpThrow(VM* vm);

#endif
