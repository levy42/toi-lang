#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

if ! command -v oha >/dev/null 2>&1 && ! command -v wrk >/dev/null 2>&1; then
  echo "Install oha or wrk to run the load generator." >&2
  exit 1
fi

echo "Start one server at a time, then run the matching load command."
if command -v oha >/dev/null 2>&1; then
  echo "  env -u NO_COLOR oha -z 30s -c 64 --no-tui http://127.0.0.1:8080/api"
else
  echo "  wrk -t4 -c64 -d30s http://127.0.0.1:8080/api"
fi
echo
echo "Toi server:"
echo "  ./toi benchmarks/http_json_bench.toi"
echo
echo "FastAPI with Uvicorn (install fastapi and uvicorn first):"
echo "  uvicorn benchmarks.http_json_bench_fastapi:app --host 127.0.0.1 --port 8080"
echo
echo "Both serve the same dictionary at GET /api. Use the same load command for each."
