# Core

This page covers the most frequently used built-ins and native modules for everyday language work.

## Global Built-ins

Available without importing a module:

- `print(...)`
- `input(prompt=nil)`
- `type(value)`
- `istype(value, ...)`
- `bool(value)`, `int(value)`, `float(value)`, `str(value)`
- `range(...)`
- `slice(value, start, stop, step=1)`
- `min(...)`, `max(...)`, `sum(...)`, `divmod(a, b)`
- `next(...)`, `inext(...)`, `gen_next(...)`
- `setmetatable(table, mt)`, `getmetatable(value)`
- `error(...)`
- `exit(code=0)`
- `mem()`

## `math`

For numeric code:

- trig: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`
- rounding: `floor`, `ceil`, `abs`, `modf`
- powers and logs: `sqrt`, `exp`, `log`, `pow`, `fmod`
- angle conversion: `deg`, `rad`
- random: `random`, `seed`
- aggregates: `min`, `max`, `sum`

```toi
import math
print math.sqrt(25)
```

## `time`

Use `time` for clocks and elapsed-time measurement:

- `time.seconds()`
- `time.nanos()`
- `time.clock()`

`time.clock()` is the most common timer for small benchmark scripts.

## `string`

Common string helpers:

- case conversion: `upper`, `lower`
- cleanup: `trim`
- search and split: `find`, `split`
- construction: `join`, `format`, `rep`
- indexing helpers: `sub`, `byte`, `char`

Method style is usually the nicest form:

```toi
print "hello".upper()
print ", ".join({"a", "b"})
```

## `table`

Core collection helpers:

- `push`
- `insert`
- `remove`
- `reserve`
- `clone`
- `concat`
- `sort`
- `keys`
- `values`
- `find_index`

```toi
import table

items = {3, 1, 2}
table.sort(items)
```

## `inspect`

`inspect.signature(fn)` is useful for tooling and debugging function shapes.

## `url`

URL helpers:

- `url.parse(...)`
- `url.encode(...)`
- `url.decode(...)`

## `uuid`

Use `uuid` when you need generated identifiers.

## Core Data Structures

### `btree`

Persistent B-tree storage for ordered key/value workloads.

### `binary`

Binary encode/decode helpers.

### `struct`

Structured binary packing and unpacking via format strings.
