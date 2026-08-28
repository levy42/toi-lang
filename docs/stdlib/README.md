# Standard Library

The Toi standard library has two layers:

- native modules implemented in C and imported directly, such as `math`, `json`, or `socket`
- shipped Toi libraries under `lib.*`, such as `lib.types`, `lib.db`, or `lib.http_server`

Import native modules like this:

```toi
import math
json = import json
```

Import Toi libraries like this:

```toi
import lib.types
db = import lib.db
```

## Module Groups

- [Core](core.md): globals, numbers, strings, tables, time, URLs, UUIDs, and basic inspection
- [Files And System](files-and-system.md): files, directories, OS helpers, mmap, signals, low-level output
- [Text And Data](text-and-data.md): JSON, TOML, CSV, regex, templates, binary data, B-trees, compression
- [Networking And Concurrency](networking-and-concurrency.md): sockets, HTTP helpers, polling, coroutines, threads, contexts
- [Toi Libraries](toi-libraries.md): the higher-level libraries implemented in Toi itself

## Native Module Index

- `math`
- `time`
- `string`
- `table`
- `io`
- `sys`
- `os`
- `stat`
- `dir`
- `signal`
- `mmap`
- `poll`
- `coroutine`
- `context`
- `socket`
- `thread`
- `json`
- `toml`
- `csv`
- `template`
- `http`
- `url`
- `inspect`
- `regex`
- `fnmatch`
- `glob`
- `binary`
- `struct`
- `btree`
- `uuid`
- `gzip` when built with zlib support
