"""Assets through automation (Docs/Architecture.md §7, §13.5 asset.* and project.refreshAssets, Roadmap M6)."""

from __future__ import annotations

import shutil
import unittest

from harness import REPOSITORY_ROOT, AutomationTestCase, engine_client, read_json

GLTF_FIXTURES = REPOSITORY_ROOT / "Tests" / "Data" / "Assets" / "Gltf"


class AssetTests(AutomationTestCase):
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

    def test_asset_delete_removes_an_emptied_folder_and_refuses_one_that_holds_files(self) -> None:
        # M16 friction S3-07: the folder a trashed recording left behind could not be removed through automation.
        client, root = self.open_editor_with_scene()
        client.call("asset.create", {"type": "Material", "path": "Assets/Tests/Probe/Probe.material"})
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("asset.delete", {"asset": "Assets/Tests/Probe"})
        self.assert_engine_error(refused.exception, engine_client.VALIDATION_FAILED, "Validation")
        message = str(refused.exception.data["issues"][0]["message"])
        self.assertIn("Assets/Tests/Probe/Probe.material", message)
        self.assertIn("delete its assets first", str(refused.exception.data["issues"][0]["hint"]))
        self.assertTrue((root / "Assets" / "Tests" / "Probe" / "Probe.material").is_file())

        client.call("asset.delete", {"asset": "Assets/Tests/Probe/Probe.material"})
        deleted = client.call("asset.delete", {"asset": "Assets/Tests/Probe"})
        self.assertEqual(deleted["removedFolders"], ["Assets/Tests/Probe"])
        self.assertEqual((deleted["asset"]["path"], deleted["asset"]["id"], deleted["trashDirectory"]),
                         ("Assets/Tests/Probe", "", ""))
        self.assertFalse((root / "Assets" / "Tests" / "Probe").exists())
        self.assertTrue((root / "Assets" / "Tests").is_dir())
        client.call("edit.undo", {"steps": 1})
        self.assertTrue((root / "Assets" / "Tests" / "Probe").is_dir())
        client.call("edit.redo", {"steps": 1})
        self.assertFalse((root / "Assets" / "Tests" / "Probe").exists())

    def test_refresh_assets_after_external_write(self) -> None:
        client, root = self.open_editor_with_scene()
        # The scene the harness saved has no .meta until a refresh gives it one (writes never create metas, ADR 0010
        # decision 11): settle that first, so the refresh below reports only the external file.
        self.assertEqual(client.call("project.refreshAssets")["createdMetas"], ["Assets/Scenes/Main.scene.meta"])
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

    def test_import_replace_gives_an_asset_new_bytes_and_keeps_its_handles(self) -> None:
        """World Tree brief 08: asset.import {replace: true} through the protocol, in an editor run with --renderer
        none. The dry run lists what the replace does and writes nothing; the replace writes the new version over the
        imported glTF and the buffer it owns, copies its new image with a dependency meta, keeps the .meta's handle and
        every sub-asset handle whose key remains, and lists the sub-assets kept, added and removed; without replace the
        file is refused as before; one undo restores the previous bytes."""
        client, root = self.open_editor_with_scene()
        source_dir = self.directory / "Downloads"
        shutil.copytree(GLTF_FIXTURES / "Replace", source_dir)
        kit = root / "Assets" / "Kit"
        imported = client.call("asset.import", {"source": str(source_dir / "V1" / "Kit.gltf"), "destDir": "Assets/Kit"})
        handle = imported["asset"]["id"]
        before = {item["path"].split("#", 1)[1]: item["id"]
                  for item in client.call("asset.info", {"asset": handle})["subAssets"]}
        bin_meta = (kit / "Kit.bin.meta").read_bytes()
        params = {"source": str(source_dir / "V2" / "Kit.gltf"), "destDir": "Assets/Kit", "replace": True}

        preview = client.call("asset.import", {**params, "dryRun": True})
        self.assertTrue(preview["dryRun"])
        self.assertEqual((kit / "Kit.gltf").read_bytes(), (source_dir / "V1" / "Kit.gltf").read_bytes())
        self.assertFalse((kit / "Textures" / "Cap.png").exists())

        replaced = client.call("asset.import", params)
        for member in ("asset", "keptSubAssets", "addedSubAssets", "removedSubAssets", "copiedFiles",
                       "unreferencedFiles"):
            self.assertEqual(preview[member], replaced[member], member)
        self.assertEqual(replaced["asset"], {"id": handle, "path": "Assets/Kit/Kit.gltf", "type": "Prefab"})
        kept = {item["key"]: item["id"] for item in replaced["keptSubAssets"]}
        self.assertEqual(sorted(kept), ["animation:0:Wave", "material:0:StripMaterial", "mesh:0:Strip", "skin:0",
                                        "texture:0"])
        self.assertEqual(kept, {key: before[key] for key in kept})
        self.assertEqual([item["key"] for item in replaced["addedSubAssets"]], ["material:1:CapMaterial", "mesh:1:Cap"])
        self.assertEqual(replaced["removedSubAssets"][1], {"key": "mesh:1:Panel", "id": before["mesh:1:Panel"],
                                                          "type": "Mesh"})
        self.assertEqual(replaced["copiedFiles"], ["Assets/Kit/Kit.bin", "Assets/Kit/Kit.gltf",
                                                   "Assets/Kit/Textures/Cap.png", "Assets/Kit/Textures/Cap.png.meta"])
        self.assertEqual(replaced["unreferencedFiles"], ["Assets/Kit/Textures/Panel.png"])
        self.assertEqual((kit / "Kit.gltf").read_bytes(), (source_dir / "V2" / "Kit.gltf").read_bytes())
        self.assertEqual((kit / "Kit.bin").read_bytes(), (source_dir / "V2" / "Kit.bin").read_bytes())
        self.assertEqual((kit / "Kit.bin.meta").read_bytes(), bin_meta)
        self.assertEqual(read_json(kit / "Kit.gltf.meta")["Handle"], handle)
        self.assertEqual(read_json(kit / "Textures" / "Cap.png.meta")["Owner"], handle)
        self.assertTrue((kit / "Textures" / "Panel.png").is_file())

        # Without replace, the existing file is refused as before.
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("asset.import", {"source": str(source_dir / "V2" / "Kit.gltf"), "destDir": "Assets/Kit"})
        self.assert_engine_error(refused.exception, engine_client.CONFLICT, "AlreadyExists")
        self.assertIn("replace: true", str(refused.exception.data["hint"]))

        # One undo restores the previous bytes, and the previous sub-assets come back with their handles.
        client.call("edit.undo")
        self.assertEqual((kit / "Kit.gltf").read_bytes(), (source_dir / "V1" / "Kit.gltf").read_bytes())
        self.assertFalse((kit / "Textures" / "Cap.png").exists())
        restored = {item["path"].split("#", 1)[1]: item["id"]
                    for item in client.call("asset.info", {"asset": "Assets/Kit/Kit.gltf"})["subAssets"]}
        self.assertEqual(restored, before)

    def test_material_uv_sets_and_gltf_attribute_stream(self) -> None:
        """M23 (Docs/Decisions/0034-m23-contract.md decisions 9, 10 and 18): Material version 2's UV sets through
        asset.*, and glTF importer version 2, which imports TEXCOORD_1 and COLOR_0 and warns only for a texture on
        TEXCOORD_2."""
        client, root = self.open_editor_with_scene()
        created = client.call("asset.create", {"type": "Material", "path": "Assets/Materials/Layered.material",
                                               "values": {"OcclusionUVSet": "UV1"}})
        asset_id = created["asset"]["id"]
        document = read_json(root / "Assets" / "Materials" / "Layered.material")
        # The current Material version: 3 since M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md
        # decision 3H), its UV sets as version 2's.
        self.assertEqual(document["Version"], 3)
        self.assertEqual(document["OcclusionUVSet"], "UV1")
        self.assertEqual(document["BaseColorUVSet"], "UV0")
        changed = client.call("asset.setProperties", {"asset": asset_id, "values": {"BaseColorUVSet": "UV1"}})
        self.assertEqual(changed["values"]["BaseColorUVSet"], "UV1")
        self.assertEqual(read_json(root / "Assets" / "Materials" / "Layered.material")["BaseColorUVSet"], "UV1")
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("asset.setProperties", {"asset": asset_id, "values": {"EmissiveUVSet": "UV2"}})
        self.assert_engine_error(refused.exception, engine_client.VALIDATION_FAILED, "Validation")

        source_dir = self.directory / "Downloads"
        shutil.copytree(GLTF_FIXTURES, source_dir)
        for name in ("VertexColors.gltf", "Attributes.gltf", "TexCoord2Occlusion.gltf"):
            client.call("asset.import", {"source": str(source_dir / name), "destDir": "Assets/Models"})
        codes = {(item["code"], item.get("file")) for item in client.call("project.validate")["diagnostics"]}
        self.assertIn(("ASSET_UNSUPPORTED_UV_SET", "Assets/Models/TexCoord2Occlusion.gltf"), codes)
        self.assertNotIn("ASSET_VERTEX_COLORS_IGNORED", {code for code, _ in codes})
        self.assertFalse(any(code == "ASSET_UNSUPPORTED_UV_SET" and file != "Assets/Models/TexCoord2Occlusion.gltf"
                             for code, file in codes))

    def test_material_maps_take_any_asset_reference(self) -> None:
        """Docs/Decisions/0034-m23-contract.md decision 21 and its Landing: a Material's maps in asset.create's and
        asset.setProperties' values take a handle, a project path or an engine path, written as the handle, through the
        resolver M21 gave a sprite sheet's Texture; a reference that names nothing is refused at the map."""
        client, root = self.open_editor_with_scene()
        shutil.copy(GLTF_FIXTURES / "Textures" / "Checker.png", root / "Assets" / "Checker.png")
        client.call("project.refreshAssets")
        texture = client.call("asset.info", {"asset": "Assets/Checker.png"})["asset"]["id"]
        created = client.call("asset.create", {"type": "Material", "path": "Assets/Materials/Checker.material",
                                               "values": {"BaseColorMap": "Assets/Checker.png",
                                                          "NormalMap": "engine://Textures/FlatNormal"}})
        document = read_json(root / "Assets" / "Materials" / "Checker.material")
        self.assertEqual(document["BaseColorMap"], texture)
        self.assertEqual(document["NormalMap"], "0000000000000183")
        changed = client.call("asset.setProperties", {"asset": created["asset"]["id"],
                                                       "values": {"EmissiveMap": "Assets/Checker.png"}})
        self.assertEqual(changed["values"]["EmissiveMap"], texture)
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("asset.setProperties", {"asset": created["asset"]["id"],
                                                "values": {"OcclusionMap": "Assets/Missing.png"}})
        self.assertEqual(refused.exception.data["errorCode"], "NotFound")
        self.assertEqual(refused.exception.data["location"]["pointer"], "/values/OcclusionMap")

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

    def test_import_settings_of_a_nested_gltf_rewrite_the_prefab_nesting_it(self) -> None:
        # Docs/Decisions/0031-m20-contract.md decision 8: a glTF that the scene holds only nested in a Room instance
        # follows a new Scale (node translations are scaled) through Room.prefab, which the same undo step rewrites;
        # one undo restores the prefab and the room.
        client, root = self.open_editor_with_scene()
        models = root / "Assets" / "Models"
        models.mkdir(parents=True, exist_ok=True)
        shutil.copy(GLTF_FIXTURES / "Hierarchy.gltf", models / "Hierarchy.gltf")
        client.call("project.refreshAssets")
        client.call("entity.create", {"name": "Room"})
        client.call("prefab.instantiate", {"prefab": "Assets/Models/Hierarchy.gltf", "parent": "/Room"})
        client.call("prefab.create", {"entity": "/Room", "path": "Assets/Prefabs/Room.prefab",
                                      "replaceWithInstance": True})
        room_file = root / "Assets" / "Prefabs" / "Room.prefab"
        room_before = room_file.read_text(encoding="utf-8")

        def base_height() -> float:
            base = client.call("entity.get", {"entity": "/Room/Hierarchy/Base", "components": ["Transform"]})
            return base["entity"]["components"]["Transform"]["Translation"][1]

        self.assertAlmostEqual(base_height(), 1.0, places=6)
        client.call("asset.setImportSettings", {"asset": "Assets/Models/Hierarchy.gltf", "settings": {"Scale": 0.5}})
        self.assertNotEqual(room_file.read_text(encoding="utf-8"), room_before)
        self.assertAlmostEqual(base_height(), 0.5, places=6)
        client.call("edit.undo")
        self.assertEqual(room_file.read_text(encoding="utf-8"), room_before)
        self.assertAlmostEqual(base_height(), 1.0, places=6)

    def test_asset_create_dry_run_writes_nothing(self) -> None:
        client, root = self.open_editor_with_scene()
        result = client.call("asset.create", {"type": "Material", "path": "Assets/Dry.material", "dryRun": True})
        self.assertTrue(result["dryRun"])
        self.assertFalse((root / "Assets" / "Dry.material").exists())
        self.assertFalse((root / "Assets" / "Dry.material.meta").exists())
        self.assertEqual(client.call("asset.list", {"type": "Material"})["assets"], [])

    def test_asset_list_lists_the_built_in_assets_below_engine(self) -> None:
        # M17 friction F02: asset.list refused engine://, so a builder could only guess the built-in paths, and an
        # unknown engine path was compared with the project's paths.
        client, _ = self.open_editor_with_scene()
        listed = client.call("asset.list", {"dir": "engine://", "limit": 1000})
        paths = [asset["path"] for asset in listed["assets"]]
        self.assertEqual(paths, sorted(paths))
        self.assertEqual(listed["total"], len(paths))
        self.assertIn("engine://Environments/Studio", paths)
        self.assertIn("engine://Meshes/Cube", paths)
        environments = client.call("asset.list", {"dir": "engine://Environments"})
        self.assertEqual(environments["assets"], [
            {"id": "0000000000000202", "path": "engine://Environments/Sky", "type": "Environment"},
            {"id": "0000000000000201", "path": "engine://Environments/Studio", "type": "Environment"}])
        with self.assertRaises(engine_client.EngineError) as unknown:
            client.call("asset.info", {"asset": "engine://Environments/Default"})
        self.assert_engine_error(unknown.exception, engine_client.NOT_FOUND, "NotFound")
        self.assertIn("'engine://Environments/Studio'", unknown.exception.data["hint"])
        with self.assertRaises(engine_client.EngineError) as other:
            client.call("asset.list", {"dir": "user://Replays"})
        self.assert_engine_error(other.exception, engine_client.INVALID_PARAMS, "InvalidArgument")

    def test_asset_create_of_an_existing_folder_creates_nothing(self) -> None:
        # M17 friction F07: the template's folders made a Folder op fail with AlreadyExists, rolling back its whole
        # batch.
        client, root = self.open_editor_with_scene()
        existing = client.call("asset.create", {"type": "Folder", "path": "Assets/Materials"})
        self.assertEqual((existing["created"], existing["undoIndex"]), (False, 0))
        batch = client.call("edit.batch", {"label": "Materials", "ops": [
            {"method": "asset.create", "params": {"type": "Folder", "path": "Assets/Materials"}},
            {"method": "asset.create", "params": {"type": "Material", "path": "Assets/Materials/Red.material"}}]})
        self.assertEqual([result["created"] for result in batch["results"]], [False, True])
        self.assertTrue((root / "Assets" / "Materials" / "Red.material").is_file())
        created = client.call("asset.create", {"type": "Folder", "path": "Assets/Levels"})
        self.assertTrue(created["created"])
        self.assertTrue((root / "Assets" / "Levels").is_dir())
        with self.assertRaises(engine_client.EngineError) as file:
            client.call("asset.create", {"type": "Folder", "path": "Assets/Materials/Red.material"})
        self.assert_engine_error(file.exception, engine_client.CONFLICT, "AlreadyExists")
        self.assertTrue((root / "Assets" / "Materials" / "Red.material").is_file())


if __name__ == "__main__":
    unittest.main()
