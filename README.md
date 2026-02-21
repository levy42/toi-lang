# Pua

Pua is a small, Lua-like scripting language with indentation-based blocks and a compact standard library.

## Documentation

Full documentation is in `docs/`:

- `docs/README.md`

## Build

```bash
make
make release
make clean
make test
make test-fmt
```

## Run

```bash
./pua                  # REPL
./pua path/to/file.pua # run script
./pua fmt file.pua     # print formatted code
./pua fmt -w file.pua  # format in place
./pua fmt --check file.pua  # exit 1 if formatting would change
```

## Syntax (quick reference)

```lua
-- variables
x = 10
s = "hello"
arr = 1, 2, 3

-- functions (default params, varargs)
fn add(a, b = 10)
  return a + b

fn sum(*vals)
  t = 0
  i = 1
  while vals[i] != nil
    t = t + vals[i]
    i = i + 1
  return t

-- conditionals
if x > 0
  print "positive"
elif x == 0
  print "zero"
else
  print "negative"

-- loops
for i = 1, 5
  print i

for k, v in {a = 1, b = 2}
  print k, v

for v in {10, 20, 30}        -- values-only
  print v

for i#, v in {10, 20, 30}    -- index + value (array-style)
  print i, v

-- tables
t = {a = 1}
t.b = 2
t["c"] = 3

-- strings
m = [[multi
line]]
f = f"hi {x + 1}"
ch = "hello"[1] -- "h"

-- ternary
y = x > 0 ? "yes" : "no"

-- exceptions
try
  risky()
except e
  print e
finally
  cleanup()

throw "boom"

-- with (context managers)
with io.open("tmp.txt", "w") as f
  f.write("hello")

-- slice (inclusive end, step can be negative)
mid = slice({10, 20, 30, 40, 50}, 2, 4)     -- {20, 30, 40}
rev = slice({10, 20, 30, 40, 50}, 4, 2, -1) -- {40, 30, 20}
mid2 = {10, 20, 30, 40, 50}[2..4:1]         -- {20, 30, 40}
first = {10, 20, 30, 40, 50}[..3]           -- {10, 20, 30}
tail = {10, 20, 30, 40, 50}[3..]            -- {30, 40, 50}
s = "hello"[2..4:1]                         -- "ell"

-- table comprehension (array-style)
tcomp = {x * x for x in {1, 2, 3}}         -- {1, 4, 9}

-- table comprehension (map-style via two expressions)
mcomp = {x = x * 10 for x in {1, 2, 3}}    -- { [1]=10, [2]=20, [3]=30 }
```

## Built-in modules (native)

- `time`: `time.seconds()`, `time.nanos()`, `time.clock()`
- `math`: sin, cos, tan, asin, acos, atan, sqrt, floor, ceil, abs, exp, log, pow, fmod, modf, deg, rad, random, seed, min, max, sum
- `string`: len, sub, lower, upper, char, byte, find, trim, split, join, rep, reverse, format
- `table`: remove, concat, sort
- `io`: open, read/write, close (file operations)
- `os`: getenv, rename, remove, system, clock
- `stat`: stat/lstat/chmod/umask metadata helpers
- `dir`: directory listing/scandir helpers
- `signal`: raise/ignore/default POSIX signals
- `mmap`: memory-mapped file helpers
- `poll`: POSIX poll wrapper (`wait`)
- `json`: encode/decode
- `binary`: `pack(value)` / `unpack(bytes)`
- `struct`: `pack(fmt, ...)` / `unpack(fmt, bytes, offset=1)`
- `socket`: tcp, select, send/recv
- `coroutine`: create, resume, status, yield
- `thread`: threads and scheduling helpers
- `template`: `compile`, `render`, `code`
- `http`: basic http parser/response helpers
- `regex`: POSIX regex wrapper (`match`, `search`, `replace`, `split`)
- `fnmatch`: POSIX glob wrapper (`match`)
- `glob`: POSIX pathname expansion wrapper (`match`)

## Pua libraries (`lib/*.pua`)

- `lib.types` – small type-check helpers (String/Number/Boolean/Integer/List/Optional/Record)
- `lib.record` – record schemas + validation + `__str`
- `lib.scheduler` – cooperative scheduler
- `lib.selector` – poll/select readiness helper
- `lib.http_server` – simple concurrent HTTP server framework

## Notes

- Userdata can expose methods via `__index` (table or function). The `io` file handle uses a function `__index` to resolve methods like `f.read` and `f.write`.

## Zed Setup (WIP)

- Rebuild extension grammars after grammar/query edits:

```bash
./editor/build_grammars.sh
```

- Prototype language server is in the separate extension repo at
  `/Users/levy/Projects/zed-extension-pua/lsp/pua_lsp.py` and supports:
  - completion (keywords + workspace symbols)
  - goto definition (cross-file in workspace)
  - document symbols
  - basic formatting
  - basic diagnostics (indentation, missing block after headers, bracket balance)

- Example Zed user settings to run the prototype server:

```json
{
  "lsp": {
    "pua-lsp": {
      "binary": {
        "path": "/usr/bin/env",
        "arguments": ["python3", "/Users/levy/Projects/zed-extension-pua/lsp/pua_lsp.py"]
      }
    }
  }
}
```
