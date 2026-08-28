# Benchmarks

This page covers the benchmark material that is useful to language users: what exists in the repository, how to run it, and how to read the numbers.

## Benchmark Files

The repository includes Toi and comparison benchmarks under `benchmarks/`:

- `benchmarks/perf.toi`
- `benchmarks/string_ops_compare.toi`
- `benchmarks/json_regex_bench.toi`
- `benchmarks/gzip_bench.toi`

There are also companion Python and Lua scripts for side-by-side comparisons.

## Recommended Commands

Build an optimized interpreter first:

```bash
make release
```

Then run the benchmark programs directly:

```bash
./toi benchmarks/perf.toi
./toi benchmarks/string_ops_compare.toi
./toi benchmarks/json_regex_bench.toi
./toi benchmarks/gzip_bench.toi
N=100000 ./toi tests/peft/btree_perf.toi
```

## Reference Numbers

Sample numbers already captured in this repository include:

- `template perf`: compile `0.026113s`, render `0.011995s`, render-with-compile `0.001019s`
- `json perf`: encode `0.029274s`, decode `0.043280s`, payload `203` bytes
- `btree perf` with `n=100000`: insert `22495 op/s`, lookup `58515 op/s`, delete `32111 op/s`

Local audit notes also recorded:

- repeated regex via module-level `regex.search(...)` / `regex.replace(...)` is much slower than using `compiled = regex.compile(pattern)`
- B-tree lookup throughput is strong relative to insert and delete
- string transforms are decent, but tight-loop string work still pays visible allocation cost

These numbers are references, not guarantees. They depend on build mode, CPU and storage, system load, and optional libraries.

## What To Benchmark

For application work, benchmark the operations that dominate your runtime:

- repeated regex usage
- JSON encode/decode
- string-heavy templating
- database or B-tree access patterns
- socket and TLS traffic

Microbenchmarks are useful, but application-shaped benchmarks are usually more honest.

## Performance Tips

### Compile regexes you reuse

```toi
regex = import regex

compiled = regex.compile("^[A-Z][a-z]+$")
ok = compiled.match("Ana")
```

If you call the module-level search helpers in a tight loop, pattern compilation can dominate runtime.

### Reuse prepared data

- cache templates instead of recompiling them repeatedly
- reuse parsed configuration data when possible
- move repeated setup outside hot loops

### Measure debug and release separately

`make` is for development. For realistic throughput numbers, use `make release`.
