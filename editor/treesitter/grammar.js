const PREC = {
  ASSIGN: 1,
  OR: 2,
  AND: 3,
  COMPARE: 4,
  TERM: 5,
  FACTOR: 6,
  POWER: 7,
  UNARY: 8,
  CALL: 9,
};

function sepBy1(sep, rule) {
  return seq(rule, repeat(seq(sep, rule)));
}

function sepBy(sep, rule) {
  return optional(sepBy1(sep, rule));
}

module.exports = grammar({
  name: "pua",

  extras: ($) => [/[ \t\r]+/, $.comment],
  word: ($) => $.identifier,

  rules: {
    source_file: ($) => repeat(choice($._terminator, $._statement)),

    _terminator: (_) => /[\n;]+/,

    comment: (_) => token(seq("--", /.*/)),

    _statement: ($) =>
      choice(
        $.decorator,
        $.function_definition,
        $.if_header,
        $.elif_header,
        $.else_header,
        $.while_header,
        $.for_header,
        $.with_header,
        $.try_header,
        $.except_header,
        $.finally_header,
        $.return_statement,
        $.throw_statement,
        $.yield_statement,
        $.break_statement,
        $.continue_statement,
        $.assignment,
        $.expression_statement,
      ),

    function_definition: ($) =>
      seq("fn", field("name", $.identifier), field("parameters", $.parameter_list)),
    parameter_list: ($) => seq("(", sepBy(",", $.parameter), ")"),
    parameter: ($) =>
      seq(
        optional("*"),
        field("name", $.identifier),
        optional(seq(":", $.identifier)),
        optional(seq("=", $.expression)),
      ),

    if_header: ($) => seq("if", field("condition", $.expression)),
    elif_header: ($) => seq("elif", field("condition", $.expression)),
    else_header: (_) => "else",
    while_header: ($) => seq("while", field("condition", $.expression)),
    for_header: ($) =>
      seq(
        "for",
        field("variables", sepBy1(",", $.identifier)),
        "in",
        field("iterables", sepBy1(",", $.expression)),
      ),
    with_header: ($) =>
      seq("with", field("value", $.expression), optional(seq("as", field("name", $.identifier)))),
    try_header: (_) => "try",
    except_header: ($) => seq("except", field("name", $.identifier)),
    finally_header: (_) => "finally",

    return_statement: ($) => seq("return", field("value", $.expression_list)),
    throw_statement: ($) => seq("throw", field("value", $.expression_list)),
    yield_statement: ($) => seq("yield", field("value", $.expression_list)),
    break_statement: (_) => "break",
    continue_statement: (_) => "continue",

    assignment: ($) =>
      prec.right(
        PREC.ASSIGN,
        seq(
          field("left", $.expression_list),
          "=",
          field("right", $.expression_list),
        ),
      ),

    expression_statement: ($) => $.expression,

    expression_list: ($) => sepBy1(",", $.expression),

    expression: ($) =>
      choice(
        $.binary_expression,
        $.unary_expression,
        $.postfix_expression,
        $.primary_expression,
      ),

    parenthesized_expression: ($) => seq("(", $.expression, ")"),
    dotted_name: ($) => prec.left(sepBy1(".", $.identifier)),

    table: ($) =>
      seq(
        "{",
        optional(
          seq(
            $.table_entry,
            repeat(seq($._table_separator, $.table_entry)),
            optional($._table_separator),
          ),
        ),
        "}",
      ),
    _table_separator: (_) => choice(",", /[\n;]+/),
    table_entry: ($) =>
      choice(
        seq(field("key", $.identifier), "=", field("value", $.expression)),
        seq("[", field("index", $.expression), "]", "=", field("value", $.expression)),
        $.expression,
      ),

    postfix_expression: ($) =>
      prec.left(PREC.CALL, seq($.primary_expression, repeat1($.postfix_operator))),
    primary_expression: ($) =>
      choice(
        $.identifier,
        $.number,
        $.string,
        $.multiline_string,
        $.fstring,
        $.import_expression,
        $.function_expression,
        $.boolean,
        $.nil,
        $.table,
        $.parenthesized_expression,
      ),
    postfix_operator: ($) =>
      choice(
        seq("(", sepBy(",", $.call_argument), ")"),
        seq(".", field("property", $.identifier)),
        seq("[", $.expression, "]"),
      ),
    call_argument: ($) =>
      choice(
        $.expression,
        seq(field("name", $.identifier), "=", field("value", $.expression)),
        seq("*", field("value", $.expression)),
      ),
    import_expression: ($) => seq("import", field("module", $.dotted_name)),
    function_expression: ($) =>
      seq("fn", field("parameters", $.parameter_list)),

    unary_expression: ($) =>
      prec(
        PREC.UNARY,
        seq(choice("not", "-", "#"), field("argument", $.expression)),
      ),

    binary_expression: ($) =>
      choice(
        prec.left(PREC.OR, seq($.expression, "or", $.expression)),
        prec.left(PREC.AND, seq($.expression, "and", $.expression)),
        prec.left(PREC.COMPARE, seq($.expression, choice("==", "!=", "<=", ">=", "<", ">", "has"), $.expression)),
        prec.left(PREC.TERM, seq($.expression, choice("+", "-", "..", "<+"), $.expression)),
        prec.left(PREC.FACTOR, seq($.expression, choice("*", "/", "//", "%"), $.expression)),
        prec.right(PREC.POWER, seq($.expression, "**", $.expression)),
      ),

    decorator: (_) => /@[^\n]+/,

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
    number: (_) => /\d+(?:_\d+)*(?:\.\d+(?:_\d+)*)?(?:[eE][+-]?\d+)?/,

    string: (_) =>
      token(
        choice(
          seq('"', repeat(choice(/[^"\\\n]+/, /\\./)), '"'),
          seq("'", repeat(choice(/[^'\\\n]+/, /\\./)), "'"),
        ),
      ),

    multiline_string: (_) => token(/\[\[[\s\S]*\]\]/),

    fstring: ($) =>
      choice(
        seq(
          'f"',
          repeat(choice($.fstring_text_double, $.fstring_escape, $.fstring_interpolation)),
          '"',
        ),
        seq(
          "f'",
          repeat(choice($.fstring_text_single, $.fstring_escape, $.fstring_interpolation)),
          "'",
        ),
      ),
    fstring_text_double: (_) => token(prec(1, /[^"\\{\n]+/)),
    fstring_text_single: (_) => token(prec(1, /[^'\\{\n]+/)),
    fstring_escape: (_) => token(/\\./),
    fstring_interpolation: ($) => seq("{", field("content", $.interpolation_content), "}"),
    interpolation_content: (_) => token(prec(1, /[^}\n]+/)),
  },
});
