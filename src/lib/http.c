#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

// HTTP status codes and reasons
static const char* getStatusReason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

// Parse HTTP request: http.parse(data) -> {method, path, version, headers, body}
static int http_parse(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* data = GET_STRING(0);
    const char* src = data->chars;
    int len = data->length;

    // Find end of request line
    const char* lineEnd = strstr(src, "\r\n");
    if (!lineEnd) {
        RETURN_NIL;
    }

    // Parse request line: METHOD PATH VERSION
    const char* p = src;

    // Method
    const char* methodStart = p;
    while (p < lineEnd && *p != ' ') p++;
    if (p >= lineEnd) { RETURN_NIL; }
    int methodLen = p - methodStart;
    p++; // skip space

    // Path
    const char* pathStart = p;
    while (p < lineEnd && *p != ' ' && *p != '?') p++;
    int pathLen = p - pathStart;

    // Skip query string if present
    const char* queryStart = NULL;
    int queryLen = 0;
    if (*p == '?') {
        p++; // skip '?'
        queryStart = p;
        while (p < lineEnd && *p != ' ') p++;
        queryLen = p - queryStart;
    }

    if (p >= lineEnd) { RETURN_NIL; }
    p++; // skip space

    // Version
    const char* versionStart = p;
    int versionLen = lineEnd - p;

    // Create result table
    ObjTable* result = newTable();
    push(vm, OBJ_VAL(result));

    // Set method
    ObjString* methodKey = copyString("method", 6);
    push(vm, OBJ_VAL(methodKey));
    ObjString* methodVal = copyString(methodStart, methodLen);
    push(vm, OBJ_VAL(methodVal));
    tableSet(&result->table, methodKey, OBJ_VAL(methodVal));
    pop(vm); pop(vm);

    // Set path
    ObjString* pathKey = copyString("path", 4);
    push(vm, OBJ_VAL(pathKey));
    ObjString* pathVal = copyString(pathStart, pathLen);
    push(vm, OBJ_VAL(pathVal));
    tableSet(&result->table, pathKey, OBJ_VAL(pathVal));
    pop(vm); pop(vm);

    // Set query (if present)
    if (queryStart) {
        ObjString* queryKey = copyString("query", 5);
        push(vm, OBJ_VAL(queryKey));
        ObjString* queryVal = copyString(queryStart, queryLen);
        push(vm, OBJ_VAL(queryVal));
        tableSet(&result->table, queryKey, OBJ_VAL(queryVal));
        pop(vm); pop(vm);
    }

    // Set version
    ObjString* versionKey = copyString("version", 7);
    push(vm, OBJ_VAL(versionKey));
    ObjString* versionVal = copyString(versionStart, versionLen);
    push(vm, OBJ_VAL(versionVal));
    tableSet(&result->table, versionKey, OBJ_VAL(versionVal));
    pop(vm); pop(vm);

    // Parse headers
    ObjTable* headers = newTable();
    push(vm, OBJ_VAL(headers));

    p = lineEnd + 2; // Skip \r\n
    while (p < src + len) {
        lineEnd = strstr(p, "\r\n");
        if (!lineEnd) break;

        // Empty line = end of headers
        if (lineEnd == p) {
            p += 2;
            break;
        }

        // Find colon
        const char* colon = strchr(p, ':');
        if (!colon || colon > lineEnd) {
            p = lineEnd + 2;
            continue;
        }

        // Header name (lowercase for consistency)
        int nameLen = colon - p;
        char* nameBuf = malloc(nameLen + 1);
        for (int i = 0; i < nameLen; i++) {
            nameBuf[i] = tolower((unsigned char)p[i]);
        }
        nameBuf[nameLen] = '\0';

        // Header value (skip leading whitespace)
        const char* valStart = colon + 1;
        while (valStart < lineEnd && isspace((unsigned char)*valStart)) valStart++;
        int valLen = lineEnd - valStart;

        ObjString* hdrKey = copyString(nameBuf, nameLen);
        push(vm, OBJ_VAL(hdrKey));
        ObjString* hdrVal = copyString(valStart, valLen);
        push(vm, OBJ_VAL(hdrVal));
        tableSet(&headers->table, hdrKey, OBJ_VAL(hdrVal));
        pop(vm); pop(vm);

        free(nameBuf);
        p = lineEnd + 2;
    }

    ObjString* headersKey = copyString("headers", 7);
    push(vm, OBJ_VAL(headersKey));
    tableSet(&result->table, headersKey, OBJ_VAL(headers));
    pop(vm);
    pop(vm); // headers table

    // Body is everything after headers
    int bodyLen = (src + len) - p;
    if (bodyLen > 0) {
        ObjString* bodyKey = copyString("body", 4);
        push(vm, OBJ_VAL(bodyKey));
        ObjString* bodyVal = copyString(p, bodyLen);
        push(vm, OBJ_VAL(bodyVal));
        tableSet(&result->table, bodyKey, OBJ_VAL(bodyVal));
        pop(vm); pop(vm);
    }

    // Result table is already on stack
    return 1;
}

// Format HTTP response: http.response(status, headers, body) -> string
static int http_response(VM* vm, int argCount, Value* args) {
    if (argCount < 1) {
        RETURN_NIL;
    }
    ASSERT_NUMBER(0);

    int status = (int)GET_NUMBER(0);
    const char* reason = getStatusReason(status);

    // Build response
    char statusLine[64];
    snprintf(statusLine, sizeof(statusLine), "HTTP/1.1 %d %s\r\n", status, reason);

    // Calculate total size
    size_t totalLen = strlen(statusLine);

    // Headers
    ObjTable* headers = NULL;
    if (argCount >= 2 && IS_TABLE(args[1])) {
        headers = GET_TABLE(1);
    }

    // Body
    const char* body = "";
    int bodyLen = 0;
    if (argCount >= 3 && IS_STRING(args[2])) {
        body = AS_CSTRING(args[2]);
        bodyLen = AS_STRING(args[2])->length;
    }

    // Build headers string
    char* headersBuf = malloc(4096);
    headersBuf[0] = '\0';
    int headersLen = 0;

    if (headers) {
        for (int i = 0; i < headers->table.capacity; i++) {
            Entry* entry = &headers->table.entries[i];
            if (entry->key != NULL) {
                const char* key = entry->key->chars;
                if (IS_STRING(entry->value)) {
                    const char* val = AS_CSTRING(entry->value);
                    headersLen += snprintf(headersBuf + headersLen, 4096 - headersLen,
                        "%s: %s\r\n", key, val);
                }
            }
        }
    }

    // Add Content-Length if body present
    char contentLength[64] = "";
    if (bodyLen > 0) {
        snprintf(contentLength, sizeof(contentLength), "Content-Length: %d\r\n", bodyLen);
    }

    totalLen += headersLen + strlen(contentLength) + 2 + bodyLen; // +2 for final \r\n

    char* response = malloc(totalLen + 1);
    char* p = response;

    memcpy(p, statusLine, strlen(statusLine));
    p += strlen(statusLine);

    memcpy(p, headersBuf, headersLen);
    p += headersLen;

    memcpy(p, contentLength, strlen(contentLength));
    p += strlen(contentLength);

    memcpy(p, "\r\n", 2);
    p += 2;

    if (bodyLen > 0) {
        memcpy(p, body, bodyLen);
        p += bodyLen;
    }

    *p = '\0';

    ObjString* result = copyString(response, p - response);

    free(headersBuf);
    free(response);

    RETURN_OBJ(result);
}

// URL decode: http.urldecode(str) -> str
static int http_urldecode(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* input = GET_STRING(0);
    char* output = malloc(input->length + 1);
    char* out = output;

    for (int i = 0; i < input->length; i++) {
        char c = input->chars[i];
        if (c == '%' && i + 2 < input->length) {
            char hex[3] = {input->chars[i+1], input->chars[i+2], '\0'};
            *out++ = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (c == '+') {
            *out++ = ' ';
        } else {
            *out++ = c;
        }
    }
    *out = '\0';

    ObjString* result = copyString(output, out - output);
    free(output);
    RETURN_OBJ(result);
}

// Parse query string: http.parsequery(str) -> table
static int http_parsequery(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* input = GET_STRING(0);
    ObjTable* result = newTable();
    push(vm, OBJ_VAL(result));

    const char* p = input->chars;
    const char* end = p + input->length;

    while (p < end) {
        // Find key
        const char* keyStart = p;
        while (p < end && *p != '=' && *p != '&') p++;
        int keyLen = p - keyStart;

        const char* valStart = p;
        int valLen = 0;

        if (p < end && *p == '=') {
            p++; // skip '='
            valStart = p;
            while (p < end && *p != '&') p++;
            valLen = p - valStart;
        }

        if (p < end && *p == '&') p++;

        if (keyLen > 0) {
            ObjString* key = copyString(keyStart, keyLen);
            push(vm, OBJ_VAL(key));
            ObjString* val = copyString(valStart, valLen);
            push(vm, OBJ_VAL(val));
            tableSet(&result->table, key, OBJ_VAL(val));
            pop(vm); pop(vm);
        }
    }

    return 1;
}

void registerHTTP(VM* vm) {
    const NativeReg httpFuncs[] = {
        {"parse", http_parse},
        {"response", http_response},
        {"urldecode", http_urldecode},
        {"parsequery", http_parsequery},
        {NULL, NULL}
    };

    registerModule(vm, "http", httpFuncs);
    pop(vm);
}
