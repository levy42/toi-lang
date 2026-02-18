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

(ERROR "fn" @indent)
(ERROR "if" @indent)
(ERROR "elif" @indent)
(ERROR "while" @indent)
(ERROR "for" @indent)
(ERROR "with" @indent)
(ERROR "except" @indent)

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
(_ "(" ")" @end) @indent
