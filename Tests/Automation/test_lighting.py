"""Baked lighting through automation (Docs/Decisions/0037-m26-contract.md decision 8; Roadmap M26's baked irradiance and
reflection probes): lighting.bake, the editor's Bake Lighting action, writes each probe's data beside the saved scene,
assigns it in one undoable edit, and two bakes write byte-identical files; a playing editor and an editor without a
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
