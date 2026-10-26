"""M18's hangs and GPU out-of-memory through the wire protocol (Docs/Decisions/0029-m18-contract.md decisions 40 to
47): a hang the driver resets is recovered like a device loss (one GpuDeviceLost event, one more stats.get
deviceLosses, the same screenshot), an out-of-memory request after startup is retried after a trim (the same
screenshot), and one that keeps running out of memory fails alone with OutOfMemory (error.data.errorCode, JSON-RPC
Internal) while the editor goes on: a screenshot fails, a debug view stays on its previous mode. Every rendering editor
runs with validation and synchronization validation and fails its exit code on any GPU message
(harness.GPU_EDITOR_ARGUMENTS); the injected faults report none.

The faults are injected with --gpu-inject-fault (hang-reset, oom-rendertarget, oom-rendertarget-persistent,
oom-pipeline-persistent, decision 46), which strike after the editor's first frame. They are non-Dist test hooks, so
FeatureTest has no coverage of them (decision 47). The fatal cases (a hang with no loss report, out-of-memory at
startup) are the EditorApp and RuntimeApp process tests'. Skipped until W18.4's streams A, B and C implement the reset
wait, the trim and the requests."""

from __future__ import annotations

import unittest
from typing import Any

from harness import GpuRecoveryTestCase, engine_client

SKIP_REASON = "contract stub: hangs and out-of-memory (Docs/Decisions/0029-m18-contract.md decisions 40 to 46)"


class GpuFaultTests(GpuRecoveryTestCase):
    """Hang resets and out-of-memory in a rendering editor: recovered with the same image, or only the request fails.
    An OutOfMemory error is answered with JSON-RPC Internal, as Gpu and Io are (decision 45)."""

    def open_rendering_editor(self, name: str, arguments: list[str], wait_for_frame: bool = True
                              ) -> tuple[engine_client.EditorProcess, engine_client.EngineClient]:
        """A rendering editor with `arguments`, a new project `name` and a scene with a cube in front of the scene
        view's camera; with `wait_for_frame`, once the scene view rendered a frame of it."""
        editor = self.start_editor(arguments, renderer="vulkan")
        client = self.connect(editor)
        self.create_project(client, name)
        client.call("scene.new", {"path": "Assets/Scenes/Main.scene"})
        client.call("viewport.camera", {"position": [0, 0, 5], "target": [0, 0, 0]})
        client.call("entity.create",
                    {"name": "Cube", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        if wait_for_frame:
            self.wait_for_rendered_frame(client, -1)
        return editor, client

    def warnings_containing(self, client: engine_client.EngineClient, text: str) -> list[dict[str, Any]]:
        """The log entries at Warn or above that contain `text`, oldest first."""
        entries: list[dict[str, Any]] = []
        cursor = ""
        while True:
            page = client.call("log.read", {"cursor": cursor, "minLevel": "Warn", "contains": text})
            if not page["entries"]:
                return entries
            entries += page["entries"]
            cursor = page["nextCursor"]

    def shut_down_cleanly(self, editor: engine_client.EditorProcess, client: engine_client.EngineClient) -> None:
        """Shuts `editor` down; it exits 0 (it runs with --expect-no-gpu-errors) and printed no GPU warning or error."""
        client.call("session.shutdown", {"force": True})
        self.assert_clean_exit(editor)

    def plain_image(self, name: str) -> bytes:
        """The scene view's screenshot of the test scene in an editor without a fault."""
        editor, client = self.open_rendering_editor(name, [])
        image = self.shoot(client)
        self.shut_down_cleanly(editor, client)
        return image

    @unittest.skip(SKIP_REASON)
    def test_hang_reset_is_recovered_like_a_device_loss_with_the_same_screenshot(self) -> None:
        if not self.require_gpu():
            return
        plain = self.plain_image("HangResetPlain")
        # The first bounded wait after the first frame spends its 10 s budget without waiting, the reset wait's first
        # slice reports the device lost, and the frame boundary recovers it (decisions 40 and 46).
        editor, client = self.open_rendering_editor("HangReset", ["--gpu-inject-fault=hang-reset"])
        events = self.wait_for_device_loss_events(client, "", 1)
        self.assertEqual(len(events), 1, events)
        self.assertIn("device lost", events[0]["message"])
        self.assertEqual(client.call("stats.get")["deviceLosses"], 1)
        frame = self.wait_for_rendered_frame(client, -1)
        self.wait_for_rendered_frame(client, frame)
        self.assertEqual(self.shoot(client), plain)
        # Once per process: the recovered device renders on without hanging again.
        self.assertEqual(len(self.wait_for_device_loss_events(client, "", 1)), 1)
        self.shut_down_cleanly(editor, client)

    @unittest.skip(SKIP_REASON)
    def test_out_of_memory_request_is_retried_after_a_trim_with_the_same_screenshot(self) -> None:
        if not self.require_gpu():
            return
        plain = self.plain_image("OomRenderTargetPlain")
        # The first render target after the first frame fails as if out of memory; its request trims and retries, and
        # nothing else changes (decisions 42 and 46).
        editor, client = self.open_rendering_editor("OomRenderTarget", ["--gpu-inject-fault=oom-rendertarget"])
        self.assertEqual(self.shoot(client), plain)
        self.assertTrue(self.warnings_containing(client, "oom-rendertarget"), "the injected out-of-memory never struck")
        self.assertEqual(client.call("stats.get")["deviceLosses"], 0)
        self.shut_down_cleanly(editor, client)

    @unittest.skip(SKIP_REASON)
    def test_persistent_out_of_memory_fails_only_the_screenshot(self) -> None:
        if not self.require_gpu():
            return
        # Every render target after the first frame runs out of memory, so the views may render nothing new.
        editor, client = self.open_rendering_editor("OomRenderTargetPersistent",
                                                    ["--gpu-inject-fault=oom-rendertarget-persistent"],
                                                    wait_for_frame=False)
        # A capture at a new size runs out of memory again after its trim: the request fails alone, with OutOfMemory
        # (decision 45), and is tried again at the next request.
        for _ in range(2):
            with self.assertRaises(engine_client.EngineError) as raised:
                client.call("viewport.screenshot", {"view": "scene", "width": 96, "height": 54})
            self.assert_engine_error(raised.exception, engine_client.INTERNAL, "OutOfMemory")
        # The editor goes on: edits, saves and queries work, and the device was never lost.
        client.call("entity.create",
                    {"name": "Sphere", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})
        client.call("scene.save")
        self.assertEqual(client.call("scene.query", {"where": {"name": "Sphere"}})["total"], 1)
        self.assertEqual(client.call("stats.get")["deviceLosses"], 0)
        self.shut_down_cleanly(editor, client)

    @unittest.skip(SKIP_REASON)
    def test_persistent_out_of_memory_debug_view_stays_on_its_previous_mode(self) -> None:
        if not self.require_gpu():
            return
        editor, client = self.open_rendering_editor("OomPipelinePersistent",
                                                    ["--gpu-inject-fault=oom-pipeline-persistent"])
        lit = self.shoot(client)
        # The scene view's switch to a debug view runs out of memory again after its trim: the view stays on its
        # previous mode with a diagnostic, and its setting follows (decision 44).
        self.assertEqual(client.call("viewport.setOptions", {"debugView": "Albedo"})["debugView"], "Albedo")
        frame = self.wait_for_rendered_frame(client, -1)
        self.wait_for_rendered_frame(client, frame)
        self.assertEqual(client.call("viewport.setOptions", {})["debugView"], "Lit")
        self.assertTrue(self.warnings_containing(client, "Albedo"), "no diagnostic named the debug view that stayed")
        # A capture of the debug view fails alone; a Lit capture still renders the same image.
        with self.assertRaises(engine_client.EngineError) as raised:
            self.shoot(client, {"debugView": "Albedo"})
        self.assert_engine_error(raised.exception, engine_client.INTERNAL, "OutOfMemory")
        self.assertEqual(self.shoot(client), lit)
        self.shut_down_cleanly(editor, client)


if __name__ == "__main__":
    unittest.main()
