"""M18's device-loss recovery through the wire protocol (Docs/Decisions/0029-m18-contract.md decisions 25 to 36): after
an injected loss the editor and an exported game keep running, the editor writes its autosave first, each recovery is
one GpuDeviceLost event and one more stats.get deviceLosses, screenshots after the recovery are byte-identical to the
ones before, and a play that loses the device in its middle keeps every checkpoint hash of a replay without the loss.
Every rendering process runs with validation and synchronization validation and fails its exit code on any GPU message
(harness.GPU_EDITOR_ARGUMENTS); an injected loss reports none (decision 28).

The losses are injected through the test hook debug.deviceLost (a recoverable loss after the editor publishes its
autosave snapshot, decision 30) and through --gpu-inject-fault=device-lost (the first submission after the first
garbage collection, so the second frame, once per process). The fatal spellings are tested elsewhere:
debug.deviceLost {unrecoverable: true} by test_autosave.test_device_loss_autosaves_current_dirty_scene, and
--gpu-inject-fault=device-lost-unrecoverable by the EditorApp and RuntimeApp process tests. A loss can only be injected
through these non-Dist test hooks, so FeatureTest has no coverage of it (decision 34)."""

from __future__ import annotations

import json
import time
import unittest
from pathlib import Path
from typing import Any

from harness import EXIT_SUCCESS, GPU_EDITOR_ARGUMENTS, AutomationTestCase, engine_client
from tiny_game import (TINY_GAME_NAME, WINDOW_HEIGHT, WINDOW_WIDTH, build_tiny_game, export_tiny_game,
                       exported_executable)

# A bound on the waits for the other process's state, never a timing a test passes by (ADR 0008 decision 15).
WAIT_SECONDS = 60.0
# A bound on the steps and the replay of the recorded play, which no passing outcome depends on.
REPLAY_TIMEOUT_SECONDS = 600.0
# The recorded play's ticks, the tick boundary at which it loses the device, and its state-hash checkpoints' interval
# (Asset/ReplayData.h's ReplayCheckpointInterval): checkpoints before and after the loss, and the final hash.
REPLAY_TICKS = 600
LOSS_TICK = 120
CHECKPOINT_INTERVAL = 60
# A behaviour that moves its entity a step further every tick, so every checkpoint's hash depends on every tick before
# it (the physics alone settles the falling box within the first few seconds).
DRIFTER = """local Drifter = {}
function Drifter.OnFixedUpdate(self: any, dt: number)
\tself.Entity.Transform:Translate(vector.create(0, 0, 0.0001))
end
return Script.Define("Drifter", Drifter)
"""


class DeviceLossTests(AutomationTestCase):
    """Recoverable device losses in a rendering editor and an exported game: recovered, with the image, the autosave and
    the simulation kept."""

    def shoot(self, client: engine_client.EngineClient, params: dict[str, Any] | None = None) -> bytes:
        """The PNG bytes of a 64 x 36 screenshot of the scene view, with `params` added."""
        result = client.call("viewport.screenshot", {"view": "scene", "width": 64, "height": 36, **(params or {})})
        return Path(result["path"]).read_bytes()

    def device_loss_events(self, client: engine_client.EngineClient, cursor: str) -> list[dict[str, Any]]:
        """The GpuDeviceLost events after `cursor` ("" for the oldest event the log holds)."""
        return client.call("events.read", {"cursor": cursor, "types": ["GpuDeviceLost"]})["events"]

    def wait_for_device_loss_events(self, client: engine_client.EngineClient, cursor: str,
                                    count: int) -> list[dict[str, Any]]:
        """The GpuDeviceLost events after `cursor`, once there are `count` of them (bounded by WAIT_SECONDS)."""
        deadline = time.monotonic() + WAIT_SECONDS
        while True:
            events = self.device_loss_events(client, cursor)
            if len(events) >= count or time.monotonic() > deadline:
                return events

    def wait_for_rendered_frame(self, client: engine_client.EngineClient, after: int) -> int:
        """The scene view's frame index once a frame after `after` rendered (bounded by WAIT_SECONDS)."""
        deadline = time.monotonic() + WAIT_SECONDS
        while True:
            view = client.call("stats.get")["views"][0]
            if view["available"] and int(view["frame"]) > after:
                return int(view["frame"])
            if time.monotonic() > deadline:
                self.fail(f"the scene view rendered no frame after frame {after} within {WAIT_SECONDS:.0f} s: {view}")

    def assert_clean_exit(self, process: engine_client.EditorProcess) -> None:
        """`process` exits 0 (it runs with --expect-no-gpu-errors) and printed no GPU warning or error."""
        self.assertEqual(process.wait(), EXIT_SUCCESS, process.output())
        gpu_messages = [line for line in process.output().splitlines()
                        if ("Vulkan validation:" in line or "NVRHI:" in line)
                        and any(level in line for level in ("[warning]", "[error]", "[critical]"))]
        self.assertFalse(gpu_messages, process.output())

    def newest_autosave(self, root: Path) -> str:
        """The scene payload of the newest autosave generation of the project at `root`."""
        manifest = json.loads((root / "Library/Autosave/Manifest.json").read_text(encoding="utf-8"))
        newest = manifest["Generations"][0]["Generation"]
        return (root / "Library/Autosave" / newest / "Scene.json").read_text(encoding="utf-8")

    def test_device_loss_is_recovered_with_the_autosave_first_and_the_same_screenshot(self) -> None:
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene("DeviceLossRecovered", renderer="vulkan")
        client.call("viewport.camera", {"position": [0, 0, 5], "target": [0, 0, 0]})
        empty = self.shoot(client)
        client.call("entity.create",
                    {"name": "Cube", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        # The lit image, a material debug view (its pipelines are created on first use) and the annotation labels (the
        # Text pipelines and their font atlas) before the loss.
        variants: list[dict[str, Any]] = [{}, {"debugView": "Normals"}, {"annotate": {"labels": "all"}}]
        before = [self.shoot(client, params) for params in variants]
        self.assertNotEqual(before[0], empty)
        self.assertEqual(client.call("stats.get")["deviceLosses"], 0)
        self.assertTrue(client.call("session.info")["_meta"]["dirty"])
        self.assertFalse((root / "Library/Autosave/Manifest.json").exists())
        cursor = client.call("events.read", {"cursor": "end"})["nextCursor"]

        self.assertTrue(client.call("debug.deviceLost")["queued"])
        events = self.wait_for_device_loss_events(client, cursor, 1)
        self.assertEqual(len(events), 1, events)
        # The event carries the loss's report and no tick (decision 31).
        self.assertIn("device lost", events[0]["message"])
        self.assertEqual(events[0]["tick"], -1)
        # The loss autosaved the dirty scene, as the fatal hook would (decision 29: before the recovery, the order the
        # Application cases check): no periodic autosave falls due within the test (every 2 minutes), and nothing else
        # writes one.
        self.assertIn('"Cube"', self.newest_autosave(root))
        self.assertEqual(client.call("stats.get")["deviceLosses"], 1)
        # The scene and its unsaved changes are untouched, the scene view renders again (the lost frame rendered
        # nothing, so a frame newer than the last one held when the event arrived rendered on the recreated device), and
        # the same snapshot renders the same images there.
        self.assertTrue(client.call("session.info")["_meta"]["dirty"])
        self.assertEqual(client.call("scene.query", {"where": {"name": "Cube"}})["total"], 1)
        held = self.wait_for_rendered_frame(client, -1)
        self.wait_for_rendered_frame(client, held)
        self.assertEqual([self.shoot(client, params) for params in variants], before)

        # A second loss is a second recovery: each is one event and one more counted loss.
        self.assertTrue(client.call("debug.deviceLost")["queued"])
        events = self.wait_for_device_loss_events(client, cursor, 2)
        self.assertEqual(len(events), 2, events)
        self.assertGreater(events[1]["seq"], events[0]["seq"])
        self.assertEqual(client.call("stats.get")["deviceLosses"], 2)
        self.assertEqual(self.shoot(client), before[0])
        editor = self.editors[-1]
        client.call("session.shutdown", {"force": True})
        self.assert_clean_exit(editor)

    def test_mid_play_device_loss_keeps_every_replay_checkpoint_hash(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene("DeviceLossMidPlay", renderer="vulkan")
        client.call("script.write", {"path": "Assets/Scripts/Drifter.luau", "source": DRIFTER})
        client.call("entity.create", {"name": "Falling", "components": {
            "Transform": {"Translation": [0, 4, 0]}, "RigidBody": {"Type": "Dynamic"}, "BoxCollider": {},
            "Script": {"Script": "Assets/Scripts/Drifter.luau"}}})
        client.call("entity.create", {"name": "Ground", "components": {
            "Transform": {"Scale": [20, 1, 20]}, "RigidBody": {"Type": "Static"}, "BoxCollider": {}}})
        client.call("scene.save")
        cursor = client.call("events.read", {"cursor": "end"})["nextCursor"]

        # A recorded lockstep play that loses the device at a fixed tick boundary: play.step stops between ticks, the
        # loss strikes the submission of a frame there, and the steps after it run as they would have (decision 29: the
        # simulation never needs the GPU and never waits for its recovery). The recovery is no edit of the session, so
        # the recording goes on across it.
        client.call("input.record", {"action": "start", "seed": 7})
        first = client.call("play.step", {"ticks": LOSS_TICK, "render": "none"}, timeout=REPLAY_TIMEOUT_SECONDS)
        self.assertEqual(first["tick"], LOSS_TICK)
        self.assertTrue(client.call("debug.deviceLost")["queued"])
        events = self.wait_for_device_loss_events(client, cursor, 1)
        self.assertEqual(len(events), 1, events)
        self.assertEqual(client.call("stats.get")["deviceLosses"], 1)
        state = client.call("play.state")
        self.assertEqual((state["tick"], state["stateHash"]), (LOSS_TICK, first["stateHash"]), state)
        self.assertTrue(state["recording"], state)
        rest = client.call("play.step", {"ticks": REPLAY_TICKS - LOSS_TICK, "render": "none"},
                           timeout=REPLAY_TIMEOUT_SECONDS)
        self.assertEqual(rest["tick"], REPLAY_TICKS)
        path = "Assets/Tests/Replays/Fall.replay"
        saved = client.call("input.record", {"action": "stop", "path": path})
        # Checkpoints recorded before and after the loss.
        self.assertGreater(saved["checkpoints"], LOSS_TICK // CHECKPOINT_INTERVAL)
        self.assertEqual(saved["stateHash"], rest["stateHash"])
        client.call("play.stop")

        # A replay without a loss verifies every checkpoint the play recorded across its loss, and its final hash.
        result = client.call("input.replay", {"path": path, "verify": True, "strictHash": True},
                             timeout=REPLAY_TIMEOUT_SECONDS)
        self.assertTrue(result["passed"], result)
        self.assertTrue(result["hashMatched"], result)
        self.assertEqual(result["stateHash"], saved["stateHash"])
        self.assertEqual(result["finalTick"], REPLAY_TICKS)
        self.assertEqual(result["checkpointsChecked"], saved["checkpoints"])
        self.assertFalse(result["checkpointFailed"], result)

        # The loss was recovered once.
        self.assertEqual(len(self.device_loss_events(client, cursor)), 1)
        self.assertEqual(client.call("stats.get")["deviceLosses"], 1)
        client.call("play.stop")
        editor = self.editors[-1]
        client.call("session.shutdown", {"force": True})
        self.assert_clean_exit(editor)

    def test_startup_device_loss_fault_is_recovered_once_and_reported(self) -> None:
        if not self.require_gpu():
            return
        # --gpu-inject-fault=device-lost strikes the second frame, once per process (decision 30).
        editor = self.start_editor(["--gpu-inject-fault=device-lost"], renderer="vulkan")
        client = self.connect(editor)
        # events.read and stats.get need an open project; the engine's event log holds the loss from before it.
        self.create_project(client, "StartupDeviceLoss")
        client.call("scene.new", {"path": "Assets/Scenes/Main.scene"})
        events = self.wait_for_device_loss_events(client, "", 1)
        self.assertEqual(len(events), 1, events)
        self.assertIn("device lost", events[0]["message"])
        self.assertEqual(client.call("stats.get")["deviceLosses"], 1)
        # The recovered device renders on, and the fault never strikes it again.
        client.call("entity.create",
                    {"name": "Cube", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        frame = self.wait_for_rendered_frame(client, -1)
        image = self.shoot(client)
        self.wait_for_rendered_frame(client, frame + 10)
        self.assertEqual(self.shoot(client), image)
        self.assertEqual(len(self.device_loss_events(client, "")), 1)
        self.assertEqual(client.call("stats.get")["deviceLosses"], 1)
        client.call("session.shutdown", {"force": True})
        self.assert_clean_exit(editor)

    def test_exported_game_recovers_a_device_loss_with_the_same_image(self) -> None:
        if not self.require_gpu():
            return
        exporter = self.connect(self.start_editor())
        self.create_project(exporter, TINY_GAME_NAME)
        build_tiny_game(exporter)
        executable = exported_executable(export_tiny_game(exporter))
        exporter.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())

        def run_game(name: str, arguments: list[str]) -> tuple[bytes, list[dict[str, Any]], int]:
            """Runs the exported game paused with the GPU test options and `arguments` (with an injected fault: once
            its loss is recovered) and returns its game view's screenshot, its GpuDeviceLost events and stats.get's
            deviceLosses; the game must then exit 0."""
            game = engine_client.launch_editor(
                executable, ["--headless", "--automation", "--paused", *GPU_EDITOR_ARGUMENTS, *arguments],
                self.directory / name, TINY_GAME_NAME)
            self.editors.append(game)
            client = self.connect(game)
            if arguments:
                self.wait_for_device_loss_events(client, "", 1)
            shot = client.call("viewport.screenshot", {"view": "game", "width": WINDOW_WIDTH, "height": WINDOW_HEIGHT})
            image = Path(shot["path"]).read_bytes()
            events = self.device_loss_events(client, "")
            losses = client.call("stats.get")["deviceLosses"]
            client.call("session.shutdown")
            self.assert_clean_exit(game)
            return image, events, losses

        plain_image, plain_events, plain_losses = run_game("Plain", [])
        self.assertEqual((plain_events, plain_losses), ([], 0))
        # The exported game recovers the loss its second frame takes, reports it through its automation subset, and
        # renders the same image as a run that never lost its device.
        lost_image, lost_events, lost_losses = run_game("Lost", ["--gpu-inject-fault=device-lost"])
        self.assertEqual(len(lost_events), 1, lost_events)
        self.assertIn("device lost", lost_events[0]["message"])
        self.assertEqual(lost_losses, 1)
        self.assertEqual(lost_image, plain_image)


if __name__ == "__main__":
    unittest.main()
