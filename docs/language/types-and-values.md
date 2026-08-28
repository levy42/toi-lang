# Types And Values

Runtime `type(x)` values include:

- `nil`
- `boolean`
- `number`
- `string`
- `table`
- `function`
- `generator`
- `thread`
- `userdata`

## Truthiness

- `nil` is falsey
- `false` is falsey
- everything else is truthy

## Numbers

Toi uses `number` as its runtime numeric type.

```toi
a = 10
b = 3.5
c = 1_000_000
```

## Strings

Strings are immutable and support indexing, slicing, interpolation, and common methods.

```toi
s = "hello"
print s.upper()
print s[2..4]
```

## Tables

Tables are used for:

- lists
- maps
- records
- lightweight objects

```toi
row = {id = 7, name = "Ana"}
tags = {"red", "green", "blue"}
```

Use:

- `for v in table` for values
- `for k, v in table` for key-value iteration
- `table[key]` for map-style lookup
- `items <+ value` for append

## Functions And Generators

Functions are first-class values:

```toi
fn add(a, b)
  return a + b

op = add
print op(2, 3)
```

Generators also behave like normal runtime values and report `type(x) == "generator"`.

## Userdata

Native modules use `userdata` for handles such as files, sockets, mmap regions, and thread handles.

## Metatables

Common metamethods in the runtime include:

- `__index`
- `__newindex`
- `__str`
- `__call`
- `__append`
- `__next`

Metatable-only dispatch is available with `::`:

```toi
obj::method(args)
```
