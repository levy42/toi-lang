# `thread` Module

Import:

```toi
thread = import thread
```

## Module Functions

- `thread.spawn(fn, ...) -> thread_handle`
- `thread.join(thread_handle) -> value(s)`
- `thread.yield()`
- `thread.sleep(seconds)`
- `thread.mutex() -> mutex`
- `thread.channel([capacity]) -> channel`

## `thread.handle` Methods

- `handle.join()`

## `thread.mutex` Methods

- `lock()`
- `unlock()`
- `trylock() -> bool`

## `thread.channel` Methods

- `send(value) -> bool`
- `recv() -> value|nil`
- `tryrecv() -> value|nil`
- `close()`

## Notes

Threading behavior is controlled in native runtime and may depend on host/platform configuration.

Globals are shared across threads. Inside worker functions, assignments to non-`local` names update shared global state.

Use `local` for loop counters and temporaries inside worker functions, and use `thread.mutex()` or channels when multiple threads access shared mutable data.

`TOI_NO_GIL=1` enables experimental lock-free shared-VM execution. This mode is currently unsafe and can race, error, hang, or crash.
