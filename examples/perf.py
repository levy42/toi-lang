import time
import tracemalloc
import gc


def bench(name, func):
    start = time.perf_counter()
    result = func()
    elapsed = time.perf_counter() - start
    print(f"{name} {elapsed:.6f} sec")
    return result


def bench_math():
    acc = 0.0
    for i in range(1, 2_000_000 + 1):
        acc += (i * 3.14159) / ((i % 97) + 1)
    return acc


def bench_string_concat():
    s = ""
    for _ in range(20_000):
        s += "abc"
    return len(s)


def bench_string_ops():
    s = "the quick brown fox jumps over the lazy dog"
    total = 0
    for _ in range(20_000):
        x = s.upper()
        y = x.lower()
        z = y[4:19]
        total += len(z)
    return total


def bench_table():
    t = [0]
    for i in range(1, 200_000 + 1):
        if i < len(t):
            t[i] = i * 2
        else:
            t.append(i * 2)
    total = 0
    for i in range(1, 200_000 + 1):
        total += t[i]
    return total


def bench_calls():
    def add(a, b):
        return a + b

    acc = 0
    for i in range(1, 2_000_000 + 1):
        acc = add(acc, i)
    return acc


def bench_memory():
    gc.collect()
    tracemalloc.start()
    before_current, before_peak = tracemalloc.get_traced_memory()
    t = []
    for i in range(200_000):
        t.append(f"abc{i}")
    current, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    return (
        current - before_current,
        peak - before_peak,
        current,
        peak,
    )


def main():
    print("Python perf:")
    bench("math", bench_math)
    bench("string concat", bench_string_concat)
    bench("string ops", bench_string_ops)
    bench("table", bench_table)
    bench("calls", bench_calls)
    cur_delta, peak_delta, current, peak = bench("memory", bench_memory)
    print(f"memory current {current} bytes (delta {cur_delta})")
    print(f"memory peak {peak} bytes (delta {peak_delta})")


if __name__ == "__main__":
    main()
