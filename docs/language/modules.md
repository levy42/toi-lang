# Modules and Imports

## Import Expression

```toi
math = import math
```

`import` can be used as an expression.

## Import Statement

```toi
import math
```

## Dotted Module Paths

```toi
import lib.http_server
srv = import lib.http_server
```

## From Import

```toi
from lib.test import assert_eq, assert_true
from pkg.mod import *
```

## Module Resolution

Toi supports native modules (registered in VM) and `.toi` source modules.
