#include <stdlib.h>
#include <time.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"


static const char ALPH[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"; // 52


static uint32_t xs = 0;
static uint64_t last_ms = ~0ULL;
static char prefix[10]; // cached

static inline uint32_t xorshift32(void) {
    uint32_t x = xs;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xs = x;
    return x;
}

static inline uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline void id_init(void) {
    uint64_t t = now_ms();
    xs = (uint32_t)(t ^ (t >> 32) ^ (uint64_t)getpid() ^ (uintptr_t)&xs);
    if (xs == 0) xs = 1;
}

static inline void enc_base52_fixed(uint64_t v, char *out, int n) {
    for (int i = n - 1; i >= 0; --i) {
        out[i] = ALPH[v % 52];
        v /= 52;
    }
}

// out must be char[17]
void id16(char out[17]) {
    if (xs == 0) id_init();

    uint64_t ms = now_ms();
    if (ms != last_ms) {
        last_ms = ms;
        // 10 chars of time (choose 9/10/11 depending on how much "time" you want)
        enc_base52_fixed(ms, prefix, 10);
    }

    // 6-char suffix (~34 bits in base52)
    uint64_t suf = ((uint64_t)xorshift32() << 32) | xorshift32();
    enc_base52_fixed(suf, out + 10, 6);

    // copy cached prefix
    for (int i = 0; i < 10; ++i) out[i] = prefix[i];

    out[16] = '\0';
}

static int uid(VM* vm, int argCount, Value* args)
{
    ASSERT_ARGC_EQ(0);
    char buf[17];
    id16(buf);

    RETURN_OBJ(takeString(buf, 16));
}

void registerUuid(VM* vm) {
    const NativeReg uuidFuncs[] = {
        {"uid", uid},
        {NULL, NULL}
    };

    registerModule(vm, "uuid", uuidFuncs);
    pop(vm); // Pop timeModule
}
