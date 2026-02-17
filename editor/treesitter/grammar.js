module.exports = grammar({
  name: "pua",

  extras: ($) => [/\s/, $.comment],
  word: ($) => $.identifier,

  rules: {
    source_file: ($) =>
      repeat(
        choice(
          $.comment,
          $.keyword_control,
          $.keyword_declaration,
          $.keyword_exception,
          $.keyword_operator,
          $.keyword_other,
          $.decorator,
          $.boolean,
          $.nil,
          $.builtin,
          $.identifier,
          $.number,
          $.string,
          $.multiline_string,
          $.fstring,
          $.operator,
          $.punctuation_bracket,
          $.punctuation_delimiter,
        ),
      ),

    comment: (_) => token(seq("--", /.*/)),

    keyword_control: (_) =>
      choice(
        "if",
        "elif",
        "else",
        "while",
        "for",
        "in",
        "break",
        "continue",
        "with",
        "as",
        "try",
        "except",
        "finally",
      ),

    keyword_declaration: (_) =>
      choice("local", "global", "import", "from", "fn", "return", "del"),

    keyword_exception: (_) => "throw",
    keyword_operator: (_) => choice("and", "or", "not", "has"),
    keyword_other: (_) => choice("print", "gc"),
    decorator: (_) => /@<[^>\n]*>/,

    boolean: (_) => choice("true", "false"),
    nil: (_) => "nil",

    builtin: (_) =>
      choice(
        "setmetatable",
        "getmetatable",
        "str",
        "len",
        "int",
        "float",
        "bool",
        "type",
        "assert",
        "error",
      ),

    identifier: (_) => /[A-Za-z_][A-Za-z0-9_]*/,

    number: (_) => /\d+(\.\d+)?([eE][+-]?\d+)?/,

    string: (_) =>
      token(
        choice(
          seq('"', repeat(choice(/[^"\\\n]+/, /\\./)), '"'),
          seq("'", repeat(choice(/[^'\\\n]+/, /\\./)), "'"),
        ),
      ),

    multiline_string: (_) => token(/\[\[[\s\S]*\]\]/),

    fstring: (_) =>
      token(
        choice(
          seq('f"', repeat(choice(/[^"\\\n]+/, /\\./)), '"'),
          seq("f'", repeat(choice(/[^'\\\n]+/, /\\./)), "'"),
        ),
      ),

    operator: (_) =>
      choice(
        "==",
        "!=",
        "<=",
        ">=",
        "..",
        "**",
        "//",
        "=",
        "<",
        ">",
        "+",
        "-",
        "*",
        "/",
        "%",
        "#",
        "?",
      ),

    punctuation_bracket: (_) => choice("(", ")", "{", "}", "[", "]"),
    punctuation_delimiter: (_) => choice(",", ".", ";", ":"),
  },
});
