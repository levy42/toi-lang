# `table` Module

Import:

```pua
table = import table
```

## Functions

- `remove(t, [index])`
- `push(t, value)`
- `reserve(t, capacity)`
- `concat(t, [sep]) -> string`
- `sort(t, [cmp])`
- `insert(t, value)`
- `insert(t, index, value)`
- `keys(t) -> table`
- `values(t) -> table`
- `find_index(t, needle)`
- `find_index(t, needle, start_index)`
- `find_index(t, needle, lookup_fn)`
- `find_index(t, needle, start_index, lookup_fn)`
