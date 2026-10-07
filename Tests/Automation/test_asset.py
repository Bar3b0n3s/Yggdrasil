"""Assets through automation (Docs/Architecture.md §7, §13.5 asset.* and project.refreshAssets, Roadmap M6)."""

from __future__ import annotations

import shutil
import unittest

from harness import REPOSITORY_ROOT, AutomationTestCase, engine_client, read_json

GLTF_FIXTURES = REPOSITORY_ROOT / "Tests" / "Data" / "Assets" / "Gltf"


class AssetTests(AutomationTestCase):
    @unittest.skip("contract stub: un-skipped by M6 stream E")
    def test_asset_create_set_move_delete_undo(self) -> None:
        client, root = self.open_editor_with_scene()
        created = client.call("asset.create", {"type": "material", "path": "Assets/Materials/Red.material",
                                               "values": {"BaseColor": [0.9, 0.15, 0.15, 1], "Roughness": 0.45}})
        asset_id = created["asset"]["id"]
        self.assertEqual(created["asset"]["type"], "Material")
        self.assertTrue((root / "Assets" / "Materials" / "Red.material.meta").is_file())
        self.assertEqual(read_json(root / "Assets" / "Materials" / "Red.material.meta")["Handle"], asset_id)

        changed = client.call("asset.setProperties", {"asset": asset_id, "values": {"Metallic": 1}})
        self.assertEqual(changed["values"]["Metallic"], 1)
        self.assertEqual(read_json(root / "Assets" / "Materials" / "Red.material")["Metallic"], 1)

        moved = client.call("asset.move", {"asset": asset_id, "path": "Assets/Red.material"})
        self.assertEqual(moved["asset"]["id"], asset_id)
        self.assertTrue((root / "Assets" / "Red.material").is_file())
        self.assertFalse((root / "Assets" / "Materials" / "Red.material").exists())

        deleted = client.call("asset.delete", {"asset": asset_id})
        trash = root / deleted["trashDirectory"]
        self.assertTrue((trash / "Assets" / "Red.material").is_file())
        self.assertTrue((trash / "Assets" / "Red.material.meta").is_file())
        self.assertFalse((root / "Assets" / "Red.material").exists())

        # Each mutation was one undo step: undoing all four removes the material again, its first content in between.
        client.call("edit.undo", {"steps": 1})
        self.assertTrue((root / "Assets" / "Red.material").is_file())
        client.call("edit.undo", {"steps": 2})
        restored = read_json(root / "Assets" / "Materials" / "Red.material")
        self.assertEqual(restored["Metallic"], 0)
        client.call("edit.undo", {"steps": 1})
        self.assertFalse((root / "Assets" / "Materials" / "Red.material").exists())
        client.call("edit.redo", {"steps": 4})
        self.assertFalse((root / "Assets" / "Red.material").exists())
        self.assertEqual(client.call("asset.list", {"type": "Material"})["assets"], [])

    @unittest.skip("contract stub: un-skipped by M6 stream E")
    def test_refresh_assets_after_external_write(self) -> None:
        client, root = self.open_editor_with_scene()
        # Written by another program: the registry learns about it through project.refreshAssets, synchronously.
        (root / "Assets" / "External.material").write_text(
            '{"Format": "Material", "Version": 1, "Roughness": 0.3}\n', encoding="utf-8")
        refreshed = client.call("project.refreshAssets")
        self.assertEqual(refreshed["createdMetas"], ["Assets/External.material.meta"])
        self.assertEqual(len(refreshed["added"]), 1)
        properties = client.call("asset.getProperties", {"asset": "Assets/External.material"})
        self.assertAlmostEqual(properties["values"]["Roughness"], 0.3, places=6)

        # A second external edit of the same file: refresh reimports it and the new values are served.
        (root / "Assets" / "External.material").write_text(
            '{"Format": "Material", "Version": 1, "Roughness": 0.7}\n', encoding="utf-8")
        refreshed = client.call("project.refreshAssets")
        self.assertEqual(len(refreshed["changed"]), 1)
        self.assertEqual(refreshed["added"], [])
        properties = client.call("asset.getProperties", {"asset": "Assets/External.material"})
        self.assertAlmostEqual(properties["values"]["Roughness"], 0.7, places=6)

    @unittest.skip("contract stub: un-skipped by M6 stream E")
    def test_import_gltf_copies_dependency_closure(self) -> None:
        client, root = self.open_editor_with_scene()
        # The source lives outside the project with its external buffer and image (§13.2 "Paths").
        source_dir = self.directory / "Downloads"
        shutil.copytree(GLTF_FIXTURES, source_dir)
        imported = client.call("asset.import", {"source": str(source_dir / "Textured.gltf"),
                                                "destDir": "Assets/Models"})
        self.assertEqual(imported["asset"]["type"], "Prefab")
        self.assertEqual(imported["copiedFiles"], [
            "Assets/Models/Textured.bin", "Assets/Models/Textured.bin.meta",
            "Assets/Models/Textured.gltf", "Assets/Models/Textured.gltf.meta",
            "Assets/Models/Textures/Checker.png", "Assets/Models/Textures/Checker.png.meta",
        ])
        # Only the closure is copied, with its relative paths; the dependency metas name the glTF as their owner.
        owner = imported["asset"]["id"]
        self.assertEqual(read_json(root / "Assets" / "Models" / "Textured.bin.meta")["Owner"], owner)
        self.assertEqual(read_json(root / "Assets" / "Models" / "Textures" / "Checker.png.meta")["Type"], "Dependency")
        self.assertFalse((root / "Assets" / "Models" / "Box.gltf").exists())
        info = client.call("asset.info", {"asset": owner})
        self.assertEqual(info["dependencyFiles"], ["Assets/Models/Textured.bin", "Assets/Models/Textures/Checker.png"])
        self.assertTrue(any(sub["type"] == "Mesh" for sub in info["subAssets"]))

        # A glTF whose closure escapes its directory is refused before anything is copied.
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("asset.import", {"source": str(source_dir / "ParentEscape.gltf"), "destDir": "Assets/Escaping"})
        self.assert_engine_error(refused.exception, engine_client.VALIDATION_FAILED, "ImportFailed")
        self.assertFalse((root / "Assets" / "Escaping").exists())

        # Undo removes the copies and their metas.
        client.call("edit.undo")
        self.assertFalse((root / "Assets" / "Models" / "Textured.gltf").exists())

    @unittest.skip("contract stub: un-skipped by M6 stream E")
    def test_asset_list_info_and_import_settings(self) -> None:
        client, root = self.open_editor_with_scene()
        shutil.copy(GLTF_FIXTURES / "Textures" / "Checker.png", root / "Assets" / "Checker.png")
        client.call("project.refreshAssets")
        listed = client.call("asset.list", {"type": "texture"})
        self.assertEqual([asset["path"] for asset in listed["assets"]], ["Assets/Checker.png"])
        settings = client.call("asset.getImportSettings", {"asset": "Assets/Checker.png"})
        self.assertEqual(settings["importer"], "Texture")
        changed = client.call("asset.setImportSettings", {"asset": "Assets/Checker.png",
                                                           "settings": {"Usage": "linear"}})
        self.assertEqual(changed["settings"]["Usage"], "Linear")
        reimported = client.call("asset.reimport", {"asset": "Assets/Checker.png"})
        self.assertEqual(reimported["asset"]["path"], "Assets/Checker.png")
        info = client.call("asset.info", {"asset": "Assets/Checker.png"})
        self.assertEqual(info["settings"]["Usage"], "Linear")
        self.assertIn(info["state"], ("Loaded", "Unloaded"))
        client.call("edit.undo")
        restored = client.call("asset.getImportSettings", {"asset": "Assets/Checker.png"})
        self.assertEqual(restored["settings"]["Usage"], "Color")

    @unittest.skip("contract stub: un-skipped by M6 stream E")
    def test_asset_create_dry_run_writes_nothing(self) -> None:
        client, root = self.open_editor_with_scene()
        result = client.call("asset.create", {"type": "Material", "path": "Assets/Dry.material", "dryRun": True})
        self.assertTrue(result["dryRun"])
        self.assertFalse((root / "Assets" / "Dry.material").exists())
        self.assertFalse((root / "Assets" / "Dry.material.meta").exists())
        self.assertEqual(client.call("asset.list", {"type": "Material"})["assets"], [])


if __name__ == "__main__":
    unittest.main()
