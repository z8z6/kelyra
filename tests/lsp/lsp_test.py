import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

# A workspace whose modules reach outside the opened folder: a local path
# dependency next to the workspace and a cached dependency clone under .kelp.
fixture = pathlib.Path(tempfile.mkdtemp(prefix="kelyra-lsp-"))
workspace = fixture / "ws"
demo = fixture / "libs/demo"
cached = workspace / ".kelp/dependencies/cached"
for directory in [workspace / "app/src", demo / "src", cached / "src"]:
    directory.mkdir(parents=True)
(workspace / "kelp.toml").write_text('[workspace]\nmembers = ["app"]\n')
(workspace / "app/kelp.toml").write_text(
    '[project]\nname = "app"\nentry = "src/main.kly"\n\n'
    '[dependencies.demo]\npath = "../../libs/demo"\n'
)
(demo / "kelp.toml").write_text('[project]\nname = "demo"\nentry = "src/demo_api.kly"\n')
(demo / "src/demo_api.kly").write_text(
    "module demo_api;\n\n// Helper from a local path dependency.\n"
    "pub fn demo_helper(value: i32) -> i32 { return value; }\n"
)
(cached / "kelp.toml").write_text(
    '[project]\nname = "cached"\nentry = "src/cached_api.kly"\n'
)
(cached / "src/cached_api.kly").write_text(
    "module cached_api;\n\n// Helper from the shared dependency cache.\n"
    "pub fn cached_helper(value: i32) -> i32 { return value; }\n"
)

process = subprocess.Popen(
    [sys.argv[1], "--stdio"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)


def send(message):
    body = json.dumps(message, separators=(",", ":")).encode()
    process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    process.stdin.flush()


def receive(request_id):
    while True:
        length = None
        while True:
            line = process.stdout.readline()
            assert line, process.stderr.read().decode()
            if line == b"\r\n":
                break
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":", 1)[1])
        message = json.loads(process.stdout.read(length))
        if message.get("id") == request_id:
            return message["result"]


send(
    {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {"capabilities": {}, "rootUri": workspace.as_uri()},
    }
)
capabilities = receive(1)["capabilities"]
assert capabilities["hoverProvider"]
assert capabilities["signatureHelpProvider"]["triggerCharacters"] == ["(", ","]
assert capabilities["documentSymbolProvider"]
assert capabilities["referencesProvider"]

source = """// Adds two numbers.
fn add(a: i32, b: i32) -> i32 {
  return a + b;
}

fn main() -> i32 {
  // The computed answer.
  let value: i32 = add(20, 22);
  return value;
}
"""
uri = "file:///tmp/kelyra-lsp-test.kly"
send(
    {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": uri,
                "languageId": "kelyra",
                "version": 1,
                "text": source,
            }
        },
    }
)

send(
    {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "textDocument/hover",
        "params": {"textDocument": {"uri": uri}, "position": {"line": 8, "character": 9}},
    }
)
hover = receive(2)["contents"]["value"]
assert "value: i32" in hover and "The computed answer." in hover

send(
    {
        "jsonrpc": "2.0",
        "id": 3,
        "method": "textDocument/hover",
        "params": {"textDocument": {"uri": uri}, "position": {"line": 7, "character": 21}},
    }
)
assert "Adds two numbers." in receive(3)["contents"]["value"]

send(
    {
        "jsonrpc": "2.0",
        "id": 4,
        "method": "textDocument/definition",
        "params": {"textDocument": {"uri": uri}, "position": {"line": 7, "character": 20}},
    }
)
definition = receive(4)[0]
assert definition["range"]["start"] == {"line": 1, "character": 3}

send(
    {
        "jsonrpc": "2.0",
        "id": 5,
        "method": "textDocument/completion",
        "params": {"textDocument": {"uri": uri}, "position": {"line": 8, "character": 2}},
    }
)
labels = {item["label"] for item in receive(5)["items"]}
assert {"add", "value", "while", "i32"} <= labels

send(
    {
        "jsonrpc": "2.0",
        "id": 7,
        "method": "textDocument/documentSymbol",
        "params": {"textDocument": {"uri": uri}},
    }
)
symbols = {symbol["name"]: symbol for symbol in receive(7)}
assert set(symbols) >= {"add", "main"}
assert symbols["add"]["kind"] == 12  # SymbolKind.Function
assert symbols["add"]["detail"].startswith("fn add(")
assert symbols["add"]["selectionRange"]["start"] == {"line": 1, "character": 3}

send(
    {
        "jsonrpc": "2.0",
        "id": 8,
        "method": "textDocument/signatureHelp",
        "params": {"textDocument": {"uri": uri}, "position": {"line": 7, "character": 26}},
    }
)
help = receive(8)
assert help["activeParameter"] == 1
signature = help["signatures"][0]
assert signature["label"] == "fn add(a: i32, b: i32) -> i32"
assert len(signature["parameters"]) == 2
assert signature["label"][slice(*signature["parameters"][0]["label"])] == "a: i32"

send(
    {
        "jsonrpc": "2.0",
        "id": 9,
        "method": "textDocument/signatureHelp",
        "params": {"textDocument": {"uri": uri}, "position": {"line": 7, "character": 24}},
    }
)
assert receive(9)["activeParameter"] == 0

class_source = """class Counter {
  count: i32;
  init(count: i32) { this.count = count; }
  fn get() -> i32 { return count; }
}
fn use() -> i32 {
  let counter = Counter(7);
  return counter.get();
}
"""
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": uri, "languageId": "kelyra", "version": 2, "text": class_source}}})
send({"jsonrpc": "2.0", "id": 10, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": {"line": 7, "character": 17}}})
members = {item["label"] for item in receive(10)["items"]}
assert {"count", "get"} <= members and "init" not in members
send({"jsonrpc": "2.0", "id": 11, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": uri}, "position": {"line": 7, "character": 18}}})
assert receive(11)[0]["range"]["start"] == {"line": 3, "character": 5}
send({"jsonrpc": "2.0", "id": 12, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": uri}, "position": {"line": 2, "character": 26}}})
assert "count: i32" in receive(12)["contents"]["value"]

send({"jsonrpc": "2.0", "id": 13, "method": "textDocument/documentSymbol", "params": {
    "textDocument": {"uri": uri}}})
class_symbols = {symbol["name"]: symbol for symbol in receive(13)}
assert class_symbols["Counter"]["kind"] == 5  # SymbolKind.Class
children = {child["name"]: child for child in class_symbols["Counter"]["children"]}
assert set(children) >= {"count", "init", "get"}
assert children["count"]["kind"] == 8  # SymbolKind.Field
assert children["init"]["kind"] == 9  # SymbolKind.Constructor
assert children["get"]["kind"] == 6  # SymbolKind.Method

send({"jsonrpc": "2.0", "id": 14, "method": "textDocument/signatureHelp", "params": {
    "textDocument": {"uri": uri}, "position": {"line": 6, "character": 25}}})
constructor_help = receive(14)
assert constructor_help["signatures"][0]["label"] == "init(count: i32)"
assert constructor_help["activeParameter"] == 0

# Modules from a path dependency and the dependency cache resolve to their own
# sources, both through a wildcard import and through a qualified name.
app_source = """module app.main;

import demo_api.*;
import cached_api.*;

fn call_demo() -> i32 {
  let first: i32 = demo_helper(1);
  let second: i32 = cached_api.cached_helper(2);
  return first + second;
}
"""
app_uri = (workspace / "app/src/main.kly").as_uri()
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": app_uri, "languageId": "kelyra", "version": 1, "text": app_source}}})


def position_in(text, needle, offset=2):
    for index, line in enumerate(text.split("\n")):
        if needle in line:
            return {"line": index, "character": line.index(needle) + offset}
    raise AssertionError(needle)


def app_position(needle, offset=2):
    return position_in(app_source, needle, offset)


send({"jsonrpc": "2.0", "id": 15, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": app_uri}, "position": app_position("demo_helper")}})
assert receive(15)[0]["uri"] == (demo / "src/demo_api.kly").as_uri()
send({"jsonrpc": "2.0", "id": 16, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": app_uri}, "position": app_position("cached_helper")}})
assert receive(16)[0]["uri"] == (cached / "src/cached_api.kly").as_uri()
send({"jsonrpc": "2.0", "id": 17, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": app_uri}, "position": app_position("demo_helper")}})
hover = receive(17)["contents"]["value"]
assert "demo_helper" in hover and "local path dependency" in hover

# An `import` navigates to the imported module's own file.
send({"jsonrpc": "2.0", "id": 18, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": app_uri}, "position": app_position("import demo_api", 8)}})
import_target = receive(18)[0]
assert import_target["uri"] == (demo / "src/demo_api.kly").as_uri()
assert import_target["range"]["start"] == {"line": 0, "character": 7}

# C functions and types are declared in the headers `import c` pulls in.
header = workspace / "app/src/bridge.h"
header.write_text(
    "#ifndef DEMO_BRIDGE_H\n#define DEMO_BRIDGE_H\n\n"
    "int demo_c_add(int left, int right);\n"
    "long demo_c_length(const char *text);\n\n"
    "#endif\n"
)
c_source = """module app.c_api;

import c "bridge.h";
import c.*;

fn call_c() -> i32 {
  let first: c.int = demo_c_add(1, 2);
  return first + c.demo_c_length("kelyra");
}
"""
c_uri = (workspace / "app/src/c_api.kly").as_uri()
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": c_uri, "languageId": "kelyra", "version": 1, "text": c_source}}})
send({"jsonrpc": "2.0", "id": 19, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": c_uri}, "position": position_in(c_source, "demo_c_add")}})
c_definition = receive(19)[0]
assert c_definition["uri"] == header.as_uri()
assert c_definition["range"]["start"] == {"line": 3, "character": 4}
send({"jsonrpc": "2.0", "id": 20, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": c_uri}, "position": position_in(c_source, "demo_c_length")}})
assert receive(20)[0]["range"]["start"] == {"line": 4, "character": 5}
send({"jsonrpc": "2.0", "id": 21, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": c_uri}, "position": position_in(c_source, '"bridge.h"', 4)}})
assert receive(21)[0]["uri"] == header.as_uri()
send({"jsonrpc": "2.0", "id": 22, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": c_uri}, "position": position_in(c_source, "demo_c_add")}})
assert "int demo_c_add(int left, int right);" in receive(22)["contents"]["value"]

# Find references: a function across its declaration and its call sites, a
# local inside its scope, and a module across the imports that name it.
send({"jsonrpc": "2.0", "id": 23, "method": "textDocument/references", "params": {
    "textDocument": {"uri": app_uri}, "position": app_position("demo_helper"),
    "context": {"includeDeclaration": True}}})
references = receive(23)
assert {reference["uri"] for reference in references} == {
    (demo / "src/demo_api.kly").as_uri(), app_uri}
assert len(references) == 2
send({"jsonrpc": "2.0", "id": 24, "method": "textDocument/references", "params": {
    "textDocument": {"uri": app_uri}, "position": app_position("demo_helper"),
    "context": {"includeDeclaration": False}}})
skip_declaration = receive(24)
assert len(skip_declaration) == 1 and skip_declaration[0]["uri"] == app_uri
send({"jsonrpc": "2.0", "id": 25, "method": "textDocument/references", "params": {
    "textDocument": {"uri": app_uri}, "position": app_position("import demo_api", 8),
    "context": {"includeDeclaration": True}}})
imports = receive(25)
assert len(imports) == 1 and imports[0]["uri"] == app_uri
assert imports[0]["range"]["start"]["line"] == 2

# Built-in annotations are implicitly available from std.annotation, and
# ordinary std functions resolve even when the project omits a kstd dependency.
std_source = """module app.std_use;
import std.math.integer;
annotation local();
@local fn declared() -> i64 { return 1; }
@cfg(os="linux") fn platform() -> i64 {
  return std.math.integer.abs_i64(-4);
}
@std.annotation.main fn entry() {}
"""
std_uri = (workspace / "app/src/std_use.kly").as_uri()
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": std_uri, "languageId": "kelyra", "version": 1,
                     "text": std_source}}})
annotation_uri = (pathlib.Path(__file__).resolve().parents[3] /
                  "kstd/src/std/annotation.kly").as_uri()
send({"jsonrpc": "2.0", "id": 31, "method": "textDocument/documentSymbol", "params": {
    "textDocument": {"uri": annotation_uri}}})
assert "cfg" in {symbol["name"] for symbol in receive(31)}
annotation_source = pathlib.Path(annotation_uri.removeprefix("file://")).read_text()
send({"jsonrpc": "2.0", "id": 32, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": annotation_uri},
    "position": position_in(annotation_source, "cfg(", 1)}})
assert receive(32), "indexed annotation has no symbol"
for request_id, needle, expected_suffix in [
    (26, "@local", "/app/src/std_use.kly"),
    (27, "@cfg", "/std/annotation.kly"),
    (28, "abs_i64(-4)", "/std/math/integer.kly"),
    (33, "@std.annotation.main", "/std/annotation.kly"),
]:
    send({"jsonrpc": "2.0", "id": request_id,
          "method": "textDocument/definition", "params": {
              "textDocument": {"uri": std_uri},
              "position": position_in(std_source, needle,
                                       16 if needle == "@std.annotation.main"
                                       else 3 if needle == "abs_i64(-4)" else 2)}})
    results = receive(request_id)
    assert results, (request_id, needle)
    target = results[0]
    assert target["uri"].endswith(expected_suffix), target
send({"jsonrpc": "2.0", "id": 29, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": std_uri},
    "position": position_in(std_source, "abs_i64(-4)", 3)}})
std_hover = receive(29)
assert std_hover["range"]["start"]["line"] == 5
assert std_hover["range"]["start"]["character"] == position_in(
    std_source, "abs_i64(-4)", 0)["character"]

io_path = pathlib.Path(__file__).resolve().parents[3] / "kstd/src/std/io.kly"
io_source = io_path.read_text()
send({"jsonrpc": "2.0", "id": 34, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": io_path.as_uri()},
    "position": position_in(io_source, "std.io.linux.read_stdin", 16)}})
assert receive(34)[0]["uri"].endswith("/std/io/linux.kly")

unimported = "fn unknown() -> i64 { return abs_i64(-4); }\n"
unimported_uri = (workspace / "app/src/unimported.kly").as_uri()
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": unimported_uri, "languageId": "kelyra", "version": 1,
                     "text": unimported}}})
send({"jsonrpc": "2.0", "id": 30, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": unimported_uri},
    "position": position_in(unimported, "abs_i64", 3)}})
assert receive(30) == []

send({"jsonrpc": "2.0", "id": 6, "method": "shutdown", "params": None})
receive(6)
send({"jsonrpc": "2.0", "method": "exit", "params": None})
process.stdin.close()
assert process.wait(timeout=5) == 0, process.stderr.read().decode()
shutil.rmtree(fixture, ignore_errors=True)
