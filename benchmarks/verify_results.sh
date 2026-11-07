#!/usr/bin/env sh
# Differential correctness check for the JIT.
#
# The benchmark harness reports a `result=` value per case. Running the same
# program with the interpreter and with different JIT configurations and then
# comparing those values is the cheapest way to catch a miscompiling JIT: a fast
# loop that computes the wrong answer looks great in a timing table.
#
# Usage:
#   benchmarks/verify_results.sh [program.toi]
#   VERIFY_STRICT=1 benchmarks/verify_results.sh   # also fail on known-open diffs
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TOI=$ROOT_DIR/toi
BENCH=${1:-$ROOT_DIR/benchmarks/perf.toi}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/toi-verify.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# Cases that still produce a different result under the JIT. They are reported
# (with values) but do not fail the run unless VERIFY_STRICT=1.
# Empty today: every perf.toi case matches the interpreter. When a new divergence
# is found, add its case name here with a TODO explaining the root cause.
KNOWN_OPEN=""

if [ ! -x "$TOI" ]; then
  echo "toi binary not found. Run 'make' first." >&2
  exit 1
fi

extract() {
  # "<case name> <seconds> sec result=<value>" -> "<case name>=<value>"
  sed -n 's/^\(.*\) [0-9][0-9.]* sec result=\(.*\)$/\1=\2/p' "$1"
}

value_of() {
  awk -F= -v n="$2" '$1 == n { print $2; exit }' "$1"
}

fail=0
total=0

run_case() {
  label=$1
  shift
  total=$((total + 1))
  if ! env "$@" "$TOI" "$BENCH" >"$WORK/$label.out" 2>"$WORK/$label.err"; then
    echo "FAILED  $label (program exited non-zero)"
    sed -n '1,10p' "$WORK/$label.err"
    fail=$((fail + 1))
    return
  fi
  extract "$WORK/$label.out" >"$WORK/$label.res"
  if [ ! -s "$WORK/$label.res" ]; then
    echo "FAILED  $label (no result= lines found)"
    fail=$((fail + 1))
    return
  fi
  awk -F= 'NR==FNR { base[$1]=$2; next }
           { if (!($1 in base)) print $1 " (missing from interpreter run)";
             else if (base[$1] != $2) print $1 }' \
      "$WORK/interpreter.res" "$WORK/$label.res" >"$WORK/$label.changed"
  if [ ! -s "$WORK/$label.changed" ]; then
    echo "ok      $label"
    return
  fi
  echo "DIFF    $label"
  unknown=0
  while IFS= read -r name; do
    base_v=$(value_of "$WORK/interpreter.res" "$name")
    case_v=$(value_of "$WORK/$label.res" "$name")
    case ",$KNOWN_OPEN," in
      *",$name,"*)
        echo "  known-open  $name (interpreter=$base_v $label=$case_v)"
        ;;
      *)
        echo "  REGRESSION  $name (interpreter=$base_v $label=$case_v)"
        unknown=$((unknown + 1))
        ;;
    esac
  done <"$WORK/$label.changed"
  if [ "$unknown" -gt 0 ] || [ "${VERIFY_STRICT:-0}" = "1" ]; then
    fail=$((fail + 1))
  fi
}

echo "Verifying $(basename "$BENCH") ..."
run_case interpreter TOI_JIT=0
run_case jit
run_case hot TOI_JIT_LOOP_HOT=8
run_case nobackend TOI_JIT_BACKEND=none

echo
if [ "$fail" -eq 0 ]; then
  echo "=== Results: $total configurations, no regressions ==="
  exit 0
fi
echo "=== Results: $fail configuration(s) diverged from the interpreter ==="
exit 1
