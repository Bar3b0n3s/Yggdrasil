"""Round trips of the M4 methods the scenario tests do not reach (Roadmap M4 "every registered method has at least one
Python test", §15.6 gate 5), each through a headless editor."""

from __future__ import annotations

import json
import unittest

from harness import REPOSITORY_ROOT, AutomationTestCase, engine_client


class MethodTests(AutomationTestCase):
    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_entity_update_destroy_duplicate_reparent(self) -> None:
        client, _ = self.open_editor_with_scene()
        game = client.call("entity.create", {"name": "Game"})["entity"]
        ball = client.call("entity.create",
                           {"name": "Ball", "components": {"Transform": {"Translation": [0, 2, 0]}}})["entity"]
        updated = client.call("entity.update", {"entity": ball["id"], "name": "Orb", "tags": ["Player"],
                                                "components": {"RigidBody": {"Mass": 2}}})["entity"]
        self.assertEqual(updated["name"], "Orb")
        self.assertEqual(updated["components"]["RigidBody"]["Mass"], 2)
        moved = client.call("entity.reparent",
                            {"entity": ball["id"], "parent": game["id"], "keepWorld": False})["entity"]
        self.assertEqual(moved["path"], "/Game/Orb")
        copies = client.call("entity.duplicate", {"entities": ["/Game"]})["entities"]
        self.assertEqual(copies[0]["name"], "Game")
        destroyed = client.call("entity.destroy", {"entities": [copies[0]["id"]]})["destroyed"]
        self.assertEqual(len(destroyed), 2)
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("entity.update", {"entity": ball["id"], "components": {"RigidBody": {"Mas": 2}}})
        self.assertIn("Mass", raised.exception.issues[0].get("hint", ""))

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_component_list_and_schema(self) -> None:
        client, _ = self.open_editor_with_scene()
        names = [component["name"] for component in client.call("component.list")["components"]]
        self.assertIn("RigidBody", names)
        schema = client.call("component.schema", {"name": "RigidBody"})
        fields = {field["name"]: field for field in schema["fields"]}
        self.assertEqual(fields["Type"]["enumValues"], ["Static", "Kinematic", "Dynamic"])
        self.assertEqual(fields["Mass"]["unit"], "kg")

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_undo_redo_history_and_selection(self) -> None:
        client, _ = self.open_editor_with_scene()
        board = client.call("entity.create", {"name": "Board"})["entity"]
        self.assertEqual(client.call("edit.undo")["undone"], 1)
        self.assertEqual(client.call("edit.redo")["redone"], 1)
        history = client.call("edit.history")
        self.assertEqual(history["entries"][0]["label"], "[agent] Create Entity 'Board'")
        selection = client.call("edit.select", {"entities": [board["id"]]})["selection"]
        self.assertEqual(selection[0]["id"], board["id"])
        self.assertEqual(client.call("edit.getSelection")["selection"], selection)

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_scene_diff_against_saved_and_revision(self) -> None:
        client, _ = self.open_editor_with_scene()
        start = client.call("session.info")["_meta"]["revision"]
        client.call("entity.create", {"name": "Board"})
        saved = client.call("scene.diff", {"against": "saved"})
        self.assertEqual([entity["change"] for entity in saved["entities"]], ["Created"])
        since = client.call("scene.diff", {"against": "revision", "revision": start})
        self.assertEqual(since["fromRevision"], start)

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_project_save_open_info_and_shutdown(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Board"})
        self.assertEqual(client.call("project.save")["savedFiles"], ["Assets/Scenes/Main.scene"])
        info = client.call("project.info")
        self.assertEqual(info["scene"]["path"], "Assets/Scenes/Main.scene")
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), 0)
        reopened = self.connect(self.start_editor())
        opened = reopened.call("project.open", {"path": str(root)})
        self.assertEqual(opened["project"]["root"], root.as_posix())
        self.assertEqual(reopened.call("scene.open", {"path": "Assets/Scenes/Main.scene"})["scene"]["entityCount"], 1)

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_rpc_discover_matches_the_dumped_catalogue(self) -> None:
        client, _ = self.open_editor_with_scene()
        discovered = {method["name"] for method in client.call("rpc.discover")["methods"]}
        output = self.directory / "Reference"
        exit_code, stderr = self.run_editor(["--headless", "--renderer", "none", "--dump-reference", str(output)])
        self.assertEqual(exit_code, 0, stderr)
        catalogue = json.loads((output / "Methods.json").read_text(encoding="utf-8"))
        self.assertEqual({method["name"] for method in catalogue["Methods"]},
                         {name for name in discovered if not name.startswith("debug.")})
        committed = (REPOSITORY_ROOT / "Tools" / "MCP" / "catalog.json").read_text(encoding="utf-8")
        self.assertEqual((output / "catalog.json").read_text(encoding="utf-8"), committed)

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_docs_get_serves_the_skills(self) -> None:
        client = self.connect(self.start_editor())
        topics = [topic["name"] for topic in client.call("docs.get")["topics"]]
        self.assertIn("skills/add-automation-method", topics)
        skill = client.call("docs.get", {"topic": "skills/add-automation-method"})
        self.assertIn("RegisterMethods.cpp", skill["content"])


if __name__ == "__main__":
    unittest.main()
