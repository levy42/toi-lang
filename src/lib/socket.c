#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/time.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

// Socket userdata structure
typedef struct {
    int fd;
    int timeout_ms; // -1 = blocking, 0 = non-blocking, >0 = timeout in ms
} SocketData;

static void socket_userdata_finalizer(void* ptr) {
    SocketData* sock = (SocketData*)ptr;
    if (sock == NULL) return;
    if (sock->fd >= 0) {
        close(sock->fd);
        sock->fd = -1;
    }
    free(sock);
}

static SocketData* get_socket_data(ObjUserdata* udata) {
    if (udata == NULL) return NULL;
    return (SocketData*)udata->data;
}

// Helper to set socket metatable
static void set_socket_metatable(VM* vm, ObjUserdata* udata) {
    Value socket_val;
    ObjString* socket_name = copy_string("socket", 6);
    if (table_get(&vm->globals, socket_name, &socket_val) && IS_TABLE(socket_val)) {
        Value mt;
        ObjString* mt_name = copy_string("_socket_mt", 10);
        if (table_get(&AS_TABLE(socket_val)->table, mt_name, &mt) && IS_TABLE(mt)) {
            udata->metatable = AS_TABLE(mt);
        }
    }
}

// socket.tcp() - create a TCP socket
static int socket_tcp(VM* vm, int arg_count, Value* args) {
    (void)arg_count; (void)args;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }

    SocketData* data = (SocketData*)malloc(sizeof(SocketData));
    if (data == NULL) {
        close(fd);
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("out of memory", 13)));
        return 2;
    }
    data->fd = fd;
    data->timeout_ms = -1; // blocking by default

    ObjUserdata* udata = new_userdata_with_finalizer(data, socket_userdata_finalizer);
    set_socket_metatable(vm, udata);
    RETURN_OBJ(udata);
}

// socket.udp() - create a UDP socket
static int socket_udp(VM* vm, int arg_count, Value* args) {
    (void)arg_count; (void)args;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }

    SocketData* data = (SocketData*)malloc(sizeof(SocketData));
    if (data == NULL) {
        close(fd);
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("out of memory", 13)));
        return 2;
    }
    data->fd = fd;
    data->timeout_ms = -1;

    ObjUserdata* udata = new_userdata_with_finalizer(data, socket_userdata_finalizer);
    set_socket_metatable(vm, udata);
    RETURN_OBJ(udata);
}

// sock:connect(host, port)
static int sock_connect(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(3);
    ASSERT_USERDATA(0);
    ASSERT_STRING(1);
    ASSERT_NUMBER(2);

    SocketData* sock = get_socket_data(GET_USERDATA(0));
    if (sock == NULL || sock->fd < 0) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("socket closed", 13)));
        return 2;
    }

    const char* host = GET_CSTRING(1);
    int port = (int)GET_NUMBER(2);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // Try to parse as IP address first
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        // Not an IP, try DNS lookup
        struct hostent* he = gethostbyname(host);
        if (he == NULL) {
            push(vm, NIL_VAL);
            push(vm, OBJ_VAL(copy_string("host not found", 14)));
            return 2;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    if (connect(sock->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }

    RETURN_TRUE;
}

// sock:bind(host, port)
static int sock_bind(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(3);
    ASSERT_USERDATA(0);
    ASSERT_STRING(1);
    ASSERT_NUMBER(2);

    SocketData* sock = get_socket_data(GET_USERDATA(0));
    if (sock == NULL || sock->fd < 0) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("socket closed", 13)));
        return 2;
    }

    const char* host = GET_CSTRING(1);
    int port = (int)GET_NUMBER(2);

    // Enable SO_REUSEADDR
    int opt = 1;
    setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (strcmp(host, "*") == 0 || strcmp(host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
            push(vm, NIL_VAL);
            push(vm, OBJ_VAL(copy_string("invalid address", 15)));
            return 2;
        }
    }

    if (bind(sock->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }

    RETURN_TRUE;
}

// sock:listen(backlog?)
static int sock_listen(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    SocketData* sock = get_socket_data(GET_USERDATA(0));
    if (sock == NULL || sock->fd < 0) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("socket closed", 13)));
        return 2;
    }

    int backlog = 5;
    if (arg_count >= 2 && IS_NUMBER(args[1])) {
        backlog = (int)AS_NUMBER(args[1]);
    }

    if (listen(sock->fd, backlog) < 0) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }

    RETURN_TRUE;
}

// sock:accept()
static int sock_accept(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    SocketData* sock = get_socket_data(GET_USERDATA(0));
    if (sock == NULL || sock->fd < 0) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("socket closed", 13)));
        return 2;
    }

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(sock->fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            push(vm, NIL_VAL);
            push(vm, OBJ_VAL(copy_string("timeout", 7)));
            return 2;
        }
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }

    SocketData* client_data = (SocketData*)malloc(sizeof(SocketData));
    if (client_data == NULL) {
        close(client_fd);
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("out of memory", 13)));
        return 2;
    }
    client_data->fd = client_fd;
    client_data->timeout_ms = -1;

    ObjUserdata* udata = new_userdata_with_finalizer(client_data, socket_userdata_finalizer);
    set_socket_metatable(vm, udata);

    // Return socket and client IP
    push(vm, OBJ_VAL(udata));
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
    push(vm, OBJ_VAL(copy_string(ip_str, strlen(ip_str))));
    return 2;
}

// sock:send(data)
static int sock_send(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_USERDATA(0);
    ASSERT_STRING(1);

    SocketData* sock = get_socket_data(GET_USERDATA(0));
    if (sock == NULL || sock->fd < 0) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("socket closed", 13)));
        return 2;
    }

    ObjString* data = GET_STRING(1);
    ssize_t sent = send(sock->fd, data->chars, data->length, 0);

    if (sent < 0) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }

    RETURN_NUMBER((double)sent);
}

// sock:recv(size?)
static int sock_recv(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    SocketData* sock = get_socket_data(GET_USERDATA(0));
    if (sock == NULL || sock->fd < 0) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("socket closed", 13)));
        return 2;
    }

    int size = 4096;
    if (arg_count >= 2 && IS_NUMBER(args[1])) {
        size = (int)AS_NUMBER(args[1]);
        if (size <= 0) size = 4096;
    }

    char* buffer = (char*)malloc(size + 1);
    ssize_t received = recv(sock->fd, buffer, size, 0);

    if (received < 0) {
        free(buffer);
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            push(vm, NIL_VAL);
            push(vm, OBJ_VAL(copy_string("timeout", 7)));
            return 2;
        }
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(errno), strlen(strerror(errno)))));
        return 2;
    }

    if (received == 0) {
        free(buffer);
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("closed", 6)));
        return 2;
    }

    buffer[received] = '\0';
    push(vm, OBJ_VAL(copy_string(buffer, (int)received)));
    free(buffer);
    return 1;
}

// sock:settimeout(seconds)
static int sock_settimeout(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    SocketData* sock = get_socket_data(GET_USERDATA(0));
    if (sock == NULL || sock->fd < 0) {
        RETURN_NIL;
    }

    double timeout = -1;
    if (arg_count >= 2) {
        if (IS_NIL(args[1])) {
            timeout = -1; // blocking
        } else if (IS_NUMBER(args[1])) {
            timeout = AS_NUMBER(args[1]);
        }
    }

    if (timeout < 0) {
        // Blocking mode
        sock->timeout_ms = -1;
        int flags = fcntl(sock->fd, F_GETFL, 0);
        fcntl(sock->fd, F_SETFL, flags & ~O_NONBLOCK);
    } else if (timeout == 0) {
        // Non-blocking mode
        sock->timeout_ms = 0;
        int flags = fcntl(sock->fd, F_GETFL, 0);
        fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK);
    } else {
        // Timeout mode using SO_RCVTIMEO/SO_SNDTIMEO
        sock->timeout_ms = (int)(timeout * 1000);
        struct timeval tv;
        tv.tv_sec = (int)timeout;
        tv.tv_usec = (int)((timeout - (int)timeout) * 1000000);
        setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        // Ensure blocking mode
        int flags = fcntl(sock->fd, F_GETFL, 0);
        fcntl(sock->fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    RETURN_TRUE;
}

// sock:close()
static int sock_close(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    SocketData* sock = get_socket_data(udata);
    if (sock != NULL && sock->fd >= 0) {
        close(sock->fd);
        sock->fd = -1;
    }
    if (sock != NULL) {
        free(sock);
        udata->data = NULL;
    }

    RETURN_TRUE;
}

// sock:getpeername()
static int sock_getpeername(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    SocketData* sock = get_socket_data(GET_USERDATA(0));
    if (sock == NULL || sock->fd < 0) {
        RETURN_NIL;
    }

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    if (getpeername(sock->fd, (struct sockaddr*)&addr, &len) < 0) {
        RETURN_NIL;
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));

    // Return IP and port
    push(vm, OBJ_VAL(copy_string(ip_str, strlen(ip_str))));
    push(vm, NUMBER_VAL(ntohs(addr.sin_port)));
    return 2;
}

// sock:getsockname()
static int sock_getsockname(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    SocketData* sock = get_socket_data(GET_USERDATA(0));
    if (sock == NULL || sock->fd < 0) {
        RETURN_NIL;
    }

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    if (getsockname(sock->fd, (struct sockaddr*)&addr, &len) < 0) {
        RETURN_NIL;
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));

    push(vm, OBJ_VAL(copy_string(ip_str, strlen(ip_str))));
    push(vm, NUMBER_VAL(ntohs(addr.sin_port)));
    return 2;
}

// sock:fileno() -> fd
static int sock_fileno(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    SocketData* sock = get_socket_data(GET_USERDATA(0));
    if (sock == NULL || sock->fd < 0) {
        RETURN_NIL;
    }
    RETURN_NUMBER((double)sock->fd);
}

// Helper to add sockets from a table (handles both array and hash parts)
static int add_sockets_from_table(ObjTable* table, fd_set* fds, int* max_fd,
                                   ObjUserdata** socket_array, int* count) {
    // Process array part (numeric keys 1, 2, 3, ...)
    for (int i = 0; i < table->table.array_capacity; i++) {
        Value val = table->table.array[i];
        if (IS_USERDATA(val)) {
            ObjUserdata* udata = AS_USERDATA(val);
            SocketData* sock = (SocketData*)udata->data;
            if (sock == NULL) continue;
            if (sock->fd >= 0 && *count < FD_SETSIZE) {
                FD_SET(sock->fd, fds);
                socket_array[(*count)++] = udata;
                if (sock->fd > *max_fd) *max_fd = sock->fd;
            }
        }
    }

    // Process hash part (string keys)
    for (int i = 0; i < table->table.capacity; i++) {
        Entry* entry = &table->table.entries[i];
        if (entry->key != NULL && IS_USERDATA(entry->value)) {
            ObjUserdata* udata = AS_USERDATA(entry->value);
            SocketData* sock = (SocketData*)udata->data;
            if (sock == NULL) continue;
            if (sock->fd >= 0 && *count < FD_SETSIZE) {
                FD_SET(sock->fd, fds);
                socket_array[(*count)++] = udata;
                if (sock->fd > *max_fd) *max_fd = sock->fd;
            }
        }
    }
    return *count;
}

// socket.select(read_list, write_list, timeout) -> ready_read, ready_write
// read_list/write_list: tables (arrays) of sockets, or nil
// timeout: seconds (number), nil = block forever, 0 = poll
static int socket_select(VM* vm, int arg_count, Value* args) {
    fd_set read_fds, write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);

    int max_fd = -1;

    // Arrays to track which sockets are in which set
    ObjUserdata* read_sockets[FD_SETSIZE];
    ObjUserdata* write_sockets[FD_SETSIZE];
    int read_count = 0;
    int write_count = 0;

    // Process read list
    if (arg_count >= 1 && IS_TABLE(args[0])) {
        add_sockets_from_table(AS_TABLE(args[0]), &read_fds, &max_fd,
                               read_sockets, &read_count);
    }

    // Process write list
    if (arg_count >= 2 && IS_TABLE(args[1])) {
        add_sockets_from_table(AS_TABLE(args[1]), &write_fds, &max_fd,
                               write_sockets, &write_count);
    }

    // Timeout
    struct timeval tv;
    struct timeval* tvp = NULL;
    if (arg_count >= 3 && IS_NUMBER(args[2])) {
        double timeout = AS_NUMBER(args[2]);
        tv.tv_sec = (long)timeout;
        tv.tv_usec = (long)((timeout - tv.tv_sec) * 1000000);
        tvp = &tv;
    }

    // Call select
    int result = select(max_fd + 1, &read_fds, &write_fds, NULL, tvp);

    if (result < 0) {
        push(vm, NIL_VAL);
        push(vm, NIL_VAL);
        return 2;
    }

    // Build ready_read table (use array storage for numeric indexing)
    ObjTable* ready_read = new_table();
    push(vm, OBJ_VAL(ready_read));
    int ready_read_count = 0;
    for (int i = 0; i < read_count; i++) {
        SocketData* sock = (SocketData*)read_sockets[i]->data;
        if (sock == NULL) continue;
        if (FD_ISSET(sock->fd, &read_fds)) {
            ready_read_count++;
            table_set_array(&ready_read->table, ready_read_count, OBJ_VAL(read_sockets[i]));
        }
    }

    // Build ready_write table
    ObjTable* ready_write = new_table();
    push(vm, OBJ_VAL(ready_write));
    int ready_write_count = 0;
    for (int i = 0; i < write_count; i++) {
        SocketData* sock = (SocketData*)write_sockets[i]->data;
        if (sock == NULL) continue;
        if (FD_ISSET(sock->fd, &write_fds)) {
            ready_write_count++;
            table_set_array(&ready_write->table, ready_write_count, OBJ_VAL(write_sockets[i]));
        }
    }

    // Return both tables (ready_write is on top, ready_read below)
    // We need to swap them so ready_read is returned first
    Value write_val = pop(vm);
    // ready_read is now on top
    push(vm, write_val);
    return 2;
}

void register_socket(VM* vm) {
    const NativeReg socket_funcs[] = {
        {"tcp", socket_tcp},
        {"udp", socket_udp},
        {"select", socket_select},
        {NULL, NULL}
    };
    register_module(vm, "socket", socket_funcs);
    ObjTable* socket_module = AS_TABLE(peek(vm, 0));

    // Socket Metatable
    ObjTable* socket_mt = new_table();
    push(vm, OBJ_VAL(socket_mt));

    const NativeReg socket_methods[] = {
        {"connect", sock_connect},
        {"bind", sock_bind},
        {"listen", sock_listen},
        {"accept", sock_accept},
        {"send", sock_send},
        {"recv", sock_recv},
        {"settimeout", sock_settimeout},
        {"close", sock_close},
        {"getpeername", sock_getpeername},
        {"getsockname", sock_getsockname},
        {"fileno", sock_fileno},
        {NULL, NULL}
    };

    for (int i = 0; socket_methods[i].name != NULL; i++) {
        ObjString* name_str = copy_string(socket_methods[i].name, (int)strlen(socket_methods[i].name));
        push(vm, OBJ_VAL(name_str));
        push(vm, OBJ_VAL(new_native(socket_methods[i].function, name_str)));
        table_set(&socket_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    // __index = socket_mt
    push(vm, OBJ_VAL(copy_string("__index", 7)));
    push(vm, OBJ_VAL(socket_mt));
    table_set(&socket_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copy_string("__name", 6)));
    push(vm, OBJ_VAL(copy_string("socket.socket", 13)));
    table_set(&socket_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    // Store metatable in socket._socket_mt
    push(vm, OBJ_VAL(copy_string("_socket_mt", 10)));
    push(vm, OBJ_VAL(socket_mt));
    table_set(&socket_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
    pop(vm); // Pop socket_mt

    pop(vm); // Pop socket_module
}
