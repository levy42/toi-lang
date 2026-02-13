# Pua

Pua is a small, Lua-like scripting language with indentation-based blocks and a compact standard library.

## Build

```bash
make
make release
make clean
make test
```

## Run

```bash
./pua                  # REPL
./pua path/to/file.pua # run script
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
- `json`: encode/decode
- `socket`: tcp, select, send/recv
- `coroutine`: create, resume, status, yield
- `thread`: threads and scheduling helpers
- `template`: `compile`, `render`, `code`
- `http`: basic http parser/response helpers

## Pua libraries (`lib/*.pua`)

- `lib.types` – small type-check helpers (String/Number/Boolean/Integer/List/Optional/Record)
- `lib.record` – record schemas + validation + `__str`
- `lib.scheduler` – cooperative scheduler
- `lib.http_server` – simple concurrent HTTP server framework

## Notes

- Userdata can expose methods via `__index` (table or function). The `io` file handle uses a function `__index` to resolve methods like `f.read` and `f.write`.
