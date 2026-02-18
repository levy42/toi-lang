(comment) @comment

[
  (if_statement)
  (elif_clause)
  (else_clause)
  (while_statement)
  (for_statement)
  (with_statement)
  (try_statement)
  (except_clause)
  (finally_clause)
  (break_statement)
  (continue_statement)
  (yield_statement)
] @keyword.control

[
  (function_definition)
  (return_statement)
  (throw_statement)
  (import_expression)
  (function_expression)
  (print_statement)
  (gc_statement)
] @keyword

(unary_expression) @keyword.operator
(binary_expression) @operator

(decorator) @attribute

(boolean) @constant.builtin.boolean
(nil) @constant.builtin

(identifier) @variable
(import_expression module: (dotted_name (identifier) @module))
(function_definition name: (identifier) @function)
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
