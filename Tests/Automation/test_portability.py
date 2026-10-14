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


def generate_one(directory: Path, system: str, action: str) -> dict[tuple[str, str], Path]:
    """One portability target generated below `directory` with the pinned premake."""
    location = directory / f"{system}-{action}"
    subprocess.run([str(premake.find_premake()), "--fatal", f"--os={system}", f"--to={location}", action],
                   cwd=paths.REPOSITORY_ROOT, capture_output=True, check=True, timeout=300.0)
    return {(system, action): location}


def generate(directory: Path) -> dict[tuple[str, str], Path]:
    """Every portability target generated below `directory` with the pinned premake."""
    generated: dict[tuple[str, str], Path] = {}
    for system, action in TARGETS:
        generated.update(generate_one(directory, system, action))
    return generated


def edit(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise AssertionError(f"{path.name} does not contain {old!r}")
    path.write_text(text.replace(old, new), encoding="utf-8")


class RuntimeLinkSettingsTests(unittest.TestCase):
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
            self.assertTrue(any("macosx-xcode4" in problem
                                for problem in portability.check_runtime_link_settings(missing)))

    def test_every_other_setting_the_exports_rely_on_is_checked(self) -> None:
        with tempfile.TemporaryDirectory(prefix="Portability-") as directory:
            generated = generate(Path(directory))

            # ninja: the rpath, and the map file of each configuration.
            ninja = generated[("linux", "ninja")] / "Runtime.ninja"
            edit(ninja, "Dist-linux-x86_64/Runtime/Runtime.map", "Dist-linux-x86_64/Runtime/Other.map")
            text = ninja.read_text(encoding="utf-8")
            ninja.write_text(text.replace(" -Wl,-rpath,'$$ORIGIN'", "", 1), encoding="utf-8")
            problems = portability.check_runtime_link_settings(generated)
            self.assertEqual(sorted(problem.split(":")[0] for problem in problems),
                             ["linux-ninja Debug", "linux-ninja Dist"], problems)
            self.assertTrue(any("ORIGIN" in problem for problem in problems))
            self.assertTrue(any("Runtime.map" in problem for problem in problems))
            shutil.rmtree(generated[("linux", "ninja")])
            generated.update(generate_one(Path(directory), "linux", "ninja"))

            # xcode4: the Frameworks rpath, the map, the deployment target and the product type.
            pbxproj = generated[("macosx", "xcode4")] / "Runtime.xcodeproj" / "project.pbxproj"
            edit(pbxproj, '"-Wl,-rpath,@executable_path/../Frameworks",', "")
            edit(pbxproj, "MACOSX_DEPLOYMENT_TARGET = 14.0;", "MACOSX_DEPLOYMENT_TARGET = 13.0;")
            edit(pbxproj, '"com.apple.product-type.tool"', '"com.apple.product-type.application"')
            problems = portability.check_runtime_link_settings(generated)
            self.assertTrue(all(problem.startswith("macosx-xcode4") for problem in problems), problems)
            for expected in ("@executable_path/../Frameworks", "MACOSX_DEPLOYMENT_TARGET is 13.0", "product type"):
                with self.subTest(expected=expected):
                    self.assertTrue(any(expected in problem for problem in problems), problems)
            shutil.rmtree(generated[("macosx", "xcode4")])
            generated.update(generate_one(Path(directory), "macosx", "xcode4"))

            # vs2026: /MAP in every configuration and the Dist subsystem.
            vcxproj = generated[("windows", "vs2026")] / "Runtime.vcxproj"
            edit(vcxproj, "<SubSystem>Windows</SubSystem>", "<SubSystem>Console</SubSystem>")
            text = vcxproj.read_text(encoding="utf-8")
            vcxproj.write_text(text.replace("/MAP %(AdditionalOptions)", "%(AdditionalOptions)", 1), encoding="utf-8")
            problems = portability.check_runtime_link_settings(generated)
            self.assertEqual(len(problems), 2, problems)
            self.assertTrue(any(problem.startswith("windows-vs2026 Debug") and "/MAP" in problem
                                for problem in problems))
            self.assertTrue(any(problem.startswith("windows-vs2026 Dist") and "SubSystem is Console" in problem
                                for problem in problems))


if __name__ == "__main__":
    unittest.main()
