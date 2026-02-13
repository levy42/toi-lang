#include <time.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int time_seconds(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(0);
    #if defined(CLOCK_REALTIME)
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        RETURN_NUMBER((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
    }
    #endif
    RETURN_NUMBER((double)time(NULL));
}

static int time_clock(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(0);
    RETURN_NUMBER((double)clock() / CLOCKS_PER_SEC);
}

static int time_nanos(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(0);
    #if defined(CLOCK_REALTIME)
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        RETURN_NUMBER((double)ns);
    }
    #endif
    RETURN_NUMBER((double)time(NULL) * 1e9);
}

static int time_sleep(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    double seconds = GET_NUMBER(0);
    if (seconds < 0) {
        vmRuntimeError(vm, "sleep() expects a non-negative number.");
        return 0;
    }

    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec < 0) ts.tv_nsec = 0;

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        // retry with remaining time
    }
    RETURN_NIL;
}

void registerTime(VM* vm) {
    const NativeReg timeFuncs[] = {
        {"time", time_seconds},
        {"seconds", time_seconds},
        {"nanos", time_nanos},
        {"sleep", time_sleep},
        {"clock", time_clock},
        {NULL, NULL}
    };
    
    registerModule(vm, "time", timeFuncs);
    pop(vm); // Pop timeModule
}
