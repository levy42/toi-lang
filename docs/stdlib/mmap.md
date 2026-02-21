# `mmap` Module

Import:

```pua
mmap = import mmap
```

POSIX memory-mapped file wrapper (`mmap(2)`).

## Functions

- `mmap.map(path, [mode]) -> region | nil, err`

`mode`:

- `"r"` (default): read-only
- `"rw"`: read-write shared mapping

## `region` Methods

- `region.len() -> number`
- `region.read([start], [count]) -> string`
- `region.write(offset, data) -> bool` (requires `"rw"`)
- `region.flush() -> bool`
- `region.close() -> bool`
- `region[start..end:step] -> string` (slice syntax)

Indexes are 1-based for `read`/`write` offset parameters.
Slice bounds are also 1-based and inclusive, matching string/table slice behavior.
