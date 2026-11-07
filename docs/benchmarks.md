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

## Verifying JIT Correctness

Timing alone hides a miscompiling JIT: a loop that computes the wrong answer can
still look fast. `benchmarks/perf.toi` therefore prints a `result=` value for
every case, and two make targets diff those values across JIT configurations:

```bash
make verify      # runs benchmarks/perf.toi with the interpreter and three JIT
                 # configurations, then diffs every reported result
make test-jit    # runs the test suite with the JIT off and with an aggressive
                 # hot threshold, then diffs stdout
```

`make verify` exits non-zero when a benchmark result differs from the
interpreter. Cases that are still known to diverge are listed in
`benchmarks/verify_results.sh` (`KNOWN_OPEN`) and reported as `known-open`
without failing the run; set `VERIFY_STRICT=1` to fail on those as well.
The list is empty today: every `perf.toi` case matches the interpreter.

Both targets are useful in CI and when touching `src/jit*.c`. `make test-jit`
also lowers `TOI_JIT_LOOP_HOT` to `8`, so even the small loops in `tests/` get
compiled and their output is compared against the interpreter.

## JIT Switches

| variable | default | effect |
|---|---|---|
| `TOI_JIT` | `1` | `0` disables the native JIT entirely (pure interpreter) |
| `TOI_JIT_LOOP_HOT` | `1024` | backedge count before a loop is compiled; lower it (`8`) to force the JIT to engage on small workloads |
| `TOI_JIT_BACKEND` | `auto` | `none` disables native codegen (IR/typed executors only); `arm64` / `x64` force a backend |
| `TOI_JIT_SLOT_LOOP` | `0` | `1` enables the arm64 slot-loop lowering |
| `TOI_JIT_CALL_TRACES` | `1` | `0` retires loop traces that contain call sites instead of running them |
| `TOI_JIT_FUNC` | `0` | `1` enables whole-function JIT (call-site entry, tier-2 templates); verifies clean but is perf-neutral here |
| `TOI_JIT_DUMP_LOOPS` | unset | dump compiled loop traces and execution failures (very verbose) |
| `TOI_JIT_DEBUG_MIXED_LOOP` | unset | dump per-operation detail for `fn`-filtered mixed-executor runs |

Example bisecting a suspected loop bug:

```bash
TOI_JIT=0 ./toi repro.toi                 # interpreter baseline
TOI_JIT_LOOP_HOT=8 ./toi repro.toi        # force the JIT to compile small loops
TOI_JIT_BACKEND=none ./toi repro.toi      # IR/typed executors only, no machine code
TOI_JIT_SLOT_LOOP=1 ./toi repro.toi       # opt into the arm64 slot-loop lowering
```

## Known JIT Gaps

The arm64 direct-core native trace used to compute several things incorrectly.
These were found by comparing traced runs against the interpreter and are fixed:

- `CMP_LT`/`CMP_GT` were emitted as an equality compare, so `if x > 0` in
  machine code meant `if x == 0`. Ordering comparisons now use the proper
  condition codes; shapes the emitter cannot do natively (ordering against a
  slot or an upvalue) now bail to the IR executor instead of guessing.
- Truthiness inside native traces only treated `nil` and `false` as falsey.
  `0`, `""` and `{}` are falsey in Toi and are now handled the same way.
- A `continue` jump was patched to the loop-exit block instead of the
  next-iteration transition, which ended the loop early.

Loops whose body calls a function were also pathological: when the callee could
not be run inside the trace (a 2-argument call to a closure whose body the trace
cannot execute, e.g. `return a + #b`), the trace aborted at the call on *every*
iteration. Each backedge then paid a trace entry plus a state commit, which made
such loops ~2.7x slower than the interpreter. `JitLoopTrace::call_stall_runs`
now counts those aborts and the trace is retired after 32 of them, so the loop
falls back to the VM's own (quickened) call path. Two related fixes: the
in-trace call cache now also covers quickened two-argument calls (`QCALL2`), and
`JOP_LENGTH` is whitelisted for callee programs, which is what lets a callee such
as `return a + #b` run inside the trace at all. `bench_calls` in `perf.toi` used
to `return a` (ignoring its argument), so it always reported `result=0` and could
not detect a miscompiled call; it now returns `a + #b`.

Calling a function inside a traced loop is cheaper now too. Two mechanisms, in
order of preference:

1. `tiny_inline_kind` (compile time, `src/compiler.c`): a two-argument function
   whose whole body is one operation is collapsed at the call site, with no call
   frame at all. Besides the arithmetic/compare/`return arg` shims it now also
   covers the length shims `return a + #b`, `return #a`, `return #b`, which is
   the shape of accumulation loops such as `acc = add(acc, "df")`. The
   classifier strips trailing `OP_RETURN` bytes and accepts the optimiser's
   folded `OP_IADD`/`OP_FADD`/`OP_QFADD`.
2. `jit_inline_call_program()` (run time): small straight-line callees that are
   not one operation (`return a + 1` bodies with locals, for example) are
   evaluated in place instead of re-entering `execute_program()`, because the
   nested-interpreter prologue (frame setup, stack probe, argument copies,
   program checks) cost more than the callee body itself.

Callees with branches, nested calls or table access still take the generic path:
correct, just not inlined. Measured on `bench_calls` (6,000,000 calls of
`fn add(a: int, b: str) { return a + #b }`, release build): 0.29 s before this
work, 0.15 s with the in-place evaluator, 0.090 s with the length shims. For
reference on the same machine: CPython 3.14 takes 0.16 s, LuaJIT 0.088 s when it
does not eliminate the loop (0.0015 s when it does, which is its optimiser
proving the loop invariant — Toi cannot do that yet).

Merged control flow (an `if`/`else` body, or any `and` / `or` short-circuit that
leaves a value on the stack) still stays on the IR executor. The emitter can
materialize the pending operand stack on both edges (see the write-through /
`MERGE_TO_SCRATCH` path in `toi_jit_arm64_compile_native_trace_direct_core`), and
that keeps `and` chains correct, but the spilling is slower than the IR executor
for merged loops (app pipeline: 0.43 s traced vs 0.24 s on the IR path), so
`toi_jit_native_trace_is_exec_safe()` only accepts branches that jump to the end
of the body. Run `TOI_JIT_DEBUG_DIRECT_CORE=1` to dump the native instruction
stream when working on that path.

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
