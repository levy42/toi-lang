#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int mkdir_one(const char* path, char** err_out) {
    if (mkdir(path, 0755) == 0) {
        return 1;
    }
    if (errno != EEXIST) {
        *err_out = strerror(errno);
        return 0;
    }

    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 1;
    }

    *err_out = strerror(errno);
    return 0;
}

static int mkdir_all(const char* path, char** err_out) {
    if (path == NULL || path[0] == '\0') {
        *err_out = "invalid path";
        return 0;
    }

    size_t len = strlen(path);
    char* buf = (char*)malloc(len + 1);
    if (buf == NULL) {
        *err_out = "out of memory";
        return 0;
    }
    memcpy(buf, path, len + 1);

    while (len > 1 && buf[len - 1] == '/') {
        buf[len - 1] = '\0';
        len--;
    }

    for (char* p = buf + 1; *p != '\0'; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (buf[0] != '\0' && !mkdir_one(buf, err_out)) {
            free(buf);
            return 0;
        }
        *p = '/';
    }

    if (!mkdir_one(buf, err_out)) {
        free(buf);
        return 0;
    }

    free(buf);
    return 1;
}

// os.exit(code)
static int os_exit(VM* vm, int arg_count, Value* args) {
    int code = 0;
    if (arg_count >= 1) {
        ASSERT_NUMBER(0);
        code = (int)GET_NUMBER(0);
    }
    exit(code);
    return 0; // Unreachable
}

// os.getenv(name, fallback?)
static int os_getenv(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    if (arg_count > 2) {
        vm_runtime_error(vm, "Expected at most 2 arguments but got %d.", arg_count);
        return 0;
    }
    ASSERT_STRING(0);
    
    const char* name = GET_CSTRING(0);
    char* value = getenv(name);
    
    if (value != NULL) {
        RETURN_STRING(value, (int)strlen(value));
    }

    if (arg_count == 2) {
        RETURN_VAL(args[1]);
    }

    RETURN_NIL;
}

// os.system(command)
static int os_system(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);
    
    const char* cmd = GET_CSTRING(0);
    int status = system(cmd);
    
    RETURN_NUMBER((double)status);
}

// os.remove(path)
static int os_remove(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);
    
    const char* path = GET_CSTRING(0);
    if (remove(path) == 0) {
        RETURN_TRUE;
    } else {
        push(vm, NIL_VAL);
        RETURN_STRING("remove failed", 13);
        return 2;
    }
}

// os.rename(old, new)
static int os_rename(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_STRING(0);
    ASSERT_STRING(1);
    
    const char* old_path = GET_CSTRING(0);
    const char* new_path = GET_CSTRING(1);
    
    if (rename(old_path, new_path) == 0) {
        RETURN_TRUE;
    } else {
        push(vm, NIL_VAL);
        RETURN_STRING("rename failed", 13);
        return 2;
    }
}

// os.clock() -> Same as time.clock, but standard in os lib in some langs
static int os_clock(VM* vm, int arg_count, Value* args) {
    (void)arg_count; (void)args;
    RETURN_NUMBER((double)clock() / CLOCKS_PER_SEC);
}

// os.mkdir(path, all?) -> true or nil, error
static int os_mkdir(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    if (arg_count > 2) {
        vm_runtime_error(vm, "Expected at most 2 arguments but got %d.", arg_count);
        return 0;
    }
    ASSERT_STRING(0);

    const char* path = GET_CSTRING(0);
    int all = 0;
    if (arg_count == 2) {
        if (!IS_BOOL(args[1])) {
            vm_runtime_error(vm, "Argument 2 must be a bool.");
            return 0;
        }
        all = AS_BOOL(args[1]) ? 1 : 0;
    }

    char* err = NULL;
    int ok = all ? mkdir_all(path, &err) : mkdir_one(path, &err);
    if (ok) {
        RETURN_TRUE;
    }

    push(vm, NIL_VAL);
    if (err == NULL) err = "mkdir failed";
    push(vm, OBJ_VAL(copy_string(err, (int)strlen(err))));
    return 2;
}

// os.rmdir(path) -> true or nil, error
static int os_rmdir(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    const char* path = GET_CSTRING(0);
    if (rmdir(path) == 0) {
        RETURN_TRUE;
    } else {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }
}

// os.listdir(path) -> table of filenames
static int os_listdir(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    const char* path = GET_CSTRING(0);
    DIR* dir = opendir(path);
    if (!dir) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }

    ObjTable* result = new_table();
    push(vm, OBJ_VAL(result)); // GC protection

    struct dirent* entry;
    int index = 1;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        ObjString* name = copy_string(entry->d_name, strlen(entry->d_name));
        table_set_array(&result->table, index++, OBJ_VAL(name));
    }
    closedir(dir);

    // Result is already on stack
    return 1;
}

// os.isfile(path) -> boolean
static int os_isfile(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    const char* path = GET_CSTRING(0);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        RETURN_TRUE;
    }
    RETURN_FALSE;
}

// os.isdir(path) -> boolean
static int os_isdir(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    const char* path = GET_CSTRING(0);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        RETURN_TRUE;
    }
    RETURN_FALSE;
}

// os.exists(path) -> boolean
static int os_exists(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    const char* path = GET_CSTRING(0);
    struct stat st;
    if (stat(path, &st) == 0) {
        RETURN_TRUE;
    }
    RETURN_FALSE;
}

// os.getcwd() -> string
static int os_getcwd(VM* vm, int arg_count, Value* args) {
    (void)arg_count; (void)args;

    char buf[4096];
    if (getcwd(buf, sizeof(buf)) != NULL) {
        RETURN_STRING(buf, strlen(buf));
    }
    RETURN_NIL;
}

// os.chdir(path) -> true or nil, error
static int os_chdir(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    const char* path = GET_CSTRING(0);
    if (chdir(path) == 0) {
        RETURN_TRUE;
    } else {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }
}

// os.rss() -> resident set size in bytes (Linux), nil on unsupported platforms
static int os_rss(VM* vm, int arg_count, Value* args) {
    (void)vm; (void)arg_count; (void)args;
#if defined(__linux__)
    FILE* fp = fopen("/proc/self/statm", "r");
    if (!fp) {
        RETURN_NIL;
    }

    unsigned long total_pages = 0;
    unsigned long rss_pages = 0;
    if (fscanf(fp, "%lu %lu", &total_pages, &rss_pages) != 2) {
        fclose(fp);
        RETURN_NIL;
    }
    fclose(fp);

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        RETURN_NIL;
    }

    RETURN_NUMBER((double)rss_pages * (double)page_size);
#else
    RETURN_NIL;
#endif
}

// os.trim() -> ask allocator to return free memory to OS when possible
static int os_trim(VM* vm, int arg_count, Value* args) {
    (void)vm; (void)arg_count; (void)args;
#if defined(__GLIBC__)
    int ok = malloc_trim(0);
    RETURN_BOOL(ok != 0);
#else
    RETURN_NIL;
#endif
}

void register_os(VM* vm) {
    const NativeReg os_funcs[] = {
        {"exit", os_exit},
        {"getenv", os_getenv},
        {"system", os_system},
        {"remove", os_remove},
        {"rename", os_rename},
        {"clock", os_clock},
        {"mkdir", os_mkdir},
        {"rmdir", os_rmdir},
        {"listdir", os_listdir},
        {"isfile", os_isfile},
        {"isdir", os_isdir},
        {"exists", os_exists},
        {"getcwd", os_getcwd},
        {"chdir", os_chdir},
        {"rss", os_rss},
        {"trim", os_trim},
        {NULL, NULL}
    };

    register_module(vm, "os", os_funcs);
    // register_module leaves module table on stack.
    ObjTable* os_module = AS_TABLE(peek(vm, 0));

    ObjString* argv_key = copy_string("argv", 4);
    ObjString* argc_key = copy_string("argc", 4);
    ObjTable* argv = new_table();
    for (int i = 0; i < vm->cli_argc; i++) {
        const char* s = vm->cli_argv[i];
        ObjString* arg = copy_string(s, (int)strlen(s));
        table_set_array(&argv->table, i + 1, OBJ_VAL(arg));
    }
    table_set(&os_module->table, argv_key, OBJ_VAL(argv));
    table_set(&os_module->table, argc_key, NUMBER_VAL((double)vm->cli_argc));
    pop(vm); // Pop os module
}
