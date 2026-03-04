# Native JIT Rewrite Plan (Temporary)

Date: 2026-03-02
Owner: Codex + Levy
Status: Active

Companion tracking doc: `docs/jit_optimization_ledger.md` (must be updated for every perf attempt, including rollbacks).

## Goal
Deliver a real native JIT path that materially improves real-app benchmarks (`app pipeline`, `app handlers`) by increasing native execution share of hot loops.

## Non-Goals
- No benchmark-specific opcode hacks.
- No new typed/value C-runner micro-optimizations unless required to keep correctness.
- No widening of interim fast paths as substitute for native codegen.

## Current Baseline (as of 2026-03-02)
- Toi app pipeline: ~0.106-0.108s
- Toi app handlers: ~0.088-0.090s
- Native loop exec share is very low (`backend_loop_exec_hits` near zero relative to `loop_exec_hits`).

## Hard Rule
Any change that does not increase native execution share or improve real-app benchmarks is reverted.

## Plan

### Phase 0: Lock Scope and Guardrails
1. Freeze typed/value fast-path feature work.
2. Keep only correctness fixes in typed/value runners.
3. Add a PR/checklist gate: every JIT change must report:
   - native exec share
   - app pipeline/app handlers delta
   - whether fallback rate changed

Acceptance:
- No new typed/value feature expansions merged during rewrite.

### Phase 1: Native Trace IR + Deopt Contract
1. Define a single native trace IR for loop traces (SSA-like or stack-machine + explicit guards).
2. Define deopt frame-state map:
   - locals/upvalues/stack mapping at each guard site.
3. Define guard failure reason taxonomy (type/shape/call target/metatable/etc).

Acceptance:
- Can lower existing hot loop traces into IR with guard metadata.
- Deopt map validated by tests (correct resume in interpreter).

### Phase 2: ARM64 Native Emitter Skeleton (No C Runner Loop)
1. Emit native prologue/epilogue and branch/control flow.
2. Emit locals/stack moves and arithmetic/comparisons.
3. Emit guard check + side exit plumbing.

Acceptance:
- At least one non-trivial loop executes as emitted machine code.
- Side exits correctly resume interpreter without corruption.

### Phase 3: Real-App Opcode Set (First Vertical Slice)
Implement native lowering + guarded helper callouts for:
1. `GET_TABLE`, `GET_TABLE_NUM`
2. `SET_TABLE`, `SET_TABLE_NUM`
3. `CALL`, `INVOKE`
4. string build ops used in app benchmark (`BUILD_STRING2..5`)

Acceptance:
- On app benchmark, native loop exec share >= 20%.
- No correctness regressions (`make test` passes).

### Phase 4: Raise Native Share and Stabilize
1. Add inline caches/shape guards where generic and reusable.
2. Reduce fallback/deopt churn via stable guard policy.
3. Expand supported ops only if they are common in app traces.

Acceptance:
- Native loop exec share >= 50% on app benchmark.
- `app pipeline` improves by >= 20% from baseline.

### Phase 5: Cleanup Interim Complexity
1. Remove dead or redundant typed/value fast-path branches replaced by native path.
2. Keep fallback interpreter as correctness net.
3. Simplify stats and code paths.

Acceptance:
- Lower code complexity with equal or better performance.

## Measurement Protocol (Mandatory)
For each milestone run:
1. `make release-perf`
2. `benchmarks/run_comparison.sh`
3. Record CSV row (already automated in `benchmarks/perf_results.csv`).
4. Report:
   - app pipeline / app handlers
   - native share = `backend_loop_exec_hits / loop_exec_hits`
   - top fallback reasons

## Revert Policy
Revert any change set that causes either:
1. Native share does not improve after two iterations, or
2. Real-app benchmarks regress >3% without measurable native-share gain.

## Immediate Next Tasks
1. Implement Phase 1 IR/deopt contract file structures in `src/jit*`.
2. Add explicit per-trace native/deopt counters for new pipeline.
3. Land first ARM64 emitted trace with side-exit restore test.

## Definition of Done
- Native backend executes the majority of hot app-loop iterations.
- Real-app benchmark improvement is sustained and visible in CSV trend.
- Interim workaround complexity is removed.
