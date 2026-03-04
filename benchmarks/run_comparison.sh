#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CSV_FILE="$ROOT_DIR/benchmarks/perf_results.csv"
PYA=""
if command -v python3.14 >/dev/null 2>&1; then
  PYA="python3.14"
elif command -v python3 >/dev/null 2>&1; then
  PYA="python3"
elif command -v python >/dev/null 2>&1; then
  PYA="python"
fi

if [ ! -x "$ROOT_DIR/toi" ]; then
  echo "toi binary not found. Run 'make release-perf' (or 'make') first." >&2
  exit 1
fi

if [ -z "$PYA" ]; then
  echo "Python not found (tried python3.14, python3, python)." >&2
  exit 1
fi

LUA_BIN=""
if command -v luajit >/dev/null 2>&1; then
  LUA_BIN="luajit"
elif command -v lua >/dev/null 2>&1; then
  LUA_BIN="lua"
fi

if [ -z "$LUA_BIN" ]; then
  echo "Lua runtime not found. Install 'luajit' or 'lua' to run comparison." >&2
  exit 1
fi

RUN_TS=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
GIT_SHA="unknown"
if command -v git >/dev/null 2>&1; then
  if git -C "$ROOT_DIR" rev-parse --short HEAD >/dev/null 2>&1; then
    GIT_SHA=$(git -C "$ROOT_DIR" rev-parse --short HEAD)
  fi
fi

if [ ! -f "$CSV_FILE" ]; then
  cat > "$CSV_FILE" <<'EOF'
timestamp_utc,git_sha,runtime,binary,version,math,random,string_concat,string_ops,table,sort,list_comprehension,calls,f_string,app_pipeline,app_handlers,memory_sec,memory_bytes
EOF
fi

metric_sec() {
  file="$1"
  key="$2"
  awk -v k="$key" '$0 ~ ("^" k " ") && $NF == "sec" { print $(NF-1); exit }' "$file"
}

toi_memory_bytes() {
  file="$1"
  awk '/^memory [0-9]+ bytes$/ { print $2; exit }' "$file"
}

python_memory_bytes() {
  file="$1"
  awk '/^memory current [0-9]+ bytes/ { print $3; exit }' "$file"
}

append_row() {
  runtime="$1"
  binary="$2"
  version="$3"
  out_file="$4"
  memory_bytes="$5"
  list_comp="$6"

  math_v=$(metric_sec "$out_file" "math")
  random_v=$(metric_sec "$out_file" "random")
  string_concat_v=$(metric_sec "$out_file" "string concat")
  string_ops_v=$(metric_sec "$out_file" "string ops")
  table_v=$(metric_sec "$out_file" "table")
  sort_v=$(metric_sec "$out_file" "sort")
  calls_v=$(metric_sec "$out_file" "calls")
  f_string_v=$(metric_sec "$out_file" "f-string")
  app_pipeline_v=$(metric_sec "$out_file" "app pipeline")
  app_handlers_v=$(metric_sec "$out_file" "app handlers")
  memory_sec_v=$(metric_sec "$out_file" "memory")

  csv_escape() {
    printf '"%s"' "$(printf '%s' "$1" | sed 's/"/""/g')"
  }

  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$(csv_escape "$RUN_TS")" "$(csv_escape "$GIT_SHA")" "$(csv_escape "$runtime")" "$(csv_escape "$binary")" "$(csv_escape "$version")" \
    "${math_v:-}" "${random_v:-}" "${string_concat_v:-}" "${string_ops_v:-}" "${table_v:-}" "${sort_v:-}" \
    "${list_comp:-}" "${calls_v:-}" "${f_string_v:-}" "${app_pipeline_v:-}" "${app_handlers_v:-}" \
    "${memory_sec_v:-}" "${memory_bytes:-}" >> "$CSV_FILE"
}

PY_OUT=$(mktemp)
LUA_OUT=$(mktemp)
TOI_OUT=$(mktemp)
TOI_ERR=$(mktemp)
trap 'rm -f "$PY_OUT" "$LUA_OUT" "$TOI_OUT" "$TOI_ERR"' EXIT INT TERM

printf "\nPython (%s)\n\n" "$PYA"
"$PYA" "$ROOT_DIR/benchmarks/perf.py" | tee "$PY_OUT"
PY_VERSION=$("$PYA" --version 2>&1 | head -n 1 | sed 's/^Python //')
append_row "python" "$PYA" "$PY_VERSION" "$PY_OUT" "$(python_memory_bytes "$PY_OUT")" "$(metric_sec "$PY_OUT" "list comprehension")"

printf "\nLua (%s)\n\n" "$LUA_BIN"
"$LUA_BIN" "$ROOT_DIR/benchmarks/perf.lua" | tee "$LUA_OUT"
LUA_VERSION=$("$LUA_BIN" -v 2>&1 | head -n 1 | sed 's/^LuaJIT //; s/^Lua //')
append_row "lua" "$LUA_BIN" "$LUA_VERSION" "$LUA_OUT" "" ""

printf "\nToi\n\n"
"$ROOT_DIR/toi" "$ROOT_DIR/benchmarks/perf.toi" >"$TOI_OUT" 2>"$TOI_ERR"
cat "$TOI_OUT"
if [ -s "$TOI_ERR" ]; then
  cat "$TOI_ERR" >&2
fi
TOI_VERSION="unknown"
append_row "toi" "toi" "$TOI_VERSION" "$TOI_OUT" "$(toi_memory_bytes "$TOI_OUT")" "$(metric_sec "$TOI_OUT" "list comprehension")"

printf "\nResults appended to %s\n" "$CSV_FILE"
printf "\nDone!\n"
