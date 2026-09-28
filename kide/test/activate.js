"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const Module = require("node:module");
const os = require("node:os");
const path = require("node:path");

// A workspace with one declared member, scanned because the mock workspace is
// untrusted and no `kelp` executable is available.
const projectRoot = fs.mkdtempSync(path.join(os.tmpdir(), "kide-activate-"));
fs.mkdirSync(path.join(projectRoot, "libs/demo"), { recursive: true });
fs.writeFileSync(path.join(projectRoot, "kelp.toml"), '[workspace]\nmembers = ["libs/demo"]\n');
fs.writeFileSync(
  path.join(projectRoot, "libs/demo/kelp.toml"),
  '[project]\nname = "demo"\nentry = "src/main.kly"\n\n[build]\nkind = "library"\n',
);
const workspaceFolder = { name: "kest", uri: { scheme: "file", fsPath: projectRoot } };

const subscriptions = [];
const commands = new Map();
const disposables = [];
const providers = [];
let inheritanceProvider;
let shownDocument;
const taskProviders = [];
const treeProviders = new Map();
const configurationUpdates = [];
let openListener;
let activeEditorListener;
let colorThemeListener;
let configurationListener;
const starts = [];
const stops = [];
const configurationValues = {
  editor: {},
  kelyra: { colorScheme: "off" },
};

function disposable() {
  const item = { dispose: () => disposables.push(item) };
  return item;
}

const statusItem = {
  text: "",
  tooltip: "",
  command: "",
  shown: false,
  show() {
    this.shown = true;
  },
  dispose() {
    disposables.push(this);
  },
};

const vscode = {
  languages: {
    registerDocumentFormattingEditProvider: () => disposable(),
    registerHoverProvider: () => disposable(),
    registerCompletionItemProvider: () => disposable(),
    registerDefinitionProvider: (language) => {
      providers.push(`definition:${language}`);
      return disposable();
    },
    registerFoldingRangeProvider: (language) => {
      providers.push(`folding:${language}`);
      return disposable();
    },
    registerInlayHintsProvider: (language) => {
      providers.push(`inlay:${language}`);
      return disposable();
    },
    registerCodeLensProvider: (language, provider) => {
      providers.push(`codelens:${language}`);
      inheritanceProvider = provider;
      return disposable();
    },
  },
  window: {
    registerTreeDataProvider: (id, provider) => {
      providers.push(`tree:${id}`);
      treeProviders.set(id, provider);
      return disposable();
    },
    createStatusBarItem: () => statusItem,
    showErrorMessage: async () => undefined,
    showInformationMessage: async () => undefined,
    showQuickPick: async (items) => items[0],
    showTextDocument: async (document, options) => {
      shownDocument = { document, options };
      return shownDocument;
    },
    activeTextEditor: undefined,
    activeColorTheme: { kind: 2 },
    onDidChangeActiveColorTheme: (listener) => {
      colorThemeListener = listener;
      return disposable();
    },
    onDidChangeActiveTextEditor: (listener) => {
      activeEditorListener = listener;
      return disposable();
    },
  },
  ColorThemeKind: { Light: 1, Dark: 2, HighContrast: 3, HighContrastLight: 4 },
  ConfigurationTarget: { Global: 1 },
  commands: {
    registerCommand: (name, handler) => {
      commands.set(name, handler);
      return disposable();
    },
    executeCommand: async () => undefined,
  },
  tasks: {
    registerTaskProvider: (type, provider) => {
      taskProviders.push(`${type}:${provider.provideTasks ? "dynamic" : "static"}`);
      return disposable();
    },
  },
  workspace: {
    isTrusted: false,
    textDocuments: [],
    workspaceFolders: [workspaceFolder],
    findFiles: async () => ["base.kly", "child.kly"].map((name) => vscode.Uri.file(path.join(projectRoot, name))),
    fs: { readFile: async (uri) => fs.readFileSync(uri.fsPath) },
    openTextDocument: async (uri) => ({ uri,
      languageId: "kelyra", getText: () => fs.readFileSync(uri.fsPath, "utf8") }),
    asRelativePath: (uri) => path.relative(projectRoot, uri.fsPath),
    getConfiguration: (section) => ({
      get: (key, fallback) => configurationValues[section]?.[key] ?? fallback,
      update: async (key, value, target) => {
        configurationValues[section] ??= {};
        configurationValues[section][key] = value;
        configurationUpdates.push({ key, value, target });
      },
    }),
    onDidOpenTextDocument: (listener) => {
      openListener = listener;
      return disposable();
    },
    onDidSaveTextDocument: () => disposable(),
    onDidChangeConfiguration: (listener) => {
      configurationListener = listener;
      return disposable();
    },
  },
  EventEmitter: class EventEmitter {
    constructor() {
      this.listeners = [];
      this.event = (listener) => {
        this.listeners.push(listener);
        return { dispose: () => {} };
      };
    }

    fire(value) {
      this.listeners.forEach((listener) => listener(value));
    }
  },
  TreeItem: class TreeItem {
    constructor(label, collapsibleState) {
      this.label = label;
      this.collapsibleState = collapsibleState;
    }
  },
  TreeItemCollapsibleState: { None: 0, Expanded: 1 },
  ThemeIcon: class ThemeIcon {
    constructor(id) {
      this.id = id;
    }
  },
  MarkdownString: class MarkdownString {
    constructor(value) {
      this.value = value;
    }
  },
  Uri: { file: (fsPath) => ({ scheme: "file", fsPath,
    toString: () => `file://${fsPath}` }) },
  Position: class Position {
    constructor(line, character) { Object.assign(this, { line, character }); }
  },
  Range: class Range {
    constructor(...positions) { this.positions = positions; }
  },
  CodeLens: class CodeLens {
    constructor(range, command) { Object.assign(this, { range, command }); }
  },
};

class LanguageClient {
  constructor(id, name, serverOptions, clientOptions) {
    this.id = id;
    this.name = name;
    this.serverOptions = serverOptions;
    this.clientOptions = clientOptions;
  }

  async start() {
    starts.push(this.serverOptions.command);
  }

  async stop() {
    stops.push(this.id);
  }
}

const load = Module._load;
Module._load = function (request, parent, main) {
  if (request === "vscode") return vscode;
  if (request === "vscode-languageclient/node")
    return { LanguageClient, TransportKind: { stdio: 0 } };
  return load(request, parent, main);
};

const { activate, deactivate } = require("../src/extension.js");
// The loader override stays installed: startLanguageServer requires the
// client lazily, after this module has loaded.

async function test() {
  fs.writeFileSync(path.join(projectRoot, "base.kly"), "module demo;\nclass Base<T> {}\n");
  fs.writeFileSync(path.join(projectRoot, "child.kly"),
    "module demo;\nclass Child<T>: Base<T> {}\n");
  await activate({ subscriptions: { push: (...items) => subscriptions.push(...items) } });
  for (const command of [
    "kelp.check",
    "kelp.build",
    "kelp.members",
    "kelp.refreshProjects",
    "kelp.openManifest",
    "kelp.revealProject",
    "kelp.output",
    "kelyra.applyTokenColors",
    "kelyra.showInheritors",
  ])
    assert.ok(commands.has(command), command);
  assert.deepEqual(providers, [
    "folding:kelyra",
    "inlay:kelyra",
    "codelens:kelyra",
    "definition:kelp",
    "tree:kelp.projects",
    "tree:kelp.actions",
  ]);
  assert.deepEqual(taskProviders, ["kelp:dynamic"]);
  const baseDocument = await vscode.workspace.openTextDocument(vscode.Uri.file(path.join(projectRoot, "base.kly")));
  const lenses = await inheritanceProvider.provideCodeLenses(baseDocument);
  assert.equal(lenses.length, 1);
  assert.match(lenses[0].command.title, /1 inheriting class/);
  await commands.get("kelyra.showInheritors")(...lenses[0].command.arguments);
  assert.equal(shownDocument.document.uri.fsPath, path.join(projectRoot, "child.kly"));
  assert.deepEqual(shownDocument.options.selection.positions, [1, 6, 1, 11]);
  assert.equal(starts.length, 0); // No Kelyra document is open yet.

  // The picker stores the scheme and writes only Kelyra-scoped rules.
  await commands.get("kelyra.applyTokenColors")();
  assert.equal(configurationUpdates.length, 2);
  assert.deepEqual(configurationUpdates[0], {
    key: "colorScheme",
    value: "laevatain",
    target: vscode.ConfigurationTarget.Global,
  });
  assert.equal(configurationUpdates[1].key, "tokenColorCustomizations");
  assert.equal(configurationUpdates[1].target, vscode.ConfigurationTarget.Global);
  assert.ok(
    configurationUpdates[1].value.textMateRules.some((rule) =>
      [].concat(rule.scope).includes("source.kelyra variable.other.readwrite"),
    ),
  );
  await configurationListener({
    affectsConfiguration: (section) => section === "kelyra.colorScheme",
  });
  assert.ok(
    configurationUpdates[1].value.textMateRules.some((rule) =>
      [].concat(rule.scope).includes("source.kelyra entity.name.function"),
    ),
  );
  vscode.window.activeColorTheme = { kind: vscode.ColorThemeKind.Light };
  colorThemeListener(vscode.window.activeColorTheme);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(configurationUpdates.length, 3);
  assert.equal(
    configurationUpdates[2].value.textMateRules[0].settings.foreground,
    "#76616B",
  );

  // The Projects view scans manifests when `kelp` cannot be run, nests members
  // by directory, and points each project at its own working directory.
  const projects = treeProviders.get("kelp.projects");
  const nodes = await projects.getChildren();
  assert.deepEqual(
    nodes.map(({ type, label }) => [type, label]),
    [["directory", "libs"]],
  );
  assert.equal(projects.getTreeItem(nodes[0]).contextValue, "kelp.directory");
  const demo = projects.getTreeItem(nodes[0].children[0]);
  assert.equal(demo.label, "demo");
  assert.equal(demo.description, "library · .kelp/build/libs/demo/demo.o");
  assert.equal(demo.contextValue, "kelp.project");
  assert.equal(demo.project.cwd, path.join(projectRoot, "libs/demo"));
  assert.equal(demo.command.command, "kelp.openManifest");
  projects.refresh();

  activeEditorListener(undefined);
  await new Promise((resolve) => setImmediate(resolve));
  assert.ok(statusItem.shown);

  openListener({ languageId: "kelyra" });
  for (let attempt = 0; starts.length === 0 && attempt < 30; ++attempt)
    await new Promise((resolve) => setTimeout(resolve, 10));
  assert.deepEqual(starts, ["kelyra-ls"]);
  assert.equal(stops.length, 0);

  // Changing the server path restarts it; unrelated settings do not.
  configurationListener({ affectsConfiguration: () => false });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(stops.length, 0);
  vscode.workspace.textDocuments.push({ languageId: "kelyra" });
  const builtServer = path.join(projectRoot, "kelyra/build/bin/kelyra-ls");
  fs.mkdirSync(path.dirname(builtServer), { recursive: true });
  fs.writeFileSync(builtServer, "");
  configurationListener({ affectsConfiguration: (section) => section === "kelyra.languageServer.path" });
  for (let attempt = 0; starts.length < 2 && attempt < 30; ++attempt)
    await new Promise((resolve) => setTimeout(resolve, 10));
  assert.deepEqual(stops, ["kelyra"]);
  assert.equal(starts.length, 2);
  assert.equal(starts[1], builtServer);

  await deactivate();
  assert.deepEqual(stops, ["kelyra", "kelyra"]);
  assert.ok(subscriptions.length > 0);
  fs.rmSync(projectRoot, { recursive: true, force: true });
}

test().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
