# `os` Module

Import:

```toi
os = import os
```

## Process/Env

- `os.exit([code])`
- `os.getenv(name) -> string|nil`
- `os.system(command) -> number`
- `os.clock() -> number`

## Filesystem

- `os.remove(path) -> true|false, err?`
- `os.rename(old, new) -> true|false, err?`
- `os.mkdir(path, [all=false]) -> true|false, err?`
- `os.rmdir(path) -> true|false, err?`
- `os.listdir(path) -> table|nil, err?`
- `os.isfile(path) -> bool`
- `os.isdir(path) -> bool`
- `os.exists(path) -> bool`
- `os.getcwd() -> string|nil, err?`
- `os.chdir(path) -> true|false, err?`

## Runtime/Host

- `os.rss() -> number|nil` (resident set size when supported)
- `os.trim() -> bool|nil` (allocator trim on supported platforms)
- `os.argv` (array of CLI args)
- `os.argc` (argument count)
