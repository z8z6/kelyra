"use strict";

const assert = require("node:assert");
const fs = require("node:fs");
const os = require("node:os");
const Module = require("node:module");
const path = require("node:path");

const root = path.join(__dirname, "..");
const manifest = JSON.parse(fs.readFileSync(path.join(root, "package.json")));
const grammar = JSON.parse(
  fs.readFileSync(path.join(root, "syntaxes/kelyra.tmLanguage.json")),
);
const language = JSON.parse(
  fs.readFileSync(path.join(root, "language-configuration.json")),
);
const kelpGrammar = JSON.parse(
  fs.readFileSync(path.join(root, "syntaxes/kelp.tmLanguage.json")),
);
const extension = fs.readFileSync(path.join(root, "src/extension.js"), "utf8");
const jetbrainsBundle = JSON.parse(
  fs.readFileSync(path.join(root, "jetbrains/textmate/package.json")),
);
const jetbrainsPlugin = fs.readFileSync(
  path.join(root, "jetbrains/src/main/resources/META-INF/plugin.xml"),
  "utf8",
);
const jetbrainsLsp = fs.readFileSync(
  path.join(
    root,
    "jetbrains/src/main/java/com/z8z6/kide/KelyraLspServerSupportProvider.java",
  ),
  "utf8",
);

assert.equal(manifest.contributes.languages[0].extensions[0], ".kly");
assert.equal(manifest.contributes.languages[1].filenames[0], "kelp.toml");
assert.equal(kelpGrammar.scopeName, "source.kelp.toml");
assert.equal(manifest.contributes.viewsContainers.activitybar[0].id, "kelp");
assert.equal(manifest.contributes.views.kelp[0].id, "kelp.projects");
assert.equal(manifest.contributes.views.kelp[1].id, "kelp.actions");
assert.equal(manifest.contributes.themes, undefined);
assert.deepEqual(
  manifest.contributes.configuration.properties["kelyra.colorScheme"].enum,
  ["off", "laevatain", "jue", "perlica"],
);
assert.equal(
  manifest.contributes.configuration.properties["kelyra.colorScheme"].default,
  "laevatain",
);
// Both languages ship light and dark file icons.
assert.deepEqual(
  manifest.contributes.languages.map(({ id, icon }) => [id, Object.keys(icon).sort()]),
  [
    ["kelyra", ["dark", "light"]],
    ["kelp", ["dark", "light"]],
  ],
);
const [kelyraIcon, kelpIcon] = manifest.contributes.languages.map(({ icon }) => icon);
assert.equal(kelyraIcon.light, "./icons/kelyra.png");
assert.equal(kelyraIcon.dark, kelyraIcon.light);
assert.deepEqual(
  [...fs.readFileSync(path.join(root, kelyraIcon.light)).subarray(0, 8)],
  [137, 80, 78, 71, 13, 10, 26, 10],
);
for (const iconPath of Object.values(kelpIcon)) {
  const svg = fs.readFileSync(path.join(root, iconPath), "utf8");
  assert.match(svg, /^<svg[\s>]/);
  assert.match(svg, /linearGradient/);
}
assert.ok(manifest.contributes.commands.some(({ command }) => command === "kelp.debug"));
assert.match(extension, /new vscode\.ProcessExecution\(executable, \[command, \.\.\.args\]/);
assert.equal(manifest.dependencies["vscode-languageclient"], "^10.1.1");
assert.equal(
  manifest.contributes.configuration.properties["kelyra.languageServer.path"].default,
  "kelyra-ls",
);
assert.equal(grammar.scopeName, "source.kelyra");
assert.equal(language.comments.lineComment, "//");
for (const file of [
  manifest.contributes.viewsContainers.activitybar[0].icon,
  ...manifest.contributes.grammars.map(({ path: grammarPath }) => grammarPath),
  ...manifest.contributes.snippets.map(({ path: snippetPath }) => snippetPath),
  ...manifest.contributes.languages.map(({ configuration }) => configuration).filter(Boolean),
  ...manifest.contributes.languages.flatMap(({ icon }) => Object.values(icon)),
])
  assert.ok(fs.existsSync(path.join(root, file)), file);
assert.equal(jetbrainsBundle.contributes.languages[1].filenames[0], "kelp.toml");
assert.equal(jetbrainsBundle.contributes.grammars[1].scopeName, "source.kelp.toml");
assert.match(jetbrainsPlugin, /platform\.lsp\.serverSupportProvider/);
assert.match(jetbrainsLsp, /GeneralCommandLine\("kelyra-ls", "--stdio"\)/);
assert.match("fn main", new RegExp(grammar.repository.declarations.patterns[0].match));
assert.ok(
  grammar.repository.declarations.patterns.some(({ match }) =>
    new RegExp(match).test("annotation route"),
  ),
);
assert.ok(
  grammar.repository.declarations.patterns.some(({ match }) =>
    new RegExp(match).test("import web"),
  ),
);
assert.match("c.longlong", new RegExp(grammar.repository.types.patterns[1].match));
assert.ok(
  grammar.repository.types.patterns.some(
    ({ name, match }) => name === "storage.type.meta.kelyra" && new RegExp(match).test("std.meta.Symbol"),
  ),
);
assert.match("@web.route", new RegExp(grammar.repository.annotations.match));
assert.match("usize", new RegExp(grammar.repository.types.patterns[0].match));
// Variables, parameters, members, and calls are scoped by the identifier rules.
const identifierScopes = grammar.repository.identifiers.patterns.flatMap(({ captures }) =>
  Object.values(captures).map(({ name }) => name),
);
for (const scope of [
  "variable.other.readwrite.kelyra",
  "variable.parameter.kelyra",
  "variable.other.member.kelyra",
  "entity.name.function.kelyra",
  "entity.name.namespace.kelyra",
])
  assert.ok(identifierScopes.includes(scope), scope);
assert.ok(
  grammar.patterns.some(({ include }) => include === "#identifiers"),
  "#identifiers is reached",
);
assert.ok(
  grammar.repository.declarations.patterns.some(({ captures }) =>
    Object.values(captures).some(({ name }) => name === "variable.other.readwrite.kelyra"),
  ),
  "let bindings are scoped",
);
assert.ok(
  grammar.repository.types.patterns.some(
    ({ name, match }) => name === "storage.type.function.kelyra" && new RegExp(match).test("fn("),
  ),
  "function types are scoped",
);
const modifiers = new RegExp(grammar.repository.keywords.patterns[1].match);
assert.match("let", modifiers);
assert.doesNotMatch("mut", modifiers);
assert.ok(
  grammar.repository.keywords.patterns.some(({ match }) =>
    new RegExp(match).test("when") && new RegExp(match).test("asm"),
  ),
);
assert.ok(
  grammar.repository.builtinMembers.patterns.some(({ match }) =>
    new RegExp(match).test(".has_annotation"),
  ),
);
assert.ok(
  grammar.repository.builtinMembers.patterns.some(({ match }) =>
    new RegExp(match).test(".in"),
  ),
);
assert.match("value != 42", new RegExp(grammar.repository.operators.match));
assert.match("&value", new RegExp(grammar.repository.operators.match));

const formatterDirectory = fs.mkdtempSync(path.join(os.tmpdir(), "kide-formatter-"));
const formatterPath = path.join(formatterDirectory, "formatter");
fs.writeFileSync(
  formatterPath,
  '#!/usr/bin/env node\nrequire("node:fs").writeFileSync(process.argv[3], "fn main() {\\n  return;\\n}\\n");\n',
);
fs.chmodSync(formatterPath, 0o755);
const originalLoad = Module._load;
Module._load = function (request, parent, isMain) {
  if (request === "vscode") {
    return {
      workspace: {
        getConfiguration: (section) => ({
          get: (key, fallback) =>
            section === "kelyra" && key === "inlayHints.parameterNames" ? "all" : formatterPath,
        }),
        findFiles: async () => ["file:///project/.kelp/dependencies/math/int.kly"],
        fs: {
          readFile: async () =>
            Buffer.from(
              "module math.int;\npub fn scale(value: i32, factor: i32) -> i32 { return 0; }\n",
            ),
        },
        textDocuments: [],
      },
      CompletionItem: class CompletionItem {
        constructor(label, kind) {
          this.label = label;
          this.kind = kind;
        }
      },
      CompletionItemKind: { Class: 1, Reference: 2, Keyword: 3, Property: 4, Module: 5 },
      SnippetString: class SnippetString {
        constructor(value) {
          this.value = value;
        }
      },
      Hover: class Hover {
        constructor(contents) {
          this.contents = contents;
        }
      },
      Range: class Range {
        constructor(startLine, startCharacter, endLine, endCharacter) {
          Object.assign(this, { startLine, startCharacter, endLine, endCharacter });
        }
      },
      TextEdit: { replace: (range, text) => ({ range, text }) },
      InlayHint: class InlayHint {
        constructor(position, label, kind) {
          Object.assign(this, { position, label, kind });
        }
      },
      InlayHintKind: { Type: 1, Parameter: 2 },
      FoldingRange: class FoldingRange {
        constructor(start, end, kind) {
          Object.assign(this, { start, end, kind });
        }
      },
      FoldingRangeKind: { Comment: 1, Imports: 2, Region: 3 },
      Position: class Position {
        constructor(line, character) {
          Object.assign(this, { line, character });
        }
      },
      Uri: { file: (fsPath) => ({ scheme: "file", fsPath }) },
      Location: class Location {
        constructor(uri, range) {
          Object.assign(this, { uri, range });
        }
      },
    };
  }
  return originalLoad(request, parent, isMain);
};

const {
  buildKelyraIndex,
  documentAnnotations,
  format,
  formatterCandidates,
  kelpFieldAt,
  kelpFields,
  kelpArtifactPath,
  kelpProjectTree,
  kelpSectionAt,
  kelyraParameterHints,
  kelyraDescendants,
  kelyraSymbolAt,
  kelyraTokenColors,
  languageKeywords,
  mergeKelyraTokenColors,
  parseKelpManifest,
  parseKelpMembers,
  parseKelyraModule,
  parseKelyraInheritance,
  provideKelpCompletions,
  provideKelpDefinition,
  provideKelyraCompletions,
  provideKelyraFoldingRanges,
  provideKelyraHover,
  provideKelyraInlayHints,
  scanKelpProjects,
} = require("../src/extension.js");
const kelp = "[build]\nsafe-level = 1\n\n[dependencies.kstd]\nrepository = \"git@example\"\n";
assert.equal(kelpSectionAt(kelp, 3), "dependencies");
assert.equal(kelpFieldAt(kelp, 1, 3)[2], "Kelyra runtime safety level.");
assert.equal(kelpFieldAt(kelp, 4, 4)[0], "repository");
const kelyra = "// annotation ignored();\nannotation route(path: std.util.string.StringSlice);\n@route\nlet size: usize;";
const inheritanceSources = [
  ["base.kly", "module demo.base;\n@interface\npub class Base<T> {}\nclass Extra<T> {}\n"],
  ["middle.kly", "module demo.middle;\nimport demo.base;\nclass Middle<T>: Base<T>, Extra<map.List<T, T>> {}\n"],
  ["leaf.kly", "module demo.leaf;\nimport demo.middle;\nclass Leaf: demo.middle.Middle<i32> {}\n"],
  ["other.kly", "module other;\nclass Base {}\nclass Unrelated: Base {}\n"],
];
const inheritingClasses = inheritanceSources.flatMap(([file, source]) =>
  parseKelyraInheritance(source, { toString: () => file }));
assert.deepEqual(kelyraDescendants(inheritingClasses, inheritingClasses[0]).map(({ name }) => name),
  ["Leaf", "Middle"]);
assert.deepEqual(kelyraDescendants(inheritingClasses, inheritingClasses[1]).map(({ name }) => name),
  ["Leaf", "Middle"]);
assert.deepEqual(kelyraDescendants(inheritingClasses, inheritingClasses[4]).map(({ name }) => name),
  ["Unrelated"]);
assert.deepEqual(inheritingClasses[2].bases, ["Base", "Extra"]);
assert.deepEqual(inheritingClasses[3].bases, ["demo.middle.Middle"]);
const document = {
  getText: () => kelyra,
  lineAt: (line) => ({ text: kelyra.split("\n")[line] }),
};
assert.deepEqual(documentAnnotations(kelyra), ["route"]);
assert.equal(kelyraSymbolAt("let size: c.longlong;", 14), "c.longlong");
const completions = provideKelyraCompletions(document, { line: 2, character: 1 });
for (const keyword of ["class", "this", "init", "deinit"])
  assert.ok(completions.some(({ label, documentation }) => label === keyword && documentation));
assert.ok(grammar.repository.declarations.patterns.some(({ match }) => new RegExp(match).test("class Counter")));
assert.equal(completions.find(({ label }) => label === "@route").insertText, "route");
for (const type of [
  "i8", "i16", "i32", "i64", "i128", "isize",
  "u8", "u16", "u32", "u64", "u128", "usize",
  "f32", "f64", "f128", "f256", "f512", "bool", "char", "void",
  "c.char", "c.schar", "c.uchar", "c.short", "c.int", "c.uint", "c.long",
  "c.longlong", "c.size", "c.ptrdiff", "c.bool", "c.wchar",
  "std.util.string.StringSlice", "std.meta.Symbol", "std.meta.Type",
  "std.meta.Class", "std.meta.Field", "std.meta.Function",
  "std.meta.Parameter", "std.meta.Annotation",
]) assert.ok(completions.some(({ label, documentation }) => label === type && documentation));
for (const annotation of ["@target", "@repeatable", "@retention", "@route"])
  assert.ok(completions.some(({ label, documentation }) => label === annotation && documentation));
assert.match(provideKelyraHover(document, { line: 2, character: 3 }).contents, /User-defined/);
assert.match(provideKelyraHover(document, { line: 3, character: 12 }).contents, /Pointer-sized/);
const constantSource = "const COUNT: i32 = 6 * 7;\nfn use() -> i32 { return COUNT; }";
assert.match(provideKelyraHover({
  getText: () => constantSource,
  lineAt: (line) => ({ text: constantSource.split("\n")[line] }),
}, { line: 1, character: 30 }).contents, /COUNT.*42/);
assert.match(provideKelyraHover({
  getText: () => constantSource,
  lineAt: (line) => ({ text: constantSource.split("\n")[line] }),
}, { line: 0, character: 23 }).contents, /COUNT.*42/);
assert.equal(manifest.version, "0.10.5");
assert.deepEqual(formatterCandidates({ uri: {} }, "/tools/kelyra-format"), [
  "/tools/kelyra-format",
]);
assert.ok(
  formatterCandidates(
    { uri: { scheme: "file", fsPath: "/workspace/project/src/main.kly" } },
    "kelyra-format",
  ).includes(path.join(
    "/workspace/project", "kelyra/build/bin",
    process.platform === "win32" ? "kelyra-format.exe" : "kelyra-format",
  )),
);
assert.ok(manifest.contributes.commands.some(({ command }) => command === "kelp.members"));
assert.ok(manifest.contributes.commands.some(({ command }) => command === "kelp.output"));
assert.ok(manifest.activationEvents.includes("onView:kelp.projects"));
assert.equal(manifest.contributes.taskDefinitions[0].type, "kelp");
assert.equal(
  manifest.contributes.configurationDefaults["[kelyra]"]["editor.defaultFormatter"],
  "z8z6.kelyra",
);
assert.equal(manifest.contributes.configurationDefaults["[kelyra]"]["editor.formatOnSave"], true);
// Hints render smaller and padded, like an inline code span.
assert.equal(manifest.contributes.configurationDefaults["[kelyra]"]["editor.inlayHints.fontSize"], 11);
assert.equal(manifest.contributes.configurationDefaults["[kelyra]"]["editor.inlayHints.padding"], true);
assert.ok(!manifest.contributes.commands.some(({ command }) => command === "kelp.format"));
assert.equal(manifest.contributes.menus["editor/context"], undefined);
assert.equal(manifest.contributes.menus["editor/title"], undefined);
assert.ok(
  manifest.contributes.menus["view/title"].some(
    ({ command, when }) => command === "kelp.refreshProjects" && when === "view == kelp.projects",
  ),
);
assert.ok(
  manifest.contributes.menus["view/item/context"].length > 0 &&
    manifest.contributes.menus["view/item/context"].every(({ when }) =>
      when.includes("viewItem == kelp.project"),
    ),
);
assert.equal(manifest.contributes.problemMatchers[0].name, "kelyra");
assert.match(
  "src/main.kly:3:7: error: type mismatch",
  new RegExp(manifest.contributes.problemMatchers[0].pattern.regexp),
);
assert.ok(
  manifest.contributes.menus["view/title"].some(({ command }) => command === "kelp.members"),
);
// Types must be matched before keywords so `std.meta.Symbol` is a type.
const typesPattern = grammar.patterns.findIndex(({ include }) => include === "#types");
const keywordsPattern = grammar.patterns.findIndex(({ include }) => include === "#keywords");
assert.ok(typesPattern !== -1 && typesPattern < keywordsPattern);
assert.match("var", new RegExp(grammar.repository.keywords.patterns[1].match));
assert.match("c.longlong", new RegExp(language.wordPattern));
assert.match("math.vector", new RegExp(language.wordPattern));
for (const [section, field] of [
  ["build", "kind"],
  ["workspace", "members"],
  ["dependencies", "path"],
])
  assert.ok(kelpFields[section].some(([name]) => name === field), `${section}.${field}`);
assert.equal(kelpFieldAt('[build]\nkind = "library"\n', 1, 2)[0], "kind");
assert.equal(kelpFieldAt('[workspace]\nmembers = ["libs/math"]\n', 1, 3)[0], "members");
assert.equal(kelpFieldAt('[dependencies.math]\npath = "../math"\n', 1, 2)[0], "path");
assert.match(
  provideKelyraHover(
    { getText: () => "let size: usize;\n", lineAt: () => ({ text: "let size: usize;" }) },
    { line: 0, character: 1 },
  ).contents,
  /Declares a local/,
);
assert.ok(languageKeywords.when && languageKeywords.meta && languageKeywords.asm);
for (const keyword of ["let", "fn", "pub", "module", "import", "when", "meta", "asm"])
  assert.ok(completions.some(({ label, documentation }) => label === keyword && documentation));
// Field completion inside a section, and section completion after `[`.
const kelpDocument = {
  getText: () => "[build]\nsafe-level = 1\n",
  lineAt: (line) => ({ text: "[build]\nsafe-level = 1\n".split("\n")[line] }),
};
assert.ok(
  provideKelpCompletions(kelpDocument, { line: 1, character: 3 }).some(
    ({ label }) => label === "kind",
  ),
);
const sections = provideKelpCompletions(
  { getText: () => "[bui", lineAt: () => ({ text: "[bui" }) },
  { line: 0, character: 4 },
);
assert.equal(sections.find(({ label }) => label === "build").insertText.value, "build]");
assert.equal(
  sections.find(({ label }) => label === "dependencies.<name>").insertText.value,
  "dependencies.${1:name}]",
);

const snippetContributions = manifest.contributes.snippets.map((entry) => ({
  language: entry.language,
  contents: JSON.parse(fs.readFileSync(path.join(root, entry.path))),
}));
assert.deepEqual(snippetContributions.map(({ language }) => language), ["kelyra", "kelp"]);
assert.ok(snippetContributions[0].contents.Class.body.length > 0);
assert.ok(snippetContributions[1].contents.Build);
assert.equal(manifest.contributes.configurationDefaults["[kelyra]"]["editor.tabSize"], 2);
assert.equal(
  manifest.contributes.configuration.properties["kelyra.inlayHints.parameterNames"].default,
  "literals",
);

// Parameter-name hints resolve local, imported, qualified, and
// constructor calls, and stay silent when the arity does not match.
const index = buildKelyraIndex([
  parseKelyraModule("module app;\n"),
  parseKelyraModule(
    "module math.int;\npub fn scale(value: i32, factor: i32) -> i32 { return 0; }\n",
  ),
  parseKelyraModule(
    "module shapes;\npub class Point { x: i32; y: i32; pub init(x: i32, y: i32) { this.x = x; this.y = y; } }\n",
  ),
  parseKelyraModule(
    "module local;\nfn add(left: i32, right: i32) -> i32 { return 0; }\n",
  ),
]);
assert.deepEqual(
  parseKelyraModule("class Box { init<Args...>(@forward args: ...Args) {} }")
    .classes.get("Box").init,
  ["args"],
);
assert.ok(parseKelyraModule("class Box { fn get_${F.name}() {} }")
  .classes.get("Box").methods.has("get_${F.name}"));
const localCall = "module local;\nfn use(value: i32) -> i32 { return add(value, 2); }";
assert.deepEqual(
  kelyraParameterHints(localCall, index, "all").map(({ name }) => name),
  ["left", "right"],
);
assert.deepEqual(
  kelyraParameterHints(localCall, index, "literals").map(({ name }) => name),
  ["right"],
);
assert.deepEqual(kelyraParameterHints(localCall, index, "off"), []);
assert.deepEqual(
  kelyraParameterHints(
    "module local;\nfn use() -> i32 { let callback = add; return callback(1, 2); }",
    index,
    "all",
  ),
  [],
);
assert.deepEqual(
  kelyraParameterHints("module local;\nfn use() -> i32 { return add(1); }", index, "all"),
  [],
);
assert.deepEqual(
  kelyraParameterHints(
    "module app;\nimport math.int;\nfn use() -> i32 { return math.int.scale(3, 4); }",
    index,
    "all",
  ).map(({ name }) => name),
  ["value", "factor"],
);
assert.deepEqual(
  kelyraParameterHints(
    "module app;\nimport math.int;\nfn use() -> i32 { return scale(3, 4); }",
    index,
    "all",
  ).map(({ name }) => name),
  ["value", "factor"],
);
assert.deepEqual(
  kelyraParameterHints(
    "module app;\nimport shapes;\nfn use() -> i32 { let point = shapes.Point(1, 2); return 0; }",
    index,
    "all",
  ).map(({ name }) => name),
  ["x", "y"],
);

// Folding follows braces and block comments.
const folded = provideKelyraFoldingRanges({
  getText: () => "fn main() -> i32 {\n  if true {\n    return 0;\n  }\n  return 1;\n}\n",
});
assert.ok(folded.some(({ start, end }) => start === 0 && end === 5));
assert.ok(folded.some(({ start, end }) => start === 1 && end === 3));
assert.ok(
  provideKelyraFoldingRanges({ getText: () => "/*\n  block\n*/\nfn main() {}\n" }).some(
    ({ kind }) => kind === 1,
  ),
);
format(
  {
    getText: () => "fn main(){return;}",
    positionAt: () => ({ line: 0, character: 18 }),
  },
  {},
  { onCancellationRequested: () => {} },
)
  .then((edits) => {
    assert.equal(edits[0].text, "fn main() {\n  return;\n}\n");
    fs.rmSync(formatterDirectory, { recursive: true, force: true });
  })
  .then(() => {
    const inlayDocument = {
      getText: () =>
        "fn add(left: i32, right: i32) -> i32 { return 0; }\n" +
        "fn use() -> i32 { return add(1, 2); }\n" +
        "fn scale() -> i32 { return math.int.scale(3, 4); }\n",
    };
    const full = { start: { line: 0, character: 0 }, end: { line: 2, character: 60 } };
    return provideKelyraInlayHints(inlayDocument, full).then((hints) => {
      // Local calls resolve from the document; the qualified call resolves
      // through the dependency index.
      assert.deepEqual(
        hints.map(({ label }) => label),
        ["left:", "right:", "value:", "factor:"],
      );
      assert.deepEqual(hints.map(({ position }) => position.line), [1, 1, 2, 2]);
      const lines = inlayDocument.getText().split("\n");
      assert.deepEqual(
        [hints[0].position.character, hints[1].position.character],
        [lines[1].indexOf("1,"), lines[1].indexOf("2)")],
      );
      const firstLine = { start: { line: 0, character: 0 }, end: { line: 0, character: 60 } };
      return provideKelyraInlayHints(inlayDocument, firstLine).then((filtered) =>
        assert.deepEqual(filtered, []),
      );
    });
  });

// The Kelp project view parses `kelp members`, nests members by directory, and
// falls back to scanning manifests when the executable is unavailable.
assert.deepEqual(
  parseKelpMembers(
    "app app executable .kelp/build/app/app\n. - workspace -\n" +
      "libs/math math library .kelp/build/libs/math/math.o\n",
  ),
  [
    { dir: "app", name: "app", buildKind: "executable", output: ".kelp/build/app/app" },
    {
      dir: "libs/math",
      name: "math",
      buildKind: "library",
      output: ".kelp/build/libs/math/math.o",
    },
  ],
);
const parsed = parseKelpManifest(
  '[project]\nname = "app"\nentry = "src/main.kly"\n\n' +
    '[workspace]\nmembers = ["libs/math"]\n\n[build]\nkind = "library"\n',
);
assert.ok(parsed.hasProject);
assert.equal(parsed.name, "app");
assert.equal(parsed.entry, "src/main.kly");
assert.equal(parsed.buildKind, "library");
assert.deepEqual(parsed.members, ["libs/math"]);
assert.equal(parsed.output, "app.o"); // The default output follows the kind.
// Artifacts live in the workspace cache, mirroring each project's path.
assert.equal(kelpArtifactPath("math.o", "libs/math"), ".kelp/build/libs/math/math.o");
assert.equal(kelpArtifactPath("build/app", "app"), ".kelp/build/app/app");
assert.equal(kelpArtifactPath("app", "."), ".kelp/build/app");
assert.equal(parseKelpManifest("[workspace]\nmembers = []\n").hasProject, false);
assert.deepEqual(parseKelpManifest(
  '[dependencies.kstd]\npath = "../../kstd"\n',
).paths, ["../../kstd"]);

const tree = kelpProjectTree([
  { dir: ".", name: "root", buildKind: "executable", output: ".kelp/build/root" },
  {
    dir: "libs/math",
    name: "math",
    buildKind: "library",
    output: ".kelp/build/libs/math/math.o",
  },
  { dir: "app", name: "app", buildKind: "executable", output: ".kelp/build/app/app" },
]);
assert.deepEqual(
  tree.map(({ type, label }) => [type, label]),
  [["directory", "libs"], ["project", "app"], ["project", "root"]],
);
assert.equal(tree[0].children[0].relDir, "libs/math");
assert.equal(tree[0].children[0].buildKind, "library");
assert.equal(tree[0].children[0].output, ".kelp/build/libs/math/math.o");

const projectRoot = fs.mkdtempSync(path.join(os.tmpdir(), "kide-projects-"));
fs.mkdirSync(path.join(projectRoot, "libs/math"), { recursive: true });
fs.mkdirSync(path.join(projectRoot, "app"), { recursive: true });
fs.mkdirSync(path.join(projectRoot, "untracked"), { recursive: true });
fs.writeFileSync(
  path.join(projectRoot, "kelp.toml"),
  '[workspace]\nmembers = ["libs/math", "app"]\n',
);
fs.writeFileSync(
  path.join(projectRoot, "libs/math/kelp.toml"),
  '[project]\nname = "math"\nentry = "src/math.kly"\n\n[build]\nkind = "library"\n',
);
fs.writeFileSync(
  path.join(projectRoot, "app/kelp.toml"),
  '[project]\nname = "app"\nentry = "src/main.kly"\n\n' +
    '[dependencies.math]\npath = "../libs/math"\n',
);
fs.writeFileSync(
  path.join(projectRoot, "untracked/kelp.toml"),
  '[project]\nname = "untracked"\nentry = "src/main.kly"\n',
);
scanKelpProjects(projectRoot)
  .then(async (projects) => {
    // Only declared members are discovered; the workspace root is a container.
    assert.deepEqual(
      projects.map(({ dir, name, buildKind }) => [dir, name, buildKind]).sort(),
      [
        ["app", "app", "executable"],
        ["libs/math", "math", "library"],
      ],
    );
    const consumer = {
      uri: { scheme: "file", fsPath: path.join(projectRoot, "app/kelp.toml") },
      getText: () => '[dependencies.math]\npath = "../libs/math"\n',
      lineAt: () => ({ text: 'path = "../libs/math"' }),
    };
    const definition = await provideKelpDefinition(consumer, { line: 1, character: 12 });
    assert.equal(definition.uri.fsPath, path.join(projectRoot, "libs/math/kelp.toml"));
    // The target is the start of the manifest, given as a Position.
    assert.equal(definition.range.line, 0);
    assert.equal(await provideKelpDefinition(consumer, { line: 1, character: 2 }), undefined);
    const members = {
      uri: { scheme: "file", fsPath: path.join(projectRoot, "kelp.toml") },
      getText: () => '[workspace]\nmembers = ["app", "missing"]\n',
      lineAt: () => ({ text: 'members = ["app", "missing"]' }),
    };
    assert.equal(
      (await provideKelpDefinition(members, { line: 1, character: 13 })).uri.fsPath,
      path.join(projectRoot, "app/kelp.toml"),
    );
    assert.equal(await provideKelpDefinition(members, { line: 1, character: 22 }), undefined);
  })
  .finally(() => fs.rmSync(projectRoot, { recursive: true, force: true }));

// Applying a palette keeps unrelated customizations and replaces only Kelyra rules.
const existingColors = {
  comments: "#ff0000",
  textMateRules: [
    { scope: "variable.other.demo", settings: { foreground: "#123456" } },
    { scope: ["source.kelyra entity.name.function"], settings: { foreground: "#000000" } },
  ],
};
const merged = mergeKelyraTokenColors(existingColors, kelyraTokenColors.laevatain.dark);
assert.equal(merged.comments, "#ff0000");
assert.equal(merged.textMateRules[0].scope, "variable.other.demo");
assert.equal(
  merged.textMateRules.filter((rule) =>
    [].concat(rule.scope).some((scope) => scope.startsWith("source.kelyra ")),
  ).length,
  kelyraTokenColors.laevatain.dark.length,
  "stale Kelyra rules are replaced, not duplicated",
);
assert.ok(
  merged.textMateRules.some((rule) =>
    [].concat(rule.scope).includes("source.kelyra variable.other.readwrite"),
  ),
);
assert.ok(
  merged.textMateRules.some((rule) =>
    [].concat(rule.scope).includes("source.kelyra entity.name.function"),
  ),
);
assert.deepEqual(
  mergeKelyraTokenColors(undefined, kelyraTokenColors.jue.light).textMateRules,
  kelyraTokenColors.jue.light,
);
assert.equal(
  mergeKelyraTokenColors(undefined, kelyraTokenColors.jue.light).textMateRules[0].settings.foreground,
  "#607178",
);
assert.notDeepEqual(kelyraTokenColors.laevatain.dark, kelyraTokenColors.jue.dark);
for (const scheme of Object.values(kelyraTokenColors))
  for (const palette of Object.values(scheme))
    for (const rule of palette)
      assert.ok([].concat(rule.scope).every((scope) => scope.startsWith("source.kelyra ")));
assert.equal(mergeKelyraTokenColors(existingColors).textMateRules.length, 1);
assert.ok(
  manifest.contributes.commands.some(({ command }) => command === "kelyra.applyTokenColors"),
);
