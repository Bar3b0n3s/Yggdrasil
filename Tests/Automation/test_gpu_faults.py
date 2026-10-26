"""M18's hangs and GPU out-of-memory through the wire protocol (Docs/Decisions/0029-m18-contract.md decisions 40 to
47): a hang the driver resets is recovered like a device loss (one GpuDeviceLost event, one more stats.get
deviceLosses, the same screenshot), and a device that hangs again after its recovery is recovered again; an
out-of-memory request after startup is retried after a trim (the same screenshot, a debug view's included), and one that
keeps running out of memory fails alone with OutOfMemory (error.data.errorCode, JSON-RPC Internal) while the editor goes
on: the screenshots fail, a debug view stays on its previous mode. An exported game recovers a reset hang and an
out-of-memory capture with the same image, and its automation subset answers a capture that keeps running out of memory
with OutOfMemory. Every rendering process runs with validation and synchronization validation and fails its exit code on
any GPU message (harness.GPU_EDITOR_ARGUMENTS); the injected faults report none.

The faults are injected with --gpu-inject-fault (decision 46: hang-reset, oom-rendertarget and oom-pipeline, each also
-persistent), which strike after the process's first frame. oom-swapchain and oom-swapchain-persistent strike only a
swapchain, which no headless process creates: the Swapchain windowed-child tests cover them. The faults are non-Dist
test hooks, so FeatureTest has no coverage of them (decision 47). The fatal cases (a hang with no loss report,
out-of-memory at startup through ENGINE_GPU_FAULT_AT_STARTUP) are the EditorApp and RuntimeApp process tests'."""

from __future__ import annotations

import unittest
from pathlib import Path
from typing import Any

from harness import EXIT_SUCCESS, GPU_EDITOR_ARGUMENTS, GpuRecoveryTestCase, engine_client
from tiny_game import (TINY_GAME_NAME, WINDOW_HEIGHT, WINDOW_WIDTH, build_tiny_game, export_tiny_game,
                       exported_executable)

# The recoveries test_persistent_hang_reset_is_recovered_every_time waits for: more than one proves that recovery is not
# once per process.
PERSISTENT_RECOVERIES = 3
# A capture size that no view of the exported game renders at, so its capture creates targets of its own.
CAPTURE_WIDTH = 96
CAPTURE_HEIGHT = 54


class GpuFaultTests(GpuRecoveryTestCase):
    """Hang resets and out-of-memory in a rendering editor and an exported game: recovered with the same image, or only
    the request fails. An OutOfMemory error is answered with JSON-RPC Internal, as Gpu and Io are (decision 45)."""

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

    def plain_image(self, name: str, params: dict[str, Any] | None = None) -> bytes:
        """The scene view's screenshot of the test scene, with `params` added, in an editor without a fault."""
        editor, client = self.open_rendering_editor(name, [])
        image = self.shoot(client, params)
        self.shut_down_cleanly(editor, client)
        return image

    def assert_editor_goes_on(self, client: engine_client.EngineClient) -> None:
        """Edits, saves and queries of the open scene work through `client`."""
        client.call("entity.create",
                    {"name": "Sphere", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})
        client.call("scene.save")
        self.assertEqual(client.call("scene.query", {"where": {"name": "Sphere"}})["total"], 1)

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
        frame = self.wait_for_rendered_frame(client, frame)
        self.assertEqual(self.shoot(client), plain)
        # Once per process: the recovered device renders on, and none of its waits (the capture's read-back included)
        # hangs again.
        self.wait_for_rendered_frame(client, frame + 10)
        self.assertEqual(len(self.device_loss_events(client, "")), 1)
        self.assertEqual(client.call("stats.get")["deviceLosses"], 1)
        self.shut_down_cleanly(editor, client)

    def test_persistent_hang_reset_is_recovered_every_time(self) -> None:
        if not self.require_gpu():
            return
        # Every bounded wait after the first frame hangs and is reset, the recovered devices' waits included, so the
        # editor loses its device again after every recovery (decision 46): recovery is not once per process.
        editor, client = self.open_rendering_editor("HangResetPersistent",
                                                    ["--gpu-inject-fault=hang-reset-persistent"], wait_for_frame=False)
        events = self.wait_for_device_loss_events(client, "", PERSISTENT_RECOVERIES)
        self.assertGreaterEqual(len(events), PERSISTENT_RECOVERIES, events)
        self.assertTrue(all("device lost" in event["message"] for event in events), events)
        sequence = [event["seq"] for event in events]
        self.assertEqual(sequence, sorted(set(sequence)), events)
        # Each recovery is one event and one more counted loss; more may have happened since the read above.
        self.assertGreaterEqual(client.call("stats.get")["deviceLosses"], len(events))
        # A capture's read-back hangs too: the request fails with the lost device's Gpu error (decision 28), and the
        # editor recovers again. The cursor counts the recoveries after the capture whatever events.read's page holds.
        cursor = client.call("events.read", {"cursor": "end"})["nextCursor"]
        with self.assertRaises(engine_client.EngineError) as raised:
            self.shoot(client)
        self.assert_engine_error(raised.exception, engine_client.INTERNAL, "Gpu")
        self.assertTrue(self.wait_for_device_loss_events(client, cursor, 1), "no recovery after the failed capture")
        # The editor goes on through every recovery.
        self.assert_editor_goes_on(client)
        self.shut_down_cleanly(editor, client)

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

    def test_out_of_memory_debug_view_is_retried_after_a_trim_with_the_same_screenshot(self) -> None:
        if not self.require_gpu():
            return
        plain = self.plain_image("OomPipelinePlain", {"debugView": "Albedo"})
        # The scene view's switch to a debug view creates its pipelines, the first pipelines after the first frame: the
        # first fails as if out of memory, the switch trims and retries, and the view renders it (decisions 42 and 44).
        editor, client = self.open_rendering_editor("OomPipeline", ["--gpu-inject-fault=oom-pipeline"])
        self.assertEqual(client.call("viewport.setOptions", {"debugView": "Albedo"})["debugView"], "Albedo")
        frame = self.wait_for_rendered_frame(client, -1)
        self.wait_for_rendered_frame(client, frame)
        self.assertEqual(client.call("viewport.setOptions", {})["debugView"], "Albedo")
        self.assertTrue(self.warnings_containing(client, "oom-pipeline"), "the injected out-of-memory never struck")
        self.assertEqual(self.shoot(client, {"debugView": "Albedo"}), plain)
        self.assertEqual(client.call("stats.get")["deviceLosses"], 0)
        self.shut_down_cleanly(editor, client)

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
        # The editor's UI capture renders into a target of its own, which fails the same way (decision 44).
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("editor.screenshot")
        self.assert_engine_error(raised.exception, engine_client.INTERNAL, "OutOfMemory")
        # The editor goes on: edits, saves and queries work, and the device was never lost.
        self.assert_editor_goes_on(client)
        self.assertEqual(client.call("stats.get")["deviceLosses"], 0)
        self.shut_down_cleanly(editor, client)

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

    def test_exported_game_recovers_hangs_and_out_of_memory_with_the_same_image(self) -> None:
        if not self.require_gpu():
            return
        exporter = self.connect(self.start_editor())
        self.create_project(exporter, TINY_GAME_NAME)
        build_tiny_game(exporter)
        executable = exported_executable(export_tiny_game(exporter))
        exporter.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())

        def launch_game(name: str, fault: str) -> tuple[engine_client.EditorProcess, engine_client.EngineClient]:
            """The exported game, paused, with the GPU test options and `fault`, and a client of its automation."""
            arguments = [f"--gpu-inject-fault={fault}"] if fault else []
            game = engine_client.launch_editor(
                executable, ["--headless", "--automation", "--paused", *GPU_EDITOR_ARGUMENTS, *arguments],
                self.directory / name, TINY_GAME_NAME)
            self.editors.append(game)
            return game, self.connect(game)

        def run_game(name: str, fault: str, losses: int) -> bytes:
            """Runs the exported game paused with the GPU test options and `fault` (--gpu-inject-fault=<fault>; none
            when empty), checks that it recovered `losses` device losses (GpuDeviceLost events and stats.get's
            deviceLosses) and returns its game view's screenshot; the game must then exit 0. An out-of-memory fault must
            have struck (its Warn line)."""
            game, client = launch_game(name, fault)
            self.assertEqual(len(self.wait_for_device_loss_events(client, "", losses)), losses)
            shot = client.call("viewport.screenshot", {"view": "game", "width": WINDOW_WIDTH, "height": WINDOW_HEIGHT})
            image = Path(shot["path"]).read_bytes()
            self.assertEqual(client.call("stats.get")["deviceLosses"], losses)
            if fault.startswith("oom-"):
                self.assertTrue(self.warnings_containing(client, fault), f"the injected {fault} never struck")
            client.call("session.shutdown")
            self.assert_clean_exit(game)
            return image

        plain = run_game("Plain", "", 0)
        # A reset hang is recovered like a loss and reported through the automation subset (decisions 40 and 46).
        self.assertEqual(run_game("HangReset", "hang-reset", 1), plain)
        # A capture whose first render target runs out of memory is retried after a trim, with the same image.
        self.assertEqual(run_game("OomRenderTarget", "oom-rendertarget", 0), plain)
        # One that keeps running out of memory fails alone with OutOfMemory, and the game goes on and exits 0
        # (decision 44).
        game, client = launch_game("OomRenderTargetPersistent", "oom-rendertarget-persistent")
        for _ in range(2):
            with self.assertRaises(engine_client.EngineError) as raised:
                client.call("viewport.screenshot", {"view": "game", "width": CAPTURE_WIDTH, "height": CAPTURE_HEIGHT})
            self.assert_engine_error(raised.exception, engine_client.INTERNAL, "OutOfMemory")
        self.assertEqual(client.call("play.step", {"ticks": 10, "render": "none"})["tick"], 10)
        self.assertEqual(client.call("stats.get")["deviceLosses"], 0)
        client.call("session.shutdown")
        self.assert_clean_exit(game)


if __name__ == "__main__":
    unittest.main()
