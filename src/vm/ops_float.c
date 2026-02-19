#include <math.h>

#include "ops_float.h"

void vmHandleOpFAdd(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a + b));
}

void vmHandleOpFSub(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a - b));
}

void vmHandleOpFMul(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a * b));
}

void vmHandleOpFDiv(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a / b));
}

void vmHandleOpFMod(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(fmod(a, b)));
}
