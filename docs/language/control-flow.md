# Control Flow

## Conditionals

```pua
if cond
  ...
elif other
  ...
else
  ...
```

## While Loop

```pua
while cond
  ...
```

## For Loop

Pua supports iterator-style `for ... in ...`.

```pua
for v in values
  ...

for k, v in table
  ...
```

Index-loop shorthand (`i#`) is available in specific iterator contexts.

## Break / Continue

`break` and `continue` are supported in loops.

## Exceptions

```pua
try
  risky()
except e
  print e
finally
  cleanup()
```

`throw value` raises an exception.

## With Statement

Context-manager style:

```pua
with resource() as r
  r.use()
```

## Return / Yield

- `return` supports multiple values.
- `yield` is supported in generator/coroutine flows.
