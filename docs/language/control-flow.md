# Control Flow

## Conditionals

```toi
if score >= 90
  print "A"
elif score >= 80
  print "B"
else
  print "C"
```

Expression assignment fits naturally in conditions:

```toi
if (line := input("> ")) != ""
  print line
```

## Match

Use `match` when branching on a single value:

```toi
match method
case "GET"
  handle_get()
case "POST"
  handle_post()
else
  handle_other()
```

## While

```toi
i = 0
while i < 10
  print i
  i += 1
```

## For

Toi favors iterator-style loops.

```toi
for v in values
  print v

for k, v in row
  print k, v
```

Use `continue` and `break` when the loop reads better with guard clauses.

## Exceptions

```toi
try
  risky()
except e
  print e
finally
  cleanup()
```

Raise errors with:

```toi
throw "boom"
throw {type = "ConfigError", msg = "missing host"}
```

## With

```toi
with io.open("tmp.txt", "w") as f
  f.write("hello")
```

## Return And Yield

- `return` may return multiple values
- `yield` is used in generator-style code
