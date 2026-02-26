#ifndef COMPILER_INTERNAL_H
#define COMPILER_INTERNAL_H

#include "../common.h"
#include "../compiler.h"
#include "../lexer.h"

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,
    PREC_TERNARY,
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_RANGE,
    PREC_TERM,
    PREC_FACTOR,
    PREC_UNARY,
    PREC_CALL,
    PREC_PRIMARY
} Precedence;

typedef struct {
    Token name;
    int depth;
    int is_captured;
    uint8_t type;
} Local;

typedef struct {
    uint8_t index;
    int is_local;
} Upvalue;

typedef enum {
    TYPE_FUNCTION,
    TYPE_SCRIPT
} FunctionType;

typedef struct LoopContext {
    int start;
    int scope_depth;
    int break_jumps[256];
    int break_count;
    int continue_jumps[256];
    int continue_count;
    int is_for_loop;
    int slots_to_pop;
    struct LoopContext* enclosing;
} LoopContext;

typedef struct Compiler {
    struct Compiler* enclosing;
    Local locals[UINT8_MAX + 1];
    int local_count;
    Token explicit_globals[UINT8_MAX + 1];
    int explicit_global_count;
    Upvalue upvalues[UINT8_MAX + 1];
    int upvalue_count;
    int scope_depth;
    ObjFunction* function;
    FunctionType type;
    LoopContext* loop_context;
} Compiler;

typedef struct {
    Token current;
    Token previous;
    int had_error;
    int panic_mode;
} Parser;

typedef struct {
    int flags_offset;
    int except_offset;
    int finally_offset;
} TryPatch;

extern Parser parser;
extern Compiler* current;
extern Lexer lexer;
extern int type_stack_top;
extern int is_repl_mode;
extern int last_expr_ends_with_call;
extern int last_expr_was_range;
extern int in_for_range_header;
extern int in_table_entry_expression;

void type_push(uint8_t type);
Chunk* current_chunk(void);
void expression(void);
void advance(void);
void consume(TokenType type, const char* message);
int match(TokenType type);
int check(TokenType type);
void emit_byte(uint8_t byte);
void emit_constant(Value value);
void emit_bytes(uint8_t byte1, uint8_t byte2);
void emit_call(uint8_t arg_count);
int emit_jump(uint8_t instruction);
TryPatch emit_try(uint8_t depth);
void patch_jump(int offset);
void patch_try(int offset);
void patch_try_finally(int offset);
void emit_loop(int loop_start);
uint8_t make_constant(Value value);
void begin_scope(void);
void end_scope(void);
int resolve_local(Compiler* compiler, Token* name);
int resolve_upvalue(Compiler* compiler, Token* name);
int is_explicit_global_name(Compiler* compiler, Token* name);
void register_explicit_global(Token name);
void add_local(Token name);
void mark_initialized(void);
void mark_initialized_count(int count);
void declare_variable(void);
uint8_t identifier_constant(Token* name);
void named_variable(Token name, int can_assign);
void consume_property_name_after_dot(void);
void variable_declaration(void);
void function_declaration(void);
void global_function_declaration(void);
void global_declaration(void);
void decorated_function_declaration(void);
void error(const char* message);
void error_at_current(const char* message);
void emit_return(void);
void set_local_type(int local_index, uint8_t type);
void update_local_type(int local_index, uint8_t rhs_type);
int emit_simple_fstring_expr(const char* expr_start, int expr_len);

#endif
