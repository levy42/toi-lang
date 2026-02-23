import json
import re
import time

PAYLOAD = {
    "name": "Ada",
    "active": True,
    "score": 98.5,
    "tags": ["toi", "json", "perf"],
    "nested": {"retries": 3, "timeout_ms": 250, "ok": True},
    "users": [
        {"id": 1, "role": "admin"},
        {"id": 2, "role": "staff"},
        {"id": 3, "role": "member"},
    ],
}

ENCODE_ITERS = 20000
DECODE_ITERS = 20000
REGEX_ITERS = 120000
TEXT = "order-123 shipped\norder-456 pending\norder-789 shipped"
PATTERN = re.compile(r"order-(\d+)\s+(shipped|pending)")


def bench(name, fn):
    start = time.perf_counter()
    result = fn()
    elapsed = time.perf_counter() - start
    print(f"{name}: {elapsed:.6f}s")
    return result, elapsed


def bench_json_encode():
    encoded = ""
    for _ in range(ENCODE_ITERS):
        encoded = json.dumps(PAYLOAD, separators=(",", ":"))
    return encoded


def bench_json_decode(encoded):
    decoded = None
    for _ in range(DECODE_ITERS):
        decoded = json.loads(encoded)
    return decoded


def bench_regex_search():
    total = 0
    for _ in range(REGEX_ITERS):
        m = PATTERN.search(TEXT)
        total += int(m.group(1))
    return total


def bench_regex_replace():
    out = ""
    for _ in range(REGEX_ITERS):
        out = PATTERN.sub(r"id=\\1 status=\\2", TEXT)
    return len(out)


if __name__ == "__main__":
    print("Python json/regex benchmark")
    print(
        f"iterations: encode={ENCODE_ITERS} decode={DECODE_ITERS} regex={REGEX_ITERS}"
    )
    encoded, _ = bench("json encode", bench_json_encode)
    decoded, _ = bench("json decode", lambda: bench_json_decode(encoded))
    regex_total, _ = bench("regex search", bench_regex_search)
    replace_len, _ = bench("regex replace", bench_regex_replace)
    print(f"payload bytes: {len(encoded)}")
    print(
        "results:"
        f" decoded_name={decoded['name']}"
        f" decoded_users={len(decoded['users'])}"
        f" regex_total={regex_total}"
        f" replace_len={replace_len}"
    )
