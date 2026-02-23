#!/usr/bin/env python3
import json
import time


def assert_true(cond, msg="assert_true failed"):
    if not cond:
        raise AssertionError(msg)


payload = {
    "name": "Ada",
    "active": True,
    "score": 98.5,
    "tags": ["toi", "json", "perf"],
    "nested": {
        "retries": 3,
        "timeout_ms": 250,
        "ok": True,
    },
    "users": [
        {"id": 1, "role": "admin"},
        {"id": 2, "role": "staff"},
        {"id": 3, "role": "member"},
    ],
}

# Correctness sanity
encoded = json.dumps(payload, separators=(",", ":"))
assert_true(isinstance(encoded, str), "json.dumps should return string")

decoded = json.loads(encoded)
assert_true(isinstance(decoded, dict), "json.loads should return dict")
assert_true(decoded["name"] == "Ada", "decoded.name mismatch")
assert_true(decoded["active"] is True, "decoded.active mismatch")
assert_true(decoded["nested"]["retries"] == 3, "decoded nested mismatch")
assert_true(decoded["tags"][0] == "toi", "decoded array mismatch")
assert_true(decoded["users"][2]["role"] == "member", "decoded array object mismatch")

# Benchmark encode
encode_iters = 4000
start = time.perf_counter()
for _ in range(encode_iters):
    encoded = json.dumps(payload, separators=(",", ":"))
encode_elapsed = time.perf_counter() - start
assert_true(encode_elapsed >= 0, "encode benchmark clock went backwards")

# Benchmark decode
decode_iters = 4000
start = time.perf_counter()
for _ in range(decode_iters):
    decoded = json.loads(encoded)
decode_elapsed = time.perf_counter() - start
assert_true(decode_elapsed >= 0, "decode benchmark clock went backwards")

print(
    f"json perf (python): encode={encode_elapsed:.6f}s "
    f"decode={decode_elapsed:.6f}s payload_bytes={len(encoded)}"
)
