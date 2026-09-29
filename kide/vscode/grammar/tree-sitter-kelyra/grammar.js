/// <reference types="tree-sitter-cli/dsl" />
// Editor syntax tree. Keep token boundaries aligned with scanKelyra in the
// extension; semantic parsing remains the compiler's responsibility.
module.exports = grammar({
  name: "kelyra",
  extras: ($) => [/\s/],
  rules: {
    source_file: ($) => repeat(choice(
      $.identifier,
      $.number,
      $.string,
      $.line_comment,
      $.block_comment,
      $.punctuation,
    )),
    identifier: ($) => /[A-Za-z_][A-Za-z0-9_]*/,
    number: ($) => /[0-9][0-9A-Za-z_.]*/,
    string: ($) => token(prec(2, /"(?:\\.|[^"\\])*"/)),
    line_comment: ($) => token(prec(2, /\/\/[^\r\n]*/)),
    block_comment: ($) => seq("/*", repeat(choice(/[^*/]+/, /[*/]/, $.block_comment)), "*/"),
    punctuation: ($) => token(prec(-1, /[^A-Za-z0-9_\s]/)),
  },
});
