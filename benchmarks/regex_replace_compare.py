import re
import time

ITERS = 100_000
LONG_ITERS = 5_000
PATTERN = re.compile(r"[0-9]+")
ONE_MATCH = "order-123 shipped"
MANY_MATCHES = "order-123 shipped, order-456 pending, order-789 shipped"
LONG_TEXT = ", ".join(f"order-{i} shipped" for i in range(1, 101))
LONG_REPL = "replacement-text-that-is-longer-than-the-match"


def bench(name, text, repl, iters):
    expected = PATTERN.sub(repl, text)
    start = time.perf_counter()
    out = ""
    for _ in range(iters):
        out = PATTERN.sub(repl, text)
    elapsed = time.perf_counter() - start
    print(f"{name}: {elapsed:.6f}s (result_bytes={len(out)})")
    assert out == expected


print(f"Python re.sub: short_iterations={ITERS} long_iterations={LONG_ITERS}")
bench("one match", ONE_MATCH, "#", ITERS)
bench("three matches", MANY_MATCHES, "#", ITERS)
bench("100 matches", LONG_TEXT, "#", LONG_ITERS)
bench("100 matches, long replacement", LONG_TEXT, LONG_REPL, LONG_ITERS)
