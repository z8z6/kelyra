import pathlib
import subprocess
import sys
import tempfile

compiler, gdb = sys.argv[1:]
source = pathlib.Path(__file__).with_name("debug_locals.kly")
with tempfile.TemporaryDirectory(prefix="kelyra-debug-") as directory:
    program = pathlib.Path(directory) / "debug-locals"
    subprocess.run([compiler, "-O0", "--emit-exe", "-o", str(program), str(source)], check=True)
    commands = [
        "set auto-load off", "set debuginfod enabled off", "set pagination off", "break marker", "run", "up",
        'printf "parameter=%d\\n", parameter',
        'printf "inner=%d\\n", value',
        'printf "pointer=%d\\n", *pointer',
        'printf "array=%d\\n", array[0]',
        'printf "field=%d\\n", object.number',
        'printf "pair=%d,%d\\n", number, ok',
        'printf "evaluate=%d\\n", value + parameter',
        "whatis callback", "info locals", "backtrace",
        "continue", "up", 'printf "outer=%d\\n", value',
        "continue", "up", 'printf "this=%d\\n", this->number',
    ]
    args = [gdb, "--nx", "--batch", str(program)]
    for command in commands:
        args += ["-ex", command]
    result = subprocess.run(args, text=True, capture_output=True, timeout=30)
    output = result.stdout + result.stderr
    if "ptrace: Operation not permitted" in output:
        print("GDB requires ptrace permission", file=sys.stderr)
        sys.exit(77)
    assert result.returncode == 0, output
    for expected in ["parameter=40", "inner=99", "pointer=42", "array=7", "field=9",
                     "pair=8,1", "evaluate=139", "outer=42", "this=9", "inspect", "main", "i32 (*)(i32)"]:
        assert expected in output, (expected, output)
    print(output)
