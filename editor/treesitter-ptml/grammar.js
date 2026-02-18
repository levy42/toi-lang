module.exports = grammar({
  name: "ptml",

  extras: (_) => [],

  rules: {
    source_file: ($) =>
      repeat(choice($.expression, $.statement, $.text, $.text_brace)),

    text: (_) => token(prec(1, /[^{}]+/)),
    text_brace: (_) => token(prec(-1, /[{}]/)),

    expression: ($) =>
      seq("{{", optional(field("content", $.expression_content)), "}}"),

    statement: ($) =>
      seq("{%", optional(field("content", $.statement_content)), "%}"),

    expression_content: (_) => token(prec(1, /[^}]+/)),
    statement_content: (_) => token(prec(1, /[^%]+/)),
  },
});
