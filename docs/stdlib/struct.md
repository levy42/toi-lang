# `struct` Module

Import:

```toi
struct = import struct
```

## Functions

- `struct.pack(fmt, ...) -> bytes_string`
- `struct.unpack(fmt, bytes_string, [offset]) -> values_table`

## Format Notes

Supported format features include:

- Endianness prefixes: `>`, `<`
- Repeats/counts: `2B`, `10s`, etc.
- Integer/float specifiers used in tests: `B`, `H`, `h`, `I`, `f`, `d`, `s`

See `tests/38_struct.toi` for practical format examples.
