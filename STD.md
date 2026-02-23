# Std Notes

## btree
File-backed B-tree key/value store.

- `btree = import btree`
- `db = btree.open(path)`
- `db = btree.open()` (in-memory, not persisted)
- `db.put(key, value)`
- `db.get(key)`
- `db.delete(key)`
- `db.close()`

### Types
- `key`: `string` or `number`
- `value`: `string` or `number`

### Return values
- `btree.open(path)` -> file-backed `db`, throws runtime error on failure
- `btree.open()` -> in-memory `db` (buffer-style), throws runtime error on failure
- `db.get(key)` -> stored value, or `nil` if missing
- `db.delete(key)` -> `true` if key existed and was deleted, `false` if missing
- `db.put(key, value)` -> returns `db` (for chaining)
- `db.close()` -> `true`

### Example
```toi
btree = import btree
os = import os

os.remove("tests/tmp_btree.db")

db = btree.open("tests/tmp_btree.db")

db.put("name", "toi")
db.put(1, 100)
print(db.get("name"))  -- toi
print(db.get(1))       -- 100

ok = db.delete("name")
print(ok)              -- true
print(db.get("name")) -- nil

db.close()
```

## io.buffer
In-memory file-like object with the same core API as file handles from `io.open`.

Note: `io.open(path, mode)` now throws runtime error on failure (no `nil, err` return).

- `io = import io`
- `buf = io.buffer(initial = "", mode = "r")`

### Methods
- `buf.read()`
- `buf.read(n)`
- `buf.readline()`
- `buf.write(str)`
- `buf.seek()`
- `buf.seek(offset)`
- `buf.seek(whence)`
- `buf.seek(whence, offset)` where `whence` is `"set"|"start"|"cur"|"current"|"end"`
- `buf.tell()`
- `buf.close()`

### Example
```toi
io = import io

b = io.buffer("line1\nline2")
print(b.readline())     -- line1
print(b.tell())         -- 6

b.seek("end", 0)
b.write("\nline3")

b.seek(0)
print(b.read())         -- line1\nline2\nline3

b.close()
```
