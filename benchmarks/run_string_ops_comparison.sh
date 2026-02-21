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

printf "\nPua\n\n"
"$ROOT_DIR/pua" "$ROOT_DIR/benchmarks/string_ops_compare.pua"

printf "\nPython (%s)\n\n" "$PYA"
"$PYA" "$ROOT_DIR/benchmarks/string_ops_compare.py"

printf "\nDone!\n"
