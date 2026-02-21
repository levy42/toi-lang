# `signal` Module

Import:

```pua
signal = import signal
```

POSIX signal wrapper (`<signal.h>`).

## Functions

- `signal.raise(sig) -> bool`
- `signal.ignore(sig) -> bool`
- `signal.default(sig) -> bool`

`sig` can be a signal number or a name like `"INT"`, `"TERM"`, `"USR1"`, or `"SIGINT"`.
