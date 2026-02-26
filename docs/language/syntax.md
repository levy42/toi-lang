# Language Syntax

## Lexical Basics

- Statements are line-oriented.
- Blocks are indentation-based (`INDENT`/`DEDENT` tokens).
- Comments start with `--`.
- Strings support single quotes, double quotes, and multiline `[[...]]`.
- Formatted strings: `f"...{expr}..."`, with optional format spec: `{expr|.2f}`.

## Literals

- `nil`, `true`, `false`
- Numbers (integer/float syntax)
- Strings
- Table literals: `{}`

Examples:

```toi
n = 42
s = "hello"
m = [[line 1
line 2]]
t = {1, 2, a = 3}
```

## Operators

Arithmetic:

- `+`, `-`, `*`, `/`, `%`, `//`, `**`

Comparison:

- `==`, `!=`, `<`, `<=`, `>`, `>=`

Logical:

- `and`, `or`, `not`

Other:

- Ternary: `cond ? a : b`
- Range: `a..b`
- Contains: `in`, `not in`, `has` (`elem in container` == `container has elem`)
- Length: `#value`
- Append operator: `++`

## Assignment

- Basic: `x = expr`
- Expression assignment (walrus): `x := expr`
- Multi-target: `a, b = expr1, expr2`
- Single-RHS unpack: `a, b = expr` (if `expr` is a table, values come from `expr[1]`, `expr[2]`, ...)
- Compound: `+=`, `-=`, `*=`, `/=`, `%=`

Walrus returns the assigned value, so it can be used inside larger expressions:

```toi
v = (x := 10)
ok = (x := x + 1) > 10
```

## Tables

Array-like + map-like hybrid:

```toi
t = {10, 20, name = "toi", [10] = "x"}
print t[1]
print t.name
print t["name"]
```

## Indexing and Slicing

- String and table indexing are supported.
- Negative indices are supported in some operations.
- Slice helper: `slice(value, start, stop, step=1)`.
- Slice syntax: `value[start..stop:step]`, plus short forms `[..stop]`, `[start..]`.
- In slice bounds, negative indices count from the end (for example, `"hello"[..-1]` -> `"hell"`).

## Strings and Interpolation

```toi
name = "Toi"
pi = 3.14159
s = f"hello {name}, pi={pi|.2f}"
```

`{expr|spec}` uses `string.format` semantics for `spec`.
