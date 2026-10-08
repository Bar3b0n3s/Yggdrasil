"""The tiny game of the M7 walking-skeleton tests (Roadmap M7; Docs/Architecture.md §14): a project with one scene
holding a primary camera, a sun and a cube, set up as the start scene and the only build scene, built through automation
only, like the demo games (§13.12). The export, runtime and game-view suites (test_export.py, test_runtime.py,
test_game_view.py) share it, and the export suite exports it (project.export; Docs/Decisions/0012-m7-decisions.md).
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from harness import automation_config, engine_client

TINY_GAME_NAME = "TinyGame"
TINY_SCENE = "Assets/Scenes/Main.scene"
# The game view's size in the screenshot comparisons (the manifest window's size, ADR 0012 decision 9).
WINDOW_WIDTH = 320
WINDOW_HEIGHT = 180


def build_tiny_game(client: engine_client.EngineClient) -> None:
    """Fills the open project of `client` (a project.create'd editor without an open scene) with the tiny game and saves
    it: Main.scene with Camera (primary, perspective, at (0, 1.5, 5)), Sun (a directional light) and Cube (the built-in
    cube), StartScene and Export.BuildScenes set to it, the window WINDOW_WIDTH x WINDOW_HEIGHT."""
    client.call("scene.new", {"path": TINY_SCENE})
    camera = {
        "Transform": {"Translation": [0, 1.5, 5]},
        "Camera": {"Primary": True, "Clear": "Color", "ClearColor": [0.1, 0.1, 0.12]},
    }
    sun = {"Transform": {"EulerAngles": [-50, -30, 0]}, "DirectionalLight": {"Intensity": 3}}
    cube = {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}
    ops: list[dict[str, Any]] = [
        {"method": "entity.create", "params": {"name": "Camera", "components": camera}},
        {"method": "entity.create", "params": {"name": "Sun", "components": sun}},
        {"method": "entity.create", "params": {"name": "Cube", "components": cube}},
    ]
    client.call("edit.batch", {"label": "Tiny game", "ops": ops})
    client.call("project.setSettings", {"patch": {
        "StartScene": TINY_SCENE,
        "Window": {"Title": TINY_GAME_NAME, "Width": WINDOW_WIDTH, "Height": WINDOW_HEIGHT},
        "Export": {"BuildScenes": [TINY_SCENE]}}})
    client.call("scene.save")


def export_tiny_game(client: engine_client.EngineClient, config: str | None = None,
                     smoke_test: bool = False) -> dict[str, Any]:
    """project.export of the open tiny game for `config` (default: harness.automation_config(), the configuration the
    suite's editor was built in); returns the export report."""
    return client.call("project.export", {"config": config or automation_config(), "smokeTest": smoke_test})


def exported_executable(report: dict[str, Any]) -> Path:
    """The exported game's executable named by an export report."""
    return Path(report["executable"])
