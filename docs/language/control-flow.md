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

Walrus (`:=`) can be used in conditions:

```pua
k = nil
if (k := 3) == 3
  print k
```

## While Loop

```pua
while cond
  ...
```

```pua
i = 0
while ((i := i + 1) <= 10)
  print i
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
