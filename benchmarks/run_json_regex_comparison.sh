#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

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

echo "== Python =="
python3 benchmarks/json_regex_bench.py

echo
echo "== Lua ($LUA_BIN) =="
"$LUA_BIN" benchmarks/json_regex_bench.lua

echo
echo "== Toi =="
./toi benchmarks/json_regex_bench.toi
