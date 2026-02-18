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

(_ "[" "]" @end) @indent
(_ "{" "}" @end) @indent
(_ "(" ")" @end) @indent
