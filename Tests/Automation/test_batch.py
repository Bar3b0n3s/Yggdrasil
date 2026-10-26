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

    def test_ref_names_an_earlier_op(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 8 (A-NREF; ADR 0027 decision 12's deferral): an op's name stands
        # for its index in a later op's $ref, so inserting an op does not shift the reference; a duplicate name refuses
        # the batch before any op runs, at the op's name.
        client, _ = self.open_editor_with_scene()
        result = client.call("edit.batch", {"label": "Scaffold", "ops": [
            {"method": "entity.create", "params": {"name": "Game"}, "name": "game"},
            {"method": "entity.create", "params": {"name": "Hud"}},
            {"method": "entity.create", "params": {"name": "Board", "parent": {"$ref": "game.entity.id"}}},
        ]})
        self.assertEqual(result["results"][2]["entity"]["path"], "/Game/Board")
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("edit.batch", {"ops": [
                {"method": "entity.create", "params": {"name": "A"}, "name": "twin"},
                {"method": "entity.create", "params": {"name": "B"}, "name": "twin"},
            ]})
        self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)
        self.assertEqual(raised.exception.data["failedOp"], 1)
        self.assertEqual(raised.exception.data["issues"][0]["pointer"], "/ops/1/name")

    def test_component_data_takes_entity_paths_and_id_prefixes(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 7 (A-EREF; ADR 0027 decision 12's deferral): an Entity value in
        # component data takes the spellings an entity param takes, resolved in the scene the request addresses: here a
        # path to the entity an earlier op of the batch created, then a unique id prefix; a path that names nothing is
        # NotFound and changes nothing.
        client, _ = self.open_editor_with_scene()
        script = "Assets/Scripts/Hud.luau"
        client.call("script.write", {"path": script, "source": """
local Hud = { Fields = { Score = Field.Entity() } }
return Script.Define("Hud", Hud)
"""})
        result = client.call("edit.batch", {"label": "Hud", "ops": [
            {"method": "entity.create", "params": {"name": "Game"}, "name": "game"},
            {"method": "entity.create", "params": {"name": "ScoreText", "parent": {"$ref": "game.entity.id"}}},
            {"method": "entity.create", "params": {"name": "Hud", "components": {
                "Script": {"Scripts": [{"Script": script, "Fields": {"Score": "/Game/ScoreText"}}]}}}},
        ]})
        score = result["results"][1]["entity"]["id"]
        fields = client.call("entity.get", {"entity": "/Hud"})["entity"]["components"]["Script"]["Scripts"][0]["Fields"]
        self.assertEqual(fields["Score"], score)
        updated = client.call("entity.update", {"entity": "/Hud", "components": {
            "Script": {"Scripts": [{"Fields": {"Score": score[:10]}}]}}})
        self.assertEqual(updated["entity"]["components"]["Script"]["Scripts"][0]["Fields"]["Score"], score)
        before = client.call("scene.get")["scene"]
        with self.assertRaises(engine_client.EngineError) as missing:
            client.call("entity.update", {"entity": "/Hud", "components": {
                "Script": {"Scripts": [{"Fields": {"Score": "/Game/Missing"}}]}}})
        self.assert_engine_error(missing.exception, engine_client.NOT_FOUND)
        self.assertEqual(missing.exception.data["issues"][0]["pointer"], "/components/Script/Scripts/0/Fields/Score")
        self.assertEqual(client.call("scene.get")["scene"], before)

    def test_batch_file_scaffolds_a_project_and_names_its_failing_line(self) -> None:
        # Editor --batch <file.jsonl> (§13.12): one request per line, "$ref" naming an earlier line's result by its
        # 1-based line number, exit code 0, or 1 at the first failing line, which stderr names.
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
        entities = {entity["Name"]: entity
                    for entity in read_json(root / "Assets" / "Scenes" / "Main.scene")["Entities"]}
        self.assertEqual(entities["Board"]["Parent"], entities["Game"]["ID"])

        failing = self.directory / "Failing.jsonl"
        failing.write_text(json.dumps(open_project) + "\n"
                           + json.dumps({"method": "scene.open",
                                         "params": {"path": "Assets/Scenes/Missing.scene"}}) + "\n"
                           + json.dumps({"method": "entity.create", "params": {"name": "Never"}}) + "\n",
                           encoding="utf-8")
        exit_code, stderr = self.run_editor(["--headless", "--renderer", "none", "--batch", str(failing)])
        self.assertEqual(exit_code, EXIT_FAILED, stderr)
        self.assertIn("line 2", stderr)
        self.assertNotIn("Never", (root / "Assets" / "Scenes" / "Main.scene").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
