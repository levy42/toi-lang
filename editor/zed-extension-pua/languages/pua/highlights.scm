(comment) @comment

(ERROR "if" @keyword.control)
(ERROR "elif" @keyword.control)
(ERROR "while" @keyword.control)
(ERROR "for" @keyword.control)
(ERROR "in" @keyword.control)
(ERROR "with" @keyword.control)
(ERROR "as" @keyword.control)
(ERROR "except" @keyword.control)
(ERROR "yield" @keyword.control)

(ERROR "fn" @keyword)
(ERROR "return" @keyword)
(ERROR "throw" @keyword)
(ERROR "import" @keyword)
(ERROR "print" @keyword)
(ERROR "gc" @keyword)

(ERROR "and" @keyword.operator)
(ERROR "or" @keyword.operator)
(ERROR "not" @keyword.operator)
(ERROR "has" @keyword.operator)

(if_header "if" @keyword.control)
(elif_header "elif" @keyword.control)
(else_header) @keyword.control
(while_header "while" @keyword.control)
(for_header "for" @keyword.control)
(for_header "in" @keyword.control)
(with_header "with" @keyword.control)
(with_header "as" @keyword.control)
(try_header) @keyword.control
(except_header "except" @keyword.control)
(finally_header) @keyword.control

(break_statement) @keyword.control
(continue_statement) @keyword.control
(yield_statement "yield" @keyword.control)

(function_header "fn" @keyword)
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


(binary_expression "==" @operator)
(binary_expression "!=" @operator)
(binary_expression "<=" @operator)
(binary_expression ">=" @operator)
(binary_expression "<" @operator)
(binary_expression ">" @operator)
(binary_expression "+" @operator)
(binary_expression "-" @operator)
(binary_expression "*" @operator)
(binary_expression "/" @operator)
(binary_expression "//" @operator)
(binary_expression "%" @operator)
(binary_expression "**" @operator)
(binary_expression ".." @operator)
(binary_expression "<+" @operator)
(unary_expression "-" @operator)
(unary_expression "#" @operator)

(decorator) @attribute

(boolean) @constant.builtin.boolean
(nil) @constant.builtin

(identifier) @variable
((identifier) @variable.special
 (#eq? @variable.special "self"))
((identifier) @function.special
 (#match? @function.special "^__"))
((identifier) @constant
 (#match? @constant "^[A-Z][A-Z0-9_]*$"))
((identifier) @type
 (#match? @type "^[A-Z][a-z0-9]+(?:[A-Z][a-z0-9]*)*$"))

(import_expression module: (dotted_name (identifier) @module))
(function_header name: (identifier) @function)
((function_header
   name: (identifier) @function.special)
 (#match? @function.special "^__"))
((call_argument
   name: (identifier) @property))
((table_entry
   key: (identifier) @function.special)
 (#match? @function.special "^__"))
((postfix_operator
   property: (identifier) @function.special)
 (#match? @function.special "^__"))
(parameter name: (identifier) @variable.parameter)
((parameter
   name: (identifier) @variable.special)
 (#eq? @variable.special "self"))



(number) @number
(string) @string
(multiline_string) @string
(fstring) @string.special
(fstring_text_double) @string.special
(fstring_text_single) @string.special
(fstring_escape) @string.escape
(fstring_interpolation "{" @punctuation.special)
(fstring_interpolation "}" @punctuation.special)
