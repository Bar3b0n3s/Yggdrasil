"""run.py re-executes itself inside the virtual environment (Docs/Architecture.md §13.8, Roadmap M4)."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

MCP_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MCP_ROOT))

import run  # noqa: E402


class RunPyTests(unittest.TestCase):
    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_run_py_reexecs_into_venv(self) -> None:
        # Started by an interpreter outside the virtual environment (the one running the suite is the venv's own, so the
        # base interpreter it was created from stands in for "py -3"), run.py must end up inside the venv: the bridge's
        # --print-interpreter diagnostic prints sys.executable after the re-exec and exits.
        base = Path(getattr(sys, "_base_executable", sys.executable))
        result = subprocess.run([str(base), str(MCP_ROOT / "run.py"), "--print-interpreter"], capture_output=True,
                                text=True, timeout=120, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(Path(result.stdout.strip()).resolve(), run.venv_interpreter().resolve())

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_missing_venv_names_setup(self) -> None:
        self.assertTrue(run.venv_interpreter().is_relative_to(run.VENV_DIRECTORY))
        self.assertEqual(run.EXIT_SETUP_MISSING, 3)


if __name__ == "__main__":
    unittest.main()
