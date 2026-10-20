"""Projects/FeatureTest as a whole (Docs/Decisions/0022-m14-contract.md decision 8): the committed scaffolds, run in the
order and with the seeds of Scaffold/Scaffolds.json from Scaffold/Baseline/, reproduce the project byte for byte. The
lead owns this test."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness import REPOSITORY_ROOT, AutomationTestCase, automation_config, editor_executable

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import Scaffold as scaffold  # noqa: E402  (Scripts/ is on sys.path above)
from Lib.report import Console  # noqa: E402


class ScaffoldManifestTests(unittest.TestCase):
    """Scaffold.py's reading of Scaffold/Scaffolds.json, on manifests in a temporary project; no editor runs."""

    def write_manifest(self, entries: list[dict[str, object]]) -> Path:
        project = Path(tempfile.mkdtemp(prefix="ScaffoldManifest-"))
        self.addCleanup(shutil.rmtree, project, ignore_errors=True)
        (project / "Scaffold").mkdir()
        for entry in entries:
            (project / "Scaffold" / str(entry["File"])).write_text("", encoding="utf-8")
        (project / scaffold.MANIFEST).write_text(json.dumps({"Format": scaffold.MANIFEST_FORMAT,
                                                             "Version": scaffold.MANIFEST_VERSION,
                                                             "Scaffolds": entries}), encoding="utf-8")
        return project

    def test_the_committed_manifest_reads_with_unique_files_and_id_seeds(self) -> None:
        scaffolds = scaffold.read_manifest(scaffold.PROJECT)
        self.assertEqual(len({entry.file for entry in scaffolds}), len(scaffolds))
        self.assertEqual(len({entry.id_seed for entry in scaffolds}), len(scaffolds))

    def test_a_manifest_listing_a_file_twice_is_refused(self) -> None:
        project = self.write_manifest([{"File": "A.jsonl", "IdSeed": 1, "Renderer": "none"},
                                       {"File": "B.jsonl", "IdSeed": 2, "Renderer": "none"},
                                       {"File": "A.jsonl", "IdSeed": 3, "Renderer": "none"}])
        with self.assertRaises(scaffold.ManifestError) as raised:
            scaffold.read_manifest(project)
        message = str(raised.exception)
        for named in ("Scaffolds[0]", "Scaffolds[2]", "A.jsonl"):
            self.assertIn(named, message)
        self.assertNotIn("B.jsonl", message)

    def test_a_manifest_giving_two_scaffolds_one_id_seed_is_refused_naming_both(self) -> None:
        # T-SEEDGUARD (Docs/Decisions/0028-backlog-program.md decision 16): two scaffolds with one IdSeed would draw the
        # same UUID sequence, so the second would create entities and assets with the first one's ids.
        project = self.write_manifest([{"File": "A.jsonl", "IdSeed": 7, "Renderer": "none"},
                                       {"File": "B.jsonl", "IdSeed": 8, "Renderer": "none"},
                                       {"File": "C.jsonl", "IdSeed": 7, "Renderer": "vulkan"}])
        with self.assertRaises(scaffold.ManifestError) as raised:
            scaffold.read_manifest(project)
        message = str(raised.exception)
        for named in ("IdSeed", "7", "Scaffolds[0]", "A.jsonl", "Scaffolds[2]", "C.jsonl"):
            self.assertIn(named, message)
        self.assertNotIn("B.jsonl", message)


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
