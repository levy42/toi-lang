#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int math_sin(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(sin(GET_NUMBER(0)));
}

static int math_cos(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(cos(GET_NUMBER(0)));
}

static int math_tan(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(tan(GET_NUMBER(0)));
}

static int math_asin(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(asin(GET_NUMBER(0)));
}

static int math_acos(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(acos(GET_NUMBER(0)));
}

static int math_atan(VM* vm, int argCount, Value* args) {
    if (argCount == 1) {
        ASSERT_NUMBER(0);
        RETURN_NUMBER(atan(GET_NUMBER(0)));
    } else if (argCount == 2) {
        ASSERT_NUMBER(0);
        ASSERT_NUMBER(1);
        RETURN_NUMBER(atan2(GET_NUMBER(0), GET_NUMBER(1)));
    }
    RETURN_NIL;
}

static int math_sqrt(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(sqrt(GET_NUMBER(0)));
}

static int math_floor(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(floor(GET_NUMBER(0)));
}

static int math_ceil(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(ceil(GET_NUMBER(0)));
}

static int math_abs(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(fabs(GET_NUMBER(0)));
}

static int math_exp(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(exp(GET_NUMBER(0)));
}

static int math_log(VM* vm, int argCount, Value* args) {
    if (argCount == 1) {
        ASSERT_NUMBER(0);
        RETURN_NUMBER(log(GET_NUMBER(0)));
    } else if (argCount == 2) {
        ASSERT_NUMBER(0);
        ASSERT_NUMBER(1);
        RETURN_NUMBER(log(GET_NUMBER(0)) / log(GET_NUMBER(1)));
    }
    RETURN_NIL;
}

static int math_pow(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_NUMBER(0);
    ASSERT_NUMBER(1);
    RETURN_NUMBER(pow(GET_NUMBER(0), GET_NUMBER(1)));
}

static int math_fmod(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_NUMBER(0);
    ASSERT_NUMBER(1);
    RETURN_NUMBER(fmod(GET_NUMBER(0), GET_NUMBER(1)));
}

static int math_modf(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    double intpart;
    double fracpart = modf(GET_NUMBER(0), &intpart);
    // Return two values: integer part, fractional part
    push(vm, NUMBER_VAL(intpart));
    push(vm, NUMBER_VAL(fracpart));
    return 2;
}

static int math_deg(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(GET_NUMBER(0) * (180.0 / 3.14159265358979323846));
}

static int math_rad(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    RETURN_NUMBER(GET_NUMBER(0) * (3.14159265358979323846 / 180.0));
}

static int math_random(VM* vm, int argCount, Value* args) {
    if (argCount == 0) {
        RETURN_NUMBER((double)rand() / (double)RAND_MAX);
    } else if (argCount == 1) {
        ASSERT_NUMBER(0);
        int max = (int)GET_NUMBER(0);
        if (max < 1) { RETURN_NIL; }
        RETURN_NUMBER((double)(rand() % max) + 1);
    } else if (argCount == 2) {
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

static int math_seed(VM* vm, int argCount, Value* args) {
    if (argCount == 1) {
        ASSERT_NUMBER(0);
        srand((unsigned int)GET_NUMBER(0));
    } else if (argCount == 0) {
        srand((unsigned int)time(NULL));
    } else {
        RETURN_NIL;
    }
    RETURN_NIL;
}

static int math_min(VM* vm, int argCount, Value* args) {
    if (argCount == 0) { RETURN_NIL; }
    ASSERT_NUMBER(0);
    double min = GET_NUMBER(0);
    for (int i = 1; i < argCount; i++) {
        ASSERT_NUMBER(i);
        double val = GET_NUMBER(i);
        if (val < min) min = val;
    }
    RETURN_NUMBER(min);
}

static int math_max(VM* vm, int argCount, Value* args) {
    if (argCount == 0) { RETURN_NIL; }
    ASSERT_NUMBER(0);
    double max = GET_NUMBER(0);
    for (int i = 1; i < argCount; i++) {
        ASSERT_NUMBER(i);
        double val = GET_NUMBER(i);
        if (val > max) max = val;
    }
    RETURN_NUMBER(max);
}

static int math_sum(VM* vm, int argCount, Value* args) {
    if (argCount == 0) { RETURN_NIL; }

    if (argCount == 1 && IS_TABLE(args[0])) {
        ObjTable* table = GET_TABLE(0);
        double sum = 0.0;
        for (int i = 1; ; i++) {
            Value val;
            if (!tableGetArray(&table->table, i, &val) || IS_NIL(val)) break;
            if (!IS_NUMBER(val)) {
                vmRuntimeError(vm, "math.sum: element %d is not a number", i);
                return 0;
            }
            sum += AS_NUMBER(val);
        }
        RETURN_NUMBER(sum);
    }

    double sum = 0.0;
    for (int i = 0; i < argCount; i++) {
        ASSERT_NUMBER(i);
        sum += GET_NUMBER(i);
    }
    RETURN_NUMBER(sum);
}

void registerMath(VM* vm) {
    const NativeReg mathFuncs[] = {
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

    registerModule(vm, "math", mathFuncs);
    ObjTable* mathModule = AS_TABLE(peek(vm, 0)); // Module is on stack

    // Constants
    push(vm, OBJ_VAL(copyString("pi", 2)));
    push(vm, NUMBER_VAL(3.14159265358979323846));
    tableSet(&mathModule->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);

    push(vm, OBJ_VAL(copyString("huge", 4)));
    push(vm, NUMBER_VAL(HUGE_VAL));
    tableSet(&mathModule->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);

    pop(vm); // Pop mathModule
}
