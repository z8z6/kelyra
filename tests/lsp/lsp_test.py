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
assert receive(1)["capabilities"]["hoverProvider"]

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

send({"jsonrpc": "2.0", "id": 6, "method": "shutdown", "params": None})
receive(6)
send({"jsonrpc": "2.0", "method": "exit", "params": None})
process.stdin.close()
assert process.wait(timeout=5) == 0, process.stderr.read().decode()
