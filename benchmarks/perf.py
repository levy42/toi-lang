import gc
import random
import time
import tracemalloc


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


def bench_random():
    acc = 0.0
    for _ in range(2_000_000):
        acc += random.random()
    return acc


def bench_string_concat():
    s = ""
    for _ in range(20_000):
        s += "abc"
    return len(s)


def bench_string_ops():
    total = 0
    for i in range(20_000):
        s = f"the quick brown fox jumps over the lazy dog {i}"
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


def bench_fstring():
    total = 0
    for i in range(1, 200_000 + 1):
        s = f"user:{i}-x{i % 10}"
        total += len(s)
    return total


def bench_app_pipeline():
    methods = ["GET", "POST", "PUT", "DELETE"]
    paths = ["/users", "/teams", "/search", "/assets", "/health", "/events"]

    routes = {
        "GET /users": 1,
        "POST /users": 2,
        "GET /teams": 3,
        "PUT /teams": 4,
        "GET /search": 5,
        "GET /assets": 6,
        "GET /health": 7,
        "POST /events": 8,
    }

    cache = {}
    total = 0
    for i in range(1, 300_000 + 1):
        method = methods[i % 4]
        path = paths[i % 6]
        key = f"{method} {path}"

        route = routes.get(key, 0)
        auth = (i % 3) != 0
        user_id = i % 10_000
        team_id = i % 100
        body_size = (i * 37) % 2048

        status = 200
        if (not auth) and path != "/health":
            status = 401
        elif method == "POST" and body_size < 32:
            status = 422
        elif route == 0:
            status = 404

        cache_key = f"{path}:{user_id % 32}:{team_id}"
        cached = cache.get(cache_key)
        if cached is not None:
            total += cached
        else:
            cache_value = route + status + body_size
            cache[cache_key] = cache_value
            total += cache_value

        total += status + route + user_id + team_id

    return total


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
    bench("random", bench_random)
    bench("string concat", bench_string_concat)
    bench("string ops", bench_string_ops)
    bench("table", bench_table)
    bench("calls", bench_calls)
    bench("f-string", bench_fstring)
    bench("app pipeline", bench_app_pipeline)
    cur_delta, peak_delta, current, peak = bench("memory", bench_memory)
    print(f"memory current {current} bytes (delta {cur_delta})")
    print(f"memory peak {peak} bytes (delta {peak_delta})")


if __name__ == "__main__":
    main()
