# Functions

## Declaration

```toi
fn add(a, b)
  return a + b
```

## Default Arguments

```toi
fn add(a, b = 10)
  return a + b
```

## Variadic Arguments

```toi
fn collect(*vals)
  return vals
```

## Multiple Returns

```toi
fn split_name()
  return "Ana", "Silva"

first, last = split_name()
```

## Anonymous Functions

```toi
double = fn(x)
  return x * 2
```

## Type Hints

```toi
fn greet(name: str, times: int)
  for _ in 1..times
    print name
```

Type hints are runtime metadata. They are useful for libraries and tooling, not a full static type system.

## Named And Spread Arguments

```toi
run(host = "127.0.0.1", port = 8080)

args = {"Ana", 3}
greet(*args)
```

Named arguments use `=`, not `:=`.

## Decorators

```toi
@app.command("hello")
fn hello(name = "world")
  print f"hello {name}"
```

Decorators are common in `lib.cli` and `lib.http_server`.

## Methods

Normal method lookup:

```toi
"toi".upper()
```

Metatable-only method lookup:

```toi
obj::method(...)
```
