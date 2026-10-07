"""run.py re-executes itself inside the virtual environment (Docs/Architecture.md §13.8, Roadmap M4)."""

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

MCP_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MCP_ROOT))

import run  # noqa: E402

# Variables through which a parent interpreter could make a child report the parent's environment (the Windows venv
# launcher's __PYVENV_LAUNCHER__) or load another one; the interpreter outside the venv is started without them.
INHERITED_INTERPRETER_VARIABLES = ("__PYVENV_LAUNCHER__", "VIRTUAL_ENV", "PYTHONHOME", "PYTHONPATH", "PYTHONEXECUTABLE",
                                   run.REEXEC_ENVIRONMENT_VARIABLE)


def read_pyvenv_configuration() -> dict[str, str]:
    """The key = value lines of Tools/MCP/.venv/pyvenv.cfg."""
    values: dict[str, str] = {}
    for line in (run.VENV_DIRECTORY / "pyvenv.cfg").read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator:
            values[key.strip().lower()] = value.strip()
    return values


def base_interpreter() -> Path:
    """The interpreter the virtual environment was created from (pyvenv.cfg: "executable" from Python 3.11, else the
    interpreter in "home"), which runs outside the venv. Raises FileNotFoundError when it cannot be found."""
    configuration = read_pyvenv_configuration()
    if configuration.get("executable"):
        candidates = [Path(configuration["executable"])]
    else:
        home = Path(configuration.get("home", ""))
        version = f"{sys.version_info.major}.{sys.version_info.minor}"
        names = ("python.exe",) if os.name == "nt" else (f"python{version}", "python3", "python")
        candidates = [home / name for name in names]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"no base interpreter among {', '.join(str(path) for path in candidates)} (pyvenv.cfg of "
                            f"{run.VENV_DIRECTORY}); run python Scripts/Setup.py to recreate the venv")


def same_directory(first: str | Path, second: Path) -> bool:
    """Whether two directories are the same: both resolved (a directory is not the symbolic link at issue here; the venv's
    bin/python is) and compared case-insensitively where the host's file names are."""
    return os.path.normcase(str(Path(first).resolve())) == os.path.normcase(str(second.resolve()))


class RunPyTests(unittest.TestCase):
    def test_run_py_reexecs_into_venv(self) -> None:
        # Started by an interpreter outside the virtual environment (the base interpreter the venv was created from stands
        # in for "py -3"), run.py must end up inside the venv. sys.prefix tells the two apart on every host; the
        # executable path does not on POSIX, where .venv/bin/python is a symbolic link to the base interpreter.
        base = base_interpreter()
        environment = {key: value for key, value in os.environ.items() if key not in INHERITED_INTERPRETER_VARIABLES}
        outside = subprocess.run([str(base), "-c", "import sys; print(sys.prefix)"], capture_output=True, text=True,
                                 timeout=120, check=False, env=environment)
        self.assertEqual(outside.returncode, 0, outside.stderr)
        self.assertFalse(same_directory(outside.stdout.strip(), run.VENV_DIRECTORY),
                         f"{base} runs inside the venv (sys.prefix {outside.stdout.strip()})")

        result = subprocess.run([str(base), str(MCP_ROOT / "run.py"), run.PRINT_INTERPRETER_OPTION], capture_output=True,
                                text=True, timeout=120, check=False, env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        executable, prefix = result.stdout.strip().splitlines()
        self.assertTrue(same_directory(prefix, run.VENV_DIRECTORY), f"sys.prefix after the re-exec is {prefix}")
        # Unresolved, so the venv's own interpreter is told apart from the base one it links to on POSIX.
        self.assertEqual(os.path.normcase(os.path.abspath(executable)),
                         os.path.normcase(os.path.abspath(run.venv_interpreter())))

    def test_missing_venv_names_setup(self) -> None:
        self.assertTrue(run.venv_interpreter().is_relative_to(run.VENV_DIRECTORY))
        self.assertEqual(run.EXIT_SETUP_MISSING, 3)


if __name__ == "__main__":
    unittest.main()
