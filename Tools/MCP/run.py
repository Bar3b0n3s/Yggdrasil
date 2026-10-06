"""Entry point of the MCP bridge (Docs/Architecture.md §13.8), registered for Claude Code in the .mcp.json that
Scripts/Setup.py generates: {"command": "<repo>/Tools/MCP/.venv/<Scripts|bin>/python", "args":
["<repo>/Tools/MCP/run.py"]}.

run.py also works when another interpreter starts it (`py -3 Tools/MCP/run.py`, any sys.executable): it locates
Tools/MCP/.venv and re-executes itself inside it with the same arguments and standard streams, so no `python` on PATH is
assumed. Without the virtual environment it exits with code 3 and "run python Scripts/Setup.py" on stderr. Inside the
environment it starts engine_mcp.server on stdio, or, with --print-interpreter (a diagnostic the bridge's tests use),
prints sys.executable and exits with 0.

The functions below are contract stubs (M4 stream D) except where noted.
"""

from __future__ import annotations

import sys
from pathlib import Path

MCP_ROOT = Path(__file__).resolve().parent
VENV_DIRECTORY = MCP_ROOT / ".venv"
EXIT_SETUP_MISSING = 3


def venv_interpreter() -> Path:
    """The virtual environment's interpreter: .venv/Scripts/python.exe on Windows, .venv/bin/python elsewhere."""
    raise NotImplementedError("contract stub: run.venv_interpreter (M4 stream D)")


def is_running_in_venv() -> bool:
    """Whether this interpreter is the virtual environment's (sys.prefix is VENV_DIRECTORY, compared resolved)."""
    raise NotImplementedError("contract stub: run.is_running_in_venv (M4 stream D)")


def reexec_in_venv(arguments: list[str]) -> int:
    """Runs venv_interpreter() with this script and `arguments`, standard streams inherited (subprocess with an argument
    list; os.exec* does not replace the process on Windows), and returns its exit code."""
    raise NotImplementedError("contract stub: run.reexec_in_venv (M4 stream D)")


def main() -> int:
    """Re-executes into the virtual environment when needed, otherwise serves MCP on stdio."""
    raise NotImplementedError("contract stub: run.main (M4 stream D)")


if __name__ == "__main__":
    sys.exit(main())
