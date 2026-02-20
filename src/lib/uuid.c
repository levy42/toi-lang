#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"


static const char ALPH[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"; // 62


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
        out[i] = ALPH[v % 62];
        v /= 62;
    }
}

static int fill_secure_random(uint8_t* out, size_t len) {
#if defined(__linux__)
    size_t off = 0;
    while (off < len) {
        ssize_t n = getrandom(out + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (n == 0) return 0;
        off += (size_t)n;
    }
    return 1;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(out, len);
    return 1;
#else
    FILE* fp = fopen("/dev/urandom", "rb");
    if (fp == NULL) return 0;
    size_t got = fread(out, 1, len, fp);
    fclose(fp);
    return got == len;
#endif
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

static int uid(VM* vm, int arg_count, Value* args)
{
    (void)args;
    ASSERT_ARGC_EQ(0);
    char buf[17];
    id16(buf);
    RETURN_OBJ(copy_string(buf, 16));
}

static int secret(VM* vm, int arg_count, Value* args)
{
    int out_len = 64;
    if (arg_count > 1) {
        vm_runtime_error(vm, "uuid.secret([length]) expects 0 or 1 argument.");
        return 0;
    }
    if (arg_count == 1) {
        ASSERT_NUMBER(0);
        double d = GET_NUMBER(0);
        int n = (int)d;
        if ((double)n != d || n <= 0 || n > 4096) {
            vm_runtime_error(vm, "uuid.secret(length) expects an integer in range 1..4096.");
            return 0;
        }
        out_len = n;
    }

    char* out = (char*)malloc((size_t)out_len + 1);
    if (out == NULL) {
        vm_runtime_error(vm, "Out of memory in uuid.secret().");
        return 0;
    }

    const int alpha_len = (int)(sizeof(ALPH) - 1);
    const uint8_t reject_above = (uint8_t)(256 - (256 % alpha_len));
    int w = 0;
    uint8_t buf[64];
    while (w < out_len) {
        if (!fill_secure_random(buf, sizeof(buf))) {
            free(out);
            vm_runtime_error(vm, "Failed to read secure random bytes.");
            return 0;
        }
        for (size_t i = 0; i < sizeof(buf) && w < out_len; ++i) {
            if (buf[i] >= reject_above) continue;
            out[w++] = ALPH[buf[i] % alpha_len];
        }
    }
    out[out_len] = '\0';
    RETURN_OBJ(take_string(out, out_len));
}

void register_uuid(VM* vm) {
    const NativeReg uuid_funcs[] = {
        {"uid", uid},
        {"secret", secret},
        {NULL, NULL}
    };

    register_module(vm, "uuid", uuid_funcs);
    pop(vm); // Pop uuid module
}
