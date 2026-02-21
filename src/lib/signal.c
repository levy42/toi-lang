#include <signal.h>
#include <string.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

typedef struct {
    const char* name;
    int value;
} SigMap;

static int signal_from_name(const char* s, int len) {
    if (len >= 3 && s[0] == 'S' && s[1] == 'I' && s[2] == 'G') {
        s += 3;
        len -= 3;
    }

    static const SigMap map[] = {
        {"HUP", SIGHUP},
        {"INT", SIGINT},
        {"QUIT", SIGQUIT},
        {"ILL", SIGILL},
        {"ABRT", SIGABRT},
        {"FPE", SIGFPE},
        {"KILL", SIGKILL},
        {"SEGV", SIGSEGV},
        {"PIPE", SIGPIPE},
        {"ALRM", SIGALRM},
        {"TERM", SIGTERM},
#ifdef SIGUSR1
        {"USR1", SIGUSR1},
#endif
#ifdef SIGUSR2
        {"USR2", SIGUSR2},
#endif
#ifdef SIGCHLD
        {"CHLD", SIGCHLD},
#endif
#ifdef SIGCONT
        {"CONT", SIGCONT},
#endif
#ifdef SIGSTOP
        {"STOP", SIGSTOP},
#endif
#ifdef SIGTSTP
        {"TSTP", SIGTSTP},
#endif
#ifdef SIGTTIN
        {"TTIN", SIGTTIN},
#endif
#ifdef SIGTTOU
        {"TTOU", SIGTTOU},
#endif
#ifdef SIGBUS
        {"BUS", SIGBUS},
#endif
#ifdef SIGTRAP
        {"TRAP", SIGTRAP},
#endif
#ifdef SIGSYS
        {"SYS", SIGSYS},
#endif
#ifdef SIGURG
        {"URG", SIGURG},
#endif
#ifdef SIGXCPU
        {"XCPU", SIGXCPU},
#endif
#ifdef SIGXFSZ
        {"XFSZ", SIGXFSZ},
#endif
#ifdef SIGVTALRM
        {"VTALRM", SIGVTALRM},
#endif
#ifdef SIGPROF
        {"PROF", SIGPROF},
#endif
#ifdef SIGWINCH
        {"WINCH", SIGWINCH},
#endif
        {NULL, 0}
    };

    for (int i = 0; map[i].name != NULL; i++) {
        if ((int)strlen(map[i].name) == len && strncmp(map[i].name, s, (size_t)len) == 0) {
            return map[i].value;
        }
    }
    return -1;
}

static int signal_from_value(VM* vm, Value v, int* out) {
    if (IS_NUMBER(v)) {
        *out = (int)AS_NUMBER(v);
        return 1;
    }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        int sig = signal_from_name(s->chars, s->length);
        if (sig >= 0) {
            *out = sig;
            return 1;
        }
    }
    vm_runtime_error(vm, "signal expects a number or known signal name.");
    return 0;
}

// signal.raise(sig) -> bool
static int signal_raise_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    int sig = 0;
    if (!signal_from_value(vm, args[0], &sig)) return 0;
    RETURN_BOOL(raise(sig) == 0);
}

// signal.ignore(sig) -> bool
static int signal_ignore_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    int sig = 0;
    if (!signal_from_value(vm, args[0], &sig)) return 0;
    RETURN_BOOL(signal(sig, SIG_IGN) != SIG_ERR);
}

// signal.default(sig) -> bool
static int signal_default_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    int sig = 0;
    if (!signal_from_value(vm, args[0], &sig)) return 0;
    RETURN_BOOL(signal(sig, SIG_DFL) != SIG_ERR);
}

void register_signal(VM* vm) {
    const NativeReg funcs[] = {
        {"raise", signal_raise_native},
        {"ignore", signal_ignore_native},
        {"default", signal_default_native},
        {NULL, NULL}
    };
    register_module(vm, "signal", funcs);
    pop(vm);
}

