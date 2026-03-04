# JIT Optimization Ledger

Date started: 2026-03-04
Owner: Codex + Levy
Purpose: Track every JIT perf attempt and its outcome so we do not repeat failed ideas.

## Status Legend
- `kept`: currently in tree and intended to stay
- `rolled_back`: attempted and removed after validation
- `superseded`: replaced by a newer approach
- `unknown`: historical item seen in metrics/discussion but not fully reconstructed

## Baseline Snapshots
- Early stable app baseline (session reference): `app pipeline ~0.100-0.101`, `app handlers ~0.076-0.077`, `calls ~0.0211`, `f-string ~0.0218-0.0224`
- Pre-ledger regression sample (session reference): `app pipeline ~0.273`, `app handlers ~0.217` (known bad state)

## Historical Reconstruction (Pre-ledger, from CSV)
Source used: `benchmarks/perf_results.csv` (all rows currently under `git_sha=e1a1cc2`), so attribution to exact code changes is inferential.

| Hist ID | Timestamp (UTC) | Observation | Inference | Confidence |
|---|---|---|---|---|
| HIST-0001 | 2026-03-02 14:34..15:20 | Stable band: `calls ~0.050-0.053`, `app pipeline ~0.104-0.109`, `app handlers ~0.086-0.090` | Pre major-call optimization baseline | medium |
| HIST-0002 | 2026-03-02 15:11:56 | Severe regression: `calls 0.0745`, `app pipeline 0.1755`, `app handlers 0.1557` | Broken/partial optimization state | medium |
| HIST-0003 | 2026-03-02 18:11:59 | Best early app sample: `app pipeline 0.098948`, `app handlers 0.080730` | Temporary good state before later instability | low |
| HIST-0004 | 2026-03-02 18:14:49 / 18:42:21 / 18:58:01 | Extreme `app pipeline` spikes: `0.305`, `0.399`, `0.289` | Non-deterministic path or unstable optimization | medium |
| HIST-0005 | 2026-03-03 13:51:52 / 14:12:02 | Severe global regression: `calls ~0.25`, `f-string ~0.043`, `app handlers ~0.30` | Major call-path regression before fix | high |
| HIST-0006 | 2026-03-03 13:52:11 / 14:12:57 | Post-fix partial recovery: `calls ~0.072-0.075`, `app pipeline ~0.104`, `app handlers ~0.154-0.160` | Correctness fixed but perf still poor in handlers | high |
| HIST-0007 | 2026-03-03 17:42:26 | Step improvement: `calls 0.0217`, `app pipeline 0.1019`, `app handlers 0.0765` | Large call-path win landed | high |
| HIST-0008 | 2026-03-04 16:33:04 | Best known in CSV: `calls 0.021389`, `app pipeline 0.100106`, `app handlers 0.075701` | Current pre-session best | high |

Notes:
- Since commit SHA does not change across these rows, this table is trend-based, not commit-based.
- Historical entries are intentionally marked separate from `JIT-` IDs to avoid false precision.

## Attempt Log

| ID | Date | Area | Change | Status | Result / Evidence |
|---|---|---|---|---|---|
| JIT-0001 | 2026-03-04 | CALL/INVOKE direct-core | Skip some call staging/materialization paths | rolled_back | Regressed app/f-string in perf runs |
| JIT-0002 | 2026-03-04 | Call helper refactor | Dispatch helper restructuring (`*_native_direct` style path) | rolled_back | Major regressions (notably f-string/app) |
| JIT-0003 | 2026-03-04 | CALL fast path | Contiguous-local all-values “skip staging entirely” | rolled_back | Mixed/noisy, generally worse than best-known |
| JIT-0004 | 2026-03-04 | JIT lookup cost | Add tracked-function hash cache (O(1) hot lookup) | kept | Neutral-to-small gain; no regressions |
| JIT-0005 | 2026-03-04 | Closure execute API | Add `toi_jit_try_execute_compiled_with_args` fast entry | kept | Neutral-to-small gain; stable correctness |
| JIT-0006 | 2026-03-04 | ARM64 call dispatch | Prefer compiled-closure fast API in call dispatch | kept | Small/mostly noise improvement |
| JIT-0007 | 2026-03-04 | CALL hinting | Seed `JOP_CALL*` constants from bytecode call IC lanes | kept | Enabled stronger guarded call lowering |
| JIT-0008 | 2026-03-04 | ARM64 guarded call | Add closure-specialized dispatch helper for guarded CALL | kept | Small improvement in some runs |
| JIT-0009 | 2026-03-04 | ARM64 call staging | Direct pointer for local callee (skip callee copy) | kept | Small/mostly noise gain |
| JIT-0010 | 2026-03-04 | Native trace metadata | Add `aux_data` per native-trace instruction | kept | Infrastructure for direct program hints |
| JIT-0011 | 2026-03-04 | Program-hint execution | Seed compiled program ptr on guarded CALL + direct execute API (`toi_jit_execute_program_direct_with_args`) | kept | Best samples improved (`app pipeline` seen to ~0.0997, `app handlers` ~0.0760) but still not step-change |
| JIT-0012 | 2026-03-04 | ARM64 guarded call | Split guarded closure path into direct execute call plus explicit fallback helper | rolled_back | Regressed (`app pipeline` ~0.106, `app handlers` ~0.080-0.081) versus JIT-0011 baseline |

## Current Best Observed (Session)
- `calls`: ~0.0208-0.0215
- `f-string`: ~0.0223-0.0232
- `app pipeline`: best seen ~0.0997, typical ~0.102
- `app handlers`: best seen ~0.0760, typical ~0.076-0.078

## Explicitly Avoid Re-trying (Without New Enabler)
- Re-run of raw call-staging skip variants from JIT-0001/0003.
- Re-run of helper-only dispatch reshuffles from JIT-0002.
- Re-run of split guarded-call direct+fallback dispatch shape from JIT-0012.
- Micro specialization by string concatenated route key only (no structural change).

## Next Major Candidate (Not Yet Implemented)
- Caller-to-callee trace linking/inlining:
  - eliminate helper call boundary for hot guarded closure calls
  - jump directly into callee compiled trace/body with deopt mapping
  - expected to be higher leverage than additional dispatch micro-optimizations

## Update Protocol (Every Perf Change)
1. Add one row here before implementation with provisional status `unknown`.
2. Run:
   - `make release-perf`
   - `./toi benchmarks/perf.toi` (>=3 runs)
   - `make test`
3. Update row status to `kept` or `rolled_back` with measured outcome.
4. If rolled back, keep the row and include why it failed.
