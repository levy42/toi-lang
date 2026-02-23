#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

echo "== Python =="
python3 benchmarks/json_regex_bench.py

echo
echo "== Lua =="
lua benchmarks/json_regex_bench.lua

echo
echo "== Toi =="
./toi benchmarks/json_regex_bench.toi
