import json
import subprocess
import sys


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


send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
capabilities = receive(1)["capabilities"]
assert capabilities["hoverProvider"]
assert capabilities["signatureHelpProvider"]["triggerCharacters"] == ["(", ","]
assert capabilities["documentSymbolProvider"]

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

send({"jsonrpc": "2.0", "id": 6, "method": "shutdown", "params": None})
receive(6)
send({"jsonrpc": "2.0", "method": "exit", "params": None})
process.stdin.close()
assert process.wait(timeout=5) == 0, process.stderr.read().decode()
