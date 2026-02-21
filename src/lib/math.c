#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int math_sin(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(sin(GET_NUMBER(0)));
}

static int math_cos(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(cos(GET_NUMBER(0)));
}

static int math_tan(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(tan(GET_NUMBER(0)));
}

static int math_asin(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(asin(GET_NUMBER(0)));
}

static int math_acos(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(acos(GET_NUMBER(0)));
}

static int math_atan(VM* vm, int arg_count, Value* args) {
    if (arg_count == 1) {
        ASSERT_NUMBER(0);
        RETURN_NUMBER(atan(GET_NUMBER(0)));
    } else if (arg_count == 2) {
        ASSERT_NUMBER(0);
        ASSERT_NUMBER(1);
        RETURN_NUMBER(atan2(GET_NUMBER(0), GET_NUMBER(1)));
    }
    RETURN_NIL;
}

static int math_sqrt(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(sqrt(GET_NUMBER(0)));
}

static int math_floor(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(floor(GET_NUMBER(0)));
}

static int math_ceil(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(ceil(GET_NUMBER(0)));
}

static int math_abs(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(fabs(GET_NUMBER(0)));
}

static int math_exp(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(exp(GET_NUMBER(0)));
}

static int math_log(VM* vm, int arg_count, Value* args) {
    if (arg_count == 1) {
        ASSERT_NUMBER(0);
        RETURN_NUMBER(log(GET_NUMBER(0)));
    } else if (arg_count == 2) {
        ASSERT_NUMBER(0);
        ASSERT_NUMBER(1);
        RETURN_NUMBER(log(GET_NUMBER(0)) / log(GET_NUMBER(1)));
    }
    RETURN_NIL;
}

static int math_pow(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_NUMBER(0);
    ASSERT_NUMBER(1);
    RETURN_NUMBER(pow(GET_NUMBER(0), GET_NUMBER(1)));
}

static int math_fmod(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_NUMBER(0);
    ASSERT_NUMBER(1);
    RETURN_NUMBER(fmod(GET_NUMBER(0), GET_NUMBER(1)));
}

static int math_divmod(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_NUMBER(0);
    ASSERT_NUMBER(1);
    double a = GET_NUMBER(0);
    double b = GET_NUMBER(1);
    if (b == 0.0) {
        vm_runtime_error(vm, "math.divmod: division by zero");
        return 0;
    }
    double q = floor(a / b);
    double r = a - (q * b);
    ObjTable* out = new_table();
    if (!table_set_array(&out->table, 1, NUMBER_VAL(q))) {
        vm_runtime_error(vm, "math.divmod: failed to set quotient");
        return 0;
    }
    if (!table_set_array(&out->table, 2, NUMBER_VAL(r))) {
        vm_runtime_error(vm, "math.divmod: failed to set remainder");
        return 0;
    }
    RETURN_OBJ(out);
}

static int math_modf(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    double intpart;
    double fracpart = modf(GET_NUMBER(0), &intpart);
    // Return two values: integer part, fractional part
    push(vm, NUMBER_VAL(intpart));
    push(vm, NUMBER_VAL(fracpart));
    return 2;
}

static int math_deg(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(GET_NUMBER(0) * (180.0 / 3.14159265358979323846));
}

static int math_rad(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(GET_NUMBER(0) * (3.14159265358979323846 / 180.0));
}

static int math_random(VM* vm, int arg_count, Value* args) {
    if (arg_count == 0) {
        RETURN_NUMBER((double)rand() / (double)RAND_MAX);
    } else if (arg_count == 1) {
        ASSERT_NUMBER(0);
        int max = (int)GET_NUMBER(0);
        if (max < 1) { RETURN_NIL; }
        RETURN_NUMBER((double)(rand() % max) + 1);
    } else if (arg_count == 2) {
        ASSERT_NUMBER(0);
        ASSERT_NUMBER(1);
        int min = (int)GET_NUMBER(0);
        int max = (int)GET_NUMBER(1);
        if (min > max) { RETURN_NIL; }
        RETURN_NUMBER((double)(min + rand() % (max - min + 1)));
    } else {
        RETURN_NIL;
    }
}

static int math_seed(VM* vm, int arg_count, Value* args) {
    if (arg_count == 1) {
        ASSERT_NUMBER(0);
        srand((unsigned int)GET_NUMBER(0));
    } else if (arg_count == 0) {
        srand((unsigned int)time(NULL));
    } else {
        RETURN_NIL;
    }
    RETURN_NIL;
}

static int math_min(VM* vm, int arg_count, Value* args) {
    if (arg_count == 0) { RETURN_NIL; }
    ASSERT_NUMBER(0);
    double min = GET_NUMBER(0);
    for (int i = 1; i < arg_count; i++) {
        ASSERT_NUMBER(i);
        double val = GET_NUMBER(i);
        if (val < min) min = val;
    }
    RETURN_NUMBER(min);
}

static int math_max(VM* vm, int arg_count, Value* args) {
    if (arg_count == 0) { RETURN_NIL; }
    ASSERT_NUMBER(0);
    double max = GET_NUMBER(0);
    for (int i = 1; i < arg_count; i++) {
        ASSERT_NUMBER(i);
        double val = GET_NUMBER(i);
        if (val > max) max = val;
    }
    RETURN_NUMBER(max);
}

static int math_sum(VM* vm, int arg_count, Value* args) {
    if (arg_count == 0) { RETURN_NIL; }

    if (arg_count == 1 && IS_TABLE(args[0])) {
        ObjTable* table = GET_TABLE(0);
        double sum = 0.0;
        for (int i = 1; ; i++) {
            Value val;
            if (!table_get_array(&table->table, i, &val) || IS_NIL(val)) break;
            if (!IS_NUMBER(val)) {
                vm_runtime_error(vm, "math.sum: element %d is not a number", i);
                return 0;
            }
            sum += AS_NUMBER(val);
        }
        RETURN_NUMBER(sum);
    }

    double sum = 0.0;
    for (int i = 0; i < arg_count; i++) {
        ASSERT_NUMBER(i);
        sum += GET_NUMBER(i);
    }
    RETURN_NUMBER(sum);
}

static void set_math_fast_kind(ObjTable* math_module, const char* name, NativeFastKind kind) {
    ObjString* key = copy_string(name, (int)strlen(name));
    Value fn = NIL_VAL;
    if (table_get(&math_module->table, key, &fn) && IS_NATIVE(fn)) {
        AS_NATIVE_OBJ(fn)->fast_kind = (uint8_t)kind;
    }
}

void register_math(VM* vm) {
    const NativeReg math_funcs[] = {
        {"sin", math_sin},
        {"cos", math_cos},
        {"tan", math_tan},
        {"asin", math_asin},
        {"acos", math_acos},
        {"atan", math_atan},
        {"sqrt", math_sqrt},
        {"floor", math_floor},
        {"ceil", math_ceil},
        {"abs", math_abs},
        {"exp", math_exp},
        {"log", math_log},
        {"pow", math_pow},
        {"fmod", math_fmod},
        {"divmod", math_divmod},
        {"modf", math_modf},
        {"deg", math_deg},
        {"rad", math_rad},
        {"random", math_random},
        {"seed", math_seed},
        {"min", math_min},
        {"max", math_max},
        {"sum", math_sum},
        {NULL, NULL}
    };

    register_module(vm, "math", math_funcs);
    ObjTable* math_module = AS_TABLE(peek(vm, 0)); // Module is on stack

    set_math_fast_kind(math_module, "sin", NATIVE_FAST_MATH_SIN);
    set_math_fast_kind(math_module, "cos", NATIVE_FAST_MATH_COS);
    set_math_fast_kind(math_module, "tan", NATIVE_FAST_MATH_TAN);
    set_math_fast_kind(math_module, "asin", NATIVE_FAST_MATH_ASIN);
    set_math_fast_kind(math_module, "acos", NATIVE_FAST_MATH_ACOS);
    set_math_fast_kind(math_module, "atan", NATIVE_FAST_MATH_ATAN);
    set_math_fast_kind(math_module, "sqrt", NATIVE_FAST_MATH_SQRT);
    set_math_fast_kind(math_module, "floor", NATIVE_FAST_MATH_FLOOR);
    set_math_fast_kind(math_module, "ceil", NATIVE_FAST_MATH_CEIL);
    set_math_fast_kind(math_module, "abs", NATIVE_FAST_MATH_ABS);
    set_math_fast_kind(math_module, "exp", NATIVE_FAST_MATH_EXP);
    set_math_fast_kind(math_module, "log", NATIVE_FAST_MATH_LOG);
    set_math_fast_kind(math_module, "pow", NATIVE_FAST_MATH_POW);
    set_math_fast_kind(math_module, "fmod", NATIVE_FAST_MATH_FMOD);
    set_math_fast_kind(math_module, "deg", NATIVE_FAST_MATH_DEG);
    set_math_fast_kind(math_module, "rad", NATIVE_FAST_MATH_RAD);

    // Constants
    push(vm, OBJ_VAL(copy_string("pi", 2)));
    push(vm, NUMBER_VAL(3.14159265358979323846));
    table_set(&math_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);

    push(vm, OBJ_VAL(copy_string("huge", 4)));
    push(vm, NUMBER_VAL(HUGE_VAL));
    table_set(&math_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);

    pop(vm); // Pop math_module
}
