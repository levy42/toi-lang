#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOI="$ROOT/toi"
TMP_DIR="$(mktemp -d /tmp/toi-fmt-regression-XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

check_equal() {
  local got="$1"
  local expected="$2"
  local label="$3"
  if [[ "$got" != "$expected" ]]; then
    echo "--- expected ($label) ---" >&2
    printf "%s" "$expected" >&2
    echo >&2
    echo "--- got ($label) ---" >&2
    printf "%s" "$got" >&2
    echo >&2
    fail "$label"
  fi
}

# Case 1: Normalize indentation width to 2 spaces.
cat > "$TMP_DIR/normalize_in.toi" <<'CASE1'
fn a()
    print 1
    if x
        print 2
CASE1
cat > "$TMP_DIR/normalize_expected.toi" <<'CASE1E'
fn a()
  print 1
  if x
    print 2
CASE1E
normalize_got="$($TOI fmt "$TMP_DIR/normalize_in.toi")"
normalize_expected="$(cat "$TMP_DIR/normalize_expected.toi")"
check_equal "$normalize_got" "$normalize_expected" "normalize indentation"

# Case 2: Preserve multiline [[...]] payload exactly.
cat > "$TMP_DIR/multiline_in.toi" <<'CASE2'
fn a()
    local s = [[
      keep this
   exact left padding
    ]]
    print s
CASE2
cat > "$TMP_DIR/multiline_expected.toi" <<'CASE2E'
fn a()
  local s = [[
      keep this
   exact left padding
    ]]
  print s
CASE2E
multiline_got="$($TOI fmt "$TMP_DIR/multiline_in.toi")"
multiline_expected="$(cat "$TMP_DIR/multiline_expected.toi")"
check_equal "$multiline_got" "$multiline_expected" "multiline string preservation"

# Case 3: Idempotence.
idem_once="$($TOI fmt "$TMP_DIR/multiline_in.toi")"
idem_twice="$(printf "%s" "$idem_once" | $TOI fmt -)"
check_equal "$idem_twice" "$idem_once" "idempotence"

# Case 4: --check exit codes.
printf "%s" "$normalize_expected" > "$TMP_DIR/formatted.toi"
if ! $TOI fmt --check "$TMP_DIR/formatted.toi" >/dev/null; then
  fail "--check should return 0 for formatted file"
fi

if $TOI fmt --check "$TMP_DIR/normalize_in.toi" >/dev/null; then
  fail "--check should return non-zero for unformatted file"
fi

if $TOI fmt --check -w "$TMP_DIR/formatted.toi" >/dev/null 2>&1; then
  fail "--check with -w should fail"
fi

echo "Formatter regression tests: PASS"
