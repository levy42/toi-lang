# `thread` Module

Import:

```pua
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
