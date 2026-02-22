# Functions

## Declaration

```pua
fn add(a, b)
  return a + b
```

## Default Parameters

```pua
fn add(a, b = 10)
  return a + b
```

## Variadic Parameters

```pua
fn sum(*vals)
  ...
```

## Type Hints

Type hints are supported in parameter lists:

```pua
fn f(user_id: int, name: str)
  ...
```

Type hints are metadata (inspectable), not a full static type system.

## Anonymous Functions

```pua
fn(x)
  return x * 2
```

## Decorators

Decorator syntax is supported for function declarations:

```pua
@decorator
fn f()
  ...
```

## Named and Spread Call Arguments

- Named args: `f(a=1, b=2)`
- Spread args: `f(*arr)`

Note: named call arguments use `=`.
Expression assignment uses `:=` (walrus), for example: `x := expr`.

## Multiple Return Values

```pua
fn pair()
  return 1, 2
```
