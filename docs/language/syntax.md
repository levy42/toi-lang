# Language Syntax

## Lexical Rules

- Statements are line-oriented.
- Blocks are indentation-based.
- Comments start with `--`.
- Strings can use single quotes, double quotes, or multiline `[[...]]`.
- Interpolated strings use `f"..."`.

## Literals

```toi
n = 42
big = 100_000
ok = true
none = nil
name = "Toi"
multi = [[hello
world]]
items = {1, 2, 3}
user = {name = "Ana", admin = true}
```

## Variables And Assignment

```toi
x = 10
a, b = 1, 2
a, b = {10, 20}
x += 1
```

Expression assignment uses `:=`:

```toi
if (line := input("> ")) != ""
  print line
```

## Operators

Arithmetic:

- `+`, `-`, `*`, `/`, `%`, `//`, `**`

Comparison:

- `==`, `!=`, `<`, `<=`, `>`, `>=`

Boolean:

- `and`, `or`, `not`

Other common forms:

- ternary: `cond ? yes : no`
- range: `1..10`
- length: `#value`
- append: `items <+ value`
- membership: `value in items`, `items has value`

## Tables

Tables are the main compound type. They can hold array-style and map-style data at the same time.

```toi
t = {10, 20, name = "toi", [10] = "x"}

print t[1]
print t.name
print t["name"]
```

## Strings

```toi
name = "Toi"
version = 0.1
msg = f"hello {name} {version|.1f}"
```

Interpolation uses normal expression syntax inside `{...}`. Formatting after `|` follows `string.format` conventions.

## Indexing And Slicing

```toi
letters = "hello"
nums = {10, 20, 30, 40, 50}

first = letters[1]
mid = nums[2..4]
head = nums[..3]
tail = nums[3..]
drop_last = letters[..-1]
```

Negative slice bounds are offsets from the end.

## Comprehensions

```toi
squares = {n * n for n in 1..5}
index = {str(n) = n * 10 for n in 1..3}
```

## Method Calls

Use method-call style when it reads naturally:

```toi
print "hello".upper()
print ", ".join({"a", "b", "c"})
```

## Decorators

```toi
@app.command
fn hello()
  print "hello"
```
