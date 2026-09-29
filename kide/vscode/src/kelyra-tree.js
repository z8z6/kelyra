"use strict";

const { Parser, Language } = require("web-tree-sitter");

function positionAt(text, offset) {
  let line = 0;
  let start = 0;
  for (let index = 0; index < offset; ++index) {
    if (text[index] === "\n") { ++line; start = index + 1; }
  }
  return { line, character: offset - start };
}

function tokensFromTree(tree, text) {
  const tokens = [];
  for (const node of tree.rootNode.namedChildren) {
    if (node.type === "line_comment" || node.type === "block_comment") continue;
    if (node.type === "ERROR") return undefined;
    const kind = node.type === "identifier" ? "name" :
      node.type === "punctuation" ? "punct" : node.type;
    tokens.push({ kind, text: text.slice(node.startIndex, node.endIndex), offset: node.startIndex });
  }
  return tokens;
}

const tokenTypes = ["identifier", "number", "string", "punctuation"];

function updateTokens(tokens, tree, text, start, oldEnd, newEnd, changedRanges) {
  const delta = newEnd - oldEnd;
  const spanStart = Math.min(start, ...changedRanges.map(({ startIndex }) => startIndex));
  const spanEnd = Math.max(newEnd, ...changedRanges.map(({ endIndex }) => endIndex));
  const oldSpanEnd = Math.max(oldEnd, spanEnd - delta);
  let prefixEnd = 0;
  while (prefixEnd < tokens.length &&
         tokens[prefixEnd].offset + tokens[prefixEnd].text.length < spanStart) ++prefixEnd;
  let suffixStart = tokens.length;
  while (suffixStart > prefixEnd &&
         tokens[suffixStart - 1].offset > oldSpanEnd) --suffixStart;
  const prefix = tokens.slice(0, prefixEnd);
  const suffix = tokens.slice(suffixStart).map((token) => ({ ...token, offset: token.offset + delta }));
  const left = prefix.length ? prefix[prefix.length - 1].offset + prefix[prefix.length - 1].text.length : 0;
  const right = suffix.length ? suffix[0].offset : text.length;
  const startPoint = positionAt(text, left);
  const endPoint = positionAt(text, right);
  const nodes = tree.rootNode.descendantsOfType(tokenTypes,
    { row: startPoint.line, column: startPoint.character },
    { row: endPoint.line, column: endPoint.character });
  const changed = nodes.filter((node) => node && node.startIndex >= left && node.endIndex <= right)
    .map((node) => ({
      kind: node.type === "identifier" ? "name" :
        node.type === "punctuation" ? "punct" : node.type,
      text: text.slice(node.startIndex, node.endIndex),
      offset: node.startIndex,
    }));
  return [...prefix, ...changed, ...suffix];
}

class KelyraTree {
  constructor() {
    this.Parser = undefined;
    this.Documents = new Map();
  }

  async initialize(wasmPath) {
    await Parser.init();
    const parser = new Parser();
    parser.setLanguage(await Language.load(wasmPath));
    this.Parser = parser;
  }

  key(document) {
    return document.uri?.toString?.() || document.uri?.fsPath;
  }

  snapshot(document, fallback) {
    const text = document.getText();
    if (!this.Parser) return fallback(text);
    const key = this.key(document);
    if (!key) {
      const tree = this.Parser.parse(text);
      const tokens = tokensFromTree(tree, text);
      tree.delete();
      return tokens ? { tokens, positionAt: (offset) =>
        document.positionAt?.(offset) || positionAt(text, offset) } : fallback(text);
    }
    let state = key && this.Documents.get(key);
    if (state?.text !== text) {
      const old = state?.tree;
      const tree = this.Parser.parse(text);
      old?.delete();
      state = { text, tree, tokens: undefined };
      if (key) this.Documents.set(key, state);
    }
    if (!state) {
      const tree = this.Parser.parse(text);
      state = { text, tree, tokens: undefined };
      if (key) this.Documents.set(key, state);
    }
    state.tokens ??= tokensFromTree(state.tree, text);
    if (!state.tokens) return fallback(text);
    return { tokens: state.tokens, positionAt: (offset) =>
      document.positionAt?.(offset) || positionAt(text, offset) };
  }

  change(event) {
    if (!this.Parser) return;
    const key = this.key(event.document);
    const state = key && this.Documents.get(key);
    if (!state || event.contentChanges.length !== 1) return;
    const change = event.contentChanges[0];
    if (change.rangeOffset === undefined || change.rangeLength === undefined) return;
    const start = change.rangeOffset;
    const oldEnd = start + change.rangeLength;
    const before = state.text;
    const after = event.document.getText();
    if (before.length - change.rangeLength + change.text.length !== after.length) return;
    const startPosition = change.range?.start || positionAt(before, start);
    const oldEndPosition = change.range?.end || positionAt(before, oldEnd);
    const addedLines = change.text.split("\n");
    const newEndPosition = addedLines.length === 1
      ? { line: startPosition.line, character: startPosition.character + change.text.length }
      : { line: startPosition.line + addedLines.length - 1,
        character: addedLines[addedLines.length - 1].length };
    state.tree.edit({
      startIndex: start,
      oldEndIndex: oldEnd,
      newEndIndex: start + change.text.length,
      startPosition: { row: startPosition.line, column: startPosition.character },
      oldEndPosition: { row: oldEndPosition.line, column: oldEndPosition.character },
      newEndPosition: { row: newEndPosition.line, column: newEndPosition.character },
    });
    const tree = this.Parser.parse(after, state.tree);
    let tokens;
    if (state.tokens && !tree.rootNode.hasError)
      tokens = updateTokens(state.tokens, tree, after, start, oldEnd,
        start + change.text.length, state.tree.getChangedRanges(tree));
    state.tree.delete();
    this.Documents.set(key, { text: after, tree, tokens });
  }

  close(document) {
    const key = this.key(document);
    const state = key && this.Documents.get(key);
    state?.tree.delete();
    this.Documents.delete(key);
  }

  dispose() {
    for (const state of this.Documents.values()) state.tree.delete();
    this.Documents.clear();
    this.Parser?.delete();
    this.Parser = undefined;
  }
}

module.exports = { KelyraTree, tokensFromTree };
