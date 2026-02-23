# `io` Module

Import:

```toi
io = import io
```

## Module Functions

- `io.open(path, [mode]) -> file|nil`
- `io.buffer([initial_string]) -> buffer`

## File Methods (`io.file` userdata)

- `f.close()`
- `f.read([n]) -> string|nil`
- `f.readline() -> string|nil`
- `f.write(string) -> number|nil`
- `f.seek(offset)`
- `f.seek(whence, offset)` where `whence` is `"set"`, `"cur"`, or `"end"`
- `f.tell() -> number|nil`

## Buffer Methods (`io.buffer` userdata)

- `b.close()`
- `b.read([n]) -> string|nil`
- `b.readline() -> string|nil`
- `b.write(string) -> number|nil`
- `b.seek(offset)`
- `b.seek(whence, offset)`
- `b.tell() -> number|nil`
