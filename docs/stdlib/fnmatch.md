# `fnmatch` Module

Import:

```toi
fnmatch = import fnmatch
```

POSIX glob-style matching wrapper (`<fnmatch.h>`).

## Functions

- `fnmatch.match(pattern, text, [flags]) -> bool`

## Flags

`flags` is an optional string:

- `p`: pathname mode (`FNM_PATHNAME`)
- `d`: dotfile mode (`FNM_PERIOD`)
- `n`: no escape (`FNM_NOESCAPE`)
- `i`: case-insensitive (`FNM_CASEFOLD`, if available)
- `l`: leading-dir (`FNM_LEADING_DIR`, if available)
