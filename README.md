# Toi

Toi is a small, Lua-like scripting language with indentation-based blocks and a compact standard library.

## Documentation

Full documentation is in `docs/`:

- `docs/README.md`

## Build

```bash
make
make release
make clean
make test
make docs
make test-fmt
```

## Run

```bash
./toi                  # REPL
./toi path/to/file.toi # run script
./toi fmt file.toi     # print formatted code
./toi fmt -w file.toi  # format in place
./toi fmt --check file.toi  # exit 1 if formatting would change
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
- `socket`: tcp, select, send/recv, optional TLS
- `coroutine`: create, resume, status, yield
- `thread`: threads and scheduling helpers
- `template`: `compile`, `render`, `code`
- `http`: basic http parser/response helpers
- `regex`: POSIX regex wrapper (`match`, `search`, `replace`, `split`)
- `fnmatch`: POSIX glob wrapper (`match`)
- `glob`: POSIX pathname expansion wrapper (`match`)
- `gzip`: zlib-backed gzip wrapper (`compress`, `decompress`)

## Toi libraries (`lib/*.toi`)

- `lib.types` – small type-check helpers (String/Number/Boolean/Integer/List/Optional/Record)
- `lib.record` – record schemas + validation + `__str`
- `lib.scheduler` – cooperative scheduler
- `lib.selector` – poll/select readiness helper
- `lib.log` – structured-ish logging helpers (levels, named loggers, handlers)
- `lib.http_server` – simple concurrent HTTP server framework
- `lib.loadtest` – minimalist HTTP/HTTPS load test CLI (RPS-oriented)

Quick run:

```bash
./toi lib/loadtest.toi rps http://127.0.0.1:8080/ 10 20 2000 false
```

- Args: `url duration_sec concurrency timeout_ms verify_tls`

## `lib.http_server` quick HTTPS example

```toi
http_server = import lib.http_server

app = http_server(
  host = "0.0.0.0",
  port = 8443,
  ssl = true,
  ssl_cert = "certs/server.crt",
  ssl_key = "certs/server.key"
)

get = app.get
@get("/health")
fn health(req)
  return {ok = true}

app.run()
```

- TLS requires a build with OpenSSL (`socket.tls_available() == true`).
- `ssl=true` requires both `ssl_cert` and `ssl_key`.
- Certificate/key aliases are also accepted: `cert` or `cert_path`, and `key` or `key_path`.

## Notes

- Userdata can expose methods via `__index` (table or function). The `io` file handle uses a function `__index` to resolve methods like `f.read` and `f.write`.

## Zed Setup (WIP)

- Rebuild extension grammars after grammar/query edits:

```bash
./editor/build_grammars.sh
```

- Prototype language server is in the separate extension repo at
  `/Users/levy/Projects/zed-extension-toi/lsp/toi_lsp.py` and supports:
  - completion (keywords + workspace symbols)
  - goto definition (cross-file in workspace)
  - document symbols
  - basic formatting
  - basic diagnostics (indentation, missing block after headers, bracket balance)

- Example Zed user settings to run the prototype server:

```json
{
  "lsp": {
    "toi-lsp": {
      "binary": {
        "path": "/usr/bin/env",
        "arguments": ["python3", "/Users/levy/Projects/zed-extension-toi/lsp/toi_lsp.py"]
      }
    }
  }
}
```


## Benchmarks (sample)

Sample benchmark results collected in this repository on:

- Host: `Linux 375b89425e85 6.12.47 #1 SMP Mon Oct 27 10:01:15 UTC 2025 x86_64`
- Build: debug (`make`)

Commands used:

```bash
./toi tests/21_template_perf.toi
./toi tests/26_json_perf.toi
N=100000 ./toi tests/peft/btree_perf.toi
```

Observed output:

- `template perf`: compile `0.026113s`, render `0.011995s`, render_with_compile `0.001019s`
- `json perf`: encode `0.029274s`, decode `0.043280s`, payload_bytes `203`
- `btree perf` (`n=100000`): insert `22495 op/s`, lookup `58515 op/s`, delete `32111 op/s`

> Notes: results are environment-dependent (CPU, storage, compiler flags, and system load). Treat these as reference numbers, not strict performance guarantees.
