"use strict";

const assert = require("node:assert/strict");
const Module = require("node:module");
const { promisify } = require("node:util");
const manifest = require("../package.json");
const folder = { uri: { fsPath: "/workspace/project" } };
let listener, disposed = 0, exitCode = 0, taskError = false, adapter = true;
let saved = true, started = true, gdbError = false;
const tasks = [], launches = [], commands = [], processes = [];
const document = { languageId: "kelyra", uri: { scheme: "file", fsPath: "/workspace/project/src/main.kly" } };
const vscode = {
  workspace: {
    isTrusted: true,
    workspaceFolders: [folder],
    getWorkspaceFolder: () => folder,
    saveAll: async () => saved,
    getConfiguration: () => ({ get: (key, fallback) => ({ path: "/tools/kelp", "debug.args": ["two words"] }[key] ?? fallback) }),
  },
  window: { activeTextEditor: { document } },
  extensions: { getExtension: () => adapter ? {} : undefined },
  commands: { executeCommand: async (command) => commands.push(command) },
  debug: { startDebugging: async (scope, config) => { launches.push({ scope, config }); return started; } },
  ProcessExecution: class { constructor(executable, args, options) { Object.assign(this, { executable, args, options }); } },
  Task: class { constructor(definition, scope, name, source, execution, problemMatchers) { Object.assign(this, { definition, scope, name, source, execution, problemMatchers }); } },
  tasks: {
    onDidEndTaskProcess: (callback) => { listener = callback; return { dispose: () => { ++disposed; listener = undefined; } }; },
    executeTask: async (task) => {
      if (taskError) throw new Error("task failed to start");
      tasks.push(task);
      const execution = { task };
      setImmediate(() => {
        listener?.({ execution: {}, exitCode: 1 }); // Unrelated tasks must be ignored.
        listener?.({ execution, exitCode });
      });
      return execution;
    },
  },
};
const execFile = () => { throw new Error("unexpected callback process"); };
execFile[promisify.custom] = async (executable, args, options) => {
  processes.push({ executable, args, options });
  if (args[0] === "--version" && gdbError) throw new Error("not found");
  return { stdout: args[0] === "output" ? "/workspace/project/custom build/app\n" : "GNU gdb", stderr: "" };
};
const load = Module._load;
Module._load = function (request, parent, main) {
  if (request === "vscode") return vscode;
  if (request === "node:child_process") return { execFile };
  if (request === "node:fs/promises") return { access: async (file) => {
    if (file !== "/workspace/project/kelp.toml") throw Object.assign(new Error("missing"), { code: "ENOENT" });
  } };
  return load(request, parent, main);
};
const { debugKelp, kelpActions, runKelp } = require("../src/extension.js");
Module._load = load;

async function test() {
  assert.ok(!manifest.contributes.commands.some(({ command }) => command === "kelp.format"));
  assert.ok(!kelpActions.some(([, command]) => command === "kelp.format"));
  assert.deepEqual(manifest.contributes.breakpoints, [{ language: "kelyra" }]);
  assert.equal(manifest.contributes.configuration.properties["kelp.debug.gdbPath"].default, "gdb");
  for (const command of ["focusVariablesView", "focusCallStackView", "focusWatchView", "focusRepl"])
    assert.ok(kelpActions.some(([, action]) => action === `workbench.debug.action.${command}`));
  assert.ok(kelpActions.some(([label, action]) => label === "Members" && action === "kelp.members"));
  await debugKelp();
  assert.equal(disposed, 1);
  assert.deepEqual(tasks[0].execution.args, ["build", "--debug"]);
  assert.equal(tasks[0].execution.options.cwd, folder.uri.fsPath);
  assert.equal(tasks[0].problemMatchers, "$kelyra");
  assert.equal(tasks[0].source, "kelp");
  const { config, scope } = launches[0];
  assert.equal(scope, folder);
  assert.equal(config.type, "cppdbg");
  assert.equal(config.MIMode, "gdb");
  assert.equal(config.miDebuggerPath, undefined); // cppdbg searches PATH for GDB.
  assert.equal(config.program, "/workspace/project/custom build/app");
  assert.deepEqual(config.args, ["two words"]);
  assert.equal(config.stopAtEntry, true);
  assert.equal(config.sourceFileMap, undefined); // Kelp compiles in place.
  assert.deepEqual(commands, ["workbench.view.debug", "workbench.debug.action.focusRepl"]);
  assert.deepEqual(processes.map(({ args }) => args), [["--version"], ["output"]]);
  exitCode = 1;
  await assert.rejects(debugKelp(), /exited with code 1/);
  assert.equal(launches.length, 1);
  taskError = true;
  await assert.rejects(runKelp("build", { wait: true }), /failed to start/);
  assert.equal(disposed, 3);
  taskError = false;
  exitCode = 0;
  adapter = false;
  await assert.rejects(debugKelp(), /ms-vscode.cpptools/);
  adapter = true;
  gdbError = true;
  await assert.rejects(debugKelp(), /Cannot run local GDB/);
  gdbError = false;
  saved = false;
  await assert.rejects(debugKelp(), /Save project files/);
  saved = true;
  started = false;
  await assert.rejects(debugKelp(), /did not start/);
  vscode.workspace.isTrusted = false;
  await assert.rejects(debugKelp(), /Trust this workspace/);
}
test().catch((error) => { console.error(error); process.exitCode = 1; });
