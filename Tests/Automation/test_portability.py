"""The portability stage's checks of the Runtime's generated link settings (Docs/Architecture.md §14.1, §15.8, §16;
Docs/Decisions/0024-m15-contract.md decision 18, stream D), over workspaces premake generates into a temporary directory
and over copies with one setting removed. These tests need no build.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

from Lib import paths, portability, premake  # noqa: E402  (Scripts/ is on sys.path above)

TARGETS = (("windows", "vs2026"), ("linux", "gmake"), ("linux", "ninja"), ("macosx", "xcode4"))


def generate(directory: Path) -> dict[tuple[str, str], Path]:
    """Every portability target generated below `directory` with the pinned premake."""
    generated: dict[tuple[str, str], Path] = {}
    for system, action in TARGETS:
        location = directory / f"{system}-{action}"
        subprocess.run([str(premake.find_premake()), "--fatal", f"--os={system}", f"--to={location}", action],
                       cwd=paths.REPOSITORY_ROOT, capture_output=True, check=True, timeout=300.0)
        generated[(system, action)] = location
    return generated


class RuntimeLinkSettingsTests(unittest.TestCase):
    @unittest.skip("contract stub: portability (ADR 0024 decision 18, stream D)")
    def test_the_generated_workspaces_pass_and_each_removed_setting_is_named(self) -> None:
        with tempfile.TemporaryDirectory(prefix="Portability-") as directory:
            generated = generate(Path(directory))
            self.assertEqual(portability.check_runtime_link_settings(generated), [])

            gmake = generated[("linux", "gmake")] / "Runtime.make"
            gmake.write_text(gmake.read_text(encoding="utf-8").replace("-Wl,-rpath,'$$ORIGIN'", ""), encoding="utf-8")
            problems = portability.check_runtime_link_settings(generated)
            self.assertEqual(len(problems), 3, problems)  # Debug, Release and Dist
            self.assertTrue(all(problem.startswith("linux-gmake ") and "ORIGIN" in problem for problem in problems))

            missing = dict(generated)
            shutil.rmtree(missing.pop(("macosx", "xcode4")))
            self.assertTrue(any("macosx-xcode4" in problem for problem in portability.check_runtime_link_settings(missing)))


if __name__ == "__main__":
    unittest.main()
