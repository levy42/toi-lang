#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

echo "Start one server at a time, then benchmark it with:"
echo "  env -u NO_COLOR oha -z 30s -c 16 --no-tui http://127.0.0.1:8080/api/sleep"
echo
echo "Toi (selector loop, yielding coroutine.sleep(10 ms)):"
echo "  ./toi benchmarks/http_json_sleep_bench.toi"
echo
echo "FastAPI/Uvicorn (async 10 ms sleep):"
echo "  uvicorn benchmarks.http_json_sleep_bench_fastapi:app --host 127.0.0.1 --port 8080"
