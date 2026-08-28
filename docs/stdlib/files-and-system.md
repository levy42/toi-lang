# Files And System

This group covers filesystem access, process helpers, low-level output, and OS-facing utilities.

## `io`

File and buffer I/O.

Module-level entry points:

- `io.open(path, mode)`
- `io.buffer(initial=nil)`

Common file methods:

- `read([n])`
- `readline()`
- `write(data)`
- `seek(...)`
- `tell()`
- `close()`

```toi
import io

with io.open("notes.txt", "w") as f
  f.write("hello")
```

## `os`

General operating-system helpers:

- environment: `getenv`, `setenv`
- process: `system`, `exit`
- files: `remove`, `rename`, `mkdir`, `rmdir`
- discovery: `listdir`, `isfile`, `isdir`, `exists`, `getcwd`
- memory/process helpers: `rss`, `trim`

## `sys`

Low-level output helpers such as `sys.write(...)`.

## `stat`

Filesystem metadata:

- `stat`
- `lstat`
- `chmod`
- `umask`

## `dir`

Directory iteration helpers such as `dir.list(...)` and `dir.scandir(...)`.

## `signal`

POSIX signal helpers:

- `raise`
- `ignore`
- `default`

## `mmap`

Memory-mapped file access for large-file workflows and zero-copy style patterns.
