# Text And Data

This group covers serialization, parsing, templating, search, and binary data.

## Structured Data

### `json`

- `json.encode(value)`
- `json.decode(text)`

### `toml`

- `toml.parse(text)`
- `toml.stringify(value)`

### `csv`

CSV parse and emit helpers.

## Search And Pattern Matching

### `regex`

- `match`
- `search`
- `replace`
- `split`
- `finditer`
- `compile`

If you reuse the same pattern many times, prefer `regex.compile(...)` and then call methods on the compiled object.

### `fnmatch`

Shell-style wildcard matching.

### `glob`

Filesystem glob expansion.

## Templates And Rendering

### `template`

- `template.compile(...)`
- `template.render(...)`
- `template.code(...)`

Used for small HTML, text, and code-generation tasks.

## Binary Formats

### `binary`

Pack and unpack general binary values.

### `struct`

Structured binary packing and unpacking with format strings.

## Ordered Storage

### `btree`

Persistent B-tree key/value storage for ordered workloads.

## Compression

### `gzip`

- `gzip.compress(...)`
- `gzip.decompress(...)`

This module is available only when Toi is built with zlib support.
