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

function repeat1(rule) {
  return seq(rule, repeat(rule));
}

module.exports = grammar({
  name: "pua",

  extras: ($) => [/[ \t\r]+/, $.comment],
  word: ($) => $.identifier,

  externals: ($) => [$.newline, $.indent, $.dedent],

  rules: {
    source_file: ($) => repeat(choice($._statement, $.newline)),

    comment: (_) => token(seq("--", /.*/)),

    _statement: ($) => choice($.simple_statement, $.compound_statement),

    simple_statement: ($) =>
      seq(
        choice(
          $.decorator,
          $.return_statement,
          $.throw_statement,
          $.yield_statement,
          $.break_statement,
          $.continue_statement,
          $.print_statement,
          $.gc_statement,
          $.assignment,
          $.expression_statement,
        ),
        $.newline,
      ),

    compound_statement: ($) =>
      choice(
        $.function_definition,
        $.if_statement,
        $.while_statement,
        $.for_statement,
        $.with_statement,
        $.try_statement,
      ),

    block: ($) => seq($.newline, $.indent, repeat(choice($._statement, $.newline)), $.dedent),

    function_definition: ($) =>
      seq("fn", field("name", $.identifier), field("parameters", $.parameter_list), field("body", $.block)),

    if_statement: ($) =>
      seq(
        "if",
        field("condition", $.expression),
        field("consequence", $.block),
        repeat($.elif_clause),
        optional($.else_clause),
      ),
    elif_clause: ($) => seq("elif", field("condition", $.expression), field("body", $.block)),
    else_clause: ($) => seq("else", field("body", $.block)),

    while_statement: ($) => seq("while", field("condition", $.expression), field("body", $.block)),

    for_statement: ($) =>
      seq(
        "for",
        field("variables", sepBy1(",", $.identifier)),
        "in",
        field("iterables", sepBy1(",", $.expression)),
        field("body", $.block),
      ),

    with_statement: ($) =>
      seq(
        "with",
        field("value", $.expression),
        optional(seq("as", field("name", $.identifier))),
        field("body", $.block),
      ),

    try_statement: ($) =>
      seq("try", field("body", $.block), repeat1($.except_clause), optional($.finally_clause)),
    except_clause: ($) => seq("except", optional(field("name", $.identifier)), field("body", $.block)),
    finally_clause: ($) => seq("finally", field("body", $.block)),

    parameter_list: ($) => seq("(", sepBy(",", $.parameter), ")"),
    parameter: ($) =>
      seq(
        optional("*"),
        field("name", $.identifier),
        optional(seq(":", $.identifier)),
        optional(seq("=", $.expression)),
      ),

    return_statement: ($) => seq("return", field("value", optional($.expression_list))),
    throw_statement: ($) => seq("throw", field("value", optional($.expression_list))),
    yield_statement: ($) => seq("yield", field("value", optional($.expression_list))),
    break_statement: (_) => "break",
    continue_statement: (_) => "continue",
    print_statement: ($) => seq("print", field("value", optional($.expression_list))),
    gc_statement: ($) => seq("gc", field("value", optional($.expression_list))),

    assignment: ($) =>
      prec.right(
        PREC.ASSIGN,
        seq(field("left", $.expression_list), "=", field("right", $.expression_list)),
      ),

    expression_statement: ($) => $.expression,

    expression_list: ($) => sepBy1(",", $.expression),

    expression: ($) =>
      choice($.binary_expression, $.unary_expression, $.postfix_expression, $.primary_expression),

    parenthesized_expression: ($) => seq("(", $.expression, ")"),
    dotted_name: ($) => prec.left(sepBy1(".", $.identifier)),

    table: ($) =>
      seq(
        "{",
        optional(repeat1($._table_separator)),
        optional(
          seq(
            $.table_entry,
            repeat(seq(repeat1($._table_separator), $.table_entry)),
            optional(repeat1($._table_separator)),
          ),
        ),
        "}",
      ),
    _table_separator: ($) => choice(",", $.newline),
    table_entry: ($) =>
      choice(
        seq(field("key", $.identifier), "=", field("value", $.expression)),
        seq("[", field("index", $.expression), "]", "=", field("value", $.expression)),
        $.expression,
      ),

    postfix_expression: ($) => prec.left(PREC.CALL, seq($.primary_expression, repeat1($.postfix_operator))),

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

    function_expression: ($) => seq("fn", field("parameters", $.parameter_list), field("body", $.block)),

    unary_expression: ($) => prec(PREC.UNARY, seq(choice("not", "-", "#"), field("argument", $.expression))),

    binary_expression: ($) =>
      choice(
        prec.left(PREC.OR, seq($.expression, "or", $.expression)),
        prec.left(PREC.AND, seq($.expression, "and", $.expression)),
        prec.left(
          PREC.COMPARE,
          seq($.expression, choice("==", "!=", "<=", ">=", "<", ">", "has"), $.expression),
        ),
        prec.left(PREC.TERM, seq($.expression, choice("+", "-", "..", "<+"), $.expression)),
        prec.left(PREC.FACTOR, seq($.expression, choice("*", "/", "//", "%"), $.expression)),
        prec.right(PREC.POWER, seq($.expression, "**", $.expression)),
      ),

    decorator: (_) => /@[^\n]+/,

    boolean: (_) => choice("true", "false"),
    nil: (_) => "nil",

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
