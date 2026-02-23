# Getting Started

## Build

```bash
make
make release
make test
```

## Build (WASM / WASI)

```bash
make wasm
make wasm-release
```

Default `make wasm` uses `zig cc` (recommended).
If `zig` is unavailable, the Makefile falls back to `clang-20`/`clang` when distro `wasi-libc`
headers and libs are present (e.g. `/usr/include/wasm32-wasi`, `/usr/lib/wasm32-wasi`).
Use `make wasm-release` for a smaller production-oriented `toi.wasm`.

If you prefer `clang` + explicit WASI sysroot, pass both:

```bash
make wasm WASM_CC=clang WASM_SYSROOT=/path/to/wasi-sysroot
```

## Browser Playground (Static HTML)

Build the wasm binary first:

```bash
make wasm
```

Open directly:

```bash
open web/playground.html
```

In the page, pick your local `toi.wasm` (built by `make wasm`) and run code from the textarea.

## Run

```bash
./toi
./toi path/to/script.toi
```

## Format Source

```bash
./toi fmt file.toi
./toi fmt -w file.toi
./toi fmt --check file.toi
```

## Hello World

```toi
print "hello, toi"
```

## Quick Example

```toi
math = import math

fn hyp(a, b)
  return math.sqrt(a * a + b * b)

print hyp(3, 4)
```

## Testing Convention

Project tests are executable `.toi` programs in `tests/`.

```bash
make test
```
