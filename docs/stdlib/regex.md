# `regex` Module

Import:

```pua
regex = import regex
```

POSIX `regex.h` wrapper (extended regular expressions).

## Functions

- `regex.match(pattern, text, [flags]) -> bool`
- `regex.search(pattern, text, [flags]) -> table|nil`
- `regex.replace(pattern, text, repl, [count], [flags]) -> string`
- `regex.split(pattern, text, [maxsplit], [flags]) -> table`

## Flags

`flags` is an optional string:

- `i`: case-insensitive
- `n`: newline-sensitive (`REG_NEWLINE`)
- `m`: clear newline-sensitive behavior
- `x`: accepted, currently no-op

## `search` Result

When found, returns:

- `start`: 1-based start index
- `end`: 1-based inclusive end index
- `match`: full matched substring
- `groups`: captures table (`groups[1]`, `groups[2]`, ...)
