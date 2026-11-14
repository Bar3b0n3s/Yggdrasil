"""Temporal anti-aliasing through automation (Docs/Architecture.md §8.3; Docs/Decisions/0037-m26-contract.md
decisions 10 and 17, stream X-C): a PostProcess with AntiAliasing Taa resolves the editor's displayed views (their
stats.get rows) and viewport.screenshot renders it from a reset history with the fixed warm-up, so two screenshots of
one state are byte-identical and differ from the unresolved image. A displayed view continues its temporal history
between its renders and starts it afresh when the shown scene changes (stats.get's historyFrames).

Every wait here is a bounded observation wait (the displayed views report a frame after the edit that changed them),
never a timing gate.
"""

from __future__ import annotations

import time
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

from harness import AutomationTestCase, engine_client

OBSERVATION_SECONDS = 60.0
SHOT = {"view": "scene", "width": 160, "height": 90}


class TaaTests(AutomationTestCase):
    """A rendering editor; each test needs a Vulkan device."""

    def shoot(self, client: engine_client.EngineClient) -> bytes:
        """The PNG bytes of a scene-view screenshot at SHOT's size."""
        return Path(client.call("viewport.screenshot", SHOT)["path"]).read_bytes()

    def wait_for_view_rows(self, client: engine_client.EngineClient, edited: int, present: bool) -> None:
        """Waits (bounded by OBSERVATION_SECONDS) until a displayed view rendered a frame after `edited` whose stats
        rows hold TAA (`present`) or not."""
        deadline = time.monotonic() + OBSERVATION_SECONDS
        while True:
            stats = client.call("stats.get")
            views: list[dict[str, Any]] = [view for view in stats["views"]
                                           if view["available"] and int(view["frame"]) > edited]
            if any(("TAA" in {entry["name"] for entry in view["passes"]}) == present for view in views):
                return
            if time.monotonic() >= deadline:
                self.fail(f"no displayed view {'recorded' if present else 'left out'} the TAA pass within "
                          f"{OBSERVATION_SECONDS:.0f} s: {stats}")

    def latest_frame(self, client: engine_client.EngineClient) -> int:
        """The latest frame a displayed view rendered, -1 before any."""
        return max((int(view["frame"]) for view in client.call("stats.get")["views"] if view["available"]), default=-1)

    def wait_for_scene_history(self, client: engine_client.EngineClient,
                               accept: Callable[[int, int], bool]) -> tuple[int, int]:
        """Waits (bounded by OBSERVATION_SECONDS) for a scene-view render whose frame and historyFrames `accept`
        takes, and returns them."""
        deadline = time.monotonic() + OBSERVATION_SECONDS
        while True:
            stats = client.call("stats.get")
            for view in stats["views"]:
                if view["name"] == "scene" and view["available"]:
                    sample = (int(view["frame"]), view["historyFrames"])
                    if accept(*sample):
                        return sample
            if time.monotonic() >= deadline:
                self.fail(f"no scene-view render met the condition within {OBSERVATION_SECONDS:.0f} s: {stats}")

    def test_the_scene_view_continues_its_history_and_play_starts_it_afresh(self) -> None:
        # W26.2's review (ADR 0037 decision 10): stats.get's historyFrames counts the renders since a view's temporal
        # history last started afresh. The displayed scene view applies its history to every render, so the count
        # rises; play starting replaces the shown scene, which starts it again, so a render's frame minus its
        # historyFrames (the frame its history started at) moves past the frame read before play started.
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("entity.create", {"name": "Cube", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        before, _ = self.wait_for_scene_history(client, lambda frame, history: history >= 3)
        client.call("play.start", {"lockstep": True})
        self.wait_for_scene_history(client, lambda frame, history: frame > before and frame - history > before)
        client.call("play.stop")

    def test_taa_resolves_the_views_and_its_screenshots_are_byte_identical(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("edit.batch", {"label": "TAA scene", "ops": [
            {"method": "entity.create", "params": {"name": "Cube", "components": {
                "Transform": {"EulerAngles": [20, 35, 0]}, "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}}},
            {"method": "entity.create", "params": {"name": "Sun", "components": {
                "Transform": {"EulerAngles": [-50, 30, 0]}, "DirectionalLight": {"Intensity": 2}}}},
            {"method": "entity.create", "params": {"name": "Post", "components": {
                "PostProcess": {"AntiAliasing": "None", "BloomEnabled": False}}}}]})
        client.call("viewport.camera", {"position": [0, 1.5, 4], "target": [0, 0, 0]})
        unresolved = self.shoot(client)
        self.wait_for_view_rows(client, self.latest_frame(client), present=False)

        client.call("entity.update", {"entity": "/Post", "components": {"PostProcess": {"AntiAliasing": "Taa"}}})
        resolved = self.shoot(client)
        # The screenshot renders the warm-up from a reset history each time: byte-identical, whatever the views did.
        self.assertEqual(self.shoot(client), resolved)
        self.assertNotEqual(resolved, unresolved)
        # The displayed views resolve with TAA from the frame after the edit.
        self.wait_for_view_rows(client, self.latest_frame(client), present=True)

        # Back to no anti-aliasing: the views leave the pass out and the screenshot is the unresolved image again.
        client.call("entity.update", {"entity": "/Post", "components": {"PostProcess": {"AntiAliasing": "None"}}})
        self.assertEqual(self.shoot(client), unresolved)
        self.wait_for_view_rows(client, self.latest_frame(client), present=False)
        client.call("scene.save")


if __name__ == "__main__":
    unittest.main()
