"use strict";

// Tokenizes a representative Kelyra source with the contributed TextMate
// grammar and asserts the scope of every declaration and reference, so
// highlighting regressions fail the test suite instead of shipping silently.
const assert = require("node:assert");
const fs = require("node:fs");
const path = require("node:path");

let Registry;
let parseRawGrammar;
let oniguruma;
try {
  ({ Registry, parseRawGrammar } = require("vscode-textmate"));
  oniguruma = require("vscode-oniguruma");
} catch (error) {
  console.error(
    "Run `npm install` first: the grammar test needs the vscode-textmate and vscode-oniguruma dev dependencies.",
  );
  throw error;
}

const root = path.join(__dirname, "..");

const sample = [
  "module demo.namespace;", // 0
  "", // 1
  "import demo.helpers;", // 2
  "", // 3
  "class Widget {", // 4
  "  field: i32;", // 5
  "", // 6
  "  init(field: i32) {", // 7
  "    this.field = field;", // 8
  "  }", // 9
  "}", // 10
  "", // 11
  "fn helper(argument: i32) -> i32 {", // 12
  "  let local: i32 = argument;", // 13
  "  return local;", // 14
  "}", // 15
  "", // 16
  "fn apply(callback: fn(i32) -> i32) -> i32 {", // 17
  "  let widget = Widget(1);", // 18
  "  return demo.helpers.helper(widget.field);", // 19
  "}", // 20
].join("\n");

async function tokenize() {
  const wasm = fs.readFileSync(require.resolve("vscode-oniguruma/release/onig.wasm"));
  await oniguruma.loadWASM(wasm.buffer);
  const registry = new Registry({
    onigLib: oniguruma,
    loadGrammar: async (scopeName) =>
      scopeName === "source.kelyra"
        ? parseRawGrammar(
            fs.readFileSync(path.join(root, "syntaxes/kelyra.tmLanguage.json"), "utf8"),
            "kelyra.tmLanguage.json",
          )
        : null,
  });
  const grammar = await registry.loadGrammar("source.kelyra");
  const lines = sample.split("\n");
  const byLine = [];
  let stack = null;
  for (const line of lines) {
    const result = grammar.tokenizeLine(line, stack);
    stack = result.ruleStack;
    byLine.push(
      result.tokens
        .map((token) => ({
          text: line.slice(token.startIndex, token.endIndex),
          scope: token.scopes[token.scopes.length - 1],
        }))
        .filter(({ text }) => text.trim().length > 0),
    );
  }
  return byLine;
}

function scopeOf(byLine, line, text, occurrence = 0) {
  const matches = byLine[line].filter((token) => token.text === text);
  assert.ok(matches.length > occurrence, `no ${text} #${occurrence} on line ${line + 1}`);
  return matches[occurrence].scope;
}

function expectScope(byLine, line, text, scope, occurrence = 0) {
  assert.equal(
    scopeOf(byLine, line, text, occurrence),
    scope,
    `line ${line + 1}: ${text} #${occurrence}`,
  );
}

async function test() {
  const byLine = await tokenize();

  // Modules: the declared path and the imported path are namespaces.
  expectScope(byLine, 0, "demo.namespace", "entity.name.namespace.kelyra");
  expectScope(byLine, 2, "demo.helpers", "entity.name.namespace.kelyra");

  // Types: the class name is a type, builtin types stay builtin types.
  expectScope(byLine, 4, "Widget", "entity.name.type.class.kelyra");
  expectScope(byLine, 5, "i32", "storage.type.builtin.kelyra");
  expectScope(byLine, 17, "callback", "variable.parameter.kelyra");
  expectScope(byLine, 17, "fn", "storage.type.function.kelyra", 1); // `fn(i32)` type

  // Fields, parameters, and locals are all variables, each with its own role.
  expectScope(byLine, 5, "field", "variable.other.member.kelyra");
  expectScope(byLine, 7, "field", "variable.parameter.kelyra");
  expectScope(byLine, 8, "field", "variable.other.member.kelyra"); // this.field
  expectScope(byLine, 8, "field", "variable.other.readwrite.kelyra", 1); // the parameter
  expectScope(byLine, 8, "this", "variable.language.this.kelyra");
  expectScope(byLine, 13, "let", "storage.modifier.kelyra");
  expectScope(byLine, 13, "local", "variable.other.readwrite.kelyra");
  expectScope(byLine, 14, "local", "variable.other.readwrite.kelyra");
  expectScope(byLine, 12, "argument", "variable.parameter.kelyra");

  // Functions: declarations, construction calls, and qualified calls.
  expectScope(byLine, 12, "helper", "entity.name.function.kelyra");
  expectScope(byLine, 18, "Widget", "entity.name.function.kelyra");
  expectScope(byLine, 19, "demo.helpers", "entity.name.namespace.kelyra");
  expectScope(byLine, 19, "helper", "entity.name.function.kelyra");
  expectScope(byLine, 19, "field", "variable.other.member.kelyra");

  // Keywords and operators are unaffected by the identifier rules.
  expectScope(byLine, 12, "fn", "storage.type.function.kelyra");
  expectScope(byLine, 14, "return", "keyword.control.kelyra");
}

test().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
