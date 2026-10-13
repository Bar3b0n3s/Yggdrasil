"""Projects/FeatureTest as a whole (Docs/Decisions/0022-m14-contract.md decision 8): the committed scaffolds, run in the
order and with the seeds of Scaffold/Scaffolds.json from Scaffold/Baseline/, reproduce the project byte for byte. The
lead owns this test."""

from __future__ import annotations

import subprocess
import sys
import unittest

from harness import REPOSITORY_ROOT, AutomationTestCase, automation_config, editor_executable

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import Scaffold as scaffold  # noqa: E402  (Scripts/ is on sys.path above)
from Lib.report import Console  # noqa: E402


class FeatureTestScaffoldTests(AutomationTestCase):
    def test_scaffolds_reproduce_the_project_byte_for_byte(self) -> None:
        # Formats.jsonl imports an .hdr, which needs the GPU environment baker.
        if not self.require_gpu():
            return
        process = subprocess.run(
            [sys.executable, str(REPOSITORY_ROOT / "Scripts" / "Scaffold.py"), "--check",
             "--config", automation_config()],
            capture_output=True, text=True, timeout=1800.0, check=False)
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)

    def test_scaffold_refuses_a_project_an_editor_holds_before_removing_anything(self) -> None:
        # Review finding M04: regeneration in place removed every scaffold output under Assets/ before it reached the
        # editor's Library/Editor.lock (on Linux and macOS the advisory flock never stopped it at all). It now probes
        # the lock first, on every OS, and removes nothing while an editor holds it.
        client, root = self.open_editor_with_scene()
        client.call("scene.save")
        scene = root / "Assets" / "Scenes" / "Main.scene"
        self.assertTrue(scene.is_file())
        self.assertTrue(scaffold.project_lock_held(root))
        steps = scaffold.regenerate(editor_executable(), root, [], Console(True))
        self.assertEqual([step.name for step in steps], ["clean"])
        self.assertTrue(steps[0].failed)
        self.assertIn("Editor.lock", steps[0].detail)
        self.assertTrue(scene.is_file(), "nothing under Assets/ is removed while the editor holds the project")
        self.assertTrue((root / "Library").is_dir())
        client.call("session.shutdown", {"force": True})
        self.assertEqual(self.editors[-1].wait(), 0)
        self.assertFalse(scaffold.project_lock_held(root))


if __name__ == "__main__":
    unittest.main()
