"""Dry runs (Docs/Architecture.md §13.4 "dryRun", Roadmap M4)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, read_json, engine_client


class DryRunTests(AutomationTestCase):
    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_dry_run_leaves_revision_unchanged(self) -> None:
        client, _ = self.open_editor_with_scene()
        revision = client.call("session.info")["_meta"]["revision"]
        result = client.call("entity.create", {"name": "Ghost", "dryRun": True})
        self.assertTrue(result["dryRun"])
        self.assertEqual(result["entity"]["name"], "Ghost")
        self.assertEqual(result["undoIndex"], 0)
        self.assertEqual(result["_meta"]["revision"], revision)
        self.assertEqual(client.call("scene.query", {"where": {"name": "Ghost"}})["total"], 0)
        self.assertEqual(client.call("edit.history")["entries"], [])

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_dry_run_writes_no_file_event_trash_or_provenance(self) -> None:
        client, root = self.open_editor_with_scene()
        provenance_before = read_json(root / "Automation" / "Provenance.json")
        cursor = client.call("events.read", {"cursor": "end"})["nextCursor"]
        settings = client.call("project.setSettings", {"patch": {"Window": {"Title": "Dry"}}, "dryRun": True})
        self.assertEqual(settings["settings"]["Window"]["Title"], "Dry")
        self.assertNotIn("Dry", (root / f"{root.name}.eproj").read_text(encoding="utf-8"))
        client.call("edit.batch", {"label": "Dry", "dryRun": True,
                                   "ops": [{"method": "entity.create", "params": {"name": "A"}}]})
        self.assertEqual(read_json(root / "Automation" / "Provenance.json"), provenance_before)
        self.assertEqual(client.call("events.read", {"cursor": cursor})["events"], [])
        self.assertFalse((root / "Library" / "Trash").exists())

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_dry_run_rejects_unsupported_op_with_failed_op(self) -> None:
        client, _ = self.open_editor_with_scene()
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("edit.batch", {"label": "Dry", "dryRun": True, "ops": [
                {"method": "entity.create", "params": {"name": "A"}},
                {"method": "entity.get", "params": {"entity": "/A"}},
            ]})
        self.assert_engine_error(raised.exception, engine_client.UNSUPPORTED, "Unsupported")
        self.assertEqual(raised.exception.data["failedOp"], 1)
        self.assertEqual(client.call("scene.query", {"where": {"name": "A"}})["total"], 0)
        with self.assertRaises(engine_client.EngineError) as single:
            client.call("scene.tree", {"dryRun": True})
        self.assert_engine_error(single.exception, engine_client.UNSUPPORTED)


if __name__ == "__main__":
    unittest.main()
