# Toi Language Manifest

This document describes the syntax accepted by the current compiler/runtime in `src/lexer.c` and `src/compiler.c`.

## 1. Lexical Rules

### 1.1 Whitespace and blocks

- Blocks are indentation-based.
- Indent can use spaces or tabs (`tab == 4 spaces` in the lexer).
- Indentation must be consistent with prior indent levels.
- Newlines are significant for indentation, but otherwise skipped.
- Inside `{ ... }`, indentation tokens are not generated.

### 1.2 Comments

- Line comments start with `--` and continue to end of line.

### 1.3 Identifiers

- Identifier pattern: `[A-Za-z_][A-Za-z0-9_]*`.

### 1.4 Keywords

`and`, `or`, `not`, `nil`, `true`, `false`, `print`, `local`, `global`, `fn`, `return`, `if`, `elif`, `else`, `while`, `for`, `in`, `break`, `continue`, `with`, `as`, `try`, `except`, `finally`, `throw`, `has`, `import`, `from`, `gc`, `del`.

### 1.5 Numbers

- Decimal numbers only.
- Supports `_` separators between digits.
- Optional fractional part.

Examples:

```toi
1
10_000
3.14
1_000.25
```

### 1.6 Strings

- Regular string: `"..."` or `'...'` with escapes.
- Multiline/raw string: `[[ ... ]]`.
- F-string: `f"...{expr}..."` and `f'...{expr}...'`.

Regular/f-string literal-part escapes:

- `\n`, `\t`, `\r`, `\"`, `\\`
- f-strings also support `\{` and `\}` in literal text.

## 2. Program and Blocks

Conceptual grammar:

```ebnf
program      := declaration* EOF
block        := INDENT declaration* DEDENT | single_statement
single_statement := statement   ; same line after header
```

Most headers (`if`, `elif`, `else`, `while`, `for`, `fn`, `try`, `except`, `finally`, `with`) accept either:

- an indented block on following lines, or
- one single statement on the same line.

## 3. Declarations and Statements

### 3.1 Declarations

```ebnf
declaration := function_decl
             | decorated_function_decl
             | import_decl
             | from_import_decl
             | global_decl
             | local_decl
             | statement

function_decl := "fn" IDENT function_body
decorated_function_decl := decorator+ ("fn" IDENT function_body
                                      | "local" "fn" IDENT function_body
                                      | "global" "fn" IDENT function_body)
decorator := "@" expression_on_same_line
local_decl    := "local" ("fn" IDENT function_body | var_decl)
global_decl   := "global" ("fn" IDENT function_body | global_var_decl)
import_decl   := "import" module_path
from_import_decl := "from" module_path "import" ("*" | IDENT ("," IDENT)*)
```

### 3.2 Variable declarations

```ebnf
var_decl        := name_list ("=" expr_list)?
global_var_decl := name_list ("=" expr_list)?
name_list       := IDENT ("," IDENT)*
expr_list       := expression ("," expression)*
```

Notes:

- If declaration has no initializer, values default to `nil`.
- For single-variable declarations/assignments, `a = x, y, z` is parsed as an array literal (`{x, y, z}`), not multi-assignment.
- For multi-target assignment with a single RHS expression (`a, b = expr`), if `expr` evaluates to one table value it is unpacked from array slots (`expr[1]`, `expr[2]`, ...); otherwise extra targets are filled with `nil`.

### 3.3 Statements

```ebnf
statement := print_stmt
           | if_stmt
           | while_stmt
           | for_stmt
           | return_stmt
           | yield_stmt
           | break_stmt
           | continue_stmt
           | try_stmt
           | with_stmt
           | throw_stmt
           | gc_stmt
           | del_stmt
           | expr_stmt

print_stmt    := "print" expression
if_stmt       := "if" expression block ("elif" expression block)* ("else" block)?
while_stmt    := "while" expression block
return_stmt   := "return" (expression ("," expression)*)?
yield_stmt    := "yield" (expression ("," expression)*)?
break_stmt    := "break"
continue_stmt := "continue"
throw_stmt    := "throw" expression
gc_stmt       := "gc"
expr_stmt     := expression
```

### 3.4 `for` statement

Only `for ... in ...` form exists:

```ebnf
for_stmt := "for" for_vars "in" iterator_exprs block
for_vars := IDENT ["#"] ("," IDENT)?
iterator_exprs := expression ("," expression ("," expression)?)?
```

Accepted patterns:

```toi
for v in table_expr
for i#, v in table_expr
for k, v in table_expr
for v in 1..10
for k, v in iter, state, control
```

Notes:

- `i#` must be attached directly to identifier (no whitespace before `#`).
- `i#` is only valid with implicit table iteration (single non-call iterator expression).
- `for v in a..b` is a numeric inclusive loop (`v = a, a+1, ..., b`).
- For implicit iteration (`for ... in expr` with one non-call expression):
  - If `expr` has `__next` (field or metamethod), loop uses `(__next, expr, nil)`.
  - Else tables/strings fall back to global `next`.
- `yield` is valid only inside functions; it lowers to `coroutine.yield(...)`.

### 3.5 `try` / `with` / `del`

```ebnf
try_stmt  := "try" block (("except" IDENT? block) ("finally" block)? | "finally" block)
with_stmt := "with" expression ("as" IDENT)? block

del_stmt  := "del" del_target ("," del_target)*
del_target := IDENT access_chain?
            | "(" expression ")" access_chain
access_chain := ("." IDENT | "[" expression "]")+
```

`try` forms supported:

```toi
try
  risky()
except
  handle_any()

try
  risky()
except e
  print e

try
  risky()
finally
  cleanup()

try
  risky()
except e
  print e
finally
  cleanup()
```

Examples:

```toi
try
  risky()
except e
  print e
finally
  cleanup()

with io.open("x.txt", "w") as f
  f.write("ok")

del x, t.a, t[k], (make()).field
```

## 4. Functions

```ebnf
function_body := "(" param_list? ")" block
param_list    := param ("," param)*
param         := ["*"] IDENT [":" type_name] ["=" const_default]
type_name     := IDENT
const_default := NUMBER | STRING | nil | true | false
```

Notes:

- `*name` is variadic and must be last parameter.
- Typed params are accepted (`a: int`, `s: string`, etc.).
- Defaults must be literal constants only.
- Parameter with default cannot be followed by non-default param.
- Anonymous functions: `fn(...) ...` are expressions.

## 5. Expressions

### 5.1 Primary expressions

```ebnf
primary := NUMBER
         | STRING
         | FSTRING
         | "nil" | "true" | "false"
         | IDENT
         | import_expr
         | anonymous_fn
         | "(" expression ")"
         | table_literal
```

### 5.2 Table literals

```ebnf
table_literal := "{" (table_entries | table_comprehension)? "}"
table_entries := table_entry (("," | implicit_newline_sep) table_entry)*
table_entry   := "[" expression "]" "=" expression
               | IDENT "=" expression
               | expression
```

Comprehension form:

```ebnf
table_comprehension := comp_expr "for" for_vars "in" iterator_exprs ["if" expression]
comp_expr := expression                ; array-style
          | expression "=" expression ; map-style key/value
```

### 5.3 Calls, indexing, member access

```ebnf
postfix := primary (call | index | member | metatable_ctor)*
call    := "(" arg_list? ")"
arg_list := argument ("," argument)*
argument := expression | named_arg
named_arg := IDENT "=" expression
index   := "[" expression "]"
slice   := "[" [expression] ".." [expression] [":" [expression]] "]"
member  := "." IDENT
metatable_ctor := table_literal  ; expr{...}
```

Notes:

- In calls, positional args cannot appear after named args.
- Calls support one spread argument: `fn(a, *args_table)`.
- Spread argument must be last and cannot be combined with named args.
- `expr{...}` is a special infix form that sets the metatable of the new table to `expr`.

### 5.4 Operators and precedence

Highest to lowest:

1. Postfix: call `()`, member `.`, index/slice `[]`, metatable constructor `{...}`
2. Unary: `not`, unary `-`, length `#`
3. Multiplicative: `*`, `/`, `//`, `%`, `**`
4. Additive and range: `+`, `-`, `<+`, `..`
5. Comparison: `<`, `<=`, `>`, `>=`, `==`, `!=`, `in`, `not in`, `has`
6. Logical `and`
7. Logical `or`
8. Ternary `?:`
9. Assignment `=` and walrus `:=` (only on valid l-values)

Notes:

- `a ? b : c` is supported and right-associative.
- `..` is both range operator and part of slice syntax.
- `elem in container` is equivalent to `container has elem`.
- `<+` appends to a table (`t <+ v`), returning the appended index. If a `__append`
  metamethod exists, it is called as `__append(lhs, rhs)` instead.

### 5.5 Assignment targets

Valid assignment targets:

- `name = expr`
- `obj.field = expr`
- `obj[index] = expr`
- `name := expr`
- `obj.field := expr`
- `obj[index] := expr`

Slice assignment is not allowed (`obj[a..b] = ...` errors).

Notes:

- `:=` returns the assigned value, so it can be used in larger expressions.
- Named call arguments still use `=` (`fn(x=1)`), not `:=`.

## 6. Import Syntax

Two forms:

1. Expression form (usable anywhere expressions are allowed):

```toi
m = import lib.types
```

2. Declaration form (binds module to last path component):

```toi
import lib.types   -- binds local/global variable: types
```

3. From-import form:

```toi
from lib.types import String, Integer
from tests.star_exports_mod import *
```

Module path grammar:

```ebnf
module_path := IDENT ("." IDENT)*
```

Module resolution order for `import a.b`:

1. native module `a.b`
2. `a/b.toi`
3. `a/b/__.toi`
4. `lib/a/b.toi`
5. `lib/a/b/__.toi`

## 7. Runtime-Visible Syntax Semantics

- Indexing is 1-based.
- Variables are local-by-default in normal script compilation.
- Negative numeric indices are supported for tables/strings (`-1` = last element/char).
- Slice bounds are inclusive.
- Omitted slice start/end default based on step direction.
- Truthiness: `nil`, `false`, `0`, empty string, empty table are falsey; others truthy.

Decorator lowering semantics:

- Decorators apply to function declarations only.
- Multiple decorators apply bottom-to-top:

```toi
@a
@b
fn f() ...
```

is equivalent to:

```toi
fn f() ...
f = a(b(f))
```

## 8. Reserved / Tokenized But Limited

- `;` is tokenized and recognized in a few parser checks, but statement sequencing is newline/indent driven.
- `!` alone is not a unary operator; only `!=` is valid (`not` is unary logical negation).
