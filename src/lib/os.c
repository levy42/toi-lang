#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

// os.exit(code)
static int os_exit(VM* vm, int argCount, Value* args) {
    int code = 0;
    if (argCount >= 1) {
        ASSERT_NUMBER(0);
        code = (int)GET_NUMBER(0);
    }
    exit(code);
    return 0; // Unreachable
}

// os.getenv(name)
static int os_getenv(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);
    
    const char* name = GET_CSTRING(0);
    char* value = getenv(name);
    
    if (value != NULL) {
        RETURN_STRING(value, (int)strlen(value));
    } else {
        RETURN_NIL;
    }
}

// os.system(command)
static int os_system(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);
    
    const char* cmd = GET_CSTRING(0);
    int status = system(cmd);
    
    RETURN_NUMBER((double)status);
}

// os.remove(path)
static int os_remove(VM* vm, int argCount, Value* args) {
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
static int os_rename(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_STRING(0);
    ASSERT_STRING(1);
    
    const char* oldPath = GET_CSTRING(0);
    const char* newPath = GET_CSTRING(1);
    
    if (rename(oldPath, newPath) == 0) {
        RETURN_TRUE;
    } else {
        push(vm, NIL_VAL);
        RETURN_STRING("rename failed", 13);
        return 2;
    }
}

// os.clock() -> Same as time.clock, but standard in os lib in some langs
static int os_clock(VM* vm, int argCount, Value* args) {
    (void)argCount; (void)args;
    RETURN_NUMBER((double)clock() / CLOCKS_PER_SEC);
}

// os.mkdir(path) -> true or nil, error
static int os_mkdir(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    const char* path = GET_CSTRING(0);
    if (mkdir(path, 0755) == 0) {
        RETURN_TRUE;
    } else {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copyString(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }
}

// os.rmdir(path) -> true or nil, error
static int os_rmdir(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    const char* path = GET_CSTRING(0);
    if (rmdir(path) == 0) {
        RETURN_TRUE;
    } else {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copyString(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }
}

// os.listdir(path) -> table of filenames
static int os_listdir(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    const char* path = GET_CSTRING(0);
    DIR* dir = opendir(path);
    if (!dir) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copyString(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }

    ObjTable* result = newTable();
    push(vm, OBJ_VAL(result)); // GC protection

    struct dirent* entry;
    int index = 1;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        ObjString* name = copyString(entry->d_name, strlen(entry->d_name));
        tableSetArray(&result->table, index++, OBJ_VAL(name));
    }
    closedir(dir);

    // Result is already on stack
    return 1;
}

// os.isfile(path) -> boolean
static int os_isfile(VM* vm, int argCount, Value* args) {
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
static int os_isdir(VM* vm, int argCount, Value* args) {
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
static int os_exists(VM* vm, int argCount, Value* args) {
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
static int os_getcwd(VM* vm, int argCount, Value* args) {
    (void)argCount; (void)args;

    char buf[4096];
    if (getcwd(buf, sizeof(buf)) != NULL) {
        RETURN_STRING(buf, strlen(buf));
    }
    RETURN_NIL;
}

// os.chdir(path) -> true or nil, error
static int os_chdir(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    const char* path = GET_CSTRING(0);
    if (chdir(path) == 0) {
        RETURN_TRUE;
    } else {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copyString(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }
}

void registerOS(VM* vm) {
    const NativeReg osFuncs[] = {
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
        {NULL, NULL}
    };

    registerModule(vm, "os", osFuncs);
    pop(vm); // Pop os module
}
