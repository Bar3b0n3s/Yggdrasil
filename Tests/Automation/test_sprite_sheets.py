"""Sprite sheets through automation (Docs/Architecture.md §6.9, §7.4, §13.5 asset.create {type: "SpriteSheet"},
asset.getProperties and asset.setProperties; Docs/Decisions/0029-m18-contract.md decisions 14 to 18): a sheet
created over an imported texture, its frames, pivots, 9-slice borders and clips edited as registry JSON, its texture
as its dependency, and the located refusals of invalid sheets."""

from __future__ import annotations

import unittest

from harness import REPOSITORY_ROOT, AutomationTestCase, engine_client, read_json

# Tests/Data's 16 x 8 RGBA texture (Tests/Data/Generate/MakeTextureFixtures.py).
TEXTURE_SOURCE = REPOSITORY_ROOT / "Tests" / "Data" / "Assets" / "Textures" / "Rgba.png"
SHEET_PATH = "Assets/Sprites/Hero.spritesheet"


def hero_values(texture: str) -> dict[str, object]:
    """Two 8 x 8 frames over `texture`, the second sliced, and a looping clip over both."""
    return {
        "Texture": texture,
        "Frames": [
            {"Name": "Idle0", "Rect": {"X": 0, "Y": 0, "Width": 8, "Height": 8}, "Pivot": [0.5, 1]},
            {"Name": "Panel", "Rect": {"X": 8, "Y": 0, "Width": 8, "Height": 8},
             "Borders": {"Left": 2, "Top": 2, "Right": 2, "Bottom": 2}},
        ],
        "Clips": [{"Name": "Idle", "Frames": [{"Frame": 0, "Ticks": 8}, {"Frame": 1, "Ticks": 8}], "Loop": True}],
    }


class SpriteSheetTests(AutomationTestCase):
    """asset.create, asset.getProperties, asset.setProperties and asset.info for sprite sheets."""

    def import_texture(self, client: engine_client.EngineClient) -> str:
        """Imports Rgba.png into Assets/Textures and returns its id."""
        imported = client.call("asset.import", {"source": str(TEXTURE_SOURCE), "destDir": "Assets/Textures"})
        return imported["asset"]["id"]

    def test_create_edit_and_undo_a_sprite_sheet_with_its_texture_as_dependency(self) -> None:
        client, root = self.open_editor_with_scene()
        texture = self.import_texture(client)
        params = {"type": "SpriteSheet", "path": SHEET_PATH, "values": hero_values(texture)}
        created = client.call("asset.create", params)
        self.assertEqual(created["asset"]["type"], "SpriteSheet")
        self.assertEqual(read_json(root / "Assets" / "Sprites" / "Hero.spritesheet")["Format"], "SpriteSheet")
        self.assertEqual(read_json(root / "Assets" / "Sprites" / "Hero.spritesheet.meta")["Importer"], "SpriteSheet")

        values = client.call("asset.getProperties", {"asset": SHEET_PATH})["values"]
        self.assertEqual(values["Texture"], texture)
        # Every field is reported, defaults included.
        self.assertEqual(values["Frames"][0]["Borders"], {"Left": 0, "Top": 0, "Right": 0, "Bottom": 0})
        self.assertEqual(values["Frames"][1]["Pivot"], [0.5, 0.5])
        self.assertEqual(values["Clips"][0]["Frames"][1], {"Frame": 1, "Ticks": 8})

        # A merge patch replaces an array whole (RFC 7386).
        clips = [{"Name": "Hit", "Frames": [{"Frame": 1, "Ticks": 3}], "Loop": False}]
        patched = client.call("asset.setProperties", {"asset": SHEET_PATH, "values": {"Clips": clips}})
        self.assertEqual(patched["values"]["Clips"], clips)
        self.assertEqual(len(patched["values"]["Frames"]), 2)
        client.call("edit.undo")
        restored = client.call("asset.getProperties", {"asset": SHEET_PATH})["values"]
        self.assertEqual(restored["Clips"][0]["Name"], "Idle")

        # An asset the editor creates imports at its first use; importing the sheet records its texture as its
        # dependency (the importer's Import.Dependencies), which asset.info and the texture's dependents then report.
        client.call("asset.reimport", {"asset": SHEET_PATH})
        info = client.call("asset.info", {"asset": SHEET_PATH})
        self.assertEqual(info["importer"], "SpriteSheet")
        self.assertEqual([dependency["id"] for dependency in info["dependencies"]], [texture])
        self.assertEqual(info["diagnostics"], [])
        listed = client.call("asset.list", {"dir": "Assets/Sprites", "type": "SpriteSheet"})["assets"]
        self.assertEqual([asset["path"] for asset in listed], [SHEET_PATH])
        dependents = client.call("asset.info", {"asset": texture})["dependents"]
        self.assertEqual([dependent["path"] for dependent in dependents], [SHEET_PATH])

    def test_invalid_sprite_sheets_are_refused_at_the_offending_value(self) -> None:
        client, _ = self.open_editor_with_scene()
        texture = self.import_texture(client)
        outside = hero_values(texture)
        outside["Frames"][1]["Rect"]["X"] = 12  # 12 + 8 > 16: past the texture's right edge
        borders = hero_values(texture)
        borders["Frames"][1]["Borders"]["Right"] = 7  # 2 + 7 > 8
        missing_frame = hero_values(texture)
        missing_frame["Clips"][0]["Frames"][1]["Frame"] = 2
        no_ticks = hero_values(texture)
        no_ticks["Clips"][0]["Frames"][0]["Ticks"] = 0
        duplicate = hero_values(texture)
        duplicate["Frames"][1]["Name"] = "Idle0"
        invalid = (
            (outside, "/values/Frames/1/Rect"),
            (borders, "/values/Frames/1/Borders"),
            (missing_frame, "/values/Clips/0/Frames/1/Frame"),
            (no_ticks, "/values/Clips/0/Frames/0/Ticks"),
            (duplicate, "/values/Frames/1/Name"),
        )
        for values, pointer in invalid:
            with self.subTest(pointer=pointer):
                with self.assertRaises(engine_client.EngineError) as raised:
                    client.call("asset.create", {"type": "SpriteSheet", "path": SHEET_PATH, "values": values})
                self.assert_engine_error(raised.exception, engine_client.VALIDATION_FAILED, "Validation")
                self.assertEqual(raised.exception.issues[0]["pointer"], pointer)
        self.assertEqual(client.call("asset.list", {"dir": "Assets", "type": "SpriteSheet"})["assets"], [])

        # The same rules hold for an edit, which leaves the file as it was.
        client.call("asset.create", {"type": "SpriteSheet", "path": SHEET_PATH, "values": hero_values(texture)})
        with self.assertRaises(engine_client.EngineError) as raised:
            wide = {"Frames": [{"Rect": {"X": 0, "Y": 0, "Width": 32, "Height": 8}}], "Clips": []}
            client.call("asset.setProperties", {"asset": SHEET_PATH, "values": wide})
        self.assert_engine_error(raised.exception, engine_client.VALIDATION_FAILED, "Validation")
        self.assertEqual(raised.exception.issues[0]["pointer"], "/values/Frames/0/Rect")
        self.assertEqual(len(client.call("asset.getProperties", {"asset": SHEET_PATH})["values"]["Frames"]), 2)


if __name__ == "__main__":
    unittest.main()
