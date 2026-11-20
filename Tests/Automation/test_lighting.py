"""Baked lighting through automation (Docs/Decisions/0037-m26-contract.md decision 8; Roadmap M26's baked irradiance and
reflection probes): lighting.bake, the editor's Bake Lighting action, writes each probe's data beside the saved scene,
assigns it in one undoable edit, and two bakes write byte-identical files; a bake of hundreds of probes stays below the
allocation warning, and a second adds no allocation (World Tree's brief 21); a playing editor and an editor without a
renderer are refused, the latter tested without a GPU."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client


def probe_data(client: engine_client.EngineClient, path: str, component: str) -> str | None:
    """The Data field of the probe `component` of the entity at `path`."""
    entity = client.call("entity.get", {"entity": path, "components": [component]})["entity"]
    return entity["components"][component]["Data"]


class LightingBakeTests(AutomationTestCase):
    def test_two_bakes_write_byte_identical_probe_files(self) -> None:
        # Decision 8: a bake renders the probes' surroundings, writes Assets/Lighting/<the scene's path below
        # Assets/, without its extension>/<entity id>.irradiance and .hdr (decision 31), imports them and sets each
        # probe's Data in one undo step; a second bake of the unchanged scene changes no file and no component, so it
        # records no undo step.
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene(renderer="vulkan")
        client.call("entity.create", {"name": "Floor",
                                      "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Plane"}}})
        client.call("entity.create", {"name": "Volume", "components": {"IrradianceVolume": {"Size": [4, 2, 4]}}})
        client.call("entity.create", {"name": "Mirror", "components": {"ReflectionProbe": {"Resolution": 32}}})
        client.call("scene.save")

        first = client.call("lighting.bake")
        self.assertEqual(len(first["irradianceVolumes"]), 1)
        self.assertEqual(len(first["reflectionProbes"]), 1)
        volume = first["irradianceVolumes"][0]
        self.assertTrue(volume["path"].startswith("Assets/Lighting/Scenes/Main/"), volume["path"])
        self.assertTrue(volume["path"].endswith(".irradiance"))
        self.assertTrue(first["reflectionProbes"][0]["path"].endswith(".hdr"))
        baked = {probe["path"]: (root / probe["path"]).read_bytes()
                 for probe in first["irradianceVolumes"] + first["reflectionProbes"]}
        data = probe_data(client, "/Volume", "IrradianceVolume")
        self.assertEqual(data, volume["asset"]["id"])

        second = client.call("lighting.bake")
        probes = second["irradianceVolumes"] + second["reflectionProbes"]
        self.assertFalse(any(probe["changed"] for probe in probes))
        for path, content in baked.items():
            self.assertEqual((root / path).read_bytes(), content, path)

        # The second bake recorded nothing, so the undo takes back the first bake's assignment.
        client.call("edit.undo")
        self.assertIsNone(probe_data(client, "/Volume", "IrradianceVolume"))

    def test_a_second_bake_of_hundreds_of_probes_adds_no_device_allocation(self) -> None:
        # World Tree's brief 21: the bake of a 13 x 3 x 13 volume (507 captures of six faces each) ended with 23,521
        # native allocations, which its frame's garbage collection freed only after the views had logged the allocation
        # warning. Each capture now releases what it used before the next, so the bake stays below the warning's 2,000,
        # and the count stats.get reads after a second bake is no higher than after the first.
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("entity.create", {"name": "Floor",
                                      "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Plane"}}})
        client.call("entity.create", {"name": "Volume", "components": {"IrradianceVolume": {
            "Size": [56, 14, 56], "ProbeCountX": 13, "ProbeCountY": 3, "ProbeCountZ": 13}}})
        client.call("scene.save")

        counts: list[int] = []
        for _ in range(2):
            baked = client.call("lighting.bake")
            self.assertEqual(len(baked["irradianceVolumes"]), 1)
            counts.append(client.call("stats.get", {"views": False})["memoryAllocationCount"])
        self.assertGreater(counts[0], 0)
        self.assertLessEqual(counts[1], counts[0])
        self.assertEqual(self.warnings_containing(client, "native memory allocations"), [])

    def test_a_playing_editor_cannot_bake(self) -> None:
        # Decision 8: the probes are baked from the edit scene and stored in it, so a playing editor is refused with a
        # hint, and nothing is written. A rendering editor, so the refusal is the play session's and not the one of an
        # editor without a renderer. (A scene never saved, the other refusal, cannot be reached through automation:
        # scene.new writes its file; the C++ case "LightingBake: an unsaved scene and an editor without a renderer are
        # refused, writing nothing" covers it in process.)
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene(renderer="vulkan")
        client.call("entity.create", {"name": "Volume", "components": {"IrradianceVolume": {"Size": [4, 2, 4]}}})
        client.call("scene.save")
        client.call("play.start", {"lockstep": True})
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("lighting.bake")
        self.assert_engine_error(refused.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertFalse((root / "Assets" / "Lighting").exists())
        client.call("play.stop")
        self.assertIsNone(probe_data(client, "/Volume", "IrradianceVolume"))

    def test_an_editor_without_a_renderer_cannot_bake(self) -> None:
        # Decision 8 (the milestone review): an editor without a renderer (--renderer none) has no probe capture, so a
        # bake of probes within their ranges is refused with a hint, writes nothing and assigns nothing. No GPU is
        # needed, so every run of the suite covers the method.
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Volume", "components": {"IrradianceVolume": {"Size": [4, 2, 4]}}})
        client.call("scene.save")
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("lighting.bake")
        self.assert_engine_error(refused.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertFalse((root / "Assets" / "Lighting").exists())
        self.assertIsNone(probe_data(client, "/Volume", "IrradianceVolume"))


if __name__ == "__main__":
    unittest.main()
