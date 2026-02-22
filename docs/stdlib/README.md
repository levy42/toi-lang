# Standard Library

Native modules are loaded with `import`:

```pua
math = import math
```

Available native modules:

- `math`
- `time`
- `io`
- `os`
- `stat`
- `dir`
- `signal`
- `mmap`
- `poll`
- `coroutine`
- `string`
- `table`
- `socket`
- `thread`
- `json`
- `template`
- `http`
- `inspect`
- `regex`
- `fnmatch`
- `glob`
- `binary`
- `struct`
- `btree`
- `uuid`
- `gzip` (optional; only when built with zlib)

Global core functions are documented in `docs/stdlib/core.md`.

Pua-implemented helper libraries are documented in `docs/stdlib/pua-libraries.md`.
