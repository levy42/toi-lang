# Control Flow

## Conditionals

```toi
if cond
  ...
elif other
  ...
else
  ...
```

Walrus (`:=`) can be used in conditions:

```toi
k = nil
if (k := 3) == 3
  print k
```

## While Loop

```toi
while cond
  ...
```

```toi
i = 0
while ((i := i + 1) <= 10)
  print i
```

## For Loop

Toi supports iterator-style `for ... in ...`.

```toi
for v in values
  ...

for k, v in table
  ...
```

Index-loop shorthand (`i#`) is available in specific iterator contexts.

## Break / Continue

`break` and `continue` are supported in loops.

## Exceptions

```toi
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

```toi
with resource() as r
  r.use()
```

## Return / Yield

- `return` supports multiple values.
- `yield` is supported in generator/coroutine flows.
