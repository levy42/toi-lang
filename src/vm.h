#ifndef VM_H
#define VM_H

#include "object.h"
#include "value.h"
#include "table.h"

typedef struct VM {
   ObjThread* currentThread;
   Table globals;
   Table modules;  // Cache of loaded native modules
   int cliArgc;
   char** cliArgv;
   int disableGC;
    int isREPL;
    int pendingSetLocalCount;
    int pendingSetLocalSlots[8];
    int pendingSetLocalFrames[8];
    int hasException;
    Value exception;
    ObjString* mm_index;
    ObjString* mm_newindex;
    ObjString* mm_str;
    ObjString* mm_call;
    ObjString* mm_new;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
} InterpretResult;

// Exposed VM Core Functions
void initVM(VM* vm);
void freeVM(VM* vm);
InterpretResult interpret(VM* vm, ObjFunction* function);
void push(VM* vm, Value value);
Value pop(VM* vm);
Value peek(VM* vm, int distance);
void defineNative(VM* vm, const char* name, NativeFn function);
void vmRuntimeError(VM* vm, const char* format, ...);
ObjString* numberKeyString(double num);
int call(VM* vm, ObjClosure* closure, int argCount);
InterpretResult vmRun(VM* vm, int minFrameCount);
Value getMetamethod(VM* vm, Value val, const char* name);
void vmRequestInterrupt(void);

#endif
