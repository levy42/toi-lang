#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int event_flag_from_name(const char* s, int len, short* out) {
    if (len == 2 && strncmp(s, "in", 2) == 0) { *out = POLLIN; return 1; }
    if (len == 3 && strncmp(s, "out", 3) == 0) { *out = POLLOUT; return 1; }
    if (len == 3 && strncmp(s, "pri", 3) == 0) { *out = POLLPRI; return 1; }
    if (len == 3 && strncmp(s, "err", 3) == 0) { *out = POLLERR; return 1; }
    if (len == 3 && strncmp(s, "hup", 3) == 0) { *out = POLLHUP; return 1; }
    if (len == 4 && strncmp(s, "nval", 4) == 0) { *out = POLLNVAL; return 1; }
    return 0;
}

static int parse_events(VM* vm, Value ev, short* out) {
    *out = POLLIN;
    if (IS_NIL(ev)) return 1;

    if (IS_STRING(ev)) {
        short f = 0;
        ObjString* s = AS_STRING(ev);
        if (!event_flag_from_name(s->chars, s->length, &f)) {
            vm_runtime_error(vm, "Unknown poll event name.");
            return 0;
        }
        *out = f;
        return 1;
    }

    if (!IS_TABLE(ev)) {
        vm_runtime_error(vm, "events must be string or table.");
        return 0;
    }

    ObjTable* t = AS_TABLE(ev);
    short flags = 0;
    int any = 0;
    for (int i = 1;; i++) {
        Value v = NIL_VAL;
        if (!table_get_array(&t->table, i, &v) || IS_NIL(v)) break;
        if (!IS_STRING(v)) {
            vm_runtime_error(vm, "events entries must be strings.");
            return 0;
        }
        short f = 0;
        ObjString* s = AS_STRING(v);
        if (!event_flag_from_name(s->chars, s->length, &f)) {
            vm_runtime_error(vm, "Unknown poll event name.");
            return 0;
        }
        flags |= f;
        any = 1;
    }
    *out = any ? flags : POLLIN;
    return 1;
}

// poll.wait(fds, timeout_ms) -> ready_table
// fds: array of numbers or {fd=number, events="in"|{"in","out"...}}
static int poll_wait_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_TABLE(0);
    int timeout_ms = -1;
    if (arg_count >= 2) {
        ASSERT_NUMBER(1);
        timeout_ms = (int)GET_NUMBER(1);
    }

    ObjTable* in = GET_TABLE(0);

    int count = 0;
    for (int i = 1;; i++) {
        Value v = NIL_VAL;
        if (!table_get_array(&in->table, i, &v) || IS_NIL(v)) break;
        count++;
    }

    if (count == 0) {
        ObjTable* empty = new_table();
        RETURN_OBJ(empty);
    }

    struct pollfd* pfds = (struct pollfd*)malloc(sizeof(struct pollfd) * (size_t)count);
    int* indices = (int*)malloc(sizeof(int) * (size_t)count);
    if (pfds == NULL || indices == NULL) {
        free(pfds);
        free(indices);
        vm_runtime_error(vm, "poll.wait out of memory.");
        return 0;
    }

    for (int i = 0; i < count; i++) {
        Value v = NIL_VAL;
        table_get_array(&in->table, i + 1, &v);
        int fd = -1;
        short events = POLLIN;

        if (IS_NUMBER(v)) {
            fd = (int)AS_NUMBER(v);
        } else if (IS_TABLE(v)) {
            ObjTable* row = AS_TABLE(v);
            Value fdv = NIL_VAL;
            if (!table_get(&row->table, copy_string("fd", 2), &fdv) || !IS_NUMBER(fdv)) {
                free(pfds);
                free(indices);
                vm_runtime_error(vm, "poll item table requires numeric 'fd'.");
                return 0;
            }
            fd = (int)AS_NUMBER(fdv);

            Value evv = NIL_VAL;
            if (table_get(&row->table, copy_string("events", 6), &evv) && !IS_NIL(evv)) {
                if (!parse_events(vm, evv, &events)) {
                    free(pfds);
                    free(indices);
                    return 0;
                }
            }
        } else {
            free(pfds);
            free(indices);
            vm_runtime_error(vm, "poll fds entries must be number or table.");
            return 0;
        }

        pfds[i].fd = fd;
        pfds[i].events = events;
        pfds[i].revents = 0;
        indices[i] = i + 1;
    }

    int rc = poll(pfds, (nfds_t)count, timeout_ms);
    if (rc < 0) {
        int err = errno;
        free(pfds);
        free(indices);
        vm_runtime_error(vm, "poll.wait failed: %s", strerror(err));
        return 0;
    }

    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));
    int out_i = 1;
    for (int i = 0; i < count; i++) {
        if (pfds[i].revents == 0) continue;
        ObjTable* row = new_table();
        push(vm, OBJ_VAL(row));
        table_set(&row->table, copy_string("index", 5), NUMBER_VAL((double)indices[i]));
        table_set(&row->table, copy_string("fd", 2), NUMBER_VAL((double)pfds[i].fd));

        table_set(&row->table, copy_string("in", 2), BOOL_VAL((pfds[i].revents & POLLIN) != 0));
        table_set(&row->table, copy_string("out", 3), BOOL_VAL((pfds[i].revents & POLLOUT) != 0));
        table_set(&row->table, copy_string("pri", 3), BOOL_VAL((pfds[i].revents & POLLPRI) != 0));
        table_set(&row->table, copy_string("err", 3), BOOL_VAL((pfds[i].revents & POLLERR) != 0));
        table_set(&row->table, copy_string("hup", 3), BOOL_VAL((pfds[i].revents & POLLHUP) != 0));
        table_set(&row->table, copy_string("nval", 4), BOOL_VAL((pfds[i].revents & POLLNVAL) != 0));
        table_set(&row->table, copy_string("revents", 7), NUMBER_VAL((double)pfds[i].revents));

        table_set_array(&out->table, out_i++, OBJ_VAL(row));
        pop(vm);
    }

    free(pfds);
    free(indices);
    RETURN_OBJ(out);
}

void register_poll(VM* vm) {
    const NativeReg funcs[] = {
        {"wait", poll_wait_native},
        {NULL, NULL}
    };
    register_module(vm, "poll", funcs);
    pop(vm);
}
