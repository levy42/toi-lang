# `poll` Module

Import:

```toi
poll = import poll
```

POSIX `poll(2)` wrapper for waiting on file-descriptor readiness.

## Functions

- `poll.wait(fds, [timeout_ms]) -> ready`

`fds` is an array where each item is either:

- `number` fd (defaults to `"in"` / `POLLIN`)
- `{ fd = number, events = "in" | {"in", "out", ...} }`

Supported event names:

- `in`
- `out`
- `pri`
- `err`
- `hup`
- `nval`

`timeout_ms`:

- omitted or `-1`: block indefinitely
- `0`: do not block
- `>0`: wait that many milliseconds

Return value `ready` is an array of rows for descriptors with non-zero events:

- `index`: original 1-based input index
- `fd`: descriptor number
- `in`, `out`, `pri`, `err`, `hup`, `nval`: booleans
- `revents`: raw numeric `revents` bitmask
