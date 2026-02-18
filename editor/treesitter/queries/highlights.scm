(comment) @comment

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
(import_expression module: (dotted_name (identifier) @module))
(function_header name: (identifier) @function)
(parameter name: (identifier) @variable.parameter)

((identifier) @keyword.control
  (#match? @keyword.control "^(if|elif|else|while|for|in|break|continue|yield|with|as|try|except|finally)$"))

((identifier) @keyword
  (#match? @keyword "^(fn|return|throw|import|print|gc|local|global|from|del)$"))

((identifier) @keyword.operator
  (#match? @keyword.operator "^(and|or|not|has)$"))

(ERROR (identifier) @keyword.control
  (#match? @keyword.control "^(if|elif|else|while|for|in|break|continue|yield|with|as|try|except|finally)$"))

(ERROR (identifier) @keyword
  (#match? @keyword "^(fn|return|throw|import|print|gc|local|global|from|del)$"))

(ERROR (identifier) @keyword.operator
  (#match? @keyword.operator "^(and|or|not|has)$"))

(number) @number
(string) @string
(multiline_string) @string
(fstring) @string.special
(fstring_text_double) @string.special
(fstring_text_single) @string.special
(fstring_escape) @string.escape
(fstring_interpolation "{" @punctuation.special)
(fstring_interpolation "}" @punctuation.special)
