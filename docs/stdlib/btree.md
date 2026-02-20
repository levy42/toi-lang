# `btree` Module

Import:

```pua
btree = import btree
```

## Functions

- `btree.open() -> db` (in-memory)
- `btree.open(path) -> db` (file-backed)

## DB Methods

- `db.put(key, value)`
- `db.get(key) -> value|nil`
- `db.delete(key) -> bool`
- `db.close()`

Keys and values support string/number usage shown in tests.

See `tests/39_btree.pua` for end-to-end usage and persistence behavior.
