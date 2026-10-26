"""Scene lifecycle through automation (Docs/Architecture.md §13.5 scene.*, Roadmap M4)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client


class SceneTests(AutomationTestCase):
    def test_scene_open_dirty_requires_save_or_discard(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("scene.new", {"path": "Assets/Scenes/Other.scene", "discardChanges": True})
        client.call("entity.create", {"name": "Unsaved"})
        self.assertTrue(client.call("session.info")["_meta"]["dirty"])

        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("scene.open", {"path": "Assets/Scenes/Main.scene"})
        self.assert_engine_error(refused.exception, engine_client.INVALID_STATE, "InvalidState")
        # §13.5: with a dirty open scene exactly one of save and discardChanges, else InvalidState.
        with self.assertRaises(engine_client.EngineError) as both:
            client.call("scene.open", {"path": "Assets/Scenes/Main.scene", "save": True, "discardChanges": True})
        self.assert_engine_error(both.exception, engine_client.INVALID_STATE, "InvalidState")

        opened = client.call("scene.open", {"path": "Assets/Scenes/Main.scene", "save": True})
        self.assertEqual(opened["scene"]["path"], "Assets/Scenes/Main.scene")
        self.assertIn("Unsaved", (root / "Assets" / "Scenes" / "Other.scene").read_text(encoding="utf-8"))

        client.call("entity.create", {"name": "Dropped"})
        client.call("scene.open", {"path": "Assets/Scenes/Other.scene", "discardChanges": True})
        self.assertNotIn("Dropped", (root / "Assets" / "Scenes" / "Main.scene").read_text(encoding="utf-8"))
        self.assertFalse(client.call("session.info")["_meta"]["dirty"])

    def test_open_scene_methods_and_the_scene_target_refuse_what_they_cannot_address(self) -> None:
        # Docs/Decisions/0031-m20-contract.md decisions 14 and 16: "scene" is a reserved member of the
        # scene-addressing methods, which list it in their schemas (scene.activate requires it, its subject), and any
        # other method refuses it as an unknown member; a target that names no open scene is NotFound; keepOpen keeps
        # every open scene, so it refuses save and discardChanges. Without an open scene, scene.list lists none and
        # scene.close has nothing to close.
        client = self.connect(self.start_editor())
        root = self.create_project(client)
        self.assertEqual(client.call("scene.list")["scenes"], [])
        with self.assertRaises(engine_client.EngineError) as nothing_open:
            client.call("scene.close")
        self.assert_engine_error(nothing_open.exception, engine_client.INVALID_STATE, "InvalidState")

        client.call("scene.new", {"path": "Assets/Scenes/Main.scene"})
        client.call("entity.create", {"name": "Unsaved"})
        discovered = engine_client.load_offloaded(client.call("rpc.discover", {"domain": "scene"}))
        methods = {method["name"]: method for method in discovered["methods"]}
        self.assertEqual(methods["scene.activate"]["params"]["required"], ["scene"])
        self.assertIn("scene", methods["scene.tree"]["params"]["properties"])
        self.assertNotIn("scene", methods["scene.list"]["params"]["properties"])
        self.assertNotIn("scene", methods["scene.open"]["params"]["properties"])

        for method, params, code, error_code in (
                ("scene.activate", {}, engine_client.INVALID_PARAMS, "InvalidArgument"),
                ("scene.activate", {"scene": 0}, engine_client.INVALID_PARAMS, "InvalidArgument"),
                ("scene.activate", {"scene": "Assets/Scenes/Missing.scene"}, engine_client.NOT_FOUND,
                 "NotFound"),
                ("scene.tree", {"scene": 4000}, engine_client.NOT_FOUND, "NotFound"),
                ("asset.list", {"scene": "Assets/Scenes/Main.scene"}, engine_client.INVALID_PARAMS,
                 "InvalidArgument"),
                ("scene.new", {"path": "Assets/Scenes/Second.scene", "keepOpen": True, "save": True},
                 engine_client.INVALID_PARAMS, "InvalidArgument"),
                ("scene.close", {"save": True, "discardChanges": True}, engine_client.INVALID_PARAMS,
                 "InvalidArgument")):
            with self.subTest(method=method, params=params):
                with self.assertRaises(engine_client.EngineError) as refused:
                    client.call(method, params)
                self.assert_engine_error(refused.exception, code, error_code)
        self.assertFalse((root / "Assets" / "Scenes" / "Second.scene").exists())
        self.assertTrue(client.call("session.info")["_meta"]["dirty"])


if __name__ == "__main__":
    unittest.main()
