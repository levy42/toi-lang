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

```pua
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
- Contains: `has`
- Length: `#value`
- Append operator: `++`

## Assignment

- Basic: `x = expr`
- Multi-target: `a, b = expr1, expr2`
- Compound: `+=`, `-=`, `*=`, `/=`, `%=`

## Tables

Array-like + map-like hybrid:

```pua
t = {10, 20, name = "pua", [10] = "x"}
print t[1]
print t.name
print t["name"]
```

## Indexing and Slicing

- String and table indexing are supported.
- Negative indices are supported in some operations.
- Slice helper: `slice(value, start, stop, step=1)`.
- Slice syntax: `value[start..stop:step]`, plus short forms `[..stop]`, `[start..]`.

## Strings and Interpolation

```pua
name = "Pua"
pi = 3.14159
s = f"hello {name}, pi={pi|.2f}"
```

`{expr|spec}` uses `string.format` semantics for `spec`.
