"use strict";

const assert = require("node:assert/strict");
const path = require("node:path");
const { KelyraTree } = require("../src/kelyra-tree");

(async () => {
  const syntax = new KelyraTree();
  await syntax.initialize(path.join(__dirname, "..", "assets", "tree-sitter-kelyra.wasm"));
  let source = '/* outer /* inner */ end */\nconst VALUE: i32 = 42; // note\n';
  const document = {
    uri: { fsPath: "/tmp/kide-tree-test.kly" },
    getText: () => source,
  };
  const fallback = () => { throw new Error("Tree-sitter unexpectedly fell back"); };
  const first = syntax.snapshot(document, fallback);
  assert.deepEqual(first.tokens.filter(({ text }) => text === "VALUE").map(({ offset }) => offset),
    [source.indexOf("VALUE")]);
  assert.ok(!first.tokens.some(({ text }) => text.includes("outer") || text === "note"));
  assert.equal(syntax.snapshot(document, fallback).tokens, first.tokens);

  const start = source.indexOf("42");
  source = source.slice(0, start) + "100" + source.slice(start + 2);
  syntax.change({ document, contentChanges: [{
    rangeOffset: start,
    rangeLength: 2,
    range: { start: { line: 1, character: 19 }, end: { line: 1, character: 21 } },
    text: "100",
  }] });
  const changed = syntax.snapshot(document, fallback);
  assert.ok(changed.tokens.some(({ text, offset }) => text === "100" && offset === start));
  assert.notEqual(changed.tokens, first.tokens);

  // A comment edit can invalidate tokens far beyond the insertion point.
  const commentStart = source.indexOf("const VALUE");
  source = source.slice(0, commentStart) + "/*" + source.slice(commentStart);
  syntax.change({ document, contentChanges: [{
    rangeOffset: commentStart,
    rangeLength: 0,
    range: { start: { line: 1, character: 0 }, end: { line: 1, character: 0 } },
    text: "/*",
  }] });
  const commented = syntax.snapshot(document, fallback);
  assert.ok(!commented.tokens.some(({ text }) => text === "VALUE"));

  const edits = [
    ["const VALUE", "const OTHER"],
    ["/*const", "const"],
    ["100", "1 + 2"],
    ["// note", "// changed"],
  ];
  for (const [from, to] of edits) {
    const offset = source.indexOf(from);
    if (offset < 0) continue;
    const before = source;
    const beforeLine = before.slice(0, offset).split("\n");
    const line = beforeLine.length - 1;
    const character = beforeLine[line].length;
    source = source.slice(0, offset) + to + source.slice(offset + from.length);
    syntax.change({ document, contentChanges: [{
      rangeOffset: offset,
      rangeLength: from.length,
      range: { start: { line, character }, end: { line, character: character + from.length } },
      text: to,
    }] });
    const incremental = syntax.snapshot(document, fallback).tokens;
    const fresh = syntax.snapshot({ uri: { fsPath: `/tmp/kide-tree-fresh-${line}-${character}.kly` },
      getText: () => source }, fallback).tokens;
    assert.deepEqual(incremental, fresh);
  }

  source = 'const TEXT = "😀";\nconst VALUE = 2;';
  const unicode = syntax.snapshot(document, fallback);
  assert.equal(unicode.tokens.find(({ text }) => text === "VALUE").offset,
    source.indexOf("VALUE"));
  syntax.close(document);
  assert.equal(syntax.Documents.size, 0);
  syntax.dispose();
})().catch((error) => { console.error(error); process.exitCode = 1; });
