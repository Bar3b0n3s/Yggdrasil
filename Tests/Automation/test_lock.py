"""The single-writer lock and read-only editors (Docs/Architecture.md §4.13, Roadmap M4)."""

from __future__ import annotations

import unittest

from harness import EXIT_INIT_FAILED, AutomationTestCase, engine_client


class LockTests(AutomationTestCase):
    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_second_editor_on_locked_project_exits_3(self) -> None:
        client, root = self.open_editor_with_scene()
        holder = client.call("session.info")["pid"]
        project_file = root / f"{root.name}.eproj"
        exit_code, stderr = self.run_editor(
            ["--headless", "--renderer", "none", "--project", str(project_file), "--frames", "5"])
        self.assertEqual(exit_code, EXIT_INIT_FAILED, stderr)
        self.assertIn(f"locked by process {holder}", stderr)
        self.assertEqual(engine_client.read_lock_holder(root), holder)

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_read_only_editor_denies_mutations(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Saved"})
        client.call("scene.save")
        project_file = root / f"{root.name}.eproj"
        snapshot = {path: path.stat().st_mtime_ns for path in root.rglob("*") if path.is_file()}
        reader = self.connect(self.start_editor(["--read-only"], project=project_file), "reader")
        self.assertTrue(reader.call("session.info")["readOnly"])
        reader.call("scene.open", {"path": "Assets/Scenes/Main.scene"})
        self.assertEqual(reader.call("scene.query", {"where": {"name": "Saved"}})["total"], 1)
        for method, params in (("entity.create", {"name": "X"}), ("scene.save", {}),
                               ("project.setSettings", {"patch": {"Window": {"Title": "X"}}})):
            with self.assertRaises(engine_client.EngineError, msg=method) as raised:
                reader.call(method, params)
            self.assert_engine_error(raised.exception, engine_client.UNAUTHORIZED, "PermissionDenied")
        self.assertTrue(reader.call("entity.create", {"name": "X", "dryRun": True})["dryRun"])
        # A read-only editor writes nothing under the project (§4.13), Library/ included.
        self.assertEqual({path: path.stat().st_mtime_ns for path in root.rglob("*") if path.is_file()}, snapshot)


if __name__ == "__main__":
    unittest.main()
