#ifndef VM_OPS_STATE_H
#define VM_OPS_STATE_H

#include "../vm.h"

void vmHandleOpConstant(VM* vm, CallFrame* frame, uint8_t** ip);
int vmHandleOpGetGlobal(VM* vm, CallFrame* frame, uint8_t** ip);
void vmHandleOpDefineGlobal(VM* vm, CallFrame* frame, uint8_t** ip);
void vmHandleOpSetGlobal(VM* vm, CallFrame* frame, uint8_t** ip);
int vmHandleOpDeleteGlobal(VM* vm, CallFrame* frame, uint8_t** ip);
void vmHandleOpGetLocal(VM* vm, CallFrame* frame, uint8_t** ip);
void vmHandleOpSetLocal(VM* vm, CallFrame* frame, uint8_t** ip);
void vmHandleOpGetUpvalue(VM* vm, CallFrame* frame, uint8_t** ip);
void vmHandleOpSetUpvalue(VM* vm, CallFrame* frame, uint8_t** ip);

#endif
