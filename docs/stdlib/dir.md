# `dir` Module

Import:

```toi
dir = import dir
```

POSIX directory traversal wrapper (`<dirent.h>`).

## Functions

- `dir.list(path) -> table | nil, err`
- `dir.scandir(path) -> table | nil, err`

`dir.list` returns an array of entry names (excluding `.` and `..`).

`dir.scandir` returns an array of entry tables:

- `name`
- `path`
- `type` (`"file"`, `"dir"`, `"link"`, etc or `"unknown"`)
- `is_file`
- `is_dir`
- `is_link`
