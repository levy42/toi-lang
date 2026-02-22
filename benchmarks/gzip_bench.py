import gzip
import time

CHUNK = (
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
    "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. "
    "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.\n"
)

REPEAT_COUNT = 3000
COMPRESS_ITERS = 50
DECOMPRESS_ITERS = 50


def bench(name, fn):
    start = time.perf_counter()
    result = fn()
    elapsed = time.perf_counter() - start
    print(f"{name}: {elapsed:.6f}s")
    return result, elapsed


def main():
    payload = (CHUNK * REPEAT_COUNT).encode("utf-8")
    print("Python gzip benchmark")
    print(f"payload bytes: {len(payload)}")
    print(
        f"iterations: compress={COMPRESS_ITERS} decompress={DECOMPRESS_ITERS}"
    )

    compressed, _ = bench(
        "gzip compress",
        lambda: [gzip.compress(payload) for _ in range(COMPRESS_ITERS)][-1],
    )

    _, _ = bench(
        "gzip decompress",
        lambda: [gzip.decompress(compressed) for _ in range(DECOMPRESS_ITERS)][-1],
    )

    print(f"compressed bytes: {len(compressed)}")
    print(f"ratio: {len(compressed) / len(payload):.3f}")


if __name__ == "__main__":
    main()
