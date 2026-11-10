"""Tilemaps through automation: the Tilemap asset, tilemap.paint and the TilemapCollider (Docs/Decisions/
0032-m21-contract.md decisions 23 to 26; Roadmap M21's tilemaps, R-2D-TILE)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client

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

    def test_a_tilemap_asset_is_created_and_read_through_asset_properties(self) -> None:
        # Decision 23: asset.create {type: Tilemap} writes an empty canonical .tilemap of the given palette, whose
        # sheets are its dependencies once imported; asset.getProperties reads it, and asset.setProperties edits its
        # cell size and palette and, W21.2's review, changes a painted map's chunk size alone, keeping every cell.
        client, _ = self.open_editor_with_scene()
        self.create_tilemap(client)
        sheet = client.call("asset.info", {"asset": "Assets/Sheets/Tiles.spritesheet"})["asset"]["id"]
        client.call("asset.reimport", {"asset": "Assets/Tilemaps/Level.tilemap"})
        info = client.call("asset.info", {"asset": "Assets/Tilemaps/Level.tilemap"})
        self.assertEqual(info["asset"]["type"], "Tilemap")
        self.assertEqual([dependency["id"] for dependency in info["dependencies"]], [sheet])
        values = client.call("asset.getProperties", {"asset": "Assets/Tilemaps/Level.tilemap"})["values"]
        self.assertEqual(values["CellSize"], [1, 1])
        self.assertEqual(values["ChunkSize"], 4)
        self.assertEqual(len(values["Palette"]), 2)
        self.assertEqual(values["Chunks"], [])
        self.assertEqual(values["Palette"][0]["Sheet"], sheet)
        client.call("asset.setProperties", {"asset": "Assets/Tilemaps/Level.tilemap", "values": {
            "CellSize": [2, 2],
            "Palette": [{"Sheet": "Assets/Sheets/Tiles.spritesheet", "Frame": 0, "Solid": True},
                        {"Sheet": "Assets/Sheets/Tiles.spritesheet", "Frame": 1, "Solid": True}]}})
        values = client.call("asset.getProperties", {"asset": "Assets/Tilemaps/Level.tilemap"})["values"]
        self.assertEqual(values["CellSize"], [2, 2])
        self.assertEqual([entry["Solid"] for entry in values["Palette"]], [True, True])

        # A new ChunkSize alone lays the painted cells in chunks of that size: each cell keeps its value, and the
        # chunks are those holding a cell, sorted by Y, then X.
        cells = [(-1, -1, 1), (0, 0, 2), (3, 0, 1), (4, 5, 2)]
        client.call("tilemap.paint", {"tilemap": "Assets/Tilemaps/Level.tilemap",
                                      "cells": [{"x": x, "y": y, "tile": tile} for x, y, tile in cells]})
        values = client.call("asset.setProperties", {"asset": "Assets/Tilemaps/Level.tilemap",
                                                     "values": {"ChunkSize": 2}})["values"]
        self.assertEqual(values["ChunkSize"], 2)
        chunks = {(chunk["X"], chunk["Y"]): chunk["Tiles"] for chunk in values["Chunks"]}
        for x, y, tile in cells:
            self.assertEqual(chunks[(x // 2, y // 2)][(y % 2) * 2 + x % 2], tile)
        self.assertEqual(sum(1 for tiles in chunks.values() for value in tiles if value != 0), len(cells))
        self.assertEqual([(chunk["X"], chunk["Y"]) for chunk in values["Chunks"]],
                         sorted({(x // 2, y // 2) for x, y, _ in cells}, key=lambda chunk: (chunk[1], chunk[0])))

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

    def test_viewport_pick_reports_a_tile_as_its_tilemap_entity(self) -> None:
        # Decision 24: viewport.pick picks a tile as its tilemap's entity in the one 2D draw order: a bare tile reports
        # the tilemap, a sprite in an earlier sorting layer stays under its tile, and one in a later layer drawn over a
        # tile is reported instead.
        client, _ = self.open_editor_with_scene()
        self.create_tilemap(client)
        client.call("project.setSettings",
                    {"patch": {"SortingLayers": {"Layers": ["Default", "Ground", "Foreground"]}}})
        client.call("tilemap.paint", {"tilemap": "Assets/Tilemaps/Level.tilemap",
                                      "cells": [{"x": x, "y": 0, "tile": 1} for x in range(0, 3)]})
        client.call("entity.create", {"name": "Ground", "components": {
            "TilemapRenderer": {"Tilemap": "Assets/Tilemaps/Level.tilemap", "PixelsPerUnit": 16,
                                "SortingLayer": "Ground"}}})
        client.call("entity.create", {"name": "Behind", "components": {
            "Transform": {"Translation": [1.5, 0.5, 0]}, "SpriteRenderer": {"SortingLayer": "Default"}}})
        client.call("entity.create", {"name": "Marker", "components": {
            "Transform": {"Translation": [2.5, 0.5, 0]}, "SpriteRenderer": {"SortingLayer": "Foreground"}}})
        client.call("viewport.setOptions", {"mode2D": True})
        for centre, name in (([0.5, 0.5], "Ground"), ([1.5, 0.5], "Ground"), ([2.5, 0.5], "Marker")):
            with self.subTest(centre=centre):
                client.call("viewport.camera",
                            {"position": [*centre, 500], "target": [*centre, 0], "orthographicSize": 2})
                picked = client.call("viewport.pick", {"x": 320, "y": 180, "view": "scene"})
                self.assertTrue(picked["raycast"]["hit"])
                self.assertEqual(picked["raycast"]["entity"]["name"], name)

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
        # On the ground row's top at y = 1 (radius 0.25), within Jolt's penetration slop (0.02), as test_physics' ball.
        self.assertAlmostEqual(ball["Transform"]["Translation"][1], 1.25, delta=0.03)
        ray = client.call("physics2d.raycast", {"origin": [2.5, 5], "direction": [0, -1], "maxDistance": 10})
        self.assertEqual(ray["hits"][0]["entity"]["name"], "Ground")
        self.assertAlmostEqual(ray["hits"][0]["point"][1], 1.0, places=3)
        client.call("play.stop")


if __name__ == "__main__":
    unittest.main()
