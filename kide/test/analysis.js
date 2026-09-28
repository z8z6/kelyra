"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");
const { constants, evaluateConstant, inactiveCfgRanges } = require("../src/kelyra-analysis");
const { cachePath, readIndex, writeIndex } = require("../src/kelyra-index");

function scan(source) {
  const tokens = [];
  for (const match of source.matchAll(/"(?:\\.|[^"\\])*"|[A-Za-z_][A-Za-z0-9_]*|[0-9]+|\S/g)) {
    const text = match[0];
    tokens.push({ text, offset: match.index, kind: text.startsWith('"') ? "string" :
      /^[0-9]/.test(text) ? "number" : /^[A-Za-z_]/.test(text) ? "name" : "punct" });
  }
  return tokens;
}

assert.equal(evaluateConstant(scan("(2 + 3) * 4")), 20);
assert.equal(evaluateConstant(scan("true && !false")), true);
assert.equal(evaluateConstant(scan("1 / 0")), undefined);
assert.equal(evaluateConstant(scan("unknown + 1")), undefined);
assert.deepEqual([...constants(scan("const A: i32 = 2 + 3; const B: i32 = A * 4;"))],
  [["A", 5], ["B", 20]]);

const source = '@cfg(os="windows") fn only_windows() { return; }\nfn all() {}';
assert.deepEqual(inactiveCfgRanges(scan(source), source, { os: "linux", arch: "x86_64" }),
  [{ start: 0, end: source.indexOf("\n") }]);
assert.deepEqual(inactiveCfgRanges(scan(source), source, { os: "windows", arch: "x86_64" }), []);
const whole = '@cfg(os="windows") module platform.windows;\nfn work() {}';
assert.deepEqual(inactiveCfgRanges(scan(whole), whole, { os: "linux", arch: "x86_64" }),
  [{ start: 0, end: whole.length }]);

(async () => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), "kide-index-test-"));
  try {
    assert.equal(await readIndex(root), undefined);
    const modules = [{ module: "demo", imports: [], functions: new Map([["f", ["x"]]]),
      classes: new Map([["C", { init: ["x"], methods: new Map([["m", ["y"]]]) }]]) }];
    await writeIndex(root, modules);
    assert.ok(cachePath(root).startsWith(path.join(root, ".kelp")));
    assert.deepEqual(await readIndex(root), modules);
  } finally {
    await fs.rm(root, { recursive: true, force: true });
  }
})().catch((error) => { console.error(error); process.exitCode = 1; });
