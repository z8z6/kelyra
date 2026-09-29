"use strict";

const childProcess = require("node:child_process");
const fs = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");
const { promisify } = require("node:util");
const vscode = require("vscode");
const { constantExpressions, inactiveCfgRanges } = require("./kelyra-analysis");
const { readIndex, writeIndex } = require("./kelyra-index");
const { KelyraTree } = require("./kelyra-tree");
const execFile = promisify(childProcess.execFile);

let client;
const kelyraTree = new KelyraTree();

const kelyraTypes = {
  i8: "8-bit signed integer.",
  i16: "16-bit signed integer.",
  i32: "32-bit signed integer.",
  i64: "64-bit signed integer.",
  i128: "128-bit signed integer.",
  isize: "Pointer-sized signed integer.",
  void: "No return value. Equivalent to omitting a function's return type; use return; or reach the end of its body.",
  u8: "8-bit unsigned integer.",
  u16: "16-bit unsigned integer.",
  u32: "32-bit unsigned integer.",
  u64: "64-bit unsigned integer.",
  u128: "128-bit unsigned integer.",
  usize: "Pointer-sized unsigned integer.",
  f32: "32-bit floating-point number.",
  f64: "64-bit floating-point number.",
  f128: "128-bit floating-point number.",
  f256: "256-bit floating-point type; currently limited to signatures and forwarding.",
  f512: "512-bit floating-point type; currently limited to signatures and forwarding.",
  bool: "Boolean value lowered to one bit.",
  char: "Unicode scalar value lowered to 32 bits.",
  "c.char": "C `char` with the target ABI width.",
  "c.schar": "C `signed char`.",
  "c.uchar": "C `unsigned char`.",
  "c.short": "C `short`.",
  "c.int": "C `int`.",
  "c.uint": "C `unsigned int`.",
  "c.long": "C `long` with the target ABI width.",
  "c.longlong": "C `long long`.",
  "c.size": "C `size_t` with the target ABI width.",
  "c.ptrdiff": "C `ptrdiff_t` with the target ABI width.",
  "c.bool": "C `_Bool`.",
  "c.float": "C `float`.",
  "c.double": "C `double`.",
  "c.wchar": "C `wchar_t` with the target ABI width.",
  "std.util.string.StringSlice": "Read-only string slice; annotation literals are evaluated at compile time.",
  "std.meta.Symbol": "Compile-time handle to a declaration.",
  "std.meta.Type": "Compile-time handle to a type.",
  "std.meta.Class": "Compile-time handle to a class.",
  "std.meta.Field": "Compile-time handle to a field.",
  "std.meta.Function": "Compile-time handle to a function or method.",
  "std.meta.Parameter": "Compile-time handle to a parameter.",
  "std.meta.Annotation": "Compile-time handle to an annotation declaration.",
};

const builtinAnnotations = {
  main: "Marks the program entry point.",
  cfg: "Includes a declaration when its target matches, for example `@cfg(os.Windows, arch.X86_64)` or `@cfg(arch.AArch64)`.",
  extern: "Declares an external symbol. An optional second string names its DLL or import library: `@extern(\"CreateWindowExW\", \"user32\")`.",
  layout: "Selects a type's memory layout, for example `@layout(Layout.C)` for C ABI records.",
  callconv: "Selects a function's calling convention, for example `@callconv(cc.System)`.",
  interface: "Declares an interface class. Implementing classes provide methods with matching parameter signatures.",
  final: "Prevents inheritance from a class or further overriding of a method.",
  meta: "Marks a module, class, function, or method as compile-time only.",
  singleton: "Standard library annotation that adds static storage and a lazy `Class.instance()` method. Import `std.sync.singleton` to use it.",
  static: "Marks a class field or method as static. Each concrete generic class has its own field storage; static methods have no `this` receiver.",
  forward: "Forwards a constructor parameter pack with each argument's type and value category preserved.",
  target: "Restricts an annotation to declaration kinds. `@target(Target.Class T)` binds `T` to the annotated class in an implementation body.",
  repeatable: "Allows an annotation to appear more than once on the same declaration.",
  retention: "Sets annotation retention to `source` or `compile`; `compile` is the default.",
  vertex: "Marks a vertex shader entry function.",
  fragment: "Marks a fragment shader entry function.",
  position: "Marks the clip-space position output field of a vertex shader.",
  location: "Assigns a shader input or output location.",
  vertex_index: "Supplies the current vertex index to a vertex shader.",
  "std.graphics.binding": "Assigns a shader resource to a descriptor set and slot: `@std.graphics.binding(set, slot)`.",
  "std.graphics.resource": "Declares a shader resource type such as `Texture2D` or `SamplerState`.",
  gpu_builtin: "Maps a declared shader function to a target GPU intrinsic.",
};

const classKeywords = {
  class: "Value type with fields, methods, inheritance, direct construction and scope-based RAII destruction.",
  this: "Implicit pointer to the current class instance. Optional for unambiguous member access; use this.field when a parameter or local shadows a field.",
  init: "Constructor. Initializes each field once in declaration order before ordinary statements. `init<Args...>(@forward args: ...Args)` forwards a compile-time parameter pack. A class without init gets a generated no-argument constructor.",
  deinit: "Destructor. Runs automatically on normal scope exits, followed by class fields in reverse order. Cannot be called explicitly.",
};

const languageKeywords = {
  let: "Declares a local with a type, an initial value, or both.",
  fn: "Declares a function. Functions and methods can overload by parameter types; return type alone does not distinguish overloads.",
  enum: "Declares a named set of constants, optionally with an underlying integer type.",
  alias: "Declares a type alias in a module, class, or local scope. Class aliases can use the class's type parameters; aliases can also declare their own type parameters.",
  pub: "Exports a declaration to importing modules.",
  module: "Declares this file's module.",
  import: "Imports a module's public names. Qualify a name with its module when imports are ambiguous; use `import c \"header.h\"` for C headers.",
  annotation: "Declares a compile-time annotation.",
  if: "Conditional branch; the braces are required.",
  else: "Alternative branch of an if.",
  while: "Repeats a block while the condition holds.",
  return: "Returns zero or more values and runs the pending scope cleanups.",
  break: "Leaves the innermost loop, destroying its locals first.",
  continue: "Jumps to the next iteration, destroying loop locals first.",
  when: "Compile-time conditional; only the selected branch is resolved.",
  meta: "Compile-time reflection operator, as in `meta(target)`.",
  asm: "Inline assembly block with `.in`, `.out`, and `.op` chains.",
  true: "Boolean true.",
  false: "Boolean false.",
};

function documentAnnotations(text) {
  return [...text.matchAll(/^\s*(?:pub\s+)?annotation\s+([A-Za-z_][A-Za-z0-9_]*)/gm)].map(
    (match) => match[1],
  );
}

function kelyraSymbolAt(line, character) {
  for (const match of line.matchAll(/@?[A-Za-z_][A-Za-z0-9_.]*/g)) {
    if (character >= match.index && character <= match.index + match[0].length)
      return match[0];
  }
  return undefined;
}

function provideKelyraHover(document, position) {
  const source = document.getText();
  const tokens = kelyraTree.snapshot(document, scanKelyra).tokens;
  const symbol = kelyraSymbolAt(document.lineAt(position.line).text, position.character);
  const offset = source.split("\n").slice(0, position.line).reduce((sum, line) =>
    sum + line.length + 1, position.character);
  const expressions = constantExpressions(tokens);
  const expression = expressions.find(({ start, end }) =>
    start <= offset && offset < end);
  if (expression)
    return new vscode.Hover(`**${expression.name}** = \`${String(expression.value)}\` (compile-time constant)`);
  if (!symbol) return undefined;
  const name = symbol.startsWith("@") ? symbol.slice(1) : symbol;
  const constants = new Map(expressions.map(({ name, value }) => [name, value]));
  if (constants.has(name))
    return new vscode.Hover(`**${name}** = \`${String(constants.get(name))}\` (compile-time constant)`);
  if (kelyraTypes[name]) return new vscode.Hover(`**${name}**\n\n${kelyraTypes[name]}`);
  if (classKeywords[name]) return new vscode.Hover(`**${name}**\n\n${classKeywords[name]}`);
  if (languageKeywords[name])
    return new vscode.Hover(`**${name}**\n\n${languageKeywords[name]}`);
  if (builtinAnnotations[name])
    return new vscode.Hover(`**@${name}**\n\n${builtinAnnotations[name]}`);
  if (documentAnnotations(document.getText()).includes(name))
    return new vscode.Hover(`**@${name}**\n\nUser-defined annotation in this document.`);
  return undefined;
}

function provideKelyraCompletions(document, position) {
  const prefix = document.lineAt(position.line).text.slice(0, position.character);
  const annotationPrefix = prefix.match(/@(?:[A-Za-z_][A-Za-z0-9_]*\.)*[A-Za-z_][A-Za-z0-9_]*$|@$/)?.[0];
  const afterAt = Boolean(annotationPrefix);
  const annotations = [...Object.entries(builtinAnnotations), ...documentAnnotations(document.getText()).map(
    (name) => [name, "User-defined annotation in this document."],
  )];
  return [
    ...[...Object.entries(classKeywords), ...Object.entries(languageKeywords)].map(
      ([name, documentation]) => {
        const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Keyword);
        item.documentation = documentation;
        return item;
      },
    ),
    ...Object.entries(kelyraTypes).map(([name, documentation]) => {
      const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Class);
      item.documentation = documentation;
      return item;
    }),
    ...annotations.map(([name, documentation]) => {
      const item = new vscode.CompletionItem(`@${name}`, vscode.CompletionItemKind.Reference);
      item.insertText = afterAt ? name : `@${name}`;
      if (annotationPrefix?.includes(".")) {
        item.insertText = `@${name}`;
        item.range = new vscode.Range(position.line,
          position.character - annotationPrefix.length,
          position.line, position.character);
      }
      item.documentation = documentation;
      return item;
    }),
  ];
}

// Names that are never the callee of a call expression.
const kelyraControlNames = new Set([
  "if", "while", "when", "meta", "asm", "fn", "init", "deinit", "return", "let",
]);
// Declarations whose following name is not a call target.
const kelyraDeclarations = new Set(["fn", "annotation"]);

// Tokenizes Kelyra source, skipping comments, and records byte offsets.
function scanKelyra(text) {
  const lineStarts = [0];
  for (let index = 0; index < text.length; ++index)
    if (text[index] === "\n") lineStarts.push(index + 1);
  const positionAt = (offset) => {
    let low = 0;
    let high = lineStarts.length - 1;
    while (low < high) {
      const middle = (low + high + 1) >> 1;
      if (lineStarts[middle] <= offset) low = middle;
      else high = middle - 1;
    }
    return { line: low, character: offset - lineStarts[low] };
  };
  const tokens = [];
  let index = 0;
  while (index < text.length) {
    const character = text[index];
    const next = text[index + 1];
    if (character === "/" && next === "/") {
      while (index < text.length && text[index] !== "\n") ++index;
      continue;
    }
    if (character === "/" && next === "*") {
      let depth = 1;
      index += 2;
      while (index < text.length && depth > 0) {
        if (text[index] === "/" && text[index + 1] === "*") {
          ++depth;
          index += 2;
        } else if (text[index] === "*" && text[index + 1] === "/") {
          --depth;
          index += 2;
        } else ++index;
      }
      continue;
    }
    if (character === '"') {
      const start = index++;
      while (index < text.length) {
        if (text[index] === "\\") index += 2;
        else if (text[index] === '"') { ++index; break; }
        else ++index;
      }
      tokens.push({ kind: "string", text: text.slice(start, index), offset: start });
      continue;
    }
    if (/[A-Za-z_]/.test(character) || (character === "$" && next === "{")) {
      const start = index;
      while (index < text.length) {
        if (/[A-Za-z0-9_]/.test(text[index])) { ++index; continue; }
        if (text[index] !== "$" || text[index + 1] !== "{") break;
        index += 2;
        let depth = 1;
        let quoted = false;
        while (index < text.length && depth > 0) {
          const part = text[index++];
          if (quoted && part === "\\" && index < text.length) ++index;
          else if (part === '"') quoted = !quoted;
          else if (!quoted && part === "{") ++depth;
          else if (!quoted && part === "}") --depth;
        }
      }
      tokens.push({ kind: "name", text: text.slice(start, index), offset: start });
      continue;
    }
    if (/[0-9]/.test(character)) {
      const start = index;
      while (index < text.length && /[0-9A-Za-z_.]/.test(text[index])) ++index;
      tokens.push({ kind: "number", text: text.slice(start, index), offset: start });
      continue;
    }
    if (/\s/.test(character)) { ++index; continue; }
    tokens.push({ kind: "punct", text: character, offset: index });
    ++index;
  }
  return { tokens, positionAt };
}

function kelyraEnumMemberRanges(text, knownEnums = new Map()) {
  const { tokens } = scanKelyra(text);
  const enums = new Map(knownEnums);
  const ranges = [];
  for (let index = 0; index + 2 < tokens.length; ++index) {
    if (tokens[index].text !== "enum" || tokens[index + 1].kind !== "name") continue;
    const name = tokens[index + 1].text;
    let open = index + 2;
    while (open < tokens.length && tokens[open].text !== "{" && tokens[open].text !== ";") ++open;
    if (tokens[open]?.text !== "{") continue;
    const members = new Set();
    let depth = 1;
    let expectMember = true;
    for (let cursor = open + 1; cursor < tokens.length && depth; ++cursor) {
      const token = tokens[cursor];
      if (token.text === "{") { ++depth; continue; }
      if (token.text === "}") { --depth; continue; }
      if (depth !== 1) continue;
      if (token.text === ",") { expectMember = true; continue; }
      if (expectMember && token.kind === "name") {
        members.add(token.text);
        ranges.push([token.offset, token.offset + token.text.length]);
        expectMember = false;
      }
    }
    enums.set(name, members);
  }
  // Builtin enum declarations may live in the standard library, outside the file.
  for (const [name, members] of Object.entries({
    os: ["Any", "Windows", "Linux", "MacOS"],
    arch: ["Any", "X86_64", "AArch64"],
    cc: ["C", "System"],
    Layout: ["System", "C"],
    Target: ["Module", "Function", "Class", "Field", "Method", "Constructor", "Destructor", "Parameter", "Annotation"],
    InlineMode: ["Auto", "Always"],
    Retention: ["Source", "Compile"],
  })) if (!enums.has(name)) enums.set(name, new Set(members));
  for (let index = 0; index + 2 < tokens.length; ++index) {
    const owner = tokens[index];
    const member = tokens[index + 2];
    if (owner.kind === "name" && tokens[index + 1].text === "." &&
        member.kind === "name" && enums.get(owner.text)?.has(member.text))
      ranges.push([member.offset, member.offset + member.text.length]);
  }
  return ranges;
}

const kelyraParameterNames = (tokens, open) => {
  const names = [];
  let depth = 0;
  let expectName = true;
  for (let index = open; index < tokens.length; ++index) {
    const token = tokens[index];
    if (token.text === "(") { ++depth; continue; }
    if (token.text === ")") {
      --depth;
      if (depth === 0) return { names, end: index };
      continue;
    }
    if (token.text === "," && depth === 1) { expectName = true; continue; }
    if (token.text === "@" && depth === 1) {
      while (tokens[index + 1]?.kind === "name") {
        ++index;
        if (tokens[index + 1]?.text !== ".") break;
        ++index;
      }
      continue;
    }
    if (token.kind === "punct") continue;
    if (depth === 1 && expectName && token.kind === "name") {
      names.push(token.text);
      expectName = false;
    }
  }
  return { names: [], end: -1 };
};

// Collects module name, imports, function signatures, and class members.
function parseKelyraTokens(tokens) {
  const parsed = { module: "", imports: [], functions: new Map(), annotations: new Map(), enums: new Map(), classes: new Map() };
  for (let index = 0; index < tokens.length; ++index) {
    const token = tokens[index];
    if (token.kind !== "name") continue;
    if (token.text === "module") {
      const parts = [];
      let cursor = index + 1;
      while (cursor < tokens.length && tokens[cursor].kind === "name") {
        parts.push(tokens[cursor].text);
        if (tokens[cursor + 1]?.text !== ".") break;
        cursor += 2;
      }
      parsed.module = parts.join(".");
      continue;
    }
    if (token.text === "import") {
      const parts = [];
      let cursor = index + 1;
      while (cursor < tokens.length) {
        if (tokens[cursor].kind === "name") { parts.push(tokens[cursor].text); ++cursor; continue; }
        if (tokens[cursor].text === ".") {
          ++cursor;
          continue;
        }
        break;
      }
      if (parts.length) parsed.imports.push({ name: parts.join(".") });
      continue;
    }
    if (token.text === "enum" && tokens[index + 1]?.kind === "name") {
      const name = tokens[index + 1].text;
      let open = index + 2;
      while (open < tokens.length && tokens[open].text !== "{" && tokens[open].text !== ";") ++open;
      if (tokens[open]?.text !== "{") continue;
      const members = new Set();
      let expectMember = true;
      let depth = 1;
      for (let cursor = open + 1; cursor < tokens.length && depth; ++cursor) {
        const part = tokens[cursor];
        if (part.text === "{") { ++depth; continue; }
        if (part.text === "}") { --depth; continue; }
        if (depth !== 1) continue;
        if (part.text === ",") { expectMember = true; continue; }
        if (expectMember && part.kind === "name") {
          members.add(part.text);
          expectMember = false;
        }
      }
      parsed.enums.set(name, members);
      continue;
    }
    if (token.text === "class" && tokens[index + 1]?.kind === "name") {
      const name = tokens[index + 1].text;
      let open = index + 2;
      while (open < tokens.length && tokens[open].text !== "{") ++open;
      let depth = 0;
      let close = tokens.length;
      for (let cursor = open; cursor < tokens.length; ++cursor) {
        if (tokens[cursor].text === "{") ++depth;
        else if (tokens[cursor].text === "}" && --depth === 0) { close = cursor; break; }
      }
      const info = { init: undefined, methods: new Map() };
      // Nested classes are not part of the language, so the body can be skipped.
      for (let cursor = open + 1; cursor < close; ++cursor) {
        const member = tokens[cursor];
        if (member.text === "fn" && tokens[cursor + 1]?.kind === "name" &&
            tokens[cursor + 2]?.text === "(") {
          info.methods.set(tokens[cursor + 1].text, kelyraParameterNames(tokens, cursor + 2).names);
        } else if (member.text === "init") {
          let parameters = cursor + 1;
          if (tokens[parameters]?.text === "<") {
            while (parameters < close && tokens[parameters].text !== ">") ++parameters;
            ++parameters;
          }
          if (tokens[parameters]?.text === "(")
            info.init = kelyraParameterNames(tokens, parameters).names;
        }
      }
      parsed.classes.set(name, info);
      index = close;
      continue;
    }
    if (token.text === "fn" && tokens[index + 1]?.kind === "name") {
      let parameters = index + 2;
      if (tokens[parameters]?.text === "<") {
        while (parameters < tokens.length && tokens[parameters].text !== ">") ++parameters;
        ++parameters;
      }
      if (tokens[parameters]?.text === "(")
        parsed.functions.set(tokens[index + 1].text, kelyraParameterNames(tokens, parameters).names);
    }
    if (token.text === "annotation" && tokens[index + 1]?.kind === "name" &&
        tokens[index + 2]?.text === "(")
      parsed.annotations.set(tokens[index + 1].text,
        kelyraParameterNames(tokens, index + 2).names);
  }
  return parsed;
}

const parseKelyraModule = (text) => parseKelyraTokens(scanKelyra(text).tokens);

function collectKelyraLocals(tokens) {
  const locals = new Set();
  for (let index = 0; index < tokens.length; ++index) {
    if (tokens[index].text === "let") {
      if (tokens[index + 1]?.kind === "name") locals.add(tokens[index + 1].text);
      for (let cursor = index + 2; tokens[index + 1]?.text === "(" && cursor < tokens.length; ++cursor) {
        if (tokens[cursor].text === ")") break;
        if (tokens[cursor].kind === "name") locals.add(tokens[cursor].text);
      }
    }
    if (tokens[index].text === "fn" && tokens[index + 1]?.kind === "name" &&
        tokens[index + 2]?.text === "(")
      for (const name of kelyraParameterNames(tokens, index + 2).names) locals.add(name);
  }
  return locals;
}

function collectKelyraCalls(tokens) {
  const calls = [];
  for (let index = 0; index < tokens.length; ++index) {
    if (tokens[index].text !== "(") continue;
    let cursor = index - 1;
    if (cursor < 0 || tokens[cursor].kind !== "name") continue;
    const chain = [tokens[cursor].text];
    --cursor;
    while (cursor >= 1 && tokens[cursor].text === "." && tokens[cursor - 1].kind === "name") {
      chain.unshift(tokens[cursor - 1].text);
      cursor -= 2;
    }
    const lead = tokens[cursor];
    if (lead && kelyraDeclarations.has(lead.text)) continue;
    if (chain.length === 1 && kelyraControlNames.has(chain[0])) continue;
    const args = [];
    let depth = 0;
    let expectStart = false;
    for (let scan = index; scan < tokens.length; ++scan) {
      const token = tokens[scan];
      if (token.text === "(") {
        if (++depth === 1) expectStart = true;
        continue;
      }
      if (token.text === ")") {
        if (--depth === 0) break;
        continue;
      }
      if (token.text === "," && depth === 1) { expectStart = true; continue; }
      if (depth === 1 && expectStart) {
        args.push({
          offset: token.offset,
          literal: token.kind !== "name" || token.text === "true" || token.text === "false",
          named: token.kind === "name" && tokens[scan + 1]?.text === "=",
          enumOwner: token.kind === "name" && tokens[scan + 1]?.text === "."
            ? token.text : undefined,
        });
        expectStart = false;
      }
    }
    calls.push({ chain, args, annotation: lead?.text === "@" });
  }
  return calls;
}

// Indexes every parsed module so calls can be resolved across the workspace.
function buildKelyraIndex(parsedModules) {
  const index = { modules: new Map(), functions: new Map(), annotations: new Map(), enums: new Map(), methods: new Map(), classes: new Map() };
  const push = (map, key, value) => {
    if (!map.has(key)) map.set(key, []);
    map.get(key).push(value);
  };
  for (const parsed of parsedModules) {
    if (parsed.module && !index.modules.has(parsed.module)) index.modules.set(parsed.module, parsed);
    for (const [name, params] of parsed.functions)
      push(index.functions, name, { params, module: parsed.module });
    for (const [name, params] of parsed.annotations)
      push(index.annotations, name, { params, module: parsed.module });
    for (const [name, members] of parsed.enums || [])
      push(index.enums, name, { members, module: parsed.module });
    for (const [name, info] of parsed.classes) {
      push(index.classes, name, { info, module: parsed.module });
      for (const [method, params] of info.methods)
        push(index.methods, method, { params, module: parsed.module, owner: name });
    }
  }
  return index;
}

function resolveKelyraCall(call, current, index, locals) {
  const { chain } = call;
  if (call.annotation) {
    const name = chain[chain.length - 1];
    const builtins = {
      cfg: ["os", "arch"], layout: ["kind"], callconv: ["abi"],
      extern: ["symbol", "library"], inline: ["mode"],
      deprecated: ["message"], accessors: ["get", "set"],
    };
    if (chain.length === 1 && current.annotations.has(name))
      return current.annotations.get(name);
    if (chain.length === 1 && builtins[name]) return builtins[name];
    for (let length = chain.length - 1; length >= 1; --length) {
      const module = chain.slice(0, length).join(".");
      const found = index.modules.get(module)?.annotations.get(chain.slice(length).join("."));
      if (found) return found;
    }
    let found;
    for (const imported of current.imports) {
      const candidate = index.modules.get(imported.name)?.annotations.get(name);
      if (candidate) {
        if (found) return undefined;
        found = candidate;
      }
    }
    return found || (index.annotations.get(name)?.length === 1
      ? index.annotations.get(name)[0].params : undefined);
  }
  if (chain.length === 1) {
    const name = chain[0];
    if (locals.has(name)) return undefined;
    const own = current.functions.get(name);
    if (own) return own;
    const ownClass = current.classes.get(name);
    if (ownClass && ownClass.init) return ownClass.init;
    let importedResult;
    for (const imported of current.imports) {
      const target = index.modules.get(imported.name);
      if (!target) continue;
      const fn = target.functions.get(name);
      if (fn) {
        if (importedResult) return undefined;
        importedResult = fn;
      }
      const cls = target.classes.get(name);
      if (cls && cls.init) {
        if (importedResult) return undefined;
        importedResult = cls.init;
      }
    }
    if (importedResult) return importedResult;
    const functions = index.functions.get(name);
    if (functions && functions.length === 1) return functions[0].params;
    const classes = index.classes.get(name);
    if (classes && classes.length === 1 && classes[0].info.init) return classes[0].info.init;
    return undefined;
  }
  for (let length = chain.length - 1; length >= 1; --length) {
    const target = index.modules.get(chain.slice(0, length).join("."));
    if (!target) continue;
    const rest = chain.slice(length);
    if (rest.length === 1) {
      const fn = target.functions.get(rest[0]);
      if (fn) return fn;
      const cls = target.classes.get(rest[0]);
      if (cls && cls.init) return cls.init;
    } else if (rest.length === 2) {
      const cls = target.classes.get(rest[0]);
      if (cls) return rest[1] === "init" ? cls.init : cls.methods.get(rest[1]);
    }
    return undefined;
  }
  const method = chain[chain.length - 1];
  if (method === "init" || method === "deinit") return undefined;
  const methods = index.methods.get(method);
  return methods && methods.length === 1 ? methods[0].params : undefined;
}

// Returns `{ offset, name }` for every argument that should show its parameter
// name. Literal-only mode keeps hints on numbers, strings, and booleans.
function kelyraParameterHints(text, index, mode, knownTokens) {
  if (mode === "off") return [];
  const tokens = knownTokens || scanKelyra(text).tokens;
  const current = parseKelyraTokens(tokens);
  const locals = collectKelyraLocals(tokens);
  const hints = [];
  for (const call of collectKelyraCalls(tokens)) {
    const params = resolveKelyraCall(call, current, index, locals);
    if (!params || (call.annotation ? call.args.length > params.length
      : params.length !== call.args.length)) continue;
    for (let position = 0; position < call.args.length; ++position) {
      const argument = call.args[position];
      if (argument.named || (mode === "literals" && !argument.literal && !call.annotation)) continue;
      const name = call.annotation && argument.enumOwner &&
        params.includes(argument.enumOwner) ? argument.enumOwner : params[position];
      if (name) hints.push({ offset: argument.offset, name });
    }
  }
  return hints;
}

function provideKelyraFoldingRanges(document) {
  const text = document.getText();
  const { positionAt } = scanKelyra(text);
  const ranges = [];
  const braces = [];
  let blockStart = -1;
  let blockDepth = 0;
  let inString = false;
  let escaped = false;
  let inLineComment = false;
  for (let index = 0; index < text.length; ++index) {
    const character = text[index];
    const next = text[index + 1];
    if (inLineComment) {
      if (character === "\n") inLineComment = false;
      continue;
    }
    if (blockDepth > 0) {
      if (character === "/" && next === "*") { ++blockDepth; ++index; continue; }
      if (character === "*" && next === "/") {
        if (--blockDepth === 0) {
          const start = positionAt(blockStart).line;
          const end = positionAt(index).line;
          if (end > start)
            ranges.push(new vscode.FoldingRange(start, end, vscode.FoldingRangeKind.Comment));
        }
        ++index;
        continue;
      }
      continue;
    }
    if (inString) {
      if (escaped) escaped = false;
      else if (character === "\\") escaped = true;
      else if (character === '"') inString = false;
      continue;
    }
    if (character === "/" && next === "/") { inLineComment = true; ++index; continue; }
    if (character === "/" && next === "*") { blockDepth = 1; blockStart = index; ++index; continue; }
    if (character === '"') { inString = true; continue; }
    if (character === "{") braces.push(index);
    else if (character === "}" && braces.length) {
      const start = positionAt(braces.pop()).line;
      const end = positionAt(index).line;
      if (end > start) ranges.push(new vscode.FoldingRange(start, end));
    }
  }
  return ranges;
}

let kelyraIndexPromise;
let kelyraIndexGeneration = 0;
let kelyraIndexRefresh;
let inlayHintsChanged;
let inheritanceLensesChanged;
let inactiveCfgDecoration;
let enumMemberDecoration;
let enumMemberDecorationContext;
let cfgRefreshTimer;

function parseKelyraInheritance(text, uri) {
  const { tokens, positionAt } = scanKelyra(text);
  const parsed = parseKelyraTokens(tokens);
  const classes = [];
  const skipTypeArguments = (start) => {
    if (tokens[start]?.text !== "<") return start;
    let depth = 0;
    for (let cursor = start; cursor < tokens.length; ++cursor) {
      if (tokens[cursor].text === "<") ++depth;
      else if (tokens[cursor].text === ">" && --depth === 0) return cursor + 1;
    }
    return tokens.length;
  };
  for (let index = 0; index < tokens.length - 1; ++index) {
    if (tokens[index].text !== "class" || tokens[index + 1].kind !== "name") continue;
    const name = tokens[index + 1].text;
    const bases = [];
    let cursor = skipTypeArguments(index + 2);
    if (tokens[cursor]?.text === ":") {
      ++cursor;
      while (cursor < tokens.length && tokens[cursor].text !== "{") {
        const parts = [];
        if (tokens[cursor]?.kind !== "name") break;
        parts.push(tokens[cursor++].text);
        while (tokens[cursor]?.text === "." && tokens[cursor + 1]?.kind === "name") {
          cursor++;
          parts.push(tokens[cursor++].text);
        }
        bases.push(parts.join("."));
        cursor = skipTypeArguments(cursor);
        if (tokens[cursor]?.text !== ",") break;
        ++cursor;
      }
    }
    classes.push({ name, module: parsed.module, imports: parsed.imports, bases,
      uri, position: positionAt(tokens[index + 1].offset) });
    // The lexer also sees method bodies. Skip the class to avoid a `class`
    // token in an expression being treated as another declaration.
    while (cursor < tokens.length && tokens[cursor].text !== "{") ++cursor;
    if (tokens[cursor]?.text === "{") {
      let depth = 1;
      while (depth && ++cursor < tokens.length)
        depth += tokens[cursor].text === "{" ? 1 : tokens[cursor].text === "}" ? -1 : 0;
      index = cursor;
    }
  }
  return classes;
}

function kelyraDescendants(classes, parent) {
  const byName = new Map();
  const byModule = new Map();
  for (const item of classes) {
    const list = byName.get(item.name) || [];
    list.push(item);
    byName.set(item.name, list);
    byModule.set(`${item.module}.${item.name}`, item);
  }
  const children = new Map(classes.map((item) => [item, []]));
  for (const child of classes) for (const base of child.bases) {
    let target;
    if (base.includes(".")) target = byModule.get(base);
    else {
      target = byModule.get(`${child.module}.${base}`);
      if (!target) {
        const imports = child.imports
          .map(({ name }) => byModule.get(`${name}.${base}`)).filter(Boolean);
        if (imports.length === 1) target = imports[0];
      }
      if (!target && byName.get(base)?.length === 1) target = byName.get(base)[0];
    }
    if (target && target !== child) children.get(target).push(child);
  }
  const found = [];
  const visited = new Set([parent]);
  const pending = [...(children.get(parent) || [])];
  while (pending.length) {
    const item = pending.shift();
    if (visited.has(item)) continue;
    visited.add(item);
    found.push(item);
    pending.push(...children.get(item));
  }
  return found.sort((a, b) => `${a.module}.${a.name}`.localeCompare(`${b.module}.${b.name}`));
}

async function workspaceInheritanceClasses(document) {
  const sources = new Map();
  try {
    for (const uri of await vscode.workspace.findFiles("**/*.kly", "**/{node_modules,build,.git}/**")) {
      const key = uri.toString();
      sources.set(key, { uri, text: Buffer.from(await vscode.workspace.fs.readFile(uri)).toString("utf8") });
    }
  } catch { /* A file may disappear while the workspace is being scanned. */ }
  for (const open of vscode.workspace.textDocuments) {
    if (open.languageId === "kelyra")
      sources.set(open.uri.toString(), { uri: open.uri, text: open.getText() });
  }
  sources.set(document.uri.toString(), { uri: document.uri, text: document.getText() });
  return [...sources.values()].flatMap(({ uri, text }) => parseKelyraInheritance(text, uri));
}

async function provideKelyraInheritanceLenses(document) {
  const classes = await workspaceInheritanceClasses(document);
  return classes.filter((item) => item.uri.toString() === document.uri.toString())
    .flatMap((item) => {
      const descendants = kelyraDescendants(classes, item);
      if (!descendants.length) return [];
      const position = new vscode.Position(item.position.line, item.position.character);
      return [new vscode.CodeLens(new vscode.Range(position, position), {
        title: `$(type-hierarchy) ${descendants.length} inheriting ${descendants.length === 1 ? "class" : "classes"}`,
        command: "kelyra.showInheritors",
        arguments: [item.uri, item.position],
      })];
    });
}

async function showKelyraInheritors(uri, position) {
  const document = await vscode.workspace.openTextDocument(uri);
  const classes = await workspaceInheritanceClasses(document);
  const parent = classes.find((item) => item.uri.toString() === uri.toString() &&
    item.position.line === position.line && item.position.character === position.character);
  if (!parent) return;
  const descendants = kelyraDescendants(classes, parent);
  const selected = await vscode.window.showQuickPick(descendants.map((item) => ({
    label: item.name,
    description: item.module || vscode.workspace.asRelativePath(item.uri),
    detail: vscode.workspace.asRelativePath(item.uri),
    item,
  })), { placeHolder: `Classes inheriting ${parent.name}` });
  if (!selected) return;
  const target = await vscode.workspace.openTextDocument(selected.item.uri);
  const at = selected.item.position;
  await vscode.window.showTextDocument(target, {
    selection: new vscode.Range(at.line, at.character, at.line, at.character + selected.item.name.length),
  });
}

function targetFromTriple(triple = "") {
  const lower = triple.toLowerCase();
  return {
    os: lower.includes("windows") || lower.includes("win32") ? "windows" :
      lower.includes("linux") ? "linux" : lower.includes("darwin") ? "macos" :
        os.platform() === "win32" ? "windows" : os.platform() === "darwin" ? "macos" : "linux",
    arch: lower.startsWith("aarch64") || lower.startsWith("arm64") ? "aarch64" :
      lower.startsWith("x86_64") || lower.startsWith("amd64") ? "x86_64" :
        os.arch() === "arm64" ? "aarch64" : os.arch() === "x64" ? "x86_64" : os.arch(),
  };
}

async function targetForDocument(document) {
  const file = document.uri?.fsPath;
  if (!file) return targetFromTriple();
  const folder = vscode.workspace.getWorkspaceFolder?.(document.uri)?.uri.fsPath;
  for (let directory = path.dirname(file); ; directory = path.dirname(directory)) {
    try {
      const manifest = await fs.readFile(path.join(directory, "kelp.toml"), "utf8");
      const build = manifest.split(/^\s*\[(?!build\])[^\]]+\]\s*$/m)
        .find((section) => /^\s*\[build\]/m.test(section)) || "";
      const triple = build.match(/^\s*target\s*=\s*"([^"]+)"/m)?.[1];
      if (triple) return targetFromTriple(triple);
    } catch { /* Keep looking for an enclosing Kelp project. */ }
    if (directory === folder || path.dirname(directory) === directory) break;
  }
  return targetFromTriple();
}

async function refreshCfgDecorations(editor) {
  if (!editor || editor.document.languageId !== "kelyra") return;
  const document = editor.document;
  if (enumMemberDecoration) {
    if (!kelyraIndexPromise) kelyraIndexPromise = loadKelyraIndex();
    const cached = await kelyraIndexPromise;
    if (editor.document !== document) return;
    const current = parseKelyraModule(document.getText());
    const knownEnums = new Map(current.enums);
    for (const imported of current.imports)
      for (const [name, members] of cached.modules.get(imported.name)?.enums || [])
        if (!knownEnums.has(name)) knownEnums.set(name, members);
    const members = kelyraEnumMemberRanges(document.getText(), knownEnums).map(([start, end]) =>
      new vscode.Range(document.positionAt(start), document.positionAt(end)));
    editor.setDecorations(enumMemberDecoration, members);
  }
  if (!inactiveCfgDecoration) return;
  const target = await targetForDocument(document);
  if (editor.document !== document) return;
  const { tokens } = kelyraTree.snapshot(document, scanKelyra);
  const ranges = inactiveCfgRanges(tokens, document.getText(), target).map(({ start, end }) =>
    new vscode.Range(document.positionAt(start), document.positionAt(end)));
  editor.setDecorations(inactiveCfgDecoration, ranges);
}

function scheduleCfgRefresh(editor = vscode.window.activeTextEditor) {
  if (cfgRefreshTimer) clearTimeout(cfgRefreshTimer);
  cfgRefreshTimer = setTimeout(() => void refreshCfgDecorations(editor), 100);
}

function invalidateKelyraIndex() {
  ++kelyraIndexGeneration;
  kelyraIndexPromise = undefined;
}

// Reads persisted sources; open documents are merged over this index by the provider.
async function scanWorkspaceIndex() {
  const modules = new Map();
  const extras = [];
  const visitedDependencies = new Set();
  const record = (text) => {
    const parsed = parseKelyraModule(text);
    if (parsed.module) {
      if (!modules.has(parsed.module)) modules.set(parsed.module, parsed);
    } else {
      extras.push(parsed);
    }
  };
  try {
    const files = await vscode.workspace.findFiles(
      "**/*.kly",
      // Dependency sources under .kelp/dependencies are indexed on purpose.
      "**/{node_modules,build,.git}/**",
    );
    for (const file of files)
      record(Buffer.from(await vscode.workspace.fs.readFile(file)).toString("utf8"));
    if (vscode.workspace.workspaceFolders?.length) {
      const manifests = await vscode.workspace.findFiles(
        "**/kelp.toml", "**/{node_modules,build,.git}/**",
      );
      const walkDependency = async (directory) => {
        directory = path.resolve(directory);
        if (visitedDependencies.has(directory)) return;
        visitedDependencies.add(directory);
        let entries;
        try { entries = await fs.readdir(directory, { withFileTypes: true }); }
        catch { return; }
        for (const entry of entries) {
          const file = path.join(directory, entry.name);
          if (entry.isDirectory()) {
            if ([".git", "build", "node_modules", ".kelp"].includes(entry.name)) continue;
            await walkDependency(file);
          } else if (entry.name.endsWith(".kly")) {
            try { record(await fs.readFile(file, "utf8")); } catch { /* Unreadable source. */ }
          } else if (entry.name === "kelp.toml") {
            try {
              const manifest = parseKelpManifest(await fs.readFile(file, "utf8"));
              for (const dependency of manifest.paths)
                await walkDependency(path.resolve(directory, dependency));
            } catch { /* Invalid or unreadable manifest. */ }
          }
        }
      };
      for (const uri of manifests) {
        const file = uri.fsPath;
        if (!file) continue;
        try {
          const manifest = parseKelpManifest(await fs.readFile(file, "utf8"));
          for (const dependency of manifest.paths)
            await walkDependency(path.resolve(path.dirname(file), dependency));
        } catch { /* Invalid or unreadable manifest. */ }
      }
    }
  } catch {
    // A missing workspace or unreadable file only limits cross-module hints.
  }
  return [...modules.values(), ...extras];
}

async function loadKelyraIndex() {
  const root = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
  const generation = kelyraIndexGeneration;
  const cached = await readIndex(root);
  const refresh = async () => {
    const parsed = await scanWorkspaceIndex();
    if (generation !== kelyraIndexGeneration) return;
    const index = buildKelyraIndex(parsed);
    kelyraIndexPromise = Promise.resolve(index);
    inlayHintsChanged?.fire();
    scheduleCfgRefresh();
    try { await writeIndex(root, parsed); } catch { /* Cache is optional. */ }
    return index;
  };
  if (cached) {
    if (!kelyraIndexRefresh) {
      kelyraIndexRefresh = refresh().catch(() => {}).finally(() => {
        kelyraIndexRefresh = undefined;
      });
    }
    return buildKelyraIndex(cached);
  }
  // On a first project open, local hints work immediately. The workspace scan
  // populates imported signatures and the persistent cache in the background.
  if (root) {
    if (!kelyraIndexRefresh) {
      kelyraIndexRefresh = refresh().catch(() => {}).finally(() => {
        kelyraIndexRefresh = undefined;
      });
    }
    return buildKelyraIndex([]);
  }
  return buildKelyraIndex(await scanWorkspaceIndex());
}

async function provideKelyraInlayHints(document, range) {
  const mode = vscode.workspace
    .getConfiguration("kelyra")
    .get("inlayHints.parameterNames", "literals");
  if (mode === "off") return [];
  if (!kelyraIndexPromise) kelyraIndexPromise = loadKelyraIndex();
  const cached = await kelyraIndexPromise;
  const open = vscode.workspace.textDocuments
    .filter(({ languageId }) => languageId === "kelyra")
    .map((item) => parseKelyraModule(item.getText()));
  const openNames = new Set(open.map(({ module }) => module).filter(Boolean));
  const index = buildKelyraIndex([...open,
    ...[...cached.modules.values()].filter(({ module }) => !openNames.has(module))]);
  const text = document.getText();
  const { tokens, positionAt } = kelyraTree.snapshot(document, scanKelyra);
  const hints = [];
  for (const hint of kelyraParameterHints(text, index, mode, tokens)) {
    const position = positionAt(hint.offset);
    if (
      position.line < range.start.line ||
      position.line > range.end.line ||
      (position.line === range.start.line && position.character < range.start.character) ||
      (position.line === range.end.line && position.character > range.end.character)
    )
      continue;
    const item = new vscode.InlayHint(
      position,
      `${hint.name}:`,
      vscode.InlayHintKind.Parameter,
    );
    item.paddingRight = true;
    hints.push(item);
  }
  return hints;
}

const kelpFields = {
  project: [
    ["name", '"${1:app}"', "Project name."],
    ["version", '"${1:0.1.0}"', "Project version."],
    ["entry", '"${1:src/main.kly}"', "Kelyra entry source."],
  ],
  build: [
    ["compiler", '"${1:kelyra}"', "Kelyra compiler executable."],
    ["kind", '"${1|executable,library|}"', "Build target: `executable` links a program, `library` compiles a linked object."],
    ["output", '"${1:app}"', "Artifact name inside the project's build directory, `.kelp/build/<project path>`."],
    ["optimization", "${1:0}", "Optimization level from 0 to 3."],
    ["safe-level", "${1:0}", "Kelyra runtime safety level."],
    ["c-sources", "[${1}]", "C source files compiled with the project."],
    ["c-args", "[${1}]", "Arguments forwarded to Clang."],
  ],
  workspace: [
    ["members", "[${1}]", "Subproject directories, each with its own `kelp.toml`. Commands accept a member name or relative path."],
  ],
  package: [["output", '"${1:app-0.1.0.tar.gz}"', "Archive name inside the project's build directory."]],
  test: [["sources", "[${1}]", "Additional Kelyra test sources."]],
  dependencies: [
    ["repository", '"${1:git@github.com:owner/repo.git}"', "Dependency Git repository."],
    ["revision", '"${1:main}"', "Optional Git revision."],
    ["path", '"${1:../libs/math}"', "Local path dependency used in place; give either this or repository/revision."],
  ],
};

const kelpActions = [
  ["Compile", "kelp.build", "tools"],
  ["Debug", "kelp.debug", "debug-alt"],
  ["Run", "kelp.run", "play"],
  ["Test", "kelp.test", "beaker"],
  ["Package", "kelp.package", "package"],
  ["Members", "kelp.members", "list-unordered"],
  ["Variables", "workbench.debug.action.focusVariablesView", "symbol-variable"],
  ["Functions / Call Stack", "workbench.debug.action.focusCallStackView", "callstack"],
  ["Watch Expressions", "workbench.debug.action.focusWatchView", "eye"],
  ["Evaluate / Debug Console", "workbench.debug.action.focusRepl", "debug-console"],
];

function kelpSectionAt(text, line) {
  let section = "";
  const lines = text.split(/\r?\n/);
  for (let index = 0; index <= line && index < lines.length; ++index) {
    const match = lines[index].match(/^\s*\[([^\]]+)\]/);
    if (match) section = match[1];
  }
  return section.startsWith("dependencies.") ? "dependencies" : section;
}

function kelpFieldAt(text, line, character) {
  const sourceLine = text.split(/\r?\n/)[line] || "";
  const match = sourceLine.match(/^\s*([A-Za-z][A-Za-z0-9-]*)\s*=/);
  if (!match) return undefined;
  const start = sourceLine.indexOf(match[1]);
  if (character < start || character > start + match[1].length) return undefined;
  return (kelpFields[kelpSectionAt(text, line)] || []).find(
    ([name]) => name === match[1],
  );
}

const kelpSections = [
  ["project", "Project name, version, and entry source."],
  ["build", "Compiler, target kind, output, optimization, and C options."],
  ["workspace", "Subproject member directories."],
  ["package", "Source archive output."],
  ["test", "Additional test sources."],
  ["dependencies.<name>", "Git or local path dependency."],
];

function provideKelpHover(document, position) {
  const field = kelpFieldAt(document.getText(), position.line, position.character);
  return field ? new vscode.Hover(`**${field[0]}**\n\n${field[2]}`) : undefined;
}

function provideKelpCompletions(document, position) {
  const prefix = document.lineAt(position.line).text.slice(0, position.character);
  const header = prefix.match(/^(\s*)\[([A-Za-z0-9_.]*)$/);
  if (header) {
    const range = new vscode.Range(
      position.line,
      header[1].length + 1,
      position.line,
      position.character,
    );
    return kelpSections.map(([name, documentation]) => {
      const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Module);
      item.insertText = new vscode.SnippetString(
        name === "dependencies.<name>" ? "dependencies.${1:name}]" : `${name}]`,
      );
      item.range = range;
      item.documentation = documentation;
      return item;
    });
  }
  return (kelpFields[kelpSectionAt(document.getText(), position.line)] || []).map(
    ([name, value, documentation]) => {
      const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Property);
      item.insertText = new vscode.SnippetString(`${name} = ${value}`);
      item.documentation = documentation;
      return item;
    },
  );
}

// `kelp members` prints one line per project: "<dir> <name> <kind> <output>".
// A workspace root that is not itself a project prints "-" for name and output.
function parseKelpMembers(text) {
  const members = [];
  for (const line of text.split(/\r?\n/)) {
    const columns = line.trim().split(/\s+/);
    if (columns.length < 3) continue;
    const [dir, name, buildKind, output] = columns;
    if (name === "-") continue;
    members.push({
      dir,
      name,
      buildKind: buildKind === "library" ? "library" : "executable",
      output: output && output !== "-" ? output : "",
    });
  }
  return members;
}

// Reads the handful of manifest fields the project view needs without pulling
// in a TOML parser. Sections, string values, and string arrays are enough.
function parseKelpManifest(text) {
  const manifest = {
    hasProject: false,
    name: "",
    buildKind: "executable",
    entry: "",
    output: "",
    members: [],
    paths: [],
  };
  let section = "";
  for (const line of text.split(/\r?\n/)) {
    const header = line.match(/^\s*\[([^\]]+)\]/);
    if (header) {
      section = header[1].trim();
      if (section === "project") manifest.hasProject = true;
      continue;
    }
    const assignment = line.match(/^\s*([A-Za-z][A-Za-z0-9-]*)\s*=/);
    if (!assignment) continue;
    const key = assignment[1];
    const strings = [...line.matchAll(/"([^"]*)"/g)].map((match) => match[1]);
    if (section === "project") {
      if (key === "name" && strings.length) manifest.name = strings[0];
      else if (key === "entry" && strings.length) manifest.entry = strings[0];
    } else if (section === "build") {
      if (key === "kind" && strings.length)
        manifest.buildKind = strings[0] === "library" ? "library" : "executable";
      else if (key === "output" && strings.length) manifest.output = strings[0];
    } else if (section === "workspace" && key === "members") {
      manifest.members = strings;
    } else if (section.startsWith("dependencies.") && key === "path" && strings.length) {
      manifest.paths.push(strings[0]);
    }
  }
  if (!manifest.output)
    manifest.output =
      manifest.buildKind === "library" ? `${manifest.name}.o` : manifest.name;
  return manifest;
}

// Kelp builds every project into the workspace cache, mirroring each project's
// path: `libs/math` produces `.kelp/build/libs/math/math.o`. A declared output
// is relative to that directory, and a leading `build/` still means it.
function kelpArtifactPath(output, relativeDir) {
  const base =
    relativeDir && relativeDir !== "." ? `.kelp/build/${relativeDir}` : ".kelp/build";
  const declared = output.startsWith("build/") ? output.slice("build/".length) : output;
  return `${base}/${declared}`;
}

// Discovers projects by walking `kelp.toml` files. Declared workspace members
// win over directory scans so the tree matches what Kelp actually builds, and
// generated directories are never descended into.
async function scanKelpProjects(root) {
  const projects = [];
  const skipped = new Set(["build", "node_modules", "target"]);
  async function walk(directory, relative) {
    let manifest;
    try {
      manifest = parseKelpManifest(await fs.readFile(path.join(directory, "kelp.toml"), "utf8"));
      if (manifest.hasProject)
        projects.push({
          dir: relative || ".",
          name: manifest.name || path.basename(directory),
          buildKind: manifest.buildKind,
          output: kelpArtifactPath(manifest.output, relative),
          entry: manifest.entry,
        });
    } catch (error) {
      if (error.code !== "ENOENT") throw error;
    }
    let entries;
    try {
      entries = await fs.readdir(directory, { withFileTypes: true });
    } catch (error) {
      return;
    }
    const childRelative = (name) => (relative ? `${relative}/${name}` : name);
    const directories = entries.filter(
      (entry) => entry.isDirectory() && !entry.name.startsWith(".") && !skipped.has(entry.name),
    );
    const children = manifest && manifest.members.length
      ? directories.filter((entry) =>
          manifest.members.some((member) => {
            const normalized = member.replace(/^\.\//, "").replace(/\/+$/, "");
            return (
              normalized === childRelative(entry.name) ||
              normalized.startsWith(`${childRelative(entry.name)}/`)
            );
          }),
        )
      : directories;
    for (const entry of children)
      await walk(path.join(directory, entry.name), childRelative(entry.name));
  }
  await walk(root, "");
  return projects;
}

// Nests projects by their relative directory so `libs/math` becomes a folder
// node that carries the project. `dir` of "." is the workspace root itself.
function kelpProjectTree(projects) {
  const roots = [];
  const nodes = new Map();
  const ensure = (relDir) => {
    const existing = nodes.get(relDir);
    if (existing) return existing;
    const segments = relDir.split("/");
    const node = {
      type: "directory",
      relDir,
      label: segments[segments.length - 1],
      children: [],
    };
    nodes.set(relDir, node);
    const parentRel = segments.slice(0, -1).join("/");
    (parentRel ? ensure(parentRel).children : roots).push(node);
    return node;
  };
  for (const project of projects) {
    if (!project.dir || project.dir === ".") {
      roots.push({ ...project, type: "project", label: project.name, children: [] });
      continue;
    }
    Object.assign(ensure(project.dir), { type: "project", label: project.name, ...project });
  }
  const order = (left, right) =>
    left.type === right.type
      ? String(left.label).localeCompare(String(right.label))
      : left.type === "directory"
        ? -1
        : 1;
  const sort = (node) => {
    node.children.sort(order);
    node.children.forEach(sort);
  };
  roots.sort(order);
  roots.forEach(sort);
  return roots;
}

class KelpProjectTreeProvider {
  constructor() {
    this.emitter = new vscode.EventEmitter();
    this.onDidChangeTreeData = this.emitter.event;
  }

  refresh() {
    this.emitter.fire();
  }

  async projects(folder) {
    let nodes;
    if (vscode.workspace.isTrusted) {
      try {
        const executable = vscode.workspace
          .getConfiguration("kelp", folder.uri)
          .get("path", "kelp");
        const { stdout } = await execFile(executable, ["members"], {
          cwd: folder.uri.fsPath,
          encoding: "utf8",
          timeout: 10000,
        });
        const members = parseKelpMembers(stdout);
        if (members.length) nodes = kelpProjectTree(members);
      } catch (error) {
        // The executable may be missing; the manifests still describe the tree.
      }
    }
    if (!nodes) nodes = kelpProjectTree(await scanKelpProjects(folder.uri.fsPath));
    const stamp = (node) => {
      node.folder = folder;
      node.children.forEach(stamp);
    };
    nodes.forEach(stamp);
    return nodes;
  }

  async getChildren(element) {
    if (!element) {
      const folders = vscode.workspace.workspaceFolders || [];
      if (folders.length <= 1) return folders.length ? this.projects(folders[0]) : [];
      return folders.map((folder) => ({
        type: "folder",
        label: folder.name,
        folder,
        children: [],
      }));
    }
    if (element.type === "folder") return this.projects(element.folder);
    return element.children;
  }

  getTreeItem(node) {
    if (node.type === "directory") {
      const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.Expanded);
      item.iconPath = new vscode.ThemeIcon("folder");
      item.contextValue = "kelp.directory";
      return item;
    }
    const item = new vscode.TreeItem(
      node.label,
      node.children.length
        ? vscode.TreeItemCollapsibleState.Expanded
        : vscode.TreeItemCollapsibleState.None,
    );
    const directory = path.join(node.folder.uri.fsPath, node.dir === "." ? "" : node.dir);
    item.description = `${node.buildKind} · ${node.output}`;
    item.tooltip = new vscode.MarkdownString(
      `**${node.label}** — ${node.buildKind}\n\nOutput: \`${node.output}\``,
    );
    item.iconPath = new vscode.ThemeIcon(node.buildKind === "library" ? "library" : "run");
    item.contextValue = "kelp.project";
    item.resourceUri = vscode.Uri.file(path.join(directory, "kelp.toml"));
    item.project = { folder: node.folder, cwd: directory };
    item.command = { command: "kelp.openManifest", title: "Open Manifest", arguments: [item] };
    return item;
  }
}

// A tree item, a bare { folder, cwd } pair, or nothing (use the active editor).
function projectFrom(argument) {
  if (argument && argument.project) return argument.project;
  if (argument && argument.folder && argument.cwd) return argument;
  return undefined;
}

async function guard(action) {
  try {
    return await action();
  } catch (error) {
    await vscode.window.showErrorMessage(error.message);
  }
}

async function kelpManifestPath(argument) {
  const project = projectFrom(argument) || (await projectContext());
  return path.join(project.cwd, "kelp.toml");
}

async function openKelpManifest(argument) {
  const document = await vscode.workspace.openTextDocument(
    vscode.Uri.file(await kelpManifestPath(argument)),
  );
  return vscode.window.showTextDocument(document);
}

async function revealKelpProject(argument) {
  return vscode.commands.executeCommand(
    "revealFileInOS",
    vscode.Uri.file(await kelpManifestPath(argument)),
  );
}

// Resolves the configured artifact path for the project, which the status bar
// and the `kelp.output` command both use. The result is deterministic for a
// manifest, so it is cached per project directory.
const kelpOutputPaths = new Map();
async function kelpOutputPath(argument) {
  const project = projectFrom(argument) || (await projectContext());
  const cached = kelpOutputPaths.get(project.cwd);
  if (cached) return cached;
  const executable = vscode.workspace
    .getConfiguration("kelp", project.folder.uri)
    .get("path", "kelp");
  const { stdout } = await execFile(executable, ["output"], {
    cwd: project.cwd,
    encoding: "utf8",
    timeout: 10000,
  });
  const artifact = stdout.replace(/\r?\n$/, "");
  if (!path.isAbsolute(artifact))
    throw new Error("kelp output did not return an absolute path. Update Kelp.");
  kelpOutputPaths.set(project.cwd, artifact);
  return artifact;
}

let kelpChannel;
function kelpOutputChannel() {
  if (!kelpChannel) kelpChannel = vscode.window.createOutputChannel("Kelp");
  return kelpChannel;
}

async function showKelpOutput(argument) {
  const artifact = await kelpOutputPath(argument);
  kelpOutputChannel().appendLine(artifact);
  await vscode.env?.clipboard?.writeText?.(artifact);
  await vscode.window.showInformationMessage(`Kelp output: ${artifact}`);
  return artifact;
}

// Resolves a `path` dependency or a `members` entry in kelp.toml to the target
// project's manifest so navigation follows the local subproject graph.
async function provideKelpDefinition(document, position) {
  const line = document.lineAt(position.line).text;
  const assignment = line.match(/^\s*([A-Za-z][A-Za-z0-9-]*)\s*=/);
  if (!assignment) return undefined;
  const section = kelpSectionAt(document.getText(), position.line);
  const isMember = section === "workspace" && assignment[1] === "members";
  const isDependencyPath = section === "dependencies" && assignment[1] === "path";
  if (!isMember && !isDependencyPath) return undefined;
  for (const match of line.matchAll(/"([^"]*)"/g)) {
    const start = match.index + 1;
    if (position.character < start || position.character > start + match[1].length) continue;
    const manifest = path.resolve(path.dirname(document.uri.fsPath), match[1], "kelp.toml");
    try {
      await fs.access(manifest);
    } catch (error) {
      return undefined;
    }
    return new vscode.Location(vscode.Uri.file(manifest), new vscode.Position(0, 0));
  }
  return undefined;
}

const kelpTaskDefinitions = [
  ["check", "Kelp: Check", "Build"],
  ["build", "Kelp: Compile", "Build"],
  ["run", "Kelp: Run", undefined],
  ["test", "Kelp: Test", "Test"],
  ["package", "Kelp: Package", "Build"],
];

const kelpTaskProvider = {
  provideTasks() {
    const groups = vscode.TaskGroup || {};
    return (vscode.workspace.workspaceFolders || []).flatMap((folder) =>
      kelpTaskDefinitions.map(([command, label, group]) => {
        const executable = vscode.workspace
          .getConfiguration("kelp", folder.uri)
          .get("path", "kelp");
        const execution = new vscode.ProcessExecution(executable, [command], {
          cwd: folder.uri.fsPath,
        });
        const task = new vscode.Task(
          { type: "kelp", command },
          folder,
          label,
          "kelp",
          execution,
          "$kelyra",
        );
        if (group && groups[group]) task.group = groups[group];
        return task;
      }),
    );
  },
  resolveTask() {
    return undefined;
  },
};

let kelpStatus;
async function refreshKelpStatus() {
  if (!kelpStatus) return;
  try {
    const artifact = await kelpOutputPath();
    kelpStatus.text = `$(file-binary) ${path.basename(artifact)}`;
    kelpStatus.tooltip = `${artifact}\nClick to show and copy the path.`;
    kelpStatus.command = "kelp.output";
  } catch (error) {
    kelpStatus.text = "$(tools) Kelp";
    kelpStatus.tooltip = "Kelp project commands";
    kelpStatus.command = "kelp.build";
  }
  kelpStatus.show();
}

function kelyraTokenRules(colors) {
  return [
    {
      scope: ["source.kelyra comment", "source.kelyra punctuation.definition.comment"],
      settings: { foreground: colors.comment, fontStyle: "italic" },
    },
    {
      scope: ["source.kelyra keyword", "source.kelyra storage.modifier"],
      settings: { foreground: colors.keyword },
    },
    {
      scope: ["source.kelyra storage.type", "source.kelyra support.type"],
      settings: { foreground: colors.type },
    },
    {
      scope: ["source.kelyra entity.name.function", "source.kelyra support.function"],
      settings: { foreground: colors.function },
    },
    {
      scope: [
        "source.kelyra entity.name.type",
        "source.kelyra entity.name.tag",
        "source.kelyra entity.name.section",
      ],
      settings: { foreground: colors.name },
    },
    {
      scope: ["source.kelyra string", "source.kelyra constant.character.escape"],
      settings: { foreground: colors.string },
    },
    {
      scope: ["source.kelyra constant.numeric", "source.kelyra constant.language"],
      settings: { foreground: colors.constant },
    },
    {
      scope: ["source.kelyra constant.other.enum"],
      settings: { foreground: colors.constant, fontStyle: "italic" },
    },
    {
      scope: ["source.kelyra entity.name.namespace", "source.kelyra variable.other.key"],
      settings: { foreground: colors.namespace },
    },
    {
      scope: [
        "source.kelyra variable.other.readwrite",
        "source.kelyra variable.other.member",
        "source.kelyra variable.parameter",
        "source.kelyra variable.other",
      ],
      settings: { foreground: colors.variable },
    },
    {
      scope: ["source.kelyra variable.language"],
      settings: { foreground: colors.namespace, fontStyle: "italic" },
    },
  ];
}

// Character palettes still follow the active theme's contrast, but every
// selector is rooted at source.kelyra so no other language is recolored.
const kelyraTokenColors = {
  laevatain: {
    dark: kelyraTokenRules({
      comment: "#9A8490", keyword: "#FF6670", type: "#FF9D66", function: "#FFD166",
      name: "#C792EA", string: "#F29AB2", constant: "#FF7A45", namespace: "#E879A6",
      variable: "#F1D8DC",
    }),
    light: kelyraTokenRules({
      comment: "#76616B", keyword: "#B42332", type: "#B64A1E", function: "#8A5A00",
      name: "#7040A0", string: "#A52F5A", constant: "#B63D12", namespace: "#9A3567",
      variable: "#702A36",
    }),
  },
  jue: {
    dark: kelyraTokenRules({
      comment: "#7F9299", keyword: "#55D6BE", type: "#65B8E8", function: "#8BE0F2",
      name: "#C8D6E5", string: "#9BD08F", constant: "#E6C875", namespace: "#5CC8C2",
      variable: "#D7E7EA",
    }),
    light: kelyraTokenRules({
      comment: "#607178", keyword: "#087F70", type: "#176F9E", function: "#006F85",
      name: "#455A70", string: "#3D7A35", constant: "#8A6500", namespace: "#167D78",
      variable: "#294F58",
    }),
  },
  perlica: {
    dark: kelyraTokenRules({
      comment: "#8B929B", keyword: "#F4C542", type: "#74B9FF", function: "#FFD866",
      name: "#E6EDF3", string: "#8BD5CA", constant: "#FFB454", namespace: "#4FC3F7",
      variable: "#D9E2EC",
    }),
    light: kelyraTokenRules({
      comment: "#687078", keyword: "#9A6A00", type: "#1769AA", function: "#8A5E00",
      name: "#465565", string: "#28786F", constant: "#A54F00", namespace: "#007A9E",
      variable: "#374957",
    }),
  },
};

const enumMemberColors = {
  laevatain: { dark: "#FFB86C", light: "#9A4D12" },
  jue: { dark: "#6FD4C2", light: "#087F70" },
  perlica: { dark: "#FFD166", light: "#8A5E00" },
};

function refreshEnumMemberDecoration(scheme, light) {
  if (!enumMemberDecorationContext || !vscode.window.createTextEditorDecorationType)
    return;
  enumMemberDecoration?.dispose();
  const color = (enumMemberColors[scheme] || enumMemberColors.laevatain)[
    light ? "light" : "dark"
  ];
  enumMemberDecoration = vscode.window.createTextEditorDecorationType({
    fontStyle: "italic", color,
  });
  enumMemberDecorationContext.subscriptions.push(enumMemberDecoration);
  scheduleCfgRefresh();
}

function isKelyraTokenRule(rule) {
  return [].concat(rule?.scope ?? []).some(
    (scope) => typeof scope === "string" && scope.startsWith("source.kelyra "),
  );
}

// Keeps the user's own customizations and replaces only previously applied
// Kelyra rules, so the command is idempotent and safe to run again.
function mergeKelyraTokenColors(existing, palette) {
  const base = existing && typeof existing === "object" ? existing : {};
  const rules = [].concat(base.textMateRules ?? []).filter((rule) => !isKelyraTokenRule(rule));
  return { ...base, textMateRules: [...rules, ...(palette ?? [])] };
}

async function applyKelyraTokenColors(showMessage = true) {
  const scheme = vscode.workspace.getConfiguration("kelyra").get("colorScheme", "laevatain");
  const kind = vscode.window.activeColorTheme?.kind;
  const light =
    kind === vscode.ColorThemeKind?.Light || kind === vscode.ColorThemeKind?.HighContrastLight;
  refreshEnumMemberDecoration(scheme, light);
  const palette = kelyraTokenColors[scheme]?.[light ? "light" : "dark"];
  const configuration = vscode.workspace.getConfiguration("editor");
  const current = configuration.get("tokenColorCustomizations");
  const updated = mergeKelyraTokenColors(current, palette);
  if (JSON.stringify(current) === JSON.stringify(updated)) return updated;
  await configuration.update(
    "tokenColorCustomizations",
    updated,
    vscode.ConfigurationTarget.Global,
  );
  if (!showMessage) return updated;
  const choice = await vscode.window.showInformationMessage(
    scheme === "off" ? "Kelyra color scheme disabled." : `Kelyra ${scheme} colors applied.`,
    "Open Settings",
  );
  if (choice === "Open Settings")
    await vscode.commands.executeCommand(
      "workbench.action.openSettings",
      "editor.tokenColorCustomizations",
    );
  return updated;
}

async function selectKelyraColorScheme() {
  const selected = await vscode.window.showQuickPick(
    [
      { label: "Laevatain / 莱万汀", value: "laevatain" },
      { label: "Jue / 诀", value: "jue" },
      { label: "Perlica / 佩丽卡", value: "perlica" },
      { label: "Off", value: "off" },
    ],
    { placeHolder: "Select a Kelyra-only color scheme" },
  );
  if (!selected) return;
  await vscode.workspace
    .getConfiguration("kelyra")
    .update("colorScheme", selected.value, vscode.ConfigurationTarget.Global);
  return applyKelyraTokenColors();
}

async function projectContext() {
  if (!vscode.workspace.isTrusted) throw new Error("Trust this workspace before running Kelp.");
  const document = vscode.window.activeTextEditor?.document;
  let folder = document && vscode.workspace.getWorkspaceFolder(document.uri);
  if (!folder) {
    const folders = vscode.workspace.workspaceFolders || [];
    folder = folders.length === 1 ? folders[0] : await vscode.window.showWorkspaceFolderPick();
  }
  if (!folder) throw new Error("Open or select a Kelp workspace folder first.");
  let cwd = document?.uri.scheme === "file" && vscode.workspace.getWorkspaceFolder(document.uri) === folder
    ? path.dirname(document.uri.fsPath) : folder.uri.fsPath;
  while (true) {
    try {
      await fs.access(path.join(cwd, "kelp.toml"));
      return { folder, cwd };
    } catch (error) {
      if (error.code !== "ENOENT") throw error;
    }
    const parent = path.dirname(cwd);
    if (parent === cwd) throw new Error("Cannot find kelp.toml in this project or its parent directories.");
    cwd = parent;
  }
}

async function runKelp(command, { wait = false, project, args = [] } = {}) {
  if (!vscode.workspace.isTrusted) throw new Error("Trust this workspace before running Kelp.");
  const { folder, cwd } = project || await projectContext();
  const executable = vscode.workspace.getConfiguration("kelp", folder.uri).get("path", "kelp");
  const executionOptions = new vscode.ProcessExecution(executable, [command, ...args], {
    cwd,
  });
  const task = new vscode.Task(
    { type: "kelp", command },
    folder || vscode.TaskScope.Workspace,
    command,
    "kelp",
    executionOptions,
    "$kelyra",
  );
  if (!wait) return vscode.tasks.executeTask(task);
  let execution;
  let subscription;
  const completed = new Promise((resolve, reject) => {
    subscription = vscode.tasks.onDidEndTaskProcess((event) => {
      if (event.execution !== execution) return;
      if (event.exitCode === 0) resolve();
      else reject(new Error(`kelp ${command} exited with code ${event.exitCode}`));
    });
  });
  try {
    execution = await vscode.tasks.executeTask(task);
    return await completed;
  } finally {
    subscription.dispose();
  }
}

async function debugKelp(argument) {
  const project = projectFrom(argument) || await projectContext();
  if (!vscode.extensions.getExtension("ms-vscode.cpptools"))
    throw new Error("Install Microsoft's C/C++ extension (ms-vscode.cpptools) to connect VS Code to local GDB.");
  const settings = vscode.workspace.getConfiguration("kelp", project.folder.uri);
  const gdb = settings.get("debug.gdbPath", "gdb");
  try {
    await execFile(gdb, ["--version"], { timeout: 10000 });
  } catch (error) {
    throw new Error(`Cannot run local GDB (${gdb}). Install GDB or set kelp.debug.gdbPath. ${error.message}`);
  }
  if (!await vscode.workspace.saveAll(false)) throw new Error("Save project files before debugging.");
  const { stdout } = await execFile(settings.get("path", "kelp"), ["output"], {
    cwd: project.cwd, encoding: "utf8", timeout: 10000,
  });
  const program = stdout.replace(/\r?\n$/, "");
  if (!path.isAbsolute(program)) throw new Error("kelp output did not return an absolute executable path. Update Kelp.");
  await runKelp("build", { wait: true, project, args: ["--debug"] });
  const started = await vscode.debug.startDebugging(project.folder, {
    name: "Kelp: Local GDB", type: "cppdbg", request: "launch",
    program, cwd: project.cwd, args: settings.get("debug.args", []),
    MIMode: "gdb",
    ...(gdb === "gdb" ? {} : { miDebuggerPath: gdb }),
    stopAtEntry: true, externalConsole: false,
    internalConsoleOptions: "openOnSessionStart",
    setupCommands: [{ text: "-enable-pretty-printing", ignoreFailures: true }],
  });
  if (!started) throw new Error("GDB debugging did not start. See the Debug Console for details.");
  await vscode.commands.executeCommand("workbench.view.debug");
  await vscode.commands.executeCommand("workbench.debug.action.focusRepl");
  return started;
}

function formatterCandidates(document, configured) {
  if (configured !== "kelyra-format") return [configured];
  const executable = process.platform === "win32" ? "kelyra-format.exe" : "kelyra-format";
  const candidates = [configured];
  let directory = vscode.workspace.getWorkspaceFolder?.(document.uri)?.uri.fsPath;
  if (!directory && document.uri?.scheme === "file") directory = path.dirname(document.uri.fsPath);
  while (directory) {
    candidates.push(
      path.join(directory, "build", "bin", executable),
      path.join(directory, "kelyra", "build", "bin", executable),
    );
    const parent = path.dirname(directory);
    if (parent === directory) break;
    directory = parent;
  }
  return candidates;
}

function runFormatter(executable, source, token) {
  return new Promise((resolve, reject) => {
    const process = childProcess.execFile(
      executable,
      ["-i", source],
      { encoding: "utf8", maxBuffer: 16 * 1024 * 1024 },
      (error, _stdout, stderr) => {
        if (error) {
          if (stderr.trim()) error.message = stderr.trim();
          reject(error);
        } else {
          resolve();
        }
      },
    );
    token.onCancellationRequested(() => process.kill());
  });
}

async function format(document, _options, token) {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), "kelyra-format-"));
  const source = path.join(directory, "source.kly");
  try {
    await fs.writeFile(source, document.getText());
    const configured = vscode.workspace
      .getConfiguration("kelyra")
      .get("formatter.path", "kelyra-format");
    let ran = false;
    for (const executable of formatterCandidates(document, configured)) {
      try {
        await runFormatter(executable, source, token);
        ran = true;
        break;
      } catch (error) {
        if (error.code !== "ENOENT") throw error;
      }
    }
    if (!ran)
      throw new Error(`Cannot run ${configured}. Install kelyra-format or set kelyra.formatter.path.`);
    const formatted = await fs.readFile(source, "utf8");
    if (formatted === document.getText()) return [];
    const end = document.positionAt(document.getText().length);
    return [vscode.TextEdit.replace(new vscode.Range(0, 0, end.line, end.character), formatted)];
  } finally {
    await fs.rm(directory, { recursive: true, force: true });
  }
}

async function startLanguageServer() {
  if (client) return;
  const { LanguageClient, TransportKind } = require("vscode-languageclient/node");
  const configured = vscode.workspace
    .getConfiguration("kelyra")
    .get("languageServer.path", "kelyra-ls");
  let command = configured;
  if (configured === "kelyra-ls") {
    const executable = process.platform === "win32" ? "kelyra-ls.exe" : "kelyra-ls";
    for (const folder of vscode.workspace.workspaceFolders || []) {
      for (let directory = folder.uri.fsPath; directory; directory = path.dirname(directory)) {
        for (const candidate of [
          path.join(directory, "build", "bin", executable),
          path.join(directory, "kelyra", "build", "bin", executable),
        ]) {
          try {
            await fs.access(candidate);
            command = candidate;
            break;
          } catch { /* Check the next workspace build. */ }
        }
        if (command !== configured || path.dirname(directory) === directory) break;
      }
      if (command !== configured) break;
    }
  }
  client = new LanguageClient(
    "kelyra",
    "Kelyra Language Server",
    { command, transport: TransportKind.stdio },
    { documentSelector: [{ scheme: "file", language: "kelyra" }] },
  );
  await client.start();
}

async function activate(context) {
  const projects = new KelpProjectTreeProvider();
  void kelyraTree.initialize(path.join(__dirname, "..", "assets", "tree-sitter-kelyra.wasm"))
    .then(() => {
      inlayHintsChanged?.fire();
      scheduleCfgRefresh();
    })
    .catch((error) => console.warn("Kelyra Tree-sitter initialization failed:", error));
  if (vscode.EventEmitter) {
    inlayHintsChanged = new vscode.EventEmitter();
    context.subscriptions.push(inlayHintsChanged);
    inheritanceLensesChanged = new vscode.EventEmitter();
    context.subscriptions.push(inheritanceLensesChanged);
  }
  if (vscode.window.createTextEditorDecorationType) {
    inactiveCfgDecoration = vscode.window.createTextEditorDecorationType({ opacity: "0.45" });
    enumMemberDecorationContext = context;
    context.subscriptions.push(inactiveCfgDecoration);
    const kind = vscode.window.activeColorTheme?.kind;
    const light = kind === vscode.ColorThemeKind?.Light ||
      kind === vscode.ColorThemeKind?.HighContrastLight;
    const scheme = vscode.workspace.getConfiguration("kelyra").get("colorScheme", "laevatain");
    refreshEnumMemberDecoration(scheme, light);
  }
  kelpStatus = vscode.window.createStatusBarItem(
    (vscode.StatusBarAlignment && vscode.StatusBarAlignment.Left) || 1,
    100,
  );
  context.subscriptions.push(
    vscode.languages.registerDocumentFormattingEditProvider("kelyra", {
      provideDocumentFormattingEdits: format,
    }),
    vscode.languages.registerHoverProvider("kelyra", {
      provideHover: provideKelyraHover,
    }),
    vscode.languages.registerCompletionItemProvider(
      "kelyra",
      { provideCompletionItems: provideKelyraCompletions },
      "@",
    ),
    vscode.languages.registerFoldingRangeProvider("kelyra", {
      provideFoldingRanges: provideKelyraFoldingRanges,
    }),
    vscode.languages.registerInlayHintsProvider("kelyra", {
      provideInlayHints: provideKelyraInlayHints,
      onDidChangeInlayHints: inlayHintsChanged?.event,
    }),
    vscode.languages.registerCodeLensProvider("kelyra", {
      provideCodeLenses: provideKelyraInheritanceLenses,
      onDidChangeCodeLenses: inheritanceLensesChanged?.event,
    }),
  );
  context.subscriptions.push(
    vscode.languages.registerHoverProvider("kelp", {
      provideHover: provideKelpHover,
    }),
    vscode.languages.registerCompletionItemProvider("kelp", {
      provideCompletionItems: provideKelpCompletions,
    }),
    vscode.languages.registerDefinitionProvider("kelp", {
      provideDefinition: provideKelpDefinition,
    }),
    vscode.tasks.registerTaskProvider("kelp", kelpTaskProvider),
    vscode.window.registerTreeDataProvider("kelp.projects", projects),
    vscode.window.registerTreeDataProvider("kelp.actions", {
      getChildren: () => kelpActions,
      getTreeItem: ([label, command, icon]) => {
        const item = new vscode.TreeItem(label, vscode.TreeItemCollapsibleState.None);
        item.command = { command, title: label };
        item.iconPath = new vscode.ThemeIcon(icon);
        return item;
      },
    }),
    kelpStatus,
  );
  for (const command of ["check", "build", "debug", "run", "test", "package", "members"])
    context.subscriptions.push(
      vscode.commands.registerCommand(`kelp.${command}`, async (argument) => {
        try {
          const project = projectFrom(argument);
          if (command === "debug") return await debugKelp(project);
          return await runKelp(command, { project });
        } catch (error) {
          await vscode.window.showErrorMessage(error.message);
        }
      }),
    );
  context.subscriptions.push(
    vscode.commands.registerCommand("kelp.refreshProjects", () => projects.refresh()),
    vscode.commands.registerCommand("kelp.openManifest", (argument) =>
      guard(async () => openKelpManifest(argument)),
    ),
    vscode.commands.registerCommand("kelp.revealProject", (argument) =>
      guard(async () => revealKelpProject(argument)),
    ),
    vscode.commands.registerCommand("kelp.output", (argument) =>
      guard(async () => showKelpOutput(argument)),
    ),
    vscode.commands.registerCommand("kelyra.applyTokenColors", () =>
      guard(selectKelyraColorScheme),
    ),
    vscode.commands.registerCommand("kelyra.showInheritors", showKelyraInheritors),
  );
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((document) => {
      if (document.languageId === "kelyra") {
        void startLanguageServer();
        inheritanceLensesChanged?.fire();
        scheduleCfgRefresh();
      }
    }),
    ...(vscode.workspace.onDidChangeTextDocument ? [vscode.workspace.onDidChangeTextDocument(
      (event) => {
        const { document } = event;
        if (document.languageId === "kelyra") {
          kelyraTree.change(event);
          inlayHintsChanged?.fire();
          inheritanceLensesChanged?.fire();
          scheduleCfgRefresh();
        }
      },
    )] : []),
    ...(vscode.workspace.onDidCloseTextDocument ? [vscode.workspace.onDidCloseTextDocument(
      (document) => {
        kelyraTree.close(document);
        if (document.languageId === "kelyra") inheritanceLensesChanged?.fire();
      },
    )] : []),
    vscode.workspace.onDidSaveTextDocument((document) => {
      if (document.languageId === "kelyra") {
        invalidateKelyraIndex();
        inheritanceLensesChanged?.fire();
      }
      else if (document.languageId === "kelp") {
        // A manifest edit can change the artifact path shown in the status bar.
        const directory = document.uri?.fsPath && path.dirname(document.uri.fsPath);
        if (directory) kelpOutputPaths.delete(directory);
        else kelpOutputPaths.clear();
        void refreshKelpStatus();
        scheduleCfgRefresh();
      }
    }),
    vscode.window.onDidChangeActiveTextEditor(() => {
      void refreshKelpStatus();
      scheduleCfgRefresh();
    }),
    vscode.window.onDidChangeActiveColorTheme(() => void applyKelyraTokenColors(false)),
    vscode.workspace.onDidChangeConfiguration(async (event) => {
      if (event.affectsConfiguration("kelyra")) scheduleCfgRefresh();
      if (event.affectsConfiguration("kelyra.colorScheme")) {
        await applyKelyraTokenColors(false);
      } else if (event.affectsConfiguration("kelyra.languageServer.path")) {
        if (client) {
          await client.stop();
          client = undefined;
        }
        if (vscode.workspace.textDocuments.some(({ languageId }) => languageId === "kelyra"))
          await startLanguageServer();
      }
    }),
  );
  if (vscode.workspace.createFileSystemWatcher) {
    const watcher = vscode.workspace.createFileSystemWatcher("**/*.kly");
    context.subscriptions.push(watcher,
      watcher.onDidCreate(() => inheritanceLensesChanged?.fire()),
      watcher.onDidChange(() => inheritanceLensesChanged?.fire()),
      watcher.onDidDelete(() => inheritanceLensesChanged?.fire()));
  }
  if (vscode.workspace.getConfiguration("kelyra").get("colorScheme", "laevatain") !== "off")
    void applyKelyraTokenColors(false);
  if (vscode.workspace.textDocuments.some(({ languageId }) => languageId === "kelyra"))
    await startLanguageServer();
  void refreshKelpStatus();
  scheduleCfgRefresh();
}

async function deactivate() {
  if (cfgRefreshTimer) clearTimeout(cfgRefreshTimer);
  kelyraTree.dispose();
  if (client) {
    await client.stop();
    client = undefined;
  }
}

module.exports = {
  activate,
  applyKelyraTokenColors,
  deactivate,
  debugKelp,
  kelpActions,
  kelpArtifactPath,
  kelpFields,
  kelpOutputPath,
  kelpProjectTree,
  kelpTaskProvider,
  languageKeywords,
  parseKelpManifest,
  parseKelpMembers,
  projectFrom,
  runKelp,
  scanKelpProjects,
  showKelpOutput,
  documentAnnotations,
  format,
  formatterCandidates,
  kelpFieldAt,
  kelpSectionAt,
  buildKelyraIndex,
  kelyraEnumMemberRanges,
  kelyraParameterHints,
  kelyraSymbolAt,
  kelyraTokenColors,
  mergeKelyraTokenColors,
  parseKelyraModule,
  parseKelyraInheritance,
  kelyraDescendants,
  provideKelyraInheritanceLenses,
  provideKelpCompletions,
  provideKelpDefinition,
  provideKelyraCompletions,
  provideKelyraFoldingRanges,
  provideKelyraHover,
  provideKelyraInlayHints,
  selectKelyraColorScheme,
};
