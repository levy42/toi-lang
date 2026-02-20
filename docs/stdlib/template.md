# `template` Module

Import:

```pua
template = import template
```

## Functions

- `template.compile(template_string) -> function(ctx)`
- `template.render(template_string, ctx_table) -> string`
- `template.code(template_string) -> string` (debug generated code)

## Template Syntax

- Expression output: `{{ expr }}`
- Control tags: `{% ... %}`

Used in tests with loops and inline expressions.
