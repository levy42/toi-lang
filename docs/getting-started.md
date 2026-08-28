# Getting Started

## Build

Debug build:

```bash
make
```

Optimized build:

```bash
make release
```

Clean build artifacts:

```bash
make clean
```

## Run

Start the REPL:

```bash
./toi
```

Run a script:

```bash
./toi hello.toi
```

## Format

```bash
./toi fmt file.toi
./toi fmt -w file.toi
./toi fmt --check file.toi
```

## Test

```bash
make test
```

Tests live in `tests/*.toi`. A minimal test looks like this:

```toi
from lib.test import assert_eq, assert_true

assert_eq(1 + 1, 2)
assert_true("toi".upper() == "TOI")
```

## First Script

```toi
import math

fn main()
  values = {3, 4, 5}
  for n in values
    print n, math.sqrt(n * n)

main()
```

## Practical Conventions

- Prefer `if value` over explicit comparisons to `true`, `false`, or `nil`.
- Prefer `for v in table` or `for k, v in table` over manual index loops.
- Use method-call style when available: `"hello".upper()`.
- Use `items <+ value` to append to a table.
- Use `match` when branching on a known value.

## Optional Browser Playground

```bash
make wasm
open web/playground.html
```
