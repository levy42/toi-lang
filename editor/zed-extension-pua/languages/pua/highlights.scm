(comment) @comment

(if_statement "if" @keyword.control)
(elif_clause "elif" @keyword.control)
(else_clause "else" @keyword.control)
(while_statement "while" @keyword.control)
(for_statement "for" @keyword.control)
(for_statement "in" @keyword.control)
(with_statement "with" @keyword.control)
(with_statement "as" @keyword.control)
(try_statement "try" @keyword.control)
(except_clause "except" @keyword.control)
(finally_clause "finally" @keyword.control)
[
  (break_statement)
  (continue_statement)
  (yield_statement)
] @keyword.control

(function_definition "fn" @keyword)
(function_expression "fn" @keyword)
(return_statement "return" @keyword)
(throw_statement "throw" @keyword)
(import_expression "import" @keyword)
(print_statement "print" @keyword)
(gc_statement "gc" @keyword)

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
