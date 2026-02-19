(function_header) @indent
(if_header) @indent
(elif_header) @indent
(else_header) @indent
(while_header) @indent
(for_header) @indent
(with_header) @indent
(try_header) @indent
(except_header) @indent
(finally_header) @indent

; Ensure Enter after `fn a()` indents as a block opener, not as a closed `()`.
(function_header
  (parameter_list ")" @indent))

(ERROR "fn" @indent)
(ERROR "if" @indent)
(ERROR "elif" @indent)
(ERROR "while" @indent)
(ERROR "for" @indent)
(ERROR "with" @indent)
(ERROR "except" @indent)

((ERROR (function_header)) @indent)
((ERROR (if_header)) @indent)
((ERROR (elif_header)) @indent)
((ERROR (else_header)) @indent)
((ERROR (while_header)) @indent)
((ERROR (for_header)) @indent)
((ERROR (with_header)) @indent)
((ERROR (try_header)) @indent)
((ERROR (except_header)) @indent)
((ERROR (finally_header)) @indent)

((simple_statement
   (function_header)
   (newline) @end) @indent)
((simple_statement
   (if_header)
   (newline) @end) @indent)
((simple_statement
   (elif_header)
   (newline) @end) @indent)
((simple_statement
   (else_header)
   (newline) @end) @indent)
((simple_statement
   (while_header)
   (newline) @end) @indent)
((simple_statement
   (for_header)
   (newline) @end) @indent)
((simple_statement
   (with_header)
   (newline) @end) @indent)
((simple_statement
   (try_header)
   (newline) @end) @indent)
((simple_statement
   (except_header)
   (newline) @end) @indent)
((simple_statement
   (finally_header)
   (newline) @end) @indent)

(_ "[" "]" @end) @indent
(_ "{" "}" @end) @indent
