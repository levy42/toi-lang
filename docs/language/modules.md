# Modules and Imports

## Import Expression

```pua
math = import math
```

`import` can be used as an expression.

## Import Statement

```pua
import math
```

## Dotted Module Paths

```pua
import lib.http_server
srv = import lib.http_server
```

## From Import

```pua
from lib.test import assert_eq, assert_true
from pkg.mod import *
```

## Module Resolution

Pua supports native modules (registered in VM) and `.pua` source modules.
