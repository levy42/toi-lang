#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PYA=""
if command -v python3.14 >/dev/null 2>&1; then
  PYA="python3.14"
elif command -v python3 >/dev/null 2>&1; then
  PYA="python3"
elif command -v python >/dev/null 2>&1; then
  PYA="python"
fi

if [ ! -x "$ROOT_DIR/pua" ]; then
  echo "pua binary not found. Run 'make' first." >&2
  exit 1
fi

if [ -z "$PYA" ]; then
  echo "Python not found (tried python3.14, python3, python)." >&2
  exit 1
fi

if ! command -v lua >/dev/null 2>&1; then
  echo "Lua not found. Install 'lua' to run comparison." >&2
  exit 1
fi

printf "\nPython (%s)\n\n" "$PYA"
"$PYA" "$ROOT_DIR/benchmarks/perf.py"

printf "\nLua\n\n"
lua "$ROOT_DIR/benchmarks/perf.lua"

printf "\nPua\n\n"
"$ROOT_DIR/pua" "$ROOT_DIR/benchmarks/perf.pua"

printf "\nDone!\n"
