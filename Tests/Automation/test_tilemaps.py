"""Tilemaps through automation: the Tilemap asset, tilemap.paint and the TilemapCollider (Docs/Decisions/
0032-m21-contract.md decisions 23 to 26; Roadmap M21's tilemaps, R-2D-TILE)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client

SKIP_TILEMAP_ASSET = "contract stub: the Tilemap asset, its importer and asset.create (ADR 0032 decision 23, stream T1)"
SKIP_TILEMAP_PAINT = "contract stub: tilemap.paint and the tile brush (ADR 0032 decision 26, stream T3)"
SKIP_TILEMAP_COLLIDER = "contract stub: the TilemapCollider in the 2D world (ADR 0032 decision 25, stream T2)"

SHEET = {
    "Texture": "engine://Textures/Checker",
    "Frames": [
        {"Name": "Ground", "Rect": {"X": 0, "Y": 0, "Width": 16, "Height": 16}},
        {"Name": "Grass", "Rect": {"X": 16, "Y": 0, "Width": 16, "Height": 16}},
    ],
    "Clips": [],
}


class TilemapTests(AutomationTestCase):
    def create_tilemap(self, client: engine_client.EngineClient) -> None:
        client.call("asset.create", {"type": "SpriteSheet", "path": "Assets/Sheets/Tiles.spritesheet", "values": SHEET})
        client.call("asset.create", {"type": "Tilemap", "path": "Assets/Tilemaps/Level.tilemap", "values": {
            "ChunkSize": 4,
            "Palette": [{"Sheet": "Assets/Sheets/Tiles.spritesheet", "Frame": 0, "Solid": True},
                        {"Sheet": "Assets/Sheets/Tiles.spritesheet", "Frame": 1, "Solid": False}]}})

    @unittest.skip(SKIP_TILEMAP_ASSET)
    def test_a_tilemap_asset_is_created_and_read_through_asset_properties(self) -> None:
        # Decision 23: asset.create {type: Tilemap} writes an empty canonical .tilemap of the given palette, whose
        # sheets are its dependencies; asset.getProperties reads it, and asset.setProperties edits its palette.
        client, _ = self.open_editor_with_scene()
        self.create_tilemap(client)
        info = client.call("asset.info", {"asset": "Assets/Tilemaps/Level.tilemap"})
        self.assertEqual(info["asset"]["type"], "Tilemap")
        values = client.call("asset.getProperties", {"asset": "Assets/Tilemaps/Level.tilemap"})["values"]
        self.assertEqual(values["CellSize"], [1, 1])
        self.assertEqual(values["ChunkSize"], 4)
        self.assertEqual(len(values["Palette"]), 2)
        self.assertEqual(values["Chunks"], [])
        sheet = client.call("asset.info", {"asset": "Assets/Sheets/Tiles.spritesheet"})["asset"]["id"]
        self.assertEqual(values["Palette"][0]["Sheet"], sheet)
        client.call("asset.setProperties", {"asset": "Assets/Tilemaps/Level.tilemap", "values": {"CellSize": [2, 2]}})
        values = client.call("asset.getProperties", {"asset": "Assets/Tilemaps/Level.tilemap"})["values"]
        self.assertEqual(values["CellSize"], [2, 2])

    @unittest.skip(SKIP_TILEMAP_PAINT)
    def test_tilemap_paint_edits_cells_in_one_undo_step(self) -> None:
        # Decision 26: one tilemap.paint is one undo step; its chunks follow its cells.
        client, _ = self.open_editor_with_scene()
        self.create_tilemap(client)
        painted = client.call("tilemap.paint", {"tilemap": "Assets/Tilemaps/Level.tilemap", "cells": [
            {"x": 0, "y": 0, "tile": 1}, {"x": 1, "y": 0, "tile": 1}, {"x": 9, "y": 0, "tile": 2}]})
        self.assertEqual(painted["changed"], 3)
        self.assertEqual(painted["chunks"], 2)
        self.assertGreater(painted["undoIndex"], 0)
        chunks = client.call("asset.getProperties", {"asset": "Assets/Tilemaps/Level.tilemap"})["values"]["Chunks"]
        self.assertEqual([(chunk["X"], chunk["Y"]) for chunk in chunks], [(0, 0), (2, 0)])
        self.assertEqual(chunks[0]["Tiles"][:2], [1, 1])
        self.assertEqual(client.call("edit.undo")["undone"], 1)
        chunks = client.call("asset.getProperties", {"asset": "Assets/Tilemaps/Level.tilemap"})["values"]["Chunks"]
        self.assertEqual(chunks, [])
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("tilemap.paint", {"tilemap": "Assets/Tilemaps/Level.tilemap",
                                          "cells": [{"x": 0, "y": 0, "tile": 3}]})
        self.assertEqual(raised.exception.error_code, "InvalidArgument")
        self.assertEqual(raised.exception.data["location"]["pointer"], "/cells/0/tile")

    @unittest.skip(SKIP_TILEMAP_COLLIDER)
    def test_a_ball_lands_on_a_painted_tilemap_collider(self) -> None:
        # Decisions 24 to 26: a painted ground row drawn by a TilemapRenderer and collided by a TilemapCollider stops a
        # falling ball on its top; a 2D ray hits the tilemap's entity there; the non-solid grass cell lets the ray
        # through.
        client, _ = self.open_editor_with_scene()
        self.create_tilemap(client)
        client.call("tilemap.paint", {"tilemap": "Assets/Tilemaps/Level.tilemap", "cells": [
            {"x": x, "y": 0, "tile": 1} for x in range(-4, 4)] + [{"x": 2, "y": 1, "tile": 2}]})
        client.call("entity.create", {"name": "Ground", "components": {
            "TilemapRenderer": {"Tilemap": "Assets/Tilemaps/Level.tilemap"}, "TilemapCollider": {}}})
        client.call("entity.create", {"name": "Ball", "components": {
            "Transform": {"Translation": [0.5, 4, 0]},
            "RigidBody2D": {}, "CircleCollider2D": {"Radius": 0.25}}})
        client.call("play.start", {"lockstep": True, "mode": "simulate"})
        client.call("play.step", {"ticks": 120})
        ball = client.call("entity.get", {"entity": "/Ball", "target": "play"})["entity"]["components"]
        self.assertAlmostEqual(ball["Transform"]["Translation"][1], 1.25, places=2)
        ray = client.call("physics2d.raycast", {"origin": [2.5, 5], "direction": [0, -1], "maxDistance": 10})
        self.assertEqual(ray["hits"][0]["entity"]["name"], "Ground")
        self.assertAlmostEqual(ray["hits"][0]["point"][1], 1.0, places=3)
        client.call("play.stop")


if __name__ == "__main__":
    unittest.main()
