# `glob` Module

Import:

```toi
glob = import glob
```

POSIX pathname expansion wrapper (`<glob.h>`).

## Functions

- `glob.match(pattern, [flags]) -> table`

Returns an array-like table of matched paths.

## Flags

`flags` is an optional string:

- `n`: no sort (`GLOB_NOSORT`)
- `e`: no escape (`GLOB_NOESCAPE`)
- `m`: append slash to directories (`GLOB_MARK`)
- `d`: no-check fallback (`GLOB_NOCHECK`)
