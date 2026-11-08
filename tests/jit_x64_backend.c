/* Execute the backend APIs directly so a passing VM fallback cannot hide a
 * broken emitter. Run with make test-jit-x64 (on a SysV x86-64 host). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jit_x64.h"
#include "jit_native_trace.h"
#include "jit_ir.h"
#include "compiler.h"
#include "jit_internal.h"
#include "jit_call_runtime.h"
#include "object.h"
#include "vm.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define N(op, arg) {op, arg}
static char error[256];
static int checks;
static void test_numeric(void) {
    void* fn; void* mem; size_t size;
    CHECK(toi_jit_x64_compile_qfor_empty(&fn, &mem, &size, error, sizeof(error)));
    CHECK(((ToiJitQforEmptyFn)fn)(2, 100) == 100);
    CHECK(((ToiJitQforEmptyFn)fn)(200, 100) == 200);
    CHECK(((ToiJitQforEmptyFn)fn)(UINT64_MAX, 0) == UINT64_MAX);
    toi_jit_x64_free_code(mem, size); checks++;
    uint8_t range[] = {OP_NIL, OP_QFOR_LOOP_NUM, 1, 2, 0, 6};
    CHECK(toi_jit_x64_compile_qfor_num_stub(range, 6, 1, error, sizeof(error)));
    CHECK(!toi_jit_x64_compile_qfor_num_stub(range, 5, 1, error, sizeof(error)));
    range[5] = 0; CHECK(!toi_jit_x64_compile_qfor_num_stub(range, 6, 1, error, sizeof(error)));
    range[1] = OP_RETURN; CHECK(!toi_jit_x64_compile_qfor_num_stub(range, 6, 1, error, sizeof(error)));
    checks++;
    ToiJitNumInstr ops[] = {
        N(TOI_JIT_NUM_PUSH_ACC, 0), N(TOI_JIT_NUM_PUSH_VAR, 0), N(TOI_JIT_NUM_BIN_ADD, 0),
        N(TOI_JIT_NUM_STORE_ACC, 0), N(TOI_JIT_NUM_POP, 0)
    };
    CHECK(toi_jit_x64_compile_numeric_loop(ops, 5, &fn, &mem, &size, error, sizeof(error)));
    double acc = 0, var = 0, end = 2000, constants[] = {2, 3};
    ((ToiJitNumericLoopFn)fn)(&acc, &var, &end, constants);
    CHECK(acc == 2001000 && var == 2001);
    acc = 7; var = 9; end = 8;
    ((ToiJitNumericLoopFn)fn)(&acc, &var, &end, constants);
    CHECK(acc == 7 && var == 10);
    var = 0; end = NAN;
    ((ToiJitNumericLoopFn)fn)(&acc, &var, &end, constants);
    CHECK(acc == 7 && var == 1);
    toi_jit_x64_free_code(mem, size); checks++;
    const uint8_t arithmetic[] = {TOI_JIT_NUM_BIN_ADD, TOI_JIT_NUM_BIN_SUB, TOI_JIT_NUM_BIN_MUL,
                                  TOI_JIT_NUM_BIN_DIV, TOI_JIT_NUM_BIN_MOD};
    const double inputs[][2] = {{3.5, -2.3}, {1e308, 3}, {INFINITY, 3}, {-0.0, 2}, {1, 0}, {NAN, 4}};
    for (size_t k = 0; k < sizeof(arithmetic); k++) {
        ToiJitNumInstr expression[] = {
            N(TOI_JIT_NUM_PUSH_CONST, 0), N(TOI_JIT_NUM_PUSH_CONST, 1), N(arithmetic[k], 0),
            N(TOI_JIT_NUM_STORE_ACC, 0), N(TOI_JIT_NUM_POP, 0)
        };
        CHECK(toi_jit_x64_compile_numeric_loop(expression, 5, &fn, &mem, &size, error, sizeof(error)));
        for (size_t j = 0; j < sizeof(inputs) / sizeof(inputs[0]); j++) {
            double x = inputs[j][0], y = inputs[j][1], expected;
            if (k == 0) expected = x + y;
            else if (k == 1) expected = x - y;
            else if (k == 2) expected = x * y;
            else if (k == 3) expected = x / y;
            else expected = fmod(x, y);
            acc = 0; var = 0; end = 1;
            ((ToiJitNumericLoopFn)fn)(&acc, &var, &end, inputs[j]);
            CHECK((isnan(acc) && isnan(expected)) || (acc == expected && (!acc ? !!signbit(acc) == !!signbit(expected) : 1)));
        }
        toi_jit_x64_free_code(mem, size);
    }
    checks++;
    /* Operand snapshots must survive stores, and fmod must preserve live XMMs. */
    ToiJitSlotInstr slot[] = {
        N(TOI_JIT_SLOT_PUSH_SLOT, 0), N(TOI_JIT_SLOT_PUSH_SLOT, 1), N(TOI_JIT_SLOT_PUSH_CONST, 0),
        N(TOI_JIT_SLOT_BIN_MOD, 0), N(TOI_JIT_SLOT_BIN_ADD, 0), N(TOI_JIT_SLOT_STORE_SLOT, 0), N(TOI_JIT_SLOT_POP, 0)
    };
    CHECK(toi_jit_x64_compile_slot_loop(slot, 7, 1, 2, &fn, &mem, &size, error, sizeof(error)));
    double slots[] = {0, 0, 2000};
    ((ToiJitSlotLoopFn)fn)(slots, constants);
    CHECK(slots[0] == 1000 && slots[1] == 2001);
    toi_jit_x64_free_code(mem, size); checks++;
    /* Branches retain the condition until POP on each edge. */
    ToiJitSlotInstr joined[] = {
        N(TOI_JIT_SLOT_PUSH_SLOT, 1), N(TOI_JIT_SLOT_PUSH_CONST, 1), N(TOI_JIT_SLOT_CMP_LT, 0),
        N(TOI_JIT_SLOT_JUMP_IF_FALSE, 11), N(TOI_JIT_SLOT_POP, 0),
        N(TOI_JIT_SLOT_PUSH_SLOT, 0), N(TOI_JIT_SLOT_PUSH_CONST, 0), N(TOI_JIT_SLOT_BIN_ADD, 0),
        N(TOI_JIT_SLOT_STORE_SLOT, 0), N(TOI_JIT_SLOT_POP, 0),
        N(TOI_JIT_SLOT_JUMP, 12), N(TOI_JIT_SLOT_POP, 0)
    };
    CHECK(toi_jit_x64_compile_slot_loop(joined, 12, 1, 2, &fn, &mem, &size, error, sizeof(error)));
    slots[0] = 0; slots[1] = 0; slots[2] = 10;
    ((ToiJitSlotLoopFn)fn)(slots, constants);
    CHECK(slots[0] == 4 && slots[1] == 11);
    toi_jit_x64_free_code(mem, size); checks++;
    CHECK(toi_jit_x64_compile_math_reduce(&fn, &mem, &size, error, sizeof(error)));
    acc = 0; var = 0; end = 20000; double expected = 0;
    for (int i = 1; i <= 20000; i++) expected += (i * 3.14159) / (fmod(i, 97) + 1);
    ((ToiJitMathReduceFn)fn)(&acc, &var, &end, 3.14159, 97, 1);
    CHECK(acc == expected && var == 20001);
    toi_jit_x64_free_code(mem, size); checks++;
}

static ToiJitNativeTraceInstr op(int code, int a) {
    ToiJitNativeTraceInstr i; memset(&i, 0, sizeof(i));
    i.op = (uint8_t)code; i.a = (uint8_t)a; i.deopt_offset = 100 + (uint32_t)code;
    i.constant = NIL_VAL; return i;
}
static ToiJitNativeTraceFn compile_trace(ToiJitNativeTraceInstr* ops, int n, void** mem, size_t* size) {
    void* fn = NULL; uint8_t direct = 0;
    CHECK(toi_jit_x64_compile_native_trace(ops, n, NULL, 0, 1, 2, &direct, &fn, mem, size, error, sizeof(error)));
    if (!direct) fprintf(stderr, "backend group=%d count=%d fell back to bridge\n", checks, n);
    CHECK(direct == 1); return (ToiJitNativeTraceFn)fn;
}
static void run_to_exit(ToiJitNativeTraceFn fn, VM* vm, ObjClosure* closure, Value* slots, int n) {
    ToiJitNativeExecState state; int result;
    int budget = 1000;
    do {
        memset(&state, 0, sizeof(state)); result = fn(vm, closure, slots, n, &state);
        CHECK(result != TOI_JIT_NATIVE_EXEC_DEOPT);
        if (result == TOI_JIT_NATIVE_EXEC_CONTINUE) {
            if (state.resume_offset != TOI_JIT_NATIVE_RESUME_BACKEDGE || state.stack_count != 0)
                fprintf(stderr, "backend group=%d resume=%u reason=%u stack=%u\n", checks, state.resume_offset, state.reason, state.stack_count);
            CHECK(state.resume_offset == TOI_JIT_NATIVE_RESUME_BACKEDGE && state.stack_count == 0);
        }
        CHECK(--budget > 0);
    } while (result != TOI_JIT_NATIVE_EXEC_EXIT);
}
static int calls;
static int counter(VM* vm, int argc, Value* args) {
    (void)argc; (void)args; calls++; push(vm, NUMBER_VAL(calls)); return 1;
}
static ObjClosure* arithmetic_closure(VM* vm, int subtract) {
    ObjFunction* f = new_function(); push(vm, OBJ_VAL(f)); f->arity = 2;
    uint8_t code[] = {OP_GET_LOCAL, 1, OP_GET_LOCAL, 2, OP_ADD,
                      OP_GET_LOCAL, 1, OP_GET_LOCAL, 2, 0, OP_MULTIPLY, OP_RETURN};
    code[9] = subtract ? OP_SUBTRACT : OP_ADD;
    for (size_t i = 0; i < sizeof(code); i++) write_chunk(&f->chunk, code[i], 1);
    ObjClosure* c = new_closure(f); push(vm, OBJ_VAL(c)); return c;
}
static void test_traces(VM* vm, ObjClosure* closure) {
    void* memory; size_t size;
    Value slots[6] = {NUMBER_VAL(0), NUMBER_VAL(0), NUMBER_VAL(2000), NIL_VAL, NIL_VAL, NIL_VAL};
    ToiJitNativeTraceInstr sum[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 0), op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 1),
        op(TOI_JIT_NATIVE_OP_BIN_ADD, 0), op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    ToiJitNativeTraceFn fn = compile_trace(sum, 5, &memory, &size);
    run_to_exit(fn, vm, closure, slots, 6);
    CHECK(AS_NUMBER(slots[0]) == 2001000 && AS_NUMBER(slots[1]) == 2001);
    ToiJitX64Stats stats;
    toi_jit_x64_code_stats(memory, &stats);
    CHECK(stats.promoted_values > 0 && stats.promoted_arithmetic > 0);
    /* A guard failure materializes operands at the exact failing bytecode. */
    slots[0] = BOOL_VAL(1); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(10);
    ToiJitNativeExecState state;
    CHECK(fn(vm, closure, slots, 6, &state) == TOI_JIT_NATIVE_EXEC_CONTINUE);
    CHECK(state.reason == TOI_JIT_NATIVE_DEOPT_TYPE_GUARD && state.resume_offset == sum[2].deopt_offset);
    CHECK(state.stack_count == 2 && IS_BOOL(state.stack[0]) && AS_NUMBER(state.stack[1]) == 1);
    CHECK(AS_NUMBER(slots[1]) == 1);
    CHECK(fn(vm, closure, slots, 2, &state) == TOI_JIT_NATIVE_EXEC_DEOPT);
    toi_jit_x64_free_code(memory, size); checks++;

    ObjTable* t = new_table(); push(vm, OBJ_VAL(t));
    for (int i = 1; i <= 2000; i++) table_set_array(&t->table, i, NUMBER_VAL(i * 2));
    slots[0] = NUMBER_VAL(0); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(2000); slots[3] = OBJ_VAL(t);
    ToiJitNativeTraceInstr read[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 0), op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 3),
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 1), op(TOI_JIT_NATIVE_OP_GET_TABLE_NUM, 0),
        op(TOI_JIT_NATIVE_OP_BIN_ADD, 0), op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    fn = compile_trace(read, 7, &memory, &size); run_to_exit(fn, vm, closure, slots, 6);
    CHECK(AS_NUMBER(slots[0]) == 4002000); toi_jit_x64_free_code(memory, size); checks++;

    ObjTable* appended = new_table(); push(vm, OBJ_VAL(appended)); slots[3] = OBJ_VAL(appended);
    slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(2000);
    ToiJitNativeTraceInstr append[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 3), op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 1),
        op(TOI_JIT_NATIVE_OP_APPEND, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    fn = compile_trace(append, 4, &memory, &size); run_to_exit(fn, vm, closure, slots, 6);
    CHECK(appended->table.array_max == 2000);
    for (int i = 0; i < 2000; i++) CHECK(AS_NUMBER(appended->table.array[i]) == i + 1);
    toi_jit_x64_free_code(memory, size); checks++;

    ObjNative* native = new_native(counter, copy_string("counter", 7)); push(vm, OBJ_VAL(native));
    ToiJitNativeTraceInstr call[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0), op(TOI_JIT_NATIVE_OP_CALL, 0),
        op(TOI_JIT_NATIVE_OP_POP, 0), op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 4),
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 1), op(TOI_JIT_NATIVE_OP_BIN_ADD, 0),
        op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    call[0].constant = OBJ_VAL(native);
    fn = compile_trace(call, 7, &memory, &size);
    calls = 0; slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(10); slots[4] = NIL_VAL;
    CHECK(fn(vm, closure, slots, 6, &state) == TOI_JIT_NATIVE_EXEC_CONTINUE);
    CHECK(calls == 1 && state.resume_offset == call[5].deopt_offset && state.stack_count == 2);
    CHECK(IS_NIL(state.stack[0]) && AS_NUMBER(state.stack[1]) == 1);
    toi_jit_x64_free_code(memory, size); checks++;

    ToiJitNativeTraceInstr spill_call[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 0), op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 1),
        op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0), op(TOI_JIT_NATIVE_OP_BIN_ADD, 0),
        op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0), op(TOI_JIT_NATIVE_OP_CALL, 0),
        op(TOI_JIT_NATIVE_OP_BIN_ADD, 0), op(TOI_JIT_NATIVE_OP_BIN_ADD, 0),
        op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    spill_call[2].constant = NUMBER_VAL(2); spill_call[4].constant = OBJ_VAL(native);
    fn = compile_trace(spill_call, 10, &memory, &size);
    calls = 0; slots[0] = NUMBER_VAL(0); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(2000);
    run_to_exit(fn, vm, closure, slots, 6);
    CHECK(AS_NUMBER(slots[0]) == 4006000 && calls == 2000);
    toi_jit_x64_free_code(memory, size); checks++;

    /* Merged control flow is tested through the API even while the shared VM
     * safety gate conservatively keeps such application traces on fallback. */
    ToiJitNativeTraceInstr branch[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 1), op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0),
        op(TOI_JIT_NATIVE_OP_CMP_LT, 0), op(TOI_JIT_NATIVE_OP_JUMP_IF_FALSE, 0),
        op(TOI_JIT_NATIVE_OP_POP, 0), op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0), op(TOI_JIT_NATIVE_OP_JUMP, 0),
        op(TOI_JIT_NATIVE_OP_POP, 0), op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0),
        op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    branch[1].constant = NUMBER_VAL(3); branch[3].target = 7;
    branch[5].constant = NUMBER_VAL(11); branch[6].target = 9; branch[8].constant = NUMBER_VAL(22);
    fn = compile_trace(branch, 11, &memory, &size);
    slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(5);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 22);
    toi_jit_x64_free_code(memory, size); checks++;
    ToiJitNativeTraceInstr continue_loop[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 1), op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0),
        op(TOI_JIT_NATIVE_OP_CMP_LT, 0), op(TOI_JIT_NATIVE_OP_JUMP_IF_TRUE, 0),
        op(TOI_JIT_NATIVE_OP_POP, 0), op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 0),
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 1), op(TOI_JIT_NATIVE_OP_BIN_ADD, 0),
        op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    continue_loop[1].constant = NUMBER_VAL(5); continue_loop[3].target = 10;
    fn = compile_trace(continue_loop, 10, &memory, &size);
    slots[0] = NUMBER_VAL(0); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(10);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 45);
    toi_jit_x64_free_code(memory, size); checks++;

    /* Closed-over values and runtime string construction. */
    Value captured = NUMBER_VAL(42);
    ObjUpvalue upvalue; memset(&upvalue, 0, sizeof(upvalue)); upvalue.location = &captured;
    ObjUpvalue* upvalues[] = {&upvalue};
    ObjClosure with_upvalue = *closure; with_upvalue.upvalues = upvalues; with_upvalue.upvalue_count = 1;
    ToiJitNativeTraceInstr capture[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_UPVALUE, 0), op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    fn = compile_trace(capture, 3, &memory, &size);
    slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(2);
    run_to_exit(fn, vm, &with_upvalue, slots, 6); CHECK(AS_NUMBER(slots[0]) == 42);
    toi_jit_x64_free_code(memory, size); checks++;

    ToiJitNativeGuard numeric_upvalue = {.kind = TOI_JIT_NATIVE_GUARD_UPVALUE_IS_NUMBER, .slot_a = 0};
    void* guarded; uint8_t direct;
    CHECK(toi_jit_x64_compile_native_trace(capture, 3, &numeric_upvalue, 1, 1, 2,
          &direct, &guarded, &memory, &size, error, sizeof(error)) && direct);
    fn = (ToiJitNativeTraceFn)guarded;
    slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(10);
    run_to_exit(fn, vm, &with_upvalue, slots, 6); CHECK(AS_NUMBER(slots[0]) == 42);
    toi_jit_x64_code_stats(memory, &stats); CHECK(stats.promoted_values > 0);
    captured = BOOL_VAL(0); slots[1] = NUMBER_VAL(0);
    CHECK(fn(vm, &with_upvalue, slots, 6, &state) == TOI_JIT_NATIVE_EXEC_CONTINUE);
    CHECK(state.stack_count == 0 && state.reason == TOI_JIT_NATIVE_DEOPT_TYPE_GUARD && state.resume_offset == capture[0].deopt_offset);
    captured = NUMBER_VAL(42);
    toi_jit_x64_free_code(memory, size); checks++;

    ToiJitNativeTraceInstr snapshot[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 0), op(TOI_JIT_NATIVE_OP_PUSH_FALSE, 0),
        op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0),
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 0), op(TOI_JIT_NATIVE_OP_BIN_ADD, 0),
        op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    ToiJitNativeGuard numeric_local = {.kind = TOI_JIT_NATIVE_GUARD_LOCAL_IS_NUMBER, .slot_a = 0};
    CHECK(toi_jit_x64_compile_native_trace(snapshot, 8, &numeric_local, 1, 1, 2,
          &direct, &guarded, &memory, &size, error, sizeof(error)) && direct);
    fn = (ToiJitNativeTraceFn)guarded;
    slots[0] = NUMBER_VAL(3); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(1);
    CHECK(fn(vm, closure, slots, 6, &state) == TOI_JIT_NATIVE_EXEC_CONTINUE);
    CHECK(state.resume_offset == snapshot[4].deopt_offset && state.stack_count == 1);
    CHECK(IS_NUMBER(state.stack[0]) && AS_NUMBER(state.stack[0]) == 3);
    CHECK(IS_BOOL(slots[0]) && !AS_BOOL(slots[0]));
    toi_jit_x64_free_code(memory, size); checks++;

    ObjString* prefix = copy_string("value=", 6); push(vm, OBJ_VAL(prefix));
    ToiJitNativeTraceInstr string[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0), op(TOI_JIT_NATIVE_OP_PUSH_UPVALUE, 0),
        op(TOI_JIT_NATIVE_OP_BUILD_STRING2, 0), op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    string[0].constant = OBJ_VAL(prefix);
    fn = compile_trace(string, 5, &memory, &size);
    slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(2);
    run_to_exit(fn, vm, &with_upvalue, slots, 6);
    CHECK(IS_STRING(slots[0]) && strcmp(AS_CSTRING(slots[0]), "value=42") == 0);
    toi_jit_x64_code_stats(memory, &stats); CHECK(stats.fused_stores == 1);
    toi_jit_x64_free_code(memory, size); checks++;

    /* Inline length-add and polymorphic identity fallback. */
    ObjFunction* tiny = new_function(); push(vm, OBJ_VAL(tiny));
    tiny->arity = 2; tiny->tiny_inline_kind = TINY_INLINE_LEN_ADD2;
    ObjClosure* callee = new_closure(tiny); push(vm, OBJ_VAL(callee));
    ToiJitNativeTraceInstr inline_call[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 4), op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 0),
        op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0), op(TOI_JIT_NATIVE_OP_CALL, 2),
        op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    inline_call[2].constant = OBJ_VAL(prefix); inline_call[3].constant = OBJ_VAL(callee);
    fn = compile_trace(inline_call, 6, &memory, &size);
    slots[0] = NUMBER_VAL(0); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(2000); slots[4] = OBJ_VAL(callee);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 12000);
    tiny->tiny_inline_kind = TINY_INLINE_RET0_2;
    ObjClosure* alternate = new_closure(tiny); push(vm, OBJ_VAL(alternate));
    slots[0] = NUMBER_VAL(5); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(2); slots[4] = OBJ_VAL(alternate);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 5);
    toi_jit_x64_free_code(memory, size); checks++;

    ObjFunction* typed_function = new_function(); push(vm, OBJ_VAL(typed_function));
    typed_function->arity = 2; typed_function->tiny_inline_kind = TINY_INLINE_LEN_ADD2;
    uint8_t hints[] = {TYPEHINT_INT, TYPEHINT_STR};
    typed_function->param_types = hints; typed_function->param_types_count = 2;
    ObjClosure* typed_closure = new_closure(typed_function); push(vm, OBJ_VAL(typed_closure));
    inline_call[3].constant = OBJ_VAL(typed_closure);
    fn = compile_trace(inline_call, 6, &memory, &size);
    slots[0] = NUMBER_VAL(0.5); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(1); slots[4] = OBJ_VAL(typed_closure);
    CHECK(fn(vm, closure, slots, 6, &state) == TOI_JIT_NATIVE_EXEC_CONTINUE);
    CHECK(state.reason == TOI_JIT_NATIVE_DEOPT_CALL_TARGET && state.resume_offset == inline_call[3].deopt_offset && state.stack_count == 3);
    CHECK(AS_NUMBER(state.stack[1]) == 0.5 && IS_STRING(state.stack[2]));
    Value invalid_args[] = {NUMBER_VAL(0.5), OBJ_VAL(prefix)}, invalid_out;
    CHECK(!jit_call_primitive(vm, OBJ_VAL(typed_closure), invalid_args, 2, &invalid_out));
    toi_jit_x64_free_code(memory, size);
    typed_function->param_types = NULL; typed_function->param_types_count = 0;
    checks++;

    /* Link a non-tiny arithmetic callee through its compiled IR. The synthetic
     * function has no bytecode, so a passing result requires the native entry. */
    ObjFunction* linked_function = new_function(); push(vm, OBJ_VAL(linked_function));
    linked_function->arity = 2;
    ObjClosure* linked_closure = new_closure(linked_function); push(vm, OBJ_VAL(linked_closure));
    JitOp body[] = {
        {.op = JOP_GET_LOCAL, .a = 1, .byte_offset = 1},
        {.op = JOP_GET_LOCAL, .a = 2, .byte_offset = 2},
        {.op = JOP_ADD, .byte_offset = 3},
        {.op = JOP_GET_LOCAL, .a = 1, .byte_offset = 4},
        {.op = JOP_GET_LOCAL, .a = 2, .byte_offset = 5},
        {.op = JOP_SUBTRACT, .byte_offset = 6},
        {.op = JOP_MULTIPLY, .byte_offset = 7},
        {.op = JOP_RETURN, .byte_offset = 8}
    };
    JitProgram program = {.function = linked_function, .ops = body, .op_count = 8, .max_slot = 2, .max_stack = 8};
    ToiJitNativeTraceInstr linked_call[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 4), op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0),
        op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0), op(TOI_JIT_NATIVE_OP_CALL, 2),
        op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    linked_call[0].constant = OBJ_VAL(linked_closure);
    linked_call[1].constant = NUMBER_VAL(7); linked_call[2].constant = NUMBER_VAL(2);
    linked_call[3].constant = OBJ_VAL(linked_closure); linked_call[3].aux_data = (uint64_t)(uintptr_t)&program;
    fn = compile_trace(linked_call, 6, &memory, &size);
    slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(10); slots[4] = OBJ_VAL(linked_closure);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 45);
    toi_jit_x64_code_stats(memory, &stats);
    CHECK(stats.linked_calls == 1 && stats.fast_call_loops == 1);
    CHECK(stats.call_pic_hits == 0 && stats.native_chain_hits == 10 && stats.callee_compiles == 1);
    // A new target must resume before the call and retain every operand.
    slots[0] = NUMBER_VAL(123); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(1);
    slots[4] = OBJ_VAL(alternate);
    CHECK(fn(vm, closure, slots, 6, &state) == TOI_JIT_NATIVE_EXEC_CONTINUE);
    CHECK(state.reason == TOI_JIT_NATIVE_DEOPT_CALL_TARGET && state.stack_count == 3);
    CHECK(AS_CLOSURE(state.stack[0]) == alternate && AS_NUMBER(state.stack[1]) == 7 && AS_NUMBER(state.stack[2]) == 2);
    CHECK(AS_NUMBER(slots[0]) == 123);
    toi_jit_x64_free_code(memory, size); checks++;

    // A guard inside the callee must not publish a partial result over the
    // original call operands, because the VM will resume this same call.
    linked_call[1].constant = OBJ_VAL(prefix);
    fn = compile_trace(linked_call, 6, &memory, &size);
    slots[0] = NUMBER_VAL(123); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(1);
    slots[4] = OBJ_VAL(linked_closure);
    CHECK(fn(vm, closure, slots, 6, &state) == TOI_JIT_NATIVE_EXEC_CONTINUE);
    CHECK(state.reason == TOI_JIT_NATIVE_DEOPT_CALL_TARGET && state.stack_count == 3);
    CHECK(state.resume_offset == linked_call[3].deopt_offset);
    CHECK(AS_CLOSURE(state.stack[0]) == linked_closure && AS_STRING(state.stack[1]) == prefix);
    CHECK(AS_NUMBER(state.stack[2]) == 2 && AS_NUMBER(slots[0]) == 123);
    toi_jit_x64_free_code(memory, size); checks++;
    linked_call[1].constant = NUMBER_VAL(7);

    // Direct linked calls must validate hints before entering the callee.
    uint8_t linked_hints[] = {TYPEHINT_INT, TYPEHINT_INT};
    linked_function->param_types = linked_hints; linked_function->param_types_count = 2;
    linked_call[2].constant = NUMBER_VAL(2.5);
    fn = compile_trace(linked_call, 6, &memory, &size);
    slots[0] = NUMBER_VAL(123); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(1);
    slots[4] = OBJ_VAL(linked_closure);
    CHECK(fn(vm, closure, slots, 6, &state) == TOI_JIT_NATIVE_EXEC_CONTINUE);
    CHECK(state.reason == TOI_JIT_NATIVE_DEOPT_CALL_TARGET && state.stack_count == 3);
    CHECK(AS_NUMBER(state.stack[2]) == 2.5 && AS_NUMBER(slots[0]) == 123);
    toi_jit_x64_free_code(memory, size);
    linked_function->param_types = NULL; linked_function->param_types_count = 0;
    checks++;

    ObjClosure* first = arithmetic_closure(vm, 1);
    ObjClosure* second = arithmetic_closure(vm, 0);
    ToiJitNativeTraceInstr dynamic_call[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 4), op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0),
        op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0), op(TOI_JIT_NATIVE_OP_CALL, 2),
        op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    dynamic_call[1].constant = NUMBER_VAL(7); dynamic_call[2].constant = NUMBER_VAL(2);
    fn = compile_trace(dynamic_call, 6, &memory, &size);
    slots[4] = OBJ_VAL(first); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(10);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 45);
    slots[4] = OBJ_VAL(second); slots[1] = NUMBER_VAL(0);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 81);
    slots[4] = OBJ_VAL(first); slots[1] = NUMBER_VAL(0);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 45);
    toi_jit_x64_code_stats(memory, &stats);
    CHECK(stats.callee_compiles == 2 && stats.call_pic_misses == 2);
    CHECK(stats.call_pic_hits == 28 && stats.native_chain_hits == 30);
    first->obj.is_marked = 0; second->obj.is_marked = 0;
    toi_jit_x64_mark_code(memory);
    CHECK(first->obj.is_marked && second->obj.is_marked);
    toi_jit_x64_free_code(memory, size); checks++;

    // Megamorphic sites stop growing the owned code cache and retain fallback.
    fn = compile_trace(dynamic_call, 6, &memory, &size);
    for (int i = 0; i < 65; i++) {
        ObjClosure* target = new_closure(first->function); push(vm, OBJ_VAL(target));
        slots[4] = OBJ_VAL(target); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(1);
        run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 45);
    }
    toi_jit_x64_code_stats(memory, &stats);
    CHECK(stats.callee_compiles == 64 && stats.native_chain_hits == 64 && stats.call_pic_misses == 65);
    toi_jit_x64_free_code(memory, size); checks++;

    /* Indexed stores use the full runtime path for sparse and negative keys. */
    ToiJitNativeTraceInstr write[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 3), op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 1),
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 1), op(TOI_JIT_NATIVE_OP_SET_TABLE_NUM, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    fn = compile_trace(write, 5, &memory, &size);
    slots[3] = OBJ_VAL(t); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(10);
    run_to_exit(fn, vm, closure, slots, 6);
    for (int i = 0; i < 10; i++) CHECK(AS_NUMBER(t->table.array[i]) == i + 1);
    toi_jit_x64_free_code(memory, size); checks++;

    /* Call and invoke results are staged identically. */
    ObjTable* methods = new_table(); push(vm, OBJ_VAL(methods));
    ObjString* method = copy_string("counter", 7); table_set(&methods->table, method, OBJ_VAL(native));
    ToiJitNativeTraceInstr invoke[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 3), op(TOI_JIT_NATIVE_OP_INVOKE, 0),
        op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    invoke[1].constant = OBJ_VAL(method); fn = compile_trace(invoke, 4, &memory, &size);
    calls = 0; slots[3] = OBJ_VAL(methods); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(10);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(calls == 10 && AS_NUMBER(slots[0]) == 10);
    toi_jit_x64_free_code(memory, size); checks++;

    /* Dense-array inline guards fall back for negative and missing indices. */
    ToiJitNativeTraceInstr lookup[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 3), op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0),
        op(TOI_JIT_NATIVE_OP_GET_TABLE_NUM, 0), op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    lookup[1].constant = NUMBER_VAL(-1); fn = compile_trace(lookup, 5, &memory, &size);
    slots[3] = OBJ_VAL(t); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(1);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 4000);
    toi_jit_x64_free_code(memory, size);
    lookup[1].constant = NUMBER_VAL(10000); fn = compile_trace(lookup, 5, &memory, &size);
    slots[1] = NUMBER_VAL(0); run_to_exit(fn, vm, closure, slots, 6); CHECK(IS_NIL(slots[0]));
    toi_jit_x64_free_code(memory, size); checks++;

    ObjTable* cached = new_table(); push(vm, OBJ_VAL(cached));
    ObjString* key = copy_string("answer", 6); push(vm, OBJ_VAL(key));
    table_set(&cached->table, key, NUMBER_VAL(11));
    ToiJitNativeTraceInstr cached_get[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 3), op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0),
        op(TOI_JIT_NATIVE_OP_GET_TABLE, 0), op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    cached_get[1].constant = OBJ_VAL(key); fn = compile_trace(cached_get, 5, &memory, &size);
    slots[3] = OBJ_VAL(cached); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(10);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 11);
    toi_jit_x64_code_stats(memory, &stats); CHECK(stats.table_ic_hits + stats.native_table_ic_hits >= 9 && stats.native_table_ic_hits >= 9);
    // Replacing a value need not change shape_version. Re-read the live entry.
    table_set(&cached->table, key, NUMBER_VAL(22)); slots[1] = NUMBER_VAL(0);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 22);
    CHECK(table_delete(&cached->table, key));
    for (int i = 0; i < 100; i++) {
        char text[32]; snprintf(text, sizeof(text), "extra%d", i);
        table_set(&cached->table, copy_string(text, (int)strlen(text)), NUMBER_VAL(i));
    }
    table_set(&cached->table, key, NUMBER_VAL(33)); slots[1] = NUMBER_VAL(0);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 33);
    toi_jit_x64_free_code(memory, size); checks++;

    // Method caches must see replacement even when the table shape is stable.
    ObjFunction* method_function = new_function(); push(vm, OBJ_VAL(method_function));
    method_function->arity = 2; method_function->tiny_inline_kind = TINY_INLINE_RET1_2;
    ObjClosure* replacement = new_closure(method_function); push(vm, OBJ_VAL(replacement));
    ToiJitNativeTraceInstr method_call[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 3), op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0),
        op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0), op(TOI_JIT_NATIVE_OP_INVOKE, 2),
        op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    method_call[1].constant = NUMBER_VAL(7); method_call[2].constant = NUMBER_VAL(9);
    method_call[3].constant = OBJ_VAL(method); table_set(&methods->table, method, OBJ_VAL(native));
    fn = compile_trace(method_call, 6, &memory, &size);
    calls = 0; slots[3] = OBJ_VAL(methods); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(10);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(calls == 10);
    toi_jit_x64_code_stats(memory, &stats); CHECK(stats.method_ic_hits >= 9);
    uint32_t shape = methods->table.shape_version;
    table_set(&methods->table, method, OBJ_VAL(replacement)); CHECK(methods->table.shape_version == shape);
    slots[1] = NUMBER_VAL(0); run_to_exit(fn, vm, closure, slots, 6);
    CHECK(AS_NUMBER(slots[0]) == 9 && calls == 10);
    toi_jit_x64_free_code(memory, size); checks++;

    ToiJitNativeTraceInstr numeric_deep[33];
    for (int i = 0; i < 16; i++) {
        numeric_deep[i] = op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0);
        numeric_deep[i].constant = NUMBER_VAL(1);
    }
    for (int i = 16; i < 31; i++) numeric_deep[i] = op(TOI_JIT_NATIVE_OP_BIN_ADD, 0);
    numeric_deep[31] = op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0);
    numeric_deep[32] = op(TOI_JIT_NATIVE_OP_POP, 0);
    fn = compile_trace(numeric_deep, 33, &memory, &size);
    slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(1);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 16);
    toi_jit_x64_free_code(memory, size); checks++;

    // Exercise the canonical spill path beyond the 14 promoted XMM values.
    ToiJitNativeTraceInstr deep[255];
    deep[0] = op(TOI_JIT_NATIVE_OP_ADJUST_STACK, 255);
    for (int i = 1; i < 255; i++) deep[i] = op(TOI_JIT_NATIVE_OP_GET_TABLE, 0);
    fn = compile_trace(deep, 255, &memory, &size);
    slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(1);
    CHECK(fn(vm, closure, slots, 6, &state) == TOI_JIT_NATIVE_EXEC_CONTINUE);
    CHECK(state.reason == TOI_JIT_NATIVE_DEOPT_TABLE_GUARD && state.stack_count == 255);
    for (int i = 0; i < 255; i++) CHECK(IS_NIL(state.stack[i]));
    toi_jit_x64_free_code(memory, size); checks++;

}
static void test_strings(VM* vm, ObjClosure* closure) {
    ObjString* text = copy_string("abcdefgh", 8); push(vm, OBJ_VAL(text));
    ToiJitNativeTraceInstr length_ops[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 4), op(TOI_JIT_NATIVE_OP_LENGTH, 0),
        op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    void* memory; size_t size;
    ToiJitNativeTraceFn fn = compile_trace(length_ops, 4, &memory, &size);
    Value slots[6] = {NUMBER_VAL(123), NUMBER_VAL(0), NUMBER_VAL(10), NIL_VAL, OBJ_VAL(text), NIL_VAL};
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 8);
    ObjTable* table = new_table(); push(vm, OBJ_VAL(table));
    table_set_array(&table->table, 1, BOOL_VAL(1)); table_set_array(&table->table, 3, BOOL_VAL(0));
    table_set(&table->table, text, BOOL_VAL(1));
    slots[4] = OBJ_VAL(table); slots[1] = NUMBER_VAL(0);
    run_to_exit(fn, vm, closure, slots, 6); CHECK(AS_NUMBER(slots[0]) == 3);
    slots[0] = NUMBER_VAL(123); slots[1] = NUMBER_VAL(0); slots[4] = BOOL_VAL(1);
    ToiJitNativeExecState state;
    CHECK(fn(vm, closure, slots, 6, &state) == TOI_JIT_NATIVE_EXEC_CONTINUE);
    CHECK(state.reason == TOI_JIT_NATIVE_DEOPT_TYPE_GUARD && state.stack_count == 1);
    CHECK(IS_BOOL(state.stack[0]) && AS_NUMBER(slots[0]) == 123);
    toi_jit_x64_free_code(memory, size); checks++;

    ToiJitNativeTraceInstr slice_ops[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 4), op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0),
        op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0), op(TOI_JIT_NATIVE_OP_PUSH_CONST, 0),
        op(TOI_JIT_NATIVE_OP_SLICE, 0), op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    slice_ops[1].constant = NUMBER_VAL(2); slice_ops[2].constant = NUMBER_VAL(5); slice_ops[3].constant = NIL_VAL;
    fn = compile_trace(slice_ops, 7, &memory, &size);
    slots[4] = OBJ_VAL(text); slots[1] = NUMBER_VAL(0);
    run_to_exit(fn, vm, closure, slots, 6);
    CHECK(IS_STRING(slots[0]) && strcmp(AS_CSTRING(slots[0]), "bcde") == 0);
    slots[0] = NUMBER_VAL(123); slots[1] = NUMBER_VAL(0); slots[4] = OBJ_VAL(table);
    CHECK(fn(vm, closure, slots, 6, &state) == TOI_JIT_NATIVE_EXEC_CONTINUE);
    CHECK(state.reason == TOI_JIT_NATIVE_DEOPT_TYPE_GUARD && state.resume_offset == slice_ops[4].deopt_offset);
    CHECK(state.stack_count == 4 && AS_TABLE(state.stack[0]) == table);
    CHECK(AS_NUMBER(state.stack[1]) == 2 && AS_NUMBER(state.stack[2]) == 5 && IS_NIL(state.stack[3]));
    CHECK(AS_NUMBER(slots[0]) == 123);
    toi_jit_x64_free_code(memory, size); checks++;

    // Reject unsafe conversions and zero/fractional strides before any loop.
    Value out;
    CHECK(!jit_slice_primitive(OBJ_VAL(text), NUMBER_VAL(1), NUMBER_VAL(8), NUMBER_VAL(0.5), &out));
    CHECK(!jit_slice_primitive(OBJ_VAL(text), NUMBER_VAL(INFINITY), NUMBER_VAL(8), NIL_VAL, &out));
    CHECK(!jit_slice_primitive(OBJ_VAL(text), NUMBER_VAL(1), NUMBER_VAL(8), NUMBER_VAL(NAN), &out));
    CHECK(!jit_slice_primitive(OBJ_VAL(text), NUMBER_VAL(99), NUMBER_VAL(1), NUMBER_VAL(-1), &out));
    CHECK(jit_slice_primitive(OBJ_VAL(text), NIL_VAL, NIL_VAL, NUMBER_VAL(-1), &out));
    CHECK(strcmp(AS_CSTRING(out), "hgfedcba") == 0); checks++;
}

static void test_frontend_strings(VM* vm) {
    const char* source =
        "fn formatted_lengths()\n"
        "  total = 0\n"
        "  for i in 1..100000\n"
        "    s = f\"user:{i}-x{i % 10}\"\n"
        "    total += #s\n"
        "  return total\n"
        "fn sliced_lengths()\n"
        "  total = 0\n"
        "  for i in 1..20000\n"
        "    s = f\"prefix:{i}\"\n"
        "    part = s[2..7]\n"
        "    total += #part\n"
        "  return total\n"
        "assert formatted_lengths() == 1288895\n"
        "assert sliced_lengths() == 120000\n"
        "fn final_string()\n"
        "  result = \"\"\n"
        "  for i in 1..100000\n"
        "    result = f\"value:{i}\"\n"
        "  return result\n"
        "assert final_string() == \"value:100000\"\n";
    ObjFunction* function = compile(source);
    CHECK(function != NULL && interpret(vm, function) == INTERPRET_OK);
    ToiJitState* st = (ToiJitState*)vm->native_jit;
    int found = 0, slices = 0;
    for (int i = 0; i < st->loop_count; i++) {
        JitLoopTrace* trace = &st->loops[i];
        if (!trace->function->name) continue;
        const char* name = trace->function->name->chars;
        if (strcmp(name, "formatted_lengths") && strcmp(name, "sliced_lengths")) continue;
        CHECK(trace->native_trace_direct_core && trace->native_trace_fast_calls && trace->native_trace_exec_safe);
        CHECK(trace->native_trace_exec_hits > 0 && trace->native_trace_deopts == 0);
        ToiJitX64Stats stats; toi_jit_x64_code_stats(trace->native_trace_code_mem, &stats);
        CHECK(stats.fast_string_loops == 1);
        int lengths = 0;
        for (int j = 0; j < trace->native_trace_instr_count; j++) {
            lengths += trace->native_trace_instrs[j].op == TOI_JIT_NATIVE_OP_LENGTH;
            slices += trace->native_trace_instrs[j].op == TOI_JIT_NATIVE_OP_SLICE;
        }
        CHECK(lengths == 1); found++;
    }
    CHECK(found == 2 && slices == 1); checks++;
}

static void test_weak_string_cache(VM* vm) {
    ObjString* live = copy_string("retained-cache-test", 19);
    ObjString* key = copy_string("cache_live", 10);
    table_set(&vm->globals, key, OBJ_VAL(live));
    vm->build_string_cache[13] = copy_string("dead-cache-test", 15);
    vm->build_string_cache[14] = live;
    ObjFunction* function = compile("gc\n");
    CHECK(function != NULL && interpret(vm, function) == INTERPRET_OK);
    CHECK(vm->build_string_cache[13] == NULL && vm->build_string_cache[14] == live);
    table_set(&vm->globals, key, NIL_VAL);
    function = compile("gc\n");
    CHECK(function != NULL && interpret(vm, function) == INTERPRET_OK);
    CHECK(vm->build_string_cache[14] == NULL); checks++;
}

static void test_random(VM* vm, ObjClosure* closure) {
    ObjString* key = copy_string("random", 6); push(vm, OBJ_VAL(key));
    ObjNative* native = new_native(counter, key); push(vm, OBJ_VAL(native));
    native->fast_kind = NATIVE_FAST_MATH_RANDOM;
    ObjTable* module = new_table(); push(vm, OBJ_VAL(module)); module->is_module = 1;
    table_set(&module->table, key, OBJ_VAL(native));
    ToiJitNativeTraceInstr ops[] = {
        op(TOI_JIT_NATIVE_OP_PUSH_LOCAL, 4), op(TOI_JIT_NATIVE_OP_INVOKE, 0),
        op(TOI_JIT_NATIVE_OP_STORE_LOCAL, 0), op(TOI_JIT_NATIVE_OP_POP, 0)
    };
    ops[1].constant = OBJ_VAL(key);
    void* memory; size_t size;
    ToiJitNativeTraceFn fn = compile_trace(ops, 4, &memory, &size);
    Value slots[6] = {NUMBER_VAL(123), NUMBER_VAL(0), NUMBER_VAL(10000), NIL_VAL, OBJ_VAL(module), NIL_VAL};
    srand(56789);
    double expected = 0;
    for (int i = 0; i < 10000; i++) expected = (double)rand() / RAND_MAX;
    int expected_next = rand();
    srand(56789);
    run_to_exit(fn, vm, closure, slots, 6);
    CHECK(AS_NUMBER(slots[0]) == expected && rand() == expected_next);
    ToiJitX64Stats stats; toi_jit_x64_code_stats(memory, &stats);
    CHECK(stats.random_calls == 1 && stats.fast_call_loops == 1);

    ObjNative* other = new_native(counter, key); push(vm, OBJ_VAL(other));
    uint32_t shape = module->table.shape_version;
    table_set(&module->table, key, OBJ_VAL(other));
    CHECK(module->table.shape_version == shape);
    slots[0] = NUMBER_VAL(123); slots[1] = NUMBER_VAL(0); slots[2] = NUMBER_VAL(1);
    ToiJitNativeExecState state;
    srand(56789); expected_next = rand(); srand(56789);
    CHECK(fn(vm, closure, slots, 6, &state) == TOI_JIT_NATIVE_EXEC_CONTINUE);
    CHECK(state.reason == TOI_JIT_NATIVE_DEOPT_INVOKE_TARGET && state.resume_offset == ops[1].deopt_offset);
    CHECK(state.stack_count == 1 && AS_TABLE(state.stack[0]) == module);
    CHECK(AS_NUMBER(slots[0]) == 123 && rand() == expected_next);
    // Modules and ordinary tables differ in implicit-self call semantics.
    table_set(&module->table, key, OBJ_VAL(native)); module->is_module = 0;
    slots[1] = NUMBER_VAL(0);
    CHECK(fn(vm, closure, slots, 6, &state) == TOI_JIT_NATIVE_EXEC_CONTINUE);
    CHECK(state.reason == TOI_JIT_NATIVE_DEOPT_INVOKE_TARGET && state.stack_count == 1);
    toi_jit_x64_free_code(memory, size); checks++;

    srand(12345);
    expected = 0;
    for (int i = 0; i < 100000; i++) expected += (double)rand() / RAND_MAX;
    double next = (double)rand() / RAND_MAX;
    char source[1024];
    snprintf(source, sizeof(source),
        "math = import math\n"
        "fn random_draws()\n"
        "  total = 0\n"
        "  for i in 1..100000\n"
        "    total += math.random()\n"
        "  return total\n"
        "math.seed(12345)\n"
        "assert random_draws() == %.17g\n"
        "assert math.random() == %.17g\n", expected, next);
    ObjFunction* function = compile(source);
    CHECK(function != NULL && interpret(vm, function) == INTERPRET_OK);
    ToiJitState* st = (ToiJitState*)vm->native_jit;
    int found = 0;
    for (int i = 0; i < st->loop_count; i++) {
        JitLoopTrace* trace = &st->loops[i];
        if (!trace->function->name || strcmp(trace->function->name->chars, "random_draws")) continue;
        CHECK(trace->native_trace_direct_core && trace->native_trace_fast_calls);
        CHECK(trace->native_trace_exec_hits > 20 && trace->native_trace_soft_bails == 0);
        toi_jit_x64_code_stats(trace->native_trace_code_mem, &stats);
        CHECK(stats.random_calls == 1 && stats.fast_call_loops == 1); found++;
    }
    CHECK(found == 1); checks++;
}

static void test_frontend_calls(VM* vm) {
    const char* source =
        "fn calls(start, n)\n"
        "  fn add(a: int, b: str)\n"
        "    return a + #b\n"
        "  acc = start\n"
        "  for i in 1..n\n"
        "    acc = add(acc, \"df\")\n"
        "  return acc\n"
        "assert calls(0, 100000) == 200000\n";
    ObjFunction* function = compile(source);
    CHECK(function != NULL && interpret(vm, function) == INTERPRET_OK);
    ToiJitState* st = (ToiJitState*)vm->native_jit;
    int found = 0;
    for (int i = 0; i < st->loop_count; i++) {
        JitLoopTrace* trace = &st->loops[i];
        if (!trace->function->name || strcmp(trace->function->name->chars, "calls") != 0) continue;
        CHECK(trace->native_trace_direct_core && trace->native_trace_fast_calls);
        CHECK(trace->native_trace_exec_hits > 0 && trace->native_trace_exec_safe);
        ToiJitX64Stats stats; toi_jit_x64_code_stats(trace->native_trace_code_mem, &stats);
        CHECK(stats.inlined_calls == 1 && stats.fast_call_loops == 1);
        int quickened = 0;
        for (int j = 0; j < trace->op_count; j++) {
            if (trace->ops[j].op == JOP_QCALL2) {
                CHECK(trace->ops[j].a == 2 && IS_CLOSURE(trace->ops[j].constant)); quickened++;
            }
        }
        CHECK(quickened == 1); found++;
    }
    CHECK(found == 1); checks++;

    source =
        "fn general_calls()\n"
        "  fn evaluate(a: int, b: int)\n"
        "    return (a + b) * (a - b)\n"
        "  total = 0\n"
        "  for i in 1..10000\n"
        "    total += evaluate(i, 2)\n"
        "  return total\n"
        "fn branching_calls()\n"
        "  fn choose(a: int, b: int, c: int)\n"
        "    if a < b\n"
        "      return a * 2 + c\n"
        "    return a + b - c\n"
        "  total = 0\n"
        "  for i in 1..10000\n"
        "    total += choose(i, 5000, 3)\n"
        "  return total\n"
        "assert general_calls() == 333383295000\n"
        "assert branching_calls() == 87507494\n";
    function = compile(source);
    CHECK(function != NULL && interpret(vm, function) == INTERPRET_OK);
    found = 0;
    for (int i = 0; i < st->loop_count; i++) {
        JitLoopTrace* trace = &st->loops[i];
        if (!trace->function->name) continue;
        const char* name = trace->function->name->chars;
        if (strcmp(name, "general_calls") && strcmp(name, "branching_calls")) continue;
        CHECK(trace->native_trace_direct_core && trace->native_trace_fast_calls);
        CHECK(trace->native_trace_exec_hits > 0 && trace->native_trace_exec_safe);
        ToiJitX64Stats stats; toi_jit_x64_code_stats(trace->native_trace_code_mem, &stats);
        CHECK(stats.inlined_calls == 0 && stats.linked_calls == 1 && stats.fast_call_loops == 1);
        CHECK(stats.native_chain_hits > 8000 && stats.call_pic_hits == 0);
        found++;
    }
    CHECK(found == 2); checks++;
}
int main(void) {
    if (!toi_jit_x64_host_supported()) { puts("SKIP: x64 host required"); return 0; }
    test_numeric();
    VM vm; init_vm(&vm);
    ObjFunction* f = new_function(); push(&vm, OBJ_VAL(f));
    ObjClosure* closure = new_closure(f); push(&vm, OBJ_VAL(closure));
    test_traces(&vm, closure); test_strings(&vm, closure); test_random(&vm, closure);
    test_frontend_calls(&vm); test_frontend_strings(&vm); test_weak_string_cache(&vm); free_vm(&vm);
    printf("x64 backend: %d groups passed\n", checks); return 0;
}
