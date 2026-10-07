"""Entry point of the MCP bridge (Docs/Architecture.md §13.8), registered for Claude Code in the .mcp.json that
Scripts/Setup.py generates: {"command": "<repo>/Tools/MCP/.venv/<Scripts|bin>/python", "args":
["<repo>/Tools/MCP/run.py"]}.

run.py also works when another interpreter starts it (`py -3 Tools/MCP/run.py`, any sys.executable): it locates
Tools/MCP/.venv and re-executes itself inside it with the same arguments and standard streams, so no `python` on PATH is
assumed. Without the virtual environment it exits with code 3 and "run python Scripts/Setup.py" on stderr. Inside the
environment it starts engine_mcp.server on stdio, or, with --print-interpreter (a diagnostic the bridge's tests use),
prints sys.executable and sys.prefix on two lines and exits with 0.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

MCP_ROOT = Path(__file__).resolve().parent
VENV_DIRECTORY = MCP_ROOT / ".venv"
EXIT_SETUP_MISSING = 3
EXIT_USAGE = 2
# Set for the re-executed child, so an environment that does not report itself as the virtual environment cannot make
# run.py re-execute forever.
REEXEC_ENVIRONMENT_VARIABLE = "ENGINE_MCP_REEXECUTED"
PRINT_INTERPRETER_OPTION = "--print-interpreter"


def venv_interpreter() -> Path:
    """The virtual environment's interpreter: .venv/Scripts/python.exe on Windows, .venv/bin/python elsewhere."""
    return VENV_DIRECTORY / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


def is_running_in_venv() -> bool:
    """Whether this interpreter is the virtual environment's (sys.prefix is VENV_DIRECTORY, compared resolved)."""
    try:
        return os.path.normcase(str(Path(sys.prefix).resolve())) == os.path.normcase(str(VENV_DIRECTORY.resolve()))
    except OSError:
        return False


def reexec_in_venv(arguments: list[str]) -> int:
    """Runs venv_interpreter() with this script and `arguments`, standard streams inherited (subprocess with an argument
    list; os.exec* does not replace the process on Windows), and returns its exit code."""
    environment = dict(os.environ)
    environment[REEXEC_ENVIRONMENT_VARIABLE] = "1"
    try:
        return subprocess.run([str(venv_interpreter()), str(Path(__file__).resolve()), *arguments], env=environment,
                              check=False).returncode
    except KeyboardInterrupt:
        return 130


def main() -> int:
    """Re-executes into the virtual environment when needed, otherwise serves MCP on stdio."""
    arguments = sys.argv[1:]
    unknown = [argument for argument in arguments if argument != PRINT_INTERPRETER_OPTION]
    if unknown:
        print(f"run.py: unknown argument(s): {' '.join(unknown)} (only {PRINT_INTERPRETER_OPTION})", file=sys.stderr)
        return EXIT_USAGE
    if not is_running_in_venv():
        if not venv_interpreter().is_file():
            print(f"run.py: the MCP bridge's virtual environment {VENV_DIRECTORY} is missing: run python "
                  f"Scripts/Setup.py", file=sys.stderr)
            return EXIT_SETUP_MISSING
        if os.environ.get(REEXEC_ENVIRONMENT_VARIABLE):
            print(f"run.py: {venv_interpreter()} does not run inside {VENV_DIRECTORY} (sys.prefix is {sys.prefix}): "
                  f"run python Scripts/Setup.py to recreate it", file=sys.stderr)
            return EXIT_SETUP_MISSING
        return reexec_in_venv(arguments)
    if PRINT_INTERPRETER_OPTION in arguments:
        print(sys.executable)
        print(sys.prefix)
        return 0
    sys.path.insert(0, str(MCP_ROOT))
    from engine_mcp import server

    return server.main()


if __name__ == "__main__":
    sys.exit(main())
