"""M18's device-loss recovery through the editor (Docs/Decisions/0029-m18-contract.md decisions 28 to 31): after an
injected loss the editor keeps running, writes its autosave first, reports one GpuDeviceLost event and one recovered
loss in stats.get, renders the same screenshot as before, and a replay with a loss injected mid-play keeps every
checkpoint hash. Skipped until streams A, B, C, D and E implement the recovery and register the event and the
counter."""

from __future__ import annotations

import time
import unittest
from pathlib import Path

from harness import AutomationTestCase, engine_client

# A bound on the waits for the other client's state, never a timing a test passes by (ADR 0008 decision 15).
WAIT_SECONDS = 60.0
SKIP_REASON = "contract stub: device-loss recovery (Docs/Decisions/0029-m18-contract.md decision 29, streams A to E)"


class DeviceLossTests(AutomationTestCase):
    """debug.deviceLost against a rendering editor: recovered, with the image, the autosave and the simulation kept."""

    def shoot(self, client: engine_client.EngineClient) -> bytes:
        """The PNG bytes of a 64 x 36 screenshot of the scene view."""
        result = client.call("viewport.screenshot", {"view": "scene", "width": 64, "height": 36})
        return Path(result["path"]).read_bytes()

    def wait_for_device_loss_events(self, client: engine_client.EngineClient, cursor: int,
                                    count: int) -> list[dict]:
        """The GpuDeviceLost events since `cursor`, once there are `count` of them (bounded by WAIT_SECONDS)."""
        deadline = time.monotonic() + WAIT_SECONDS
        while True:
            events = client.call("events.read", {"cursor": cursor, "types": ["GpuDeviceLost"]})["events"]
            if len(events) >= count or time.monotonic() > deadline:
                return events

    @unittest.skip(SKIP_REASON)
    def test_device_loss_is_recovered_with_the_autosave_first_and_the_same_screenshot(self) -> None:
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene("DeviceLossRecovered", renderer="vulkan")
        client.call("entity.create",
                    {"name": "Cube", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        client.call("viewport.camera", {"position": [0, 0, 5], "target": [0, 0, 0]})
        before = self.shoot(client)
        self.assertEqual(client.call("stats.get")["deviceLosses"], 0)
        self.assertFalse((root / "Library/Autosave/Manifest.json").exists())
        cursor = client.call("events.read", {"cursor": "end"})["nextCursor"]

        self.assertTrue(client.call("debug.deviceLost")["queued"])
        events = self.wait_for_device_loss_events(client, cursor, 1)
        self.assertEqual(len(events), 1, events)
        self.assertIn("device lost", events[0]["message"])
        # The editor autosaved the dirty scene before it recovered (decision 29), as its fatal hook would.
        self.assertTrue((root / "Library/Autosave/Manifest.json").is_file())
        self.assertEqual(client.call("stats.get")["deviceLosses"], 1)
        self.assertEqual(client.call("scene.query", {"where": {"name": "Cube"}})["total"], 1)
        self.assertEqual(self.shoot(client), before)
        editor = self.editors[-1]
        client.call("session.shutdown", {"force": True})
        self.assertEqual(editor.wait(), 0, editor.output())
        gpu_messages = [line for line in editor.output().splitlines()
                        if ("Vulkan validation:" in line or "NVRHI:" in line)
                        and any(level in line for level in ("[warning]", "[error]", "[critical]"))]
        self.assertFalse(gpu_messages, editor.output())

    @unittest.skip(SKIP_REASON)
    def test_mid_play_device_loss_keeps_every_replay_checkpoint_hash(self) -> None:
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene("DeviceLossMidPlay", renderer="vulkan")
        client.call("entity.create", {"name": "Falling", "components": {
            "Transform": {"Translation": [0, 40, 0]}, "RigidBody": {"Type": "Dynamic"}, "BoxCollider": {}}})
        client.call("entity.create", {"name": "Ground", "components": {
            "Transform": {"Scale": [20, 1, 20]}, "RigidBody": {"Type": "Static"}, "BoxCollider": {}}})
        client.call("scene.save")
        client.call("input.record", {"action": "start"})
        client.call("play.step", {"ticks": 600})
        path = "Assets/Tests/Replays/Fall.replay"
        saved = client.call("input.record", {"action": "stop", "path": path})
        cursor = client.call("events.read", {"cursor": "end"})["nextCursor"]

        # The replay verifies every checkpoint (ticks 60 to 540) and its final hash while a second client loses the
        # device once the session has played past its first checkpoint: the simulation never needs the GPU
        # (decision 29).
        editor = self.editors[-1]
        assert editor.session is not None
        replayer = engine_client.EngineClient.connect_session(editor.session, client_name="replayer")
        replayer.send_raw(engine_client.encode_frame(
            ('{"jsonrpc":"2.0","id":1,"method":"input.replay","params":{"path":"' + path
             + '","verify":true,"strictHash":true}}').encode("utf-8")))
        deadline = time.monotonic() + WAIT_SECONDS
        while client.call("play.state").get("tick", 0) <= 60 and time.monotonic() < deadline:
            pass
        self.assertTrue(client.call("debug.deviceLost")["queued"])
        response = replayer.receive_message(WAIT_SECONDS)
        self.assertNotIn("error", response, response)
        self.assertEqual(response["result"]["stateHash"], saved["stateHash"])
        events = self.wait_for_device_loss_events(client, cursor, 1)
        self.assertEqual(len(events), 1, events)


if __name__ == "__main__":
    unittest.main()
