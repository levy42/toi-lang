# Performance audit (local run)

This audit was run in the container using `make release-perf` and the built-in benchmarks.

## What was measured

- `./toi benchmarks/perf.toi`
- `./toi benchmarks/string_ops_compare.toi`
- `./toi benchmarks/json_regex_bench.toi`
- `python3 benchmarks/perf.py`
- `python3 benchmarks/string_ops_compare.py`
- `python3 benchmarks/json_regex_bench.py`
- `make perf` (`tests/peft/btree_perf.toi`)

## Key findings

1. **Regex hot path is the largest performance gap for real workloads using repeated patterns.**
   - Toi regex search/replace in `benchmarks/json_regex_bench.toi` took ~2.52s / ~2.43s for 120k iterations.
   - Python in the sibling benchmark took ~0.108s / ~0.182s.
   - This is expected given current API usage: `regex.search(pattern, text)` and `regex.replace(pattern, ...)` recompile the pattern every call.

2. **Compiled regex usage is much faster, but not the default fast path.**
   - Local repro script in this audit:
     - `regex.search(pattern, text)` loop: ~2.52s
     - `compiled = regex.compile(pattern); compiled.search(text)` loop: ~0.32s
   - That is about an **8x speedup** by avoiding repeated `regcomp`/`regfree` churn.

3. **String transforms/slicing are slower than Python for tight loops.**
   - Toi (`iter=200000`): `upper ~0.063s`, `lower ~0.059s`, `substring ~0.089s`.
   - Python (`iter=200000`): `upper ~0.030s`, `lower ~0.032s`, `substring ~0.033s`.
   - Indicates per-call allocation and copy overhead in string operations.

4. **B-tree throughput is solid in current microbench.**
   - `tests/peft/btree_perf.toi` with `n=100000` produced:
     - insert ~91k op/s
     - lookup ~210k op/s
     - delete ~90k op/s

## Concrete fixes

### 1) Regex API fast path (highest impact)

- **Immediate usage fix (no runtime changes):**
  - For repeated patterns, use:
    - `compiled = regex.compile(pattern)`
    - `compiled.search(text)` / `compiled.match(text)` / `compiled.finditer(text)`
- **Runtime fix (recommended):**
  - Add an internal small LRU cache in `src/lib/regex.c` keyed by `(pattern, flags)` for module-level `regex.search/match/replace/split/finditer`.
  - Keep it bounded (e.g., 32-128 entries) and invalidate on GC teardown.
  - This preserves ergonomic APIs while eliminating repeated `regcomp` for stable patterns.

### 2) String op optimization

- Add ASCII fast paths and avoid unnecessary heap churn for:
  - `upper/lower`
  - short-range slicing
- Consider returning interned/reused strings where safe for no-op transforms.

### 3) VM call overhead

- `benchmarks/perf.toi` shows `calls` as one of the more expensive categories.
- Potential improvements:
  - specialize zero/small-arity call opcodes,
  - reduce stack shuffling for common call forms,
  - inline native fast paths for common builtin calls.

## Build-system observation

`make release-perf` initially failed in this environment because POSIX networking symbols were hidden unless feature-test macros were enabled. The Makefile now defines `_POSIX_C_SOURCE=200809L` in `EXTRA_CFLAGS` so perf builds are reproducible on glibc-like environments.
