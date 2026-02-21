import time


def bench(name, func):
    start = time.perf_counter()
    result = func()
    elapsed = time.perf_counter() - start
    print(f"{name} {elapsed:.6f} sec (checksum={result})")
    return elapsed


ITER = 200_000
S = "The Quick Brown Fox Jumps Over The Lazy Dog 12345"


def bench_upper():
    total = 0
    for _ in range(ITER):
        x = S.upper()
        total += len(x)
    return total


def bench_lower():
    total = 0
    for _ in range(ITER):
        x = S.lower()
        total += len(x)
    return total


def bench_substring_range():
    total = 0
    for _ in range(ITER):
        x = S[4:19]
        total += len(x)
    return total


def main():
    print(f"Python string ops benchmark (iter={ITER})")
    bench("upper", bench_upper)
    bench("lower", bench_lower)
    bench("substring range", bench_substring_range)


if __name__ == "__main__":
    main()
