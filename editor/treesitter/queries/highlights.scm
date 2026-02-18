(comment) @comment

[
  (if_header)
  (elif_header)
  (else_header)
  (while_header)
  (for_header)
  (with_header)
  (try_header)
  (except_header)
  (finally_header)
  (break_statement)
  (continue_statement)
  (yield_statement)
] @keyword.control

[
  (function_header)
  (function_definition)
  (function_expression)
  (return_statement)
  (throw_statement)
  (import_expression)
  (print_statement)
  (gc_statement)
] @keyword

(binary_expression "and" @keyword.operator)
(binary_expression "or" @keyword.operator)
(binary_expression "has" @keyword.operator)
(unary_expression "not" @keyword.operator)

(binary_expression) @operator

(decorator) @attribute

(boolean) @constant.builtin.boolean
(nil) @constant.builtin

(identifier) @variable
(import_expression module: (dotted_name (identifier) @module))
(function_header name: (identifier) @function)
(parameter name: (identifier) @variable.parameter)

(number) @number
(string) @string
(multiline_string) @string
(fstring) @string.special
(fstring_text_double) @string.special
(fstring_text_single) @string.special
(fstring_escape) @string.escape
(fstring_interpolation "{" @punctuation.special)
(fstring_interpolation "}" @punctuation.special)
