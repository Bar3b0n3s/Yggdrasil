"""Skeletal and morph animation through automation (Docs/Decisions/0034-m23-contract.md decisions 25 to 32; Roadmap
M23's skeletal and morph animation with script-driven blending): the editor's animation preview, animation.preview, its
one action; and M41's bone attachments (Docs/Decisions/0058-m41-bone-attachments.md decision 3), which go through the
generic entity methods."""

from __future__ import annotations

import shutil
import time
import unittest
from pathlib import Path
from typing import Any

from harness import REPOSITORY_ROOT, AutomationTestCase, engine_client

GLTF_FIXTURES = REPOSITORY_ROOT / "Tests" / "Data" / "Assets" / "Gltf"
MAIN = "Assets/Scenes/Main.scene"
SECOND = "Assets/Scenes/Second.scene"
BODY = "/Skinned/Armature/Body"
# How long a test waits for the editor's frame to end a preview after a tab switch (a bound on a failure only).
FRAME_WAIT_SECONDS = 30.0


def import_skinned_fixture(client: engine_client.EngineClient, root: Path) -> None:
    """Copies the skinned glTF fixture into the project at `root` and imports it."""
    models = root / "Assets" / "Models"
    models.mkdir(parents=True, exist_ok=True)
    shutil.copy(GLTF_FIXTURES / "Skinned.gltf", models / "Skinned.gltf")
    client.call("project.refreshAssets")


def body_bounds(client: engine_client.EngineClient, scene: str) -> dict[str, Any]:
    """The world box of the skinned body in `scene` (entity.bounds' min and max): a skinned mesh is bounded by its pose,
    so it shows a preview. The two scenes' bodies are different entities, so the box leaves out the entity."""
    bounds = client.call("entity.bounds", {"entities": [BODY], "scene": scene})["bounds"][0]
    return {"min": bounds["min"], "max": bounds["max"]}


class AnimationPreviewTests(AutomationTestCase):
    def test_animation_preview_poses_the_edit_scene_without_changing_it(self) -> None:
        # Decision 32: the preview poses the Animator in the edit scene without an undo step, a revision or a file
        # change, keeps its time and playing flag between calls, ends with stop, and refuses an entity without an
        # Animator and a time below 0.
        client, root = self.open_editor_with_scene()
        import_skinned_fixture(client, root)
        client.call("prefab.instantiate", {"prefab": "Assets/Models/Skinned.gltf"})
        before = client.call("scene.tree")

        previewed = client.call("animation.preview", {"entity": "/Skinned/Armature", "time": 0.5})
        self.assertTrue(previewed["previewing"])
        self.assertAlmostEqual(previewed["time"], 0.5, places=6)
        self.assertFalse(previewed["playing"])
        self.assertGreater(previewed["duration"], 0.0)
        self.assertEqual(previewed["entity"]["path"], "/Skinned/Armature")
        kept = client.call("animation.preview", {"entity": "/Skinned/Armature"})
        self.assertAlmostEqual(kept["time"], 0.5, places=6)
        self.assertEqual(client.call("scene.tree"), before)

        stopped = client.call("animation.preview", {"entity": "/Skinned/Armature", "stop": True})
        self.assertFalse(stopped["previewing"])

        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("animation.preview", {"entity": "/Skinned/Armature/Body", "time": 0.0})
        self.assert_engine_error(refused.exception, engine_client.INVALID_PARAMS, "InvalidArgument")
        with self.assertRaises(engine_client.EngineError) as negative:
            client.call("animation.preview", {"entity": "/Skinned/Armature", "time": -1.0})
        self.assert_engine_error(negative.exception, engine_client.INVALID_PARAMS)

    def test_animation_preview_belongs_to_its_scene(self) -> None:
        # Decision 32 (C23.3's review): with two open scenes the preview poses only the scene it belongs to: previewing
        # in the other scene removes the first one's pose, switching the active tab ends the preview and removes its
        # pose, and closing its scene ends it.
        client, root = self.open_editor_with_scene()
        import_skinned_fixture(client, root)
        client.call("prefab.instantiate", {"prefab": "Assets/Models/Skinned.gltf"})
        client.call("scene.new", {"path": SECOND, "keepOpen": True})
        client.call("prefab.instantiate", {"prefab": "Assets/Models/Skinned.gltf"})
        client.call("scene.activate", {"scene": MAIN})
        rest = body_bounds(client, MAIN)
        self.assertEqual(body_bounds(client, SECOND), rest)

        client.call("animation.preview", {"entity": "/Skinned/Armature", "time": 0.5, "scene": MAIN})
        self.assertNotEqual(body_bounds(client, MAIN), rest)
        moved = client.call("animation.preview", {"entity": "/Skinned/Armature", "time": 0.5, "scene": SECOND})
        self.assertTrue(moved["previewing"])
        self.assertEqual(body_bounds(client, MAIN), rest)
        self.assertNotEqual(body_bounds(client, SECOND), rest)

        # The tab switch ends the preview at the editor's next frame: its pose goes, and a preview of the entity
        # without a time is then a new one, at time 0, never the ended one's 0.5.
        client.call("scene.activate", {"scene": SECOND})
        deadline = time.monotonic() + FRAME_WAIT_SECONDS
        while body_bounds(client, SECOND) != rest:
            if time.monotonic() > deadline:
                self.fail(f"the preview kept its pose {FRAME_WAIT_SECONDS:.0f} s after the tab switch")
        restarted = client.call("animation.preview", {"entity": "/Skinned/Armature", "scene": SECOND})
        self.assertTrue(restarted["previewing"])
        self.assertEqual(restarted["time"], 0.0)
        client.call("animation.preview", {"entity": "/Skinned/Armature", "scene": SECOND, "stop": True})

        # Closing its scene ends the preview with the scene: the other scene keeps its rest pose, previews by itself,
        # and its stop restores the rest pose.
        client.call("animation.preview", {"entity": "/Skinned/Armature", "time": 0.5, "scene": MAIN})
        client.call("scene.close", {"scene": MAIN, "discardChanges": True})
        self.assertEqual(body_bounds(client, SECOND), rest)
        posed = client.call("animation.preview", {"entity": "/Skinned/Armature", "time": 0.5, "scene": SECOND})
        self.assertTrue(posed["previewing"])
        self.assertNotEqual(body_bounds(client, SECOND), rest)
        client.call("animation.preview", {"entity": "/Skinned/Armature", "scene": SECOND, "stop": True})
        self.assertEqual(body_bounds(client, SECOND), rest)

    def test_animation_preview_refuses_a_time_above_a_layer_times_bound(self) -> None:
        # W23.3's review: the preview time is bounded like a layer's own time (the time field's maximum), so the time
        # times a layer's speed stays finite: 3e38 at a looping layer's speed 2 would be an infinite step. It is refused
        # before anything is posed, and the bound itself previews.
        client, root = self.open_editor_with_scene()
        import_skinned_fixture(client, root)
        client.call("prefab.instantiate", {"prefab": "Assets/Models/Skinned.gltf"})
        animator = client.call("entity.get", {"entity": "/Skinned/Armature", "components": ["Animator"]})
        layers = animator["entity"]["components"]["Animator"]["Layers"]
        self.assertTrue(layers[0]["Loop"])
        layers[0]["Speed"] = 2.0
        client.call("entity.update", {"entity": "/Skinned/Armature", "components": {"Animator": {"Layers": layers}}})
        rest = body_bounds(client, MAIN)

        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("animation.preview", {"entity": "/Skinned/Armature", "time": 3.0e38})
        self.assert_engine_error(refused.exception, engine_client.INVALID_PARAMS)
        self.assertEqual(body_bounds(client, MAIN), rest)

        bound = client.call("animation.preview", {"entity": "/Skinned/Armature", "time": 1.0e6})
        self.assertTrue(bound["previewing"])
        client.call("animation.preview", {"entity": "/Skinned/Armature", "stop": True})



class BoneAttachmentTests(AutomationTestCase):
    def test_bone_attachment_follows_its_bone_in_play_and_survives_a_reopen(self) -> None:
        # M41 (decision 3): a BoneAttachment is created, updated and read through the generic entity methods. In the
        # edit scene nothing places it; the scene, with the attached entity a user child of a skinned model instance,
        # saves and reopens clean (brief 21's third fix, which M41's FeatureTest scene relies on); in play the
        # AnimationSystem phase places it on its bone, and a Bone written in play takes effect at the next tick.
        client, root = self.open_editor_with_scene()
        import_skinned_fixture(client, root)
        client.call("prefab.instantiate", {"prefab": "Assets/Models/Skinned.gltf"})
        attachment = {"Bone": "Joint1", "Offset": [0, 0.6, 0.05], "Rotation": [0, 0.7071068, 0, 0.7071068]}
        client.call("entity.create", {"name": "Staff", "parent": "/Skinned/Armature",
                                      "components": {"BoneAttachment": attachment}})
        staff = "/Skinned/Armature/Staff"
        edited = client.call("entity.get", {"entity": staff})["entity"]["components"]
        self.assertEqual(edited["BoneAttachment"]["Bone"], "Joint1")
        self.assertEqual(edited["Transform"]["Translation"], [0, 0, 0])

        client.call("scene.save")
        opened = client.call("scene.open", {"path": MAIN, "reload": True})
        self.assertEqual(opened["rebuiltInstances"], [])
        self.assertFalse(opened["scene"]["dirty"])
        reopened = client.call("entity.get", {"entity": staff})["entity"]["components"]["BoneAttachment"]
        self.assertEqual(reopened["Bone"], "Joint1")
        for got, expected in zip(reopened["Offset"], attachment["Offset"]):
            self.assertAlmostEqual(got, expected, places=6)

        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 20})
        distance = "\n".join([
            "local staff = Scene.FindByPath(\"/Skinned/Armature/Staff\")",
            "local armature = Scene.FindByPath(\"/Skinned/Armature\")",
            "local position, rotation = armature.Animator:GetJointTransform(\"Joint1\")",
            "local expected = position + rotation * staff.BoneAttachment.Offset",
            "return vector.magnitude(staff.Transform.WorldPosition - expected)",
        ])
        self.assertLess(client.call("script.eval", {"context": "play", "code": distance})["value"], 1e-3)
        placed = client.call("entity.get", {"entity": staff, "target": "play"})["entity"]["components"]["Transform"]
        self.assertNotEqual(placed["Translation"], [0, 0, 0])

        # Joint0 never moves in the Wave clip and sits at the Armature's origin, so the Staff lands on its Offset.
        client.call("entity.update", {"entity": staff, "target": "play",
                                      "components": {"BoneAttachment": {"Bone": "Joint0"}}})
        client.call("play.step", {"ticks": 1})
        folded = client.call("entity.get", {"entity": staff, "target": "play"})["entity"]["components"]
        self.assertEqual(folded["BoneAttachment"]["Bone"], "Joint0")
        for got, expected in zip(folded["Transform"]["Translation"], attachment["Offset"]):
            self.assertAlmostEqual(got, expected, places=6)
        client.call("play.stop")
        # Nothing of play reaches the edit scene: the Staff is back at its authored Transform.
        authored = client.call("entity.get", {"entity": staff})["entity"]["components"]["Transform"]
        self.assertEqual(authored["Translation"], [0, 0, 0])


if __name__ == "__main__":
    unittest.main()
