#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

typedef struct {
    char* data;
    int len;
    int cap;
} CsvBuffer;

typedef enum {
    CSV_FIELD_START = 0,
    CSV_IN_UNQUOTED = 1,
    CSV_IN_QUOTED = 2,
    CSV_AFTER_QUOTE = 3
} CsvParseState;

static int csv_buf_reserve(CsvBuffer* b, int extra) {
    if (extra < 0) return 0;
    int needed = b->len + extra + 1;
    if (needed <= b->cap) return 1;

    int new_cap = b->cap == 0 ? 64 : b->cap;
    while (new_cap < needed) new_cap *= 2;

    char* grown = (char*)realloc(b->data, (size_t)new_cap);
    if (grown == NULL) return 0;
    b->data = grown;
    b->cap = new_cap;
    return 1;
}

static int csv_buf_append_char(CsvBuffer* b, char c) {
    if (!csv_buf_reserve(b, 1)) return 0;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
    return 1;
}

static int csv_buf_append(CsvBuffer* b, const char* s, int len) {
    if (len < 0) len = (int)strlen(s);
    if (!csv_buf_reserve(b, len)) return 0;
    if (len > 0) {
        memcpy(b->data + b->len, s, (size_t)len);
    }
    b->len += len;
    b->data[b->len] = '\0';
    return 1;
}

static void csv_buf_reset(CsvBuffer* b) {
    b->len = 0;
    if (b->data != NULL) b->data[0] = '\0';
}

static int csv_validate_delimiter(VM* vm, int arg_count, Value* args, int index, char* out_delim) {
    char delimiter = ',';
    if (arg_count > index) {
        ASSERT_STRING(index);
        ObjString* delim = GET_STRING(index);
        if (delim->length != 1) {
            vm_runtime_error(vm, "csv delimiter must be a single-character string.");
            return 0;
        }
        delimiter = delim->chars[0];
    }
    *out_delim = delimiter;
    return 1;
}

static int csv_ensure_row(VM* vm, ObjTable** row, int* row_pushed) {
    if (*row != NULL) return 1;
    *row = new_table();
    if (*row == NULL) {
        vm_runtime_error(vm, "csv.parse: out of memory.");
        return 0;
    }
    push(vm, OBJ_VAL(*row));
    *row_pushed = 1;
    return 1;
}

static int csv_emit_field(VM* vm, ObjTable* row, int* field_index, CsvBuffer* field) {
    if (row == NULL) {
        vm_runtime_error(vm, "csv.parse: internal state error.");
        return 0;
    }
    ObjString* s = copy_string(field->data != NULL ? field->data : "", field->len);
    if (!table_set_array(&row->table, *field_index, OBJ_VAL(s))) {
        vm_runtime_error(vm, "csv.parse: out of memory.");
        return 0;
    }
    (*field_index)++;
    csv_buf_reset(field);
    return 1;
}

static int csv_emit_row(VM* vm, ObjTable* rows, ObjTable* row, int* row_index) {
    if (row == NULL) return 1;
    if (!table_set_array(&rows->table, *row_index, OBJ_VAL(row))) {
        vm_runtime_error(vm, "csv.parse: out of memory.");
        return 0;
    }
    (*row_index)++;
    return 1;
}

static int csv_parse(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    char delimiter = ',';
    if (!csv_validate_delimiter(vm, arg_count, args, 1, &delimiter)) return 0;

    ObjString* input = GET_STRING(0);

    ObjTable* rows = new_table();
    push(vm, OBJ_VAL(rows));

    if (input->length == 0) {
        pop(vm);
        RETURN_OBJ(rows);
    }

    CsvBuffer field = {0};
    ObjTable* row = NULL;
    int row_pushed = 0;
    int row_index = 1;
    int field_index = 1;
    CsvParseState state = CSV_FIELD_START;

    if (!csv_ensure_row(vm, &row, &row_pushed)) {
        free(field.data);
        pop(vm);
        return 0;
    }

    for (int i = 0; i <= input->length; i++) {
        char c = (i < input->length) ? input->chars[i] : '\0';

        if (state == CSV_FIELD_START) {
            if (c == '"') {
                state = CSV_IN_QUOTED;
                continue;
            }
            if (c == delimiter) {
                if (!csv_emit_field(vm, row, &field_index, &field)) goto parse_error;
                continue;
            }
            if (c == '\n' || c == '\r' || c == '\0') {
                if (!csv_emit_field(vm, row, &field_index, &field)) goto parse_error;
                if (!csv_emit_row(vm, rows, row, &row_index)) goto parse_error;

                if (row_pushed) {
                    pop(vm);
                    row_pushed = 0;
                }
                row = NULL;
                field_index = 1;

                if (c == '\r' && i + 1 < input->length && input->chars[i + 1] == '\n') i++;
                if (c == '\0') break;
                if (i + 1 < input->length && !csv_ensure_row(vm, &row, &row_pushed)) goto parse_error;
                continue;
            }
            if (!csv_buf_append_char(&field, c)) goto oom_error;
            state = CSV_IN_UNQUOTED;
            continue;
        }

        if (state == CSV_IN_UNQUOTED) {
            if (c == '"') {
                vm_runtime_error(vm, "csv.parse: unexpected quote in unquoted field.");
                goto parse_error;
            }
            if (c == delimiter) {
                if (!csv_emit_field(vm, row, &field_index, &field)) goto parse_error;
                state = CSV_FIELD_START;
                continue;
            }
            if (c == '\n' || c == '\r' || c == '\0') {
                if (!csv_emit_field(vm, row, &field_index, &field)) goto parse_error;
                if (!csv_emit_row(vm, rows, row, &row_index)) goto parse_error;

                if (row_pushed) {
                    pop(vm);
                    row_pushed = 0;
                }
                row = NULL;
                field_index = 1;
                state = CSV_FIELD_START;

                if (c == '\r' && i + 1 < input->length && input->chars[i + 1] == '\n') i++;
                if (c == '\0') break;
                if (i + 1 < input->length && !csv_ensure_row(vm, &row, &row_pushed)) goto parse_error;
                continue;
            }
            if (!csv_buf_append_char(&field, c)) goto oom_error;
            continue;
        }

        if (state == CSV_IN_QUOTED) {
            if (c == '\0') {
                vm_runtime_error(vm, "csv.parse: unterminated quoted field.");
                goto parse_error;
            }
            if (c == '"') {
                if (i + 1 < input->length && input->chars[i + 1] == '"') {
                    if (!csv_buf_append_char(&field, '"')) goto oom_error;
                    i++;
                } else {
                    state = CSV_AFTER_QUOTE;
                }
                continue;
            }
            if (!csv_buf_append_char(&field, c)) goto oom_error;
            continue;
        }

        if (state == CSV_AFTER_QUOTE) {
            if (c == delimiter) {
                if (!csv_emit_field(vm, row, &field_index, &field)) goto parse_error;
                state = CSV_FIELD_START;
                continue;
            }
            if (c == '\n' || c == '\r' || c == '\0') {
                if (!csv_emit_field(vm, row, &field_index, &field)) goto parse_error;
                if (!csv_emit_row(vm, rows, row, &row_index)) goto parse_error;

                if (row_pushed) {
                    pop(vm);
                    row_pushed = 0;
                }
                row = NULL;
                field_index = 1;
                state = CSV_FIELD_START;

                if (c == '\r' && i + 1 < input->length && input->chars[i + 1] == '\n') i++;
                if (c == '\0') break;
                if (i + 1 < input->length && !csv_ensure_row(vm, &row, &row_pushed)) goto parse_error;
                continue;
            }
            vm_runtime_error(vm, "csv.parse: invalid character after closing quote.");
            goto parse_error;
        }
    }

    free(field.data);
    if (row_pushed) pop(vm);
    pop(vm);
    RETURN_OBJ(rows);

oom_error:
    vm_runtime_error(vm, "csv.parse: out of memory.");
parse_error:
    free(field.data);
    if (row_pushed) pop(vm);
    pop(vm);
    return 0;
}

static int csv_append_escaped_field(CsvBuffer* out, const char* s, int len, char delimiter) {
    int needs_quotes = 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c == delimiter || c == '"' || c == '\n' || c == '\r') {
            needs_quotes = 1;
            break;
        }
    }

    if (!needs_quotes) {
        return csv_buf_append(out, s, len);
    }

    if (!csv_buf_append_char(out, '"')) return 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '"') {
            if (!csv_buf_append(out, "\"\"", 2)) return 0;
        } else {
            if (!csv_buf_append_char(out, s[i])) return 0;
        }
    }
    if (!csv_buf_append_char(out, '"')) return 0;
    return 1;
}

static int csv_stringify_value(VM* vm, CsvBuffer* out, Value v, char delimiter) {
    if (IS_NIL(v)) {
        return 1;
    }

    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        return csv_append_escaped_field(out, s->chars, s->length, delimiter);
    }

    if (IS_NUMBER(v)) {
        char num[64];
        snprintf(num, sizeof(num), "%.17g", AS_NUMBER(v));
        return csv_append_escaped_field(out, num, (int)strlen(num), delimiter);
    }

    if (IS_BOOL(v)) {
        const char* text = AS_BOOL(v) ? "true" : "false";
        return csv_append_escaped_field(out, text, (int)strlen(text), delimiter);
    }

    vm_runtime_error(vm, "csv.stringify: row values must be string, number, bool, or nil.");
    return -1;
}

static int csv_stringify(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_TABLE(0);

    char delimiter = ',';
    if (!csv_validate_delimiter(vm, arg_count, args, 1, &delimiter)) return 0;

    ObjTable* rows = GET_TABLE(0);
    CsvBuffer out = {0};

    int row_idx = 1;
    int first_row = 1;
    int row_max = rows->table.array_max;
    for (; row_idx <= row_max; row_idx++) {
        Value row_val = NIL_VAL;
        if (!table_get_array(&rows->table, row_idx, &row_val) || IS_NIL(row_val)) {
            free(out.data);
            vm_runtime_error(vm, "csv.stringify: row %d must be a table.", row_idx);
            return 0;
        }

        if (!IS_TABLE(row_val)) {
            free(out.data);
            vm_runtime_error(vm, "csv.stringify: row %d must be a table.", row_idx);
            return 0;
        }

        ObjTable* row = AS_TABLE(row_val);
        if (!first_row) {
            if (!csv_buf_append_char(&out, '\n')) {
                free(out.data);
                vm_runtime_error(vm, "csv.stringify: out of memory.");
                return 0;
            }
        }
        first_row = 0;

        int col_idx = 1;
        int col_max = row->table.array_max;
        int first_col = 1;
        for (; col_idx <= col_max; col_idx++) {
            Value cell = NIL_VAL;
            if (!table_get_array(&row->table, col_idx, &cell)) {
                cell = NIL_VAL;
            }

            if (!first_col) {
                if (!csv_buf_append_char(&out, delimiter)) {
                    free(out.data);
                    vm_runtime_error(vm, "csv.stringify: out of memory.");
                    return 0;
                }
            }
            first_col = 0;

            int rc = csv_stringify_value(vm, &out, cell, delimiter);
            if (rc == -1) {
                free(out.data);
                return 0;
            }
            if (rc == 0) {
                free(out.data);
                vm_runtime_error(vm, "csv.stringify: out of memory.");
                return 0;
            }
        }
    }

    ObjString* result = copy_string(out.data != NULL ? out.data : "", out.len);
    free(out.data);
    RETURN_OBJ(result);
}

void register_csv(VM* vm) {
    static const NativeReg csv_funcs[] = {
        {"parse", csv_parse},
        {"stringify", csv_stringify},
        {NULL, NULL}
    };
    register_module(vm, "csv", csv_funcs);
}
