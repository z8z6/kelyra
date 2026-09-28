"use strict";

// Editor-only analysis. It never executes source text, and leaves values it
// cannot prove to the compiler rather than guessing.
function evaluateConstant(tokens, values = new Map()) {
  let cursor = 0;
  const precedence = { "||": 1, "&&": 2, "==": 3, "!=": 3, "<": 4,
    "<=": 4, ">": 4, ">=": 4, "+": 5, "-": 5, "*": 6, "/": 6, "%": 6 };
  const operator = () => {
    const pair = tokens[cursor]?.text + tokens[cursor + 1]?.text;
    return precedence[pair] ? pair : tokens[cursor]?.text;
  };
  const primary = () => {
    const token = tokens[cursor++];
    if (!token) return undefined;
    if (token.text === "(") {
      const value = expression(1);
      if (tokens[cursor++]?.text !== ")") return undefined;
      return value;
    }
    if (["!", "-", "+"].includes(token.text)) {
      const value = primary();
      if (token.text === "!") return typeof value === "boolean" ? !value : undefined;
      return typeof value === "number" ? (token.text === "-" ? -value : value) : undefined;
    }
    if (token.text === "true") return true;
    if (token.text === "false") return false;
    if (token.kind === "number") {
      const number = Number(token.text.replace(/_/g, ""));
      return Number.isFinite(number) &&
        (!Number.isInteger(number) || Number.isSafeInteger(number)) ? number : undefined;
    }
    if (token.kind === "string") {
      try { return JSON.parse(token.text); } catch { return undefined; }
    }
    return values.get(token.text);
  };
  const expression = (minimum) => {
    let left = primary();
    while (precedence[operator()] >= minimum) {
      const op = operator();
      cursor += op.length === 2 ? 2 : 1;
      const right = expression(precedence[op] + 1);
      if (left === undefined || right === undefined) return undefined;
      if (op === "+") left = left + right;
      else if (op === "-") left = left - right;
      else if (op === "*") left = left * right;
      else if (op === "/") left = typeof left === "number" && right !== 0 &&
        (!Number.isInteger(left) || !Number.isInteger(right) || left % right === 0)
        ? left / right : undefined;
      else if (op === "%") left = typeof left === "number" && right !== 0 ? left % right : undefined;
      else if (op === "==") left = left === right;
      else if (op === "!=") left = left !== right;
      else if (op === "<") left = left < right;
      else if (op === "<=") left = left <= right;
      else if (op === ">") left = left > right;
      else if (op === ">=") left = left >= right;
      else if (op === "&&") left = left && right;
      else if (op === "||") left = left || right;
    }
    return left;
  };
  const result = expression(1);
  return cursor === tokens.length && ["number", "boolean", "string"].includes(typeof result) &&
    (typeof result !== "number" || Number.isFinite(result) &&
      (!Number.isInteger(result) || Number.isSafeInteger(result)))
    ? result : undefined;
}

function constants(tokens) {
  const values = new Map();
  for (const entry of constantExpressions(tokens)) values.set(entry.name, entry.value);
  return values;
}

function constantExpressions(tokens) {
  const values = new Map();
  const expressions = [];
  for (let index = 0; index < tokens.length; ++index) {
    if (tokens[index].text !== "const" || tokens[index + 1]?.kind !== "name") continue;
    const name = tokens[index + 1].text;
    let start = index + 2;
    while (start < tokens.length && !["=", ";"].includes(tokens[start].text)) ++start;
    if (tokens[start]?.text !== "=") continue;
    let end = start + 1;
    while (end < tokens.length && tokens[end].text !== ";") ++end;
    const value = evaluateConstant(tokens.slice(start + 1, end), values);
    if (value !== undefined) {
      values.set(name, value);
      expressions.push({ name, value, start: tokens[start + 1]?.offset,
        end: tokens[end - 1]?.offset + tokens[end - 1]?.text.length });
    }
    index = end;
  }
  return expressions;
}

// Returns source-offset ranges for declarations rejected by @cfg. Invalid or
// unresolved annotations are left alone so the editor does not hide errors.
function inactiveCfgRanges(tokens, source, target) {
  const ranges = [];
  for (let index = 0; index < tokens.length - 5; ++index) {
    if (tokens[index].text !== "@" || tokens[index + 1]?.text !== "cfg" ||
        tokens[index + 2]?.text !== "(") continue;
    let cursor = index + 3;
    let enabled = true;
    let valid = false;
    while (cursor < tokens.length && tokens[cursor].text !== ")") {
      const key = tokens[cursor]?.text;
      const value = tokens[cursor + 2];
      if (!["os", "arch"].includes(key) || tokens[cursor + 1]?.text !== "=" ||
          value?.kind !== "string") { valid = false; break; }
      try {
        enabled &&= JSON.parse(value.text) === target[key];
      } catch { valid = false; break; }
      valid = true;
      cursor += 3;
      if (tokens[cursor]?.text === ",") ++cursor;
      else if (tokens[cursor]?.text !== ")") { valid = false; break; }
    }
    if (!valid || tokens[cursor]?.text !== ")" || enabled) continue;
    let declaration = cursor + 1;
    while (tokens[declaration]?.text === "@") {
      ++declaration;
      while (declaration < tokens.length && !["(", "module", "import", "fn", "class", "const", "let"].includes(tokens[declaration].text)) ++declaration;
      if (tokens[declaration]?.text === "(") {
        let depth = 1;
        while (++declaration < tokens.length && depth)
          depth += (tokens[declaration].text === "(") - (tokens[declaration].text === ")");
        ++declaration;
      }
    }
    if (!tokens[declaration]) continue;
    let end = declaration;
    let braces = 0;
    for (; end < tokens.length; ++end) {
      if (tokens[end].text === "{") ++braces;
      if (tokens[end].text === "}" && braces > 0 && --braces === 0) break;
      if (tokens[end].text === ";" && braces === 0) break;
    }
    if (!tokens[end]) continue;
    const from = tokens[index].offset;
    const to = tokens[end].offset + tokens[end].text.length;
    if (tokens[declaration].text === "module") return [{ start: from, end: source.length }];
    ranges.push({ start: from, end: to });
    index = end;
  }
  return ranges;
}

module.exports = { constantExpressions, constants, evaluateConstant, inactiveCfgRanges };
