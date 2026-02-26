# Functions

## Declaration

```toi
fn add(a, b)
  return a + b
```

## Default Parameters

```toi
fn add(a, b = 10)
  return a + b
```

## Variadic Parameters

```toi
fn sum(*vals)
  ...
```

## Type Hints

Type hints are supported in parameter lists:

```toi
fn f(user_id: int, name: str)
  ...
```

Type hints are metadata (inspectable), not a full static type system.

## Anonymous Functions

```toi
fn(x)
  return x * 2
```

## Decorators

Decorator syntax is supported for function declarations:

```toi
@decorator
fn f()
  ...
```

## Named and Spread Call Arguments

- Named args: `f(a=1, b=2)`
- Spread args: `f(*arr)`
- Generator comprehension arg: `f(x for x in iterable)`

Note: named call arguments use `=`.
Expression assignment uses `:=` (walrus), for example: `x := expr`.

## Multiple Return Values

```toi
fn pair()
  return 1, 2
```
