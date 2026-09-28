"use strict";

const fs = require("node:fs/promises");
const path = require("node:path");

const version = 2;

function encodeModule(module) {
  return {
    module: module.module,
    imports: module.imports,
    functions: [...module.functions],
    classes: [...module.classes].map(([name, info]) => [name, {
      init: info.init,
      methods: [...info.methods],
    }]),
  };
}

function decodeModule(module) {
  return {
    module: module.module,
    imports: module.imports,
    functions: new Map(module.functions),
    classes: new Map(module.classes.map(([name, info]) => [name, {
      init: info.init,
      methods: new Map(info.methods),
    }])),
  };
}

function cachePath(root) {
  return path.join(root, ".kelp", "kide-index.json");
}

async function readIndex(root) {
  if (!root) return undefined;
  try {
    const data = JSON.parse(await fs.readFile(cachePath(root), "utf8"));
    if (data.version !== version || !Array.isArray(data.modules)) return undefined;
    return data.modules.map(decodeModule);
  } catch { return undefined; }
}

async function writeIndex(root, modules) {
  if (!root) return;
  const directory = path.dirname(cachePath(root));
  await fs.mkdir(directory, { recursive: true });
  const destination = cachePath(root);
  const temporary = `${destination}.${process.pid}.tmp`;
  try {
    await fs.writeFile(temporary, JSON.stringify({ version, modules: modules.map(encodeModule) }));
    await fs.rename(temporary, destination);
  } finally {
    await fs.rm(temporary, { force: true });
  }
}

module.exports = { cachePath, readIndex, writeIndex };
