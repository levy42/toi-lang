# `coroutine` Module

Import:

```toi
coroutine = import coroutine
```

## Functions

- `coroutine.create(fn) -> thread`
- `coroutine.resume(thread, ...) -> value(s)`
- `coroutine.yield(...)`
- `coroutine.status(thread) -> "running"|"suspended"|"normal"|"dead"`

Coroutines are used to build custom iterators and generator-like flows.
