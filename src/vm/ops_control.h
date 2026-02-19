#ifndef VM_OPS_CONTROL_H
#define VM_OPS_CONTROL_H

#include "../vm.h"

void vmHandleOpJump(uint8_t** ip);
void vmHandleOpJumpIfFalse(VM* vm, uint8_t** ip);
void vmHandleOpJumpIfTrue(VM* vm, uint8_t** ip);
void vmHandleOpLoop(uint8_t** ip);
int vmHandleOpForPrep(VM* vm, CallFrame* frame, uint8_t** ip);
int vmHandleOpForLoop(VM* vm, CallFrame* frame, uint8_t** ip);

#endif
