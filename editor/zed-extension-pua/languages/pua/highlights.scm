(comment) @comment

[
  "if"
  "elif"
  "else"
  "while"
  "for"
  "in"
  "break"
  "continue"
  "yield"
  "with"
  "as"
  "try"
  "except"
  "finally"
] @keyword.control

[
  "local"
  "global"
  "import"
  "from"
  "fn"
  "return"
  "del"
  "throw"
  "print"
  "gc"
] @keyword

[
  "and"
  "or"
  "not"
  "has"
] @keyword.operator

(decorator) @attribute
(decorator "@" @attribute)

(boolean) @constant.builtin.boolean
(nil) @constant.builtin

(builtin) @function.builtin
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

[
  "+"
  "-"
  "*"
  "/"
  "%"
  "//"
  "**"
  "="
  "=="
  "!="
  "<"
  ">"
  "<="
  ">="
  ".."
  "#"
  "?"
  "<+"
] @operator

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

[
  ","
  "."
  ":"
  ";"
] @punctuation.delimiter
