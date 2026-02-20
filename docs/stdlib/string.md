# `string` Module

Import:

```pua
string = import string
```

The module is callable as `str(x)` via metatable, and is also aliased globally as `str`.

## Functions

- `len(s)`
- `sub(s, start, [end])`
- `lower(s)`
- `upper(s)`
- `char(...)`
- `byte(s, [index])`
- `find(s, pattern, [start])`
- `trim(s)`
- `split(s, [sep])`
- `join(sep, table)`
- `rep(s, n)`
- `reverse(s)`
- `format(fmt, ...)`

## `string.format`

Supports printf-like formatting with guarded specifiers, including width/precision (for example: `"%.2f"`, `"%08x"`).
