# gzip (optional)

`gzip` is available only when Toi is built with system zlib (`libz`).

```toi
gzip = import gzip
```

## API

### `gzip.compress(data, level=nil) -> string`

Compress a string into gzip bytes.

- `data`: input string (binary-safe)
- `level`: optional compression level `-1..9` (`-1` uses zlib default)

### `gzip.decompress(data) -> string`

Decompress gzip bytes into the original string.

Raises on invalid/truncated gzip input.
