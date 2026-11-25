"""The Sky through automation (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Docs/Architecture.md §5.3,
§8.6, §13.7): a Sky made and changed through edit.batch and entity.update, its values read back by entity.get and a
value outside its range refused; Scene-view screenshots that differ between dawn and noon and that come back byte for
byte to the Sky-less image once the Sky is removed; a lighting.bake under the Sky that bakes its IrradianceVolume
relightable; stats.get's skyUpdates and relitIrradianceVolumes, 1 in the game view's frames while a script turns the
sun and 0 once it stands still; and project.validate's SKY_ENVIRONMENT_IGNORED for an Environment beside the Sky.

The game view renders at the editor's frames (--headless-game-view), so the stats test waits, bounded by
FRAME_WAIT_SECONDS, for a frame of the state it expects; no passing outcome depends on how long that takes.
"""

from __future__ import annotations

import time
import unittest
from pathlib import Path
from typing import Any, Callable

from harness import AutomationTestCase, engine_client

SHOT = {"view": "scene", "width": 160, "height": 90}
FRAME_WAIT_SECONDS = 60.0
DAWN = [-2, -30, 0]
NOON = [-70, -30, 0]
# Turns its entity half a degree every frame, in OnUpdate, so every frame the game view renders shows a new sun.
SUN_TURNER = """--!strict
local SunTurner = {}

function SunTurner.OnUpdate(self: ScriptInstance, dt: number)
\tself.Entity.Transform:Rotate(vector.create(0.5, 0, 0))
end

return Script.Define("SunTurner", SunTurner)
"""


def sky_ops(sun_rotation: list[float]) -> list[dict[str, Any]]:
    """edit.batch ops of a small cove: a primary camera that clears to the sky, a ground and a hut, a Sun and a Moon
    DirectionalLight, and a Sky naming them."""
    return [
        {"method": "entity.create", "params": {"name": "Camera", "components": {
            "Transform": {"Translation": [0, 1.7, 8], "EulerAngles": [4, 0, 0]},
            "Camera": {"Primary": True, "Clear": "Skybox"}}}},
        {"method": "entity.create", "params": {"name": "Ground", "components": {
            "Transform": {"Scale": [40, 1, 40]}, "MeshRenderer": {"Mesh": "engine://Meshes/Plane"}}}},
        {"method": "entity.create", "params": {"name": "Hut", "components": {
            "Transform": {"Translation": [1, 1, -2], "Scale": [2, 2, 2]},
            "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}}},
        {"method": "entity.create", "params": {"name": "Sun", "components": {
            "Transform": {"EulerAngles": sun_rotation},
            "DirectionalLight": {"Intensity": 4, "LightAngle": 0.53, "CastShadows": True}}}},
        {"method": "entity.create", "params": {"name": "Moon", "components": {
            "Transform": {"EulerAngles": [-40, 150, 0]},
            "DirectionalLight": {"Color": [0.75, 0.82, 1], "Intensity": 0.02}}}},
        {"method": "entity.create", "params": {"name": "Sky", "components": {"Sky": {"Sun": "/Sun", "Moon": "/Moon"}}}},
    ]


def game_view(client: engine_client.EngineClient) -> dict[str, Any]:
    """stats.get's entry of the view named "game"."""
    return next(view for view in client.call("stats.get")["views"] if view["name"] == "game")


def codes(client: engine_client.EngineClient) -> set[str]:
    """The codes project.validate reports for the active scene."""
    return {item["code"] for item in client.call("project.validate", {"scope": "Scene"})["diagnostics"]}


class SkyTests(AutomationTestCase):
    def shoot(self, client: engine_client.EngineClient) -> bytes:
        """The PNG bytes of a scene-view screenshot at SHOT's size."""
        return Path(client.call("viewport.screenshot", SHOT)["path"]).read_bytes()

    def wait_for_game_view(self, client: engine_client.EngineClient, condition: Callable[[dict[str, Any]], bool],
                           what: str) -> dict[str, Any]:
        """The game view's stats once `condition` holds of a rendered frame, failing after FRAME_WAIT_SECONDS."""
        deadline = time.monotonic() + FRAME_WAIT_SECONDS
        while True:
            view = game_view(client)
            if view["available"] and condition(view):
                return view
            if time.monotonic() > deadline:
                self.fail(f"the game view did not report {what} within {FRAME_WAIT_SECONDS:.0f} s: {view}")

    def test_the_sky_changes_through_entity_updates_and_scene_view_screenshots_follow_the_sun(self) -> None:
        # A rendering editor: the Sky's fields through entity.update, and the Scene view's screenshots, which render it.
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("viewport.camera", {"position": [0, 1.7, 8], "target": [0, 2.5, -10]})
        ops = sky_ops(DAWN)
        client.call("edit.batch", {"label": "Cove", "ops": ops[:-1]})
        bare = self.shoot(client)

        created = client.call("edit.batch", {"label": "Sky", "ops": ops[-1:]})
        self.assertEqual(len(created["results"]), 1)
        sun = client.call("entity.get", {"entity": "/Sun"})["entity"]["id"]
        moon = client.call("entity.get", {"entity": "/Moon"})["entity"]["id"]
        read = client.call("entity.get", {"entity": "/Sky", "components": ["Sky"]})["entity"]["components"]["Sky"]
        self.assertEqual((read["Sun"], read["Moon"]), (sun, moon))
        self.assertEqual(read["Intensity"], 1)
        self.assertTrue(read["DriveLights"])
        dawn = self.shoot(client)
        self.assertNotEqual(dawn, bare)
        self.assertEqual(self.shoot(client), dawn)

        # The sun climbs to noon: the sky, the ground's light and the shadows change.
        client.call("entity.update", {"entity": "/Sun", "components": {"Transform": {"EulerAngles": NOON}}})
        noon = self.shoot(client)
        self.assertNotEqual(noon, dawn)

        # The Sky's own fields: a haze, a cloud layer drifting and a darker ground read back and change the image.
        client.call("entity.update", {"entity": "/Sky", "components": {"Sky": {
            "Haze": 3, "Clouds": 0.6, "CloudSpeed": [2, -1], "GroundAlbedo": [0.1, 0.12, 0.08], "Intensity": 0.5}}})
        read = client.call("entity.get", {"entity": "/Sky", "components": ["Sky"]})["entity"]["components"]["Sky"]
        self.assertAlmostEqual(read["Haze"], 3, places=6)
        self.assertAlmostEqual(read["Clouds"], 0.6, places=6)
        self.assertEqual([round(value, 6) for value in read["CloudSpeed"]], [2, -1])
        self.assertEqual([round(value, 6) for value in read["GroundAlbedo"]], [0.1, 0.12, 0.08])
        self.assertAlmostEqual(read["Intensity"], 0.5, places=6)
        hazy = self.shoot(client)
        self.assertNotEqual(hazy, noon)

        # A value outside its range and a second Sky are refused, and the image stays.
        with self.assertRaises(engine_client.EngineError):
            client.call("entity.update", {"entity": "/Sky", "components": {"Sky": {"Haze": 11}}})
        with self.assertRaises(engine_client.EngineError):
            client.call("entity.update", {"entity": "/Sky", "components": {"Sky": {"CloudSpeed": [101, 0]}}})
        with self.assertRaises(engine_client.EngineError):
            client.call("entity.create", {"name": "SecondSky", "components": {"Sky": {}}})
        self.assertAlmostEqual(
            client.call("entity.get", {"entity": "/Sky", "components": ["Sky"]})["entity"]["components"]["Sky"]["Haze"],
            3, places=6)
        self.assertEqual(self.shoot(client), hazy)

        # Without the Sky, the view lights and draws as before it, byte for byte (the sun back at dawn).
        client.call("edit.batch", {"label": "No sky", "ops": [
            {"method": "entity.update", "params": {"entity": "/Sky", "removeComponents": ["Sky"]}},
            {"method": "entity.update",
             "params": {"entity": "/Sun", "components": {"Transform": {"EulerAngles": DAWN}}}}]})
        self.assertEqual(self.shoot(client), bare)
        client.call("scene.save")

    def test_a_relightable_bake_and_the_sky_updates_while_a_script_turns_the_sun(self) -> None:
        # stats.get's game view (--headless-game-view): a still sky regenerates nothing; a script that turns the sun
        # every frame regenerates the sky's light and relights the baked volume in every frame; still again, neither.
        if not self.require_gpu():
            return
        client = self.connect(self.start_editor(["--headless-game-view"], renderer="vulkan"))
        self.create_project(client)
        client.call("scene.new", {"path": "Assets/Scenes/Main.scene"})
        client.call("script.write", {"path": "Assets/Scripts/SunTurner.luau", "source": SUN_TURNER})
        client.call("edit.batch", {"label": "Cove", "ops": [*sky_ops([-40, -30, 0]),
            {"method": "entity.create", "params": {"name": "Volume", "components": {
                "Transform": {"Translation": [0, 1.5, 0]},
                "IrradianceVolume": {"Size": [8, 3, 8], "ProbeCountX": 2, "ProbeCountY": 2, "ProbeCountZ": 2}}}}]})
        self.assertIn("PROBE_NOT_BAKED", codes(client))
        client.call("scene.save")

        baked = client.call("lighting.bake")
        self.assertEqual(len(baked["irradianceVolumes"]), 1)
        self.assertTrue(baked["irradianceVolumes"][0]["relightable"])
        client.call("scene.save")
        self.assertFalse({"PROBE_NOT_BAKED", "PROBE_NOT_RELIGHTABLE"} & codes(client))

        still = self.wait_for_game_view(client, lambda view: view["skyUpdates"] == 0, "a still sky")
        self.assertEqual(still["relitIrradianceVolumes"], 0)
        self.assertIn("SkyIbl", [entry["name"] for entry in still["passes"]])

        client.call("entity.update", {"entity": "/Sun", "components": {
            "Script": {"Scripts": [{"Script": "Assets/Scripts/SunTurner.luau"}]}}})
        client.call("play.start")
        turning = self.wait_for_game_view(
            client, lambda view: view["skyUpdates"] == 1 and view["relitIrradianceVolumes"] == 1,
            "the sky's light regenerated and the volume relit")
        self.assertIn("ProbeRelight", [entry["name"] for entry in turning["passes"]])
        client.call("play.stop")
        stopped = self.wait_for_game_view(
            client, lambda view: view["skyUpdates"] == 0 and view["relitIrradianceVolumes"] == 0, "a still sky again")
        self.assertNotIn("ProbeRelight", [entry["name"] for entry in stopped["passes"]])

    def test_validate_reports_an_environment_beside_a_sky(self) -> None:
        # SKY_ENVIRONMENT_IGNORED (Warning): an enabled Environment beside an enabled Sky is unused; without the Sky, or
        # with it disabled, nothing is reported.
        client, _ = self.open_editor_with_scene()
        client.call("edit.batch", {"label": "Cove", "ops": sky_ops(NOON)})
        self.assertNotIn("SKY_ENVIRONMENT_IGNORED", codes(client))
        client.call("entity.create", {"name": "Rendering", "components": {"Environment": {}}})
        report = client.call("project.validate", {"scope": "Scene"})
        ignored = [item for item in report["diagnostics"] if item["code"] == "SKY_ENVIRONMENT_IGNORED"]
        self.assertEqual(len(ignored), 1, report["diagnostics"])
        self.assertEqual(ignored[0]["severity"], "Warning")
        client.call("entity.update", {"entity": "/Sky", "removeComponents": ["Sky"]})
        self.assertNotIn("SKY_ENVIRONMENT_IGNORED", codes(client))


if __name__ == "__main__":
    unittest.main()
