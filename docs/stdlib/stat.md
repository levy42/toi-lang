# `stat` Module

Import:

```toi
stat = import stat
```

POSIX filesystem metadata wrapper (`stat(2)` family).

## Functions

- `stat.stat(path) -> table | nil, err`
- `stat.lstat(path) -> table | nil, err`
- `stat.chmod(path, mode) -> true | nil, err`
- `stat.umask([mask]) -> number`

## Result Table Fields

- `size`
- `mode`
- `mtime`
- `atime`
- `ctime`
- `uid`
- `gid`
- `nlink`
- `ino`
- `dev`
- `is_file`
- `is_dir`
- `is_link`
