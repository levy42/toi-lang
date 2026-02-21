#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int push_error_pair(VM* vm) {
    push(vm, NIL_VAL);
    push(vm, OBJ_VAL(copy_string(strerror(errno), (int)strlen(strerror(errno)))));
    return 2;
}

static int push_stat_table(VM* vm, const struct stat* st) {
    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));

    table_set(&out->table, copy_string("size", 4), NUMBER_VAL((double)st->st_size));
    table_set(&out->table, copy_string("mode", 4), NUMBER_VAL((double)st->st_mode));
    table_set(&out->table, copy_string("mtime", 5), NUMBER_VAL((double)st->st_mtime));
    table_set(&out->table, copy_string("atime", 5), NUMBER_VAL((double)st->st_atime));
    table_set(&out->table, copy_string("ctime", 5), NUMBER_VAL((double)st->st_ctime));
    table_set(&out->table, copy_string("uid", 3), NUMBER_VAL((double)st->st_uid));
    table_set(&out->table, copy_string("gid", 3), NUMBER_VAL((double)st->st_gid));
    table_set(&out->table, copy_string("nlink", 5), NUMBER_VAL((double)st->st_nlink));
    table_set(&out->table, copy_string("ino", 3), NUMBER_VAL((double)st->st_ino));
    table_set(&out->table, copy_string("dev", 3), NUMBER_VAL((double)st->st_dev));

    table_set(&out->table, copy_string("is_file", 7), BOOL_VAL(S_ISREG(st->st_mode)));
    table_set(&out->table, copy_string("is_dir", 6), BOOL_VAL(S_ISDIR(st->st_mode)));
#ifdef S_ISLNK
    table_set(&out->table, copy_string("is_link", 7), BOOL_VAL(S_ISLNK(st->st_mode)));
#else
    table_set(&out->table, copy_string("is_link", 7), BOOL_VAL(0));
#endif

    return 1;
}

// stat.stat(path) -> table | nil, err
static int stat_stat(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    const char* path = GET_CSTRING(0);
    struct stat st;
    if (stat(path, &st) != 0) {
        return push_error_pair(vm);
    }
    return push_stat_table(vm, &st);
}

// stat.lstat(path) -> table | nil, err
static int stat_lstat(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    const char* path = GET_CSTRING(0);
    struct stat st;
    if (lstat(path, &st) != 0) {
        return push_error_pair(vm);
    }
    return push_stat_table(vm, &st);
}

// stat.chmod(path, mode) -> true | nil, err
static int stat_chmod(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_STRING(0);
    ASSERT_NUMBER(1);

    const char* path = GET_CSTRING(0);
    mode_t mode = (mode_t)((int)GET_NUMBER(1));
    if (chmod(path, mode) != 0) {
        return push_error_pair(vm);
    }
    RETURN_TRUE;
}

// stat.umask(mask?) -> old_mask
static int stat_umask(VM* vm, int arg_count, Value* args) {
    mode_t old;
    if (arg_count == 0) {
        old = umask(0);
        umask(old);
        RETURN_NUMBER((double)old);
    }
    ASSERT_ARGC_EQ(1);
    ASSERT_NUMBER(0);
    mode_t new_mask = (mode_t)((int)GET_NUMBER(0));
    old = umask(new_mask);
    RETURN_NUMBER((double)old);
}

void register_stat(VM* vm) {
    const NativeReg funcs[] = {
        {"stat", stat_stat},
        {"lstat", stat_lstat},
        {"chmod", stat_chmod},
        {"umask", stat_umask},
        {NULL, NULL}
    };
    register_module(vm, "stat", funcs);
    pop(vm);
}

