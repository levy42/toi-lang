# Native JIT backends

The SysV x86-64 backend (Linux and Intel macOS) and the ARM64 backend expose the
same loop compiler interfaces. `TOI_JIT=0` selects the interpreter;
`TOI_JIT_BACKEND=none` keeps the shared JIT executors but disables the native
backend. Apple Silicon uses ARM64; the backend distinction is CPU architecture,
not operating system.

## x86-64 code generation

`src/jit_x64_codegen.h` is the private emitter implementation included by
`src/jit_x64.c`. It emits instructions directly for:

- Empty range loops, numeric reductions, and numeric slot loops.
- Scalar SSE2 arithmetic, numeric comparisons, and loop/branch control flow.
- Local Value loads/stores, constants, upvalues, and numeric type guards.
- Dense numeric array reads, with runtime lookup for negative, sparse,
  fractional, missing, or metatable-bearing keys.
- Identity-guarded tiny numeric, argument-return, and string-length callees.
- Links to native entries for larger acyclic, side-effect-free compiled callees. The
  function-to-trace lowerer is shared with ARM64 in `src/jit_native_lower.h`.
- Native trace bodies containing table writes/appends, calls, method invokes,
  and string construction through the shared runtime primitives.

Numeric expression values and guard-proven numeric locals/upvalues stay in XMM
registers. Value traces lazily spill promoted values at helper calls and control
flow joins; each guard exit records which registers must be materialized. Local
stores preserve snapshots already on the operand stack. Live XMM values are
saved across `fmod`; division/truncation/multiplication would change floating
point remainder semantics for large values, infinities, and NaNs.

Generated code preserves SysV callee-saved registers and call alignment.
Executable pages are writable during emission and read/execute after publication.
The code bundle owns its generated memory, embedded trace context, and linked
callee entries. Calls use four-lane polymorphic caches and native entry
trampolines, including runtime discovery of callees not known at compile time.
Caches stop growing after 64 owned callee entries per trace; further targets use
the existing runtime path. Published entries are immutable and remain alive
until their parent trace is freed. Cached closures and nested code contexts are
marked by the JIT GC root traversal.

String-key loads have native slot-cache hit paths. Cache guards check table
identity, shape, capacity, and the current entry key; reads fetch the current
value, including method replacements that leave the shape unchanged. Generic
stores use the same slot cache and runtime metadata updates. Build-string/store/
pop sequences write directly to the destination local when no branch enters the
fused sequence.

## Continuations and fallback

A completed numeric-for body returns `TOI_JIT_NATIVE_RESUME_BACKEDGE`, allowing
the interpreter to advance the induction variable before running another body.
A failed operation within a direct trace returns its original bytecode offset
and pending operands. Previously completed stores and side effects remain
committed: resuming the entire body would repeat them.

`TOI_JIT_NATIVE_TRACE_FORCE_BRIDGE=1` selects the reference C bridge instead of
direct trace emission. Unsupported expression depths or control-flow shapes
also retain the existing fallback paths.

## ARM64 parity audit

The x64 implementation covers the active ARM64 backend optimization classes:

| Active path | x64 implementation / direct check |
| --- | --- |
| Range validation, empty loops, numeric and slot loops | Emitted code; boundary and malformed-bytecode checks |
| Numeric temporary/local/upvalue promotion | XMM values; guard, alias, helper-spill and merge checks |
| Dense numeric table reads | Native loads with full-runtime slow edges |
| String-key table caches | Native four-lane hit path; replacement, deletion and rehash checks |
| Table writes/appends | Runtime primitives and cached string stores |
| Tiny-call inlining | Identity guards and polymorphic fallback |
| Compiled callee entries | Native one-shot entry and cached program-dispatch trampolines |
| Polymorphic call sites | Four lanes, bounded ownership, native hit counters and GC roots |
| Method lookup/call caches | Live value/shape checks and implicit-self handling |
| String-result store fusion | Fused destination store with control-flow validation |
| Precise side exits | Materialized pending operands and committed prior effects |

The shared VM safety gates still exclude certain branch merges and dynamic-call
traces on both architectures. Supporting an operation in a backend does not
mean every source loop containing it reaches that backend. Value-loop fallback,
trace selection, quickening and profitability decisions remain shared.

ARM64's three-part build-string/lookup fusion is disabled (`if (0 && ...)`) and
is not an active optimization to reproduce. Its optional native program-entry
preference currently requires a call-containing program that the shared
chain-safe predicate excludes; x64 instead links eligible acyclic, side-effect-
free bodies directly and uses guarded program dispatch for other callees. The
cache layouts and register assignments are architecture-specific. Parity here
means active execution/optimization capabilities, not identical instructions or
equal wall-clock speed on different processors. ARM64 execution still needs to
be validated on an ARM64 machine.

`toi_jit_print_stats` prints an additional `[vm-native-jit-x64]` line. Promotion
and fusion counts describe emitted operations; PIC, chain and table-cache hits
are runtime counters. `toi_jit_x64_code_stats` exposes per-code-bundle counters
for the direct backend tests.

## Verification

Use the repository's compiler flags and dependency prerequisites, then run:

```sh
make test-jit-x64
make test
make test-jit
make verify
```

The direct backend tests execute generated functions through the public backend
interfaces. Native trace tests require the direct-emission indicator, avoiding
a false pass caused by interpreter fallback. They exercise loop boundaries,
branch joins, arithmetic edge cases, table access, upvalues, strings, call
identity changes, bounded batches, and deoptimization after a side effect.

The benchmark verifier compares result values across interpreter, normal JIT,
low hot-threshold JIT, and backend-disabled configurations. Keep those result
checks when evaluating timing improvements.

Quickened two-argument calls (`QCALL2`) use their dedicated closure cache when
seeding loop call hints and lower to native calls with arity two. This matters
for source-level hot loops: the generic call cache does not hold that target.
Tiny inline calls guard both closure identity and declared argument types;
a failed guard resumes at the original call with its operands intact.

Known allocation-free native callees use direct entry calls guarded by closure
identity and declared argument types. These bypass the C PIC scan; dynamically
discovered targets and callees requiring runtime execution keep their caches.
A failed native callee returns before publishing its result, so its original
call operands remain available for interpreter fallback.

Straight-line x64 loops whose calls are inlined, directly linked, or guarded
zero-argument random builtins, with no allocating or user-code side-effect
operations, execute consecutive native
batches of up to 4,096 iterations. A directly linked callee may contain branches;
its native lowering must still be acyclic and side-effect-free. Other traces
retain the existing batch size and sampling. `inlined_calls`, `linked_calls`, `random_calls`
and `fast_call_loops` report emitted paths; `native_chain_hits` counts successful
linked entry executions, while `call_pic_hits` counts runtime cache hits.

Run `TOI_JIT=1 ./toi benchmarks/perf_calls.toi` to measure arithmetic and branching
calls that are too large for tiny-call inlining. Compare the printed results
with `TOI_JIT=0` before comparing elapsed times.

Zero-argument module `random()` invokes have a specialized allocation-free x64
runtime path that avoids generic native-call dispatch and VM stack staging.
It validates the live table entry's native fast kind on every draw, including
replacements that do not change the table shape. Non-module receivers, custom
methods and metatables leave native execution at the original invoke, before
consuming a random draw. Range calls and first-class builtin calls retain their
existing dispatch paths. This specialization keeps libc `rand()` and the
existing seed/stream behavior; it does not change the RNG algorithm.

Length and slice operations lower through the shared value/native IR. x64 string
lengths are direct field loads; table lengths use the existing counting helper.
String slices use the shared primitive with guarded bounds/strides and preserve
all four operands on failure. Table/metamethod slices resume in the VM. ARM64
currently executes the new operations through its C bridge/value fallback.

Straight-line traces of primitive string builders and slices, without calls or
table operations, run bounded 4,096-iteration batches (`fast_string_loops`). GC
checks run after completed allocating batches, with frame values committed and
no pending operands, so compiled loops cannot defer collection indefinitely.
The two-invoke safety gate still keeps the upper/lower/slice benchmark in the
shared mixed executor; native string support does not yet remove that gate.

The general f-string content cache is weak. GC clears unmarked entries before
sweeping them, while ordinary program roots and the separate three-string cache
still preserve live outputs. Stack-built f-strings copy directly into one object
allocation with inline character storage; externally owned buffers retain their
existing `take_string` ownership. Both storage forms use the same accounting and
GC traversal. Library and JIT case conversion share the same implementation,
including lazy hashing, ASCII SIMD for long inputs, and unchanged-string reuse.
