"""Atomic batches (Docs/Architecture.md §13.4 "Atomic batch", Roadmap M4)."""

from __future__ import annotations

import json
import unittest

from harness import EXIT_FAILED, EXIT_SUCCESS, AutomationTestCase, engine_client, read_json


class BatchTests(AutomationTestCase):
    def test_batch_rollback_reports_failed_op(self) -> None:
        client, _ = self.open_editor_with_scene()
        before = client.call("scene.get")["scene"]
        revision = client.call("session.info")["_meta"]["revision"]
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("edit.batch", {"label": "Broken", "ops": [
                {"method": "entity.create", "params": {"name": "A"}},
                {"method": "entity.create", "params": {"name": "B", "components": {"RigidBody": {"Mass": -1}}}},
            ]})
        self.assertEqual(raised.exception.data["failedOp"], 1)
        self.assertEqual(raised.exception.code, engine_client.INVALID_PARAMS)
        self.assertEqual(client.call("scene.get")["scene"], before)
        self.assertEqual(client.call("edit.history")["entries"], [])
        self.assertGreaterEqual(client.call("session.info")["_meta"]["revision"], revision)

    def test_ref_substitution(self) -> None:
        client, _ = self.open_editor_with_scene()
        result = client.call("edit.batch", {"label": "Scaffold", "ops": [
            {"method": "entity.create", "params": {"name": "Game"}},
            {"method": "entity.create", "params": {"name": "Board", "parent": {"$ref": "0.entity.id"}}},
            {"method": "entity.get", "params": {"entity": {"$ref": "1.entity.id"}}},
        ]})
        game_id = result["results"][0]["entity"]["id"]
        self.assertEqual(result["results"][2]["entity"]["parent"], game_id)
        self.assertEqual(result["results"][1]["entity"]["path"], "/Game/Board")
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("edit.batch", {"ops": [
                {"method": "entity.create", "params": {"name": "Orphan", "parent": {"$ref": "5.entity.id"}}},
            ]})
        self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)

    def test_batch_file_scaffolds_a_project_and_names_its_failing_line(self) -> None:
        # Editor --batch <file.jsonl> (§13.12): one request per line, "$ref" naming an earlier line's result by its 1-based
        # line number, exit code 0, or 1 at the first failing line, which stderr names.
        client = self.connect(self.start_editor())
        root = self.create_project(client)
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS)
        open_project = {"method": "project.open", "params": {"path": str(root / f"{root.name}.eproj")}}
        scaffold = [
            open_project,
            {"method": "scene.new", "params": {"path": "Assets/Scenes/Main.scene"}},
            {"method": "entity.create", "params": {"name": "Game"}},
            {"method": "entity.create", "params": {"name": "Board", "parent": {"$ref": "3.entity.id"}}},
            {"method": "scene.save", "params": {}},
        ]
        batch = self.directory / "Scaffold.jsonl"
        batch.write_text("".join(json.dumps(line) + "\n" for line in scaffold), encoding="utf-8")
        exit_code, stderr = self.run_editor(["--headless", "--renderer", "none", "--batch", str(batch)])
        self.assertEqual(exit_code, EXIT_SUCCESS, stderr)
        entities = {entity["Name"]: entity for entity in read_json(root / "Assets" / "Scenes" / "Main.scene")["Entities"]}
        self.assertEqual(entities["Board"]["Parent"], entities["Game"]["ID"])

        failing = self.directory / "Failing.jsonl"
        failing.write_text(json.dumps(open_project) + "\n"
                           + json.dumps({"method": "scene.open", "params": {"path": "Assets/Scenes/Missing.scene"}}) + "\n"
                           + json.dumps({"method": "entity.create", "params": {"name": "Never"}}) + "\n", encoding="utf-8")
        exit_code, stderr = self.run_editor(["--headless", "--renderer", "none", "--batch", str(failing)])
        self.assertEqual(exit_code, EXIT_FAILED, stderr)
        self.assertIn("line 2", stderr)
        self.assertNotIn("Never", (root / "Assets" / "Scenes" / "Main.scene").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
