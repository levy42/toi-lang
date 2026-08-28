# Modules

Toi has two main kinds of modules:

- native modules implemented in C, such as `math`, `json`, or `socket`
- source modules implemented in `.toi`, such as `lib.types` or `lib.http_server`

## Import Statement

```toi
import math
import os, io
```

## Import Expression

```toi
math = import math
http_server = import lib.http_server
```

This form is useful when you want the import as part of an expression or assignment.

## From Import

```toi
from lib.test import assert_eq, assert_true
from pkg.mod import *
```

## Dotted Module Paths

```toi
import lib.types
db = import lib.db
```

## Practical Style

- use plain module names for native modules: `import math`
- use dotted paths for shipped Toi libraries: `import lib.types`
- keep imports near the top of the file
- prefer explicit imports over deep hidden globals

## Example

```toi
import math
from lib.test import assert_eq

fn area(r)
  return math.pi * r * r

assert_eq(area(2), math.pi * 4)
```
