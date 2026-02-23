# `binary` Module

Import:

```toi
binary = import binary
```

## Functions

- `binary.pack(value) -> bytes_string`
- `binary.unpack(bytes_string) -> value`
- `binary.hex(bytes_string) -> hex_string`
- `binary.unhex(hex_string) -> bytes_string`

## Notes

- Binary pack/unpack supports common Toi values (`nil`, booleans, numbers, strings, tables).
- Functions are not serialized (decoded as `nil`).
- `binary.unhex` requires even-length valid hex input.
