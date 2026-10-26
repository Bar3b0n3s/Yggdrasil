"""Round trips of the M4 methods the scenario tests do not reach (Roadmap M4 "every registered method has at least one
Python test", §15.6 gate 5), each through a headless editor."""

from __future__ import annotations

import json
import math
import unittest

from harness import DEFAULT_PROJECT_NAME, REPOSITORY_ROOT, AutomationTestCase, engine_client


def euler_degrees_to_quaternion(x: float, y: float, z: float) -> tuple[float, float, float, float]:
    """The engine's Euler convention (TransformSystem::QuaternionFromEulerDegrees): q = qY * qX * qZ, returned as
    [x, y, z, w] like the Rotation field."""
    def about(axis: int, degrees: float) -> tuple[float, float, float, float]:
        half = math.radians(degrees) / 2.0
        vector = [0.0, 0.0, 0.0]
        vector[axis] = math.sin(half)
        return (math.cos(half), *vector)  # (w, x, y, z)

    def multiply(a: tuple[float, ...], b: tuple[float, ...]) -> tuple[float, float, float, float]:
        aw, ax, ay, az = a
        bw, bx, by, bz = b
        return (aw * bw - ax * bx - ay * by - az * bz, aw * bx + ax * bw + ay * bz - az * by,
                aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw)

    w, qx, qy, qz = multiply(multiply(about(1, y), about(0, x)), about(2, z))
    return (qx, qy, qz, w)


class MethodTests(AutomationTestCase):
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

    def test_entity_create_sets_virtual_transform_fields(self) -> None:
        client, _ = self.open_editor_with_scene()
        # The batch of Architecture §13.10: a light aimed with Transform.EulerAngles, a virtual field.
        client.call("edit.batch", {"label": "Lighting", "ops": [
            {"method": "entity.create", "params": {"name": "Sun", "components": {
                "Transform": {"EulerAngles": [-50, -30, 0]}, "DirectionalLight": {"Intensity": 3}}}}]})
        sun = client.call("entity.get", {"entity": "/Sun", "components": ["Transform"]})["entity"]
        # Degrees applied Z, then X, then Y (q = qY * qX * qZ), serialized as [x, y, z, w] (§5.1, §5.4).
        expected = euler_degrees_to_quaternion(-50, -30, 0)
        for actual, wanted in zip(sun["components"]["Transform"]["Rotation"], expected, strict=True):
            self.assertAlmostEqual(actual, wanted, places=5)
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("entity.update", {"entity": "/Sun", "components": {"Transform": {"WorldScale": [1, 1, 1]}}})
        self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)
        self.assertEqual(raised.exception.issues[0]["pointer"], "/components/Transform/WorldScale")

    def test_events_read_reports_entity_and_component_events(self) -> None:
        client, _ = self.open_editor_with_scene()
        cursor = client.call("events.read", {"cursor": "end"})["nextCursor"]
        board = client.call("entity.create", {"name": "Board"})["entity"]
        client.call("entity.update", {"entity": board["id"], "components": {"Transform": {"Translation": [1, 2, 3]}}})
        client.call("entity.update", {"entity": board["id"], "name": "Grid"})  # a name has no event type (§4.9)
        client.call("entity.destroy", {"entities": [board["id"]]})
        client.call("edit.undo")
        events = client.call("events.read", {"cursor": cursor})["events"]
        self.assertEqual([(event["type"], event["id"], event["name"]) for event in events],
                         [("EntityCreated", board["id"], "Board"), ("ComponentChanged", board["id"], "Transform"),
                          ("EntityDestroyed", board["id"], ""), ("EntityCreated", board["id"], "Grid")])
        changed = client.call("events.read", {"cursor": cursor, "types": ["ComponentChanged"]})["events"]
        self.assertEqual([event["name"] for event in changed], ["Transform"])

    def test_component_list_and_schema(self) -> None:
        client, _ = self.open_editor_with_scene()
        names = [component["name"] for component in client.call("component.list")["components"]]
        self.assertIn("RigidBody", names)
        schema = client.call("component.schema", {"name": "RigidBody"})
        fields = {field["name"]: field for field in schema["fields"]}
        self.assertEqual(fields["Type"]["enumValues"], ["Static", "Kinematic", "Dynamic"])
        self.assertEqual(fields["Mass"]["unit"], "kg")

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

    def test_undo_and_redo_restore_the_scene_file_byte_for_byte(self) -> None:
        # §15.7: undo/redo byte-equality, through the files the editor writes.
        client, root = self.open_editor_with_scene()
        scene_file = root / "Assets" / "Scenes" / "Main.scene"
        client.call("scene.save")
        original = scene_file.read_bytes()
        game = client.call("entity.create", {"name": "Game", "tags": ["Root"]})["entity"]
        ball = client.call("entity.create", {"name": "Ball", "parent": game["id"],
                                             "components": {"Transform": {"Translation": [1, 2, 3]}}})["entity"]
        client.call("entity.update", {"entity": ball["id"], "components": {"RigidBody": {"Mass": 2}}, "active": False})
        spare = client.call("entity.create", {"name": "Spare"})["entity"]
        client.call("entity.reparent", {"entity": spare["id"], "parent": game["id"], "index": 0})
        client.call("entity.duplicate", {"entities": [ball["id"]]})
        client.call("entity.destroy", {"entities": [spare["id"]]})
        steps = len(client.call("edit.history")["entries"])
        self.assertEqual(steps, 7)
        client.call("scene.save")
        edited = scene_file.read_bytes()
        self.assertNotEqual(edited, original)

        self.assertEqual(client.call("edit.undo", {"steps": steps})["undone"], steps)
        client.call("scene.save")
        self.assertEqual(scene_file.read_bytes(), original)
        self.assertEqual(client.call("edit.redo", {"steps": steps})["redone"], steps)
        client.call("scene.save")
        self.assertEqual(scene_file.read_bytes(), edited)

    def test_scene_diff_against_saved_and_revision(self) -> None:
        client, _ = self.open_editor_with_scene()
        start = client.call("session.info")["_meta"]["revision"]
        client.call("entity.create", {"name": "Board"})
        saved = client.call("scene.diff", {"against": "saved"})
        self.assertEqual([entity["change"] for entity in saved["entities"]], ["Created"])
        since = client.call("scene.diff", {"against": "revision", "revision": start})
        self.assertEqual(since["fromRevision"], start)

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

    def test_project_set_settings_names_a_dropped_member_and_a_strict_editor_refuses_it(self) -> None:
        # Docs/Decisions/0030-m19-contract.md "Integration of W19.2": a member the settings do not have was dropped
        # with a warning nothing returned, so a mistyped key was lost without a word.
        client, root = self.open_editor_with_scene()
        patched = client.call("project.setSettings", {"patch": {"Window": {"Titel": "Tetris", "Width": 800}}})
        self.assertEqual(patched["settings"]["Window"]["Width"], 800)
        self.assertEqual(len(patched["warnings"]), 1, patched["warnings"])
        self.assertTrue(patched["warnings"][0].startswith("/patch/Window/Titel: "), patched["warnings"])
        self.assertIn("'Title'", patched["warnings"][0])
        self.assertNotIn("Titel", (root / f"{DEFAULT_PROJECT_NAME}.eproj").read_text(encoding="utf-8"))
        self.assertEqual(client.call("project.setSettings", {"patch": {"Window": {"Title": "Tetris"}}})["warnings"], [])
        # A mistyped reset (a null member) is named too, although the merge itself would erase it unread.
        reset = client.call("project.setSettings", {"patch": {"Window": {"Titel": None}}})
        self.assertEqual(len(reset["warnings"]), 1, reset["warnings"])
        self.assertTrue(reset["warnings"][0].startswith("/patch/Window/Titel: "), reset["warnings"])
        self.assertEqual(reset["settings"]["Window"]["Title"], "Tetris")

        strict = self.connect(self.start_editor(["--strict"]))
        self.create_project(strict, "StrictProject")
        with self.assertRaises(engine_client.EngineError) as refused:
            strict.call("project.setSettings", {"patch": {"Window": {"Titel": "Tetris", "Width": 800}}})
        self.assert_engine_error(refused.exception, engine_client.VALIDATION_FAILED, "Validation")
        self.assertIn("/patch/Window/Titel", [issue.get("pointer") for issue in refused.exception.issues])
        self.assertNotEqual(strict.call("project.getSettings")["settings"]["Window"]["Width"], 800)

    def test_rpc_discover_matches_the_dumped_catalogue(self) -> None:
        client, _ = self.open_editor_with_scene()
        discovered = {method["name"] for method in engine_client.load_offloaded(client.call("rpc.discover"))["methods"]}
        output = self.directory / "Reference"
        exit_code, stderr = self.run_editor(["--headless", "--renderer", "none", "--dump-reference", str(output)])
        self.assertEqual(exit_code, 0, stderr)
        catalogue = json.loads((output / "Methods.json").read_text(encoding="utf-8"))
        self.assertEqual({method["name"] for method in catalogue["Methods"]},
                         {name for name in discovered if not name.startswith("debug.")})
        committed = (REPOSITORY_ROOT / "Tools" / "MCP" / "catalog.json").read_text(encoding="utf-8")
        self.assertEqual((output / "catalog.json").read_text(encoding="utf-8"), committed)

    def test_docs_get_serves_the_skills(self) -> None:
        client = self.connect(self.start_editor())
        topics = [topic["name"] for topic in client.call("docs.get")["topics"]]
        self.assertIn("skills/add-automation-method", topics)
        skill = client.call("docs.get", {"topic": "skills/add-automation-method"})
        self.assertIn("RegisterMethods.cpp", skill["content"])

    def test_docs_get_serves_sections_pages_and_the_generated_topics(self) -> None:
        # Docs/Decisions/0026-m16-contract.md, friction S1-01: the large reference topics came back offloaded, as one
        # line.
        client = self.connect(self.start_editor())
        topics = {topic["name"]: topic for topic in client.call("docs.get")["topics"]}
        for name in ("reference/Schemas/Scene", "reference/BuiltinAssets", "scripting/Engine.d.luau",
                     "templates/Scripts/Behaviour"):
            self.assertIn(name, topics)
        builtins = client.call("docs.get", {"topic": "reference/BuiltinAssets"})
        self.assertIn("engine://Environments/Studio", builtins["content"])
        text = (REPOSITORY_ROOT / "Docs" / "Reference" / "ScriptAPI.md").read_bytes().decode("utf-8")
        first = client.call("docs.get", {"topic": "reference/ScriptAPI"})
        self.assertNotIn("truncated", first)
        self.assertIn("input", [section["anchor"] for section in first["sections"]])
        joined, page = first["content"], first
        while page["nextOffset"]:
            page = client.call("docs.get", {"topic": "reference/ScriptAPI", "offset": page["nextOffset"]})
            self.assertNotIn("truncated", page)
            joined += page["content"]
        self.assertEqual(joined, text)
        section = client.call("docs.get", {"topic": "reference/ScriptAPI#Input"})
        self.assertTrue(section["content"].startswith("## Input\n"))
        self.assertEqual(section["section"], "input")
        declaration = client.call("docs.get", {"topic": "scripting/Engine.d.luau", "section": "Entity"})
        self.assertIn("declare extern type Entity with", declaration["content"])
        page = client.call("docs.get", {"topic": "reference/Schemas/Scene", "offset": 0, "length": 100})
        self.assertLessEqual(len(page["content"].encode("utf-8")), 100)
        self.assertGreater(page["nextOffset"], 0)
        with self.assertRaises(engine_client.EngineError) as missing:
            client.call("docs.get", {"topic": "reference/ScriptAPI", "section": "no such section"})
        self.assert_engine_error(missing.exception, engine_client.NOT_FOUND, "NotFound")

    def test_docs_get_serves_the_native_module_interface(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 26: a builder reads only what docs.get serves, and a native
        # module's sources include NativeApi.h, so the header is a topic of its own; no other engine header is one.
        client = self.connect(self.start_editor())
        topics = {topic["name"]: topic for topic in client.call("docs.get")["topics"]}
        self.assertIn("native/NativeApi.h", topics)
        header = (REPOSITORY_ROOT / "Engine" / "Source" / "Engine" / "Native" / "NativeApi.h").read_bytes()
        self.assertEqual(topics["native/NativeApi.h"]["bytes"], len(header))
        served = client.call("docs.get", {"topic": "native/NativeApi.h"})
        self.assertEqual(served["path"], "Engine/Source/Engine/Native/NativeApi.h")
        self.assertEqual(served["nextOffset"], 0)
        self.assertEqual(served["content"].encode("utf-8"), header)
        self.assertIn("ENGINE_NATIVE_MODULE", served["content"])
        with self.assertRaises(engine_client.EngineError) as other:
            client.call("docs.get", {"topic": "native/NativeModule.h"})
        self.assert_engine_error(other.exception, engine_client.NOT_FOUND, "NotFound")

    def test_docs_get_lists_sections_with_toc_and_finds_them_by_heading_text(self) -> None:
        # Docs/Decisions/0026-m16-contract.md, friction S2-05: the section list cost a 32 KB first page, and a section
        # asked for by its text resolved through a shared anchor to another heading ("TestSuite" gave Test.Suite).
        client = self.connect(self.start_editor())
        toc = client.call("docs.get", {"topic": "reference/ScriptAPI", "toc": True})
        self.assertEqual(toc["content"], "")
        self.assertNotIn("truncated", toc)
        text = (REPOSITORY_ROOT / "Docs" / "Reference" / "ScriptAPI.md").read_bytes()
        self.assertEqual(toc["totalBytes"], len(text))
        titles = [section["title"] for section in toc["sections"]]
        self.assertEqual(len(titles), len(set(titles)))
        for section in toc["sections"]:
            # The section list's offset and size are the section's bytes, which its text finds.
            found = client.call("docs.get", {"topic": "reference/ScriptAPI", "section": section["title"]})
            self.assertEqual(found["section"], section["anchor"])
            self.assertEqual(found["totalBytes"], section["bytes"])
            self.assertEqual(text[section["offset"]:section["offset"] + len(found["content"].encode("utf-8"))],
                             found["content"].encode("utf-8"))
        expected = {"TestSuite": "## TestSuite (type)\n", "Test.Suite": "### Test.Suite\n",
                    "Script": "## Script\n", "Script (component)": "## Script (component)\n",
                    "TextAlignment": "## TextAlignment (enum)\n"}
        for name, heading in expected.items():
            with self.subTest(section=name):
                found = client.call("docs.get", {"topic": "reference/ScriptAPI", "section": name})
                self.assertTrue(found["content"].startswith(heading), found["content"][:80])
        entity = client.call("docs.get", {"topic": "reference/ScriptAPI", "section": "Entity", "toc": True})
        self.assertIn("Entity.Name", [section["title"] for section in entity["sections"]])
        with self.assertRaises(engine_client.EngineError) as paged:
            client.call("docs.get", {"topic": "reference/ScriptAPI", "toc": True, "offset": 0})
        self.assert_engine_error(paged.exception, engine_client.INVALID_PARAMS)

    def test_session_info_reports_the_open_scene(self) -> None:
        # Friction S2-03: an opened project has no open scene, and session.info (so editor_status) did not say so.
        client, root = self.open_editor_with_scene()
        client.call("scene.save")
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), 0)
        reopened = self.connect(self.start_editor())
        self.assertIsNone(reopened.call("session.info")["scene"])
        reopened.call("project.open", {"path": str(root)})
        self.assertIsNone(reopened.call("session.info")["scene"])
        reopened.call("scene.open", {"path": "Assets/Scenes/Main.scene"})
        scene = reopened.call("session.info")["scene"]
        self.assertEqual((scene["path"], scene["dirty"]), ("Assets/Scenes/Main.scene", False))
        reopened.call("entity.create", {"name": "Board"})
        self.assertTrue(reopened.call("session.info")["scene"]["dirty"])

    def test_session_reports_the_build_id(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 13 (B-BVER): session.hello, session.info and the session file
        # name the build that answers, as semantic-versioning build metadata whose version is the engine's.
        editor = self.start_editor()
        client = self.connect(editor)
        build = client.hello["build"]
        self.assertRegex(build, r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(\+[0-9a-f]{12}(\.dirty)?)?$")
        self.assertEqual(build.split("+", 1)[0], client.hello["engineVersion"])
        self.assertEqual(client.call("session.info")["build"], build)
        self.assertIsNotNone(editor.session)
        session_file = json.loads(editor.session.path.read_text(encoding="utf-8"))
        self.assertEqual(session_file["build"], build)
        self.assertEqual(list(session_file).index("build"), list(session_file).index("engineVersion") + 1)


if __name__ == "__main__":
    unittest.main()
