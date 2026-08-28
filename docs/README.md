# Quick Start

Toi is a small scripting language with indentation-based blocks, Python-like control flow, Lua-like tables, and a compact standard library.

This documentation set is intentionally focused on three things:

- the language
- the standard library
- benchmark guidance

If you want one page that shows most of the language quickly, start here.

## Build And Run

```bash
make
./toi
./toi path/to/file.toi
make test
```

## A Tour Of The Syntax

```toi
import math
from lib.test import assert_eq

name = "Toi"
nums = {1, 2, 3}
point = {x = 3, y = 4}
mixed = {1, 2, 3, arg = 4}
-- comment

fn hyp(x, y)
  return math.sqrt(x * x + y * y)

fn describe(value)
  match type(value)
  case "number"
    return f"number:{value}"
  case "string"
    return f"string:{value}"
  else
    return "other"

if hyp(point.x, point.y) == 5
  print f"hello {name.upper()}"

for i in 1..3
  nums <+ hyp(i, i + 1)

for k, v in point
  print k, v

first_two = nums[..2]
last = nums[-1]

try
  assert_eq(describe(last), "number:5")
except e
  print "unexpected:", e
```

## What To Know First

- Blocks are indentation-based. There are no braces for `if`, `for`, `fn`, `match`, or `try`.
- Comments start with `--`.
- Tables are the main compound type. They act like arrays and maps.
- Strings support interpolation: `f"hello {name}"`.
- Slices use `..`: `items[2..5]`, `items[..3]`, `items[3..]`.
- Append to a table with `items <+ value`.
- Import native modules with `import math` or `math = import math`.
- Import shipped Toi libraries with dotted paths such as `import lib.types`.

## Common Forms

### Functions

```toi
fn add(a, b = 10)
  return a + b

fn collect(*vals)
  return vals
```

### Tables

```toi
user = {
  name = "Ana",
  email = "ana@example.com"
}

print user.name
print user["email"]
```

### Comprehensions

```toi
squares = {n * n for n in 1..5}
lookup = {str(n) = n * 10 for n in 1..3}
```

### Decorators

```toi
cli = import lib.cli
app = cli.App(name = "demo")

@app.command
fn hello(name = "world")
  print f"hello {name}"
```

### Error Handling

```toi
try
  risky()
except e
  print e
finally
  cleanup()
```

### Context Managers

```toi
with io.open("notes.txt", "w") as f
  f.write("hello")
```

## Next Steps

- [Getting Started](getting-started.md)
- [Language Syntax](language/syntax.md)
- [Control Flow](language/control-flow.md)
- [Functions](language/functions.md)
- [Modules](language/modules.md)
- [Standard Library](stdlib/README.md)
- [Benchmarks](benchmarks.md)
