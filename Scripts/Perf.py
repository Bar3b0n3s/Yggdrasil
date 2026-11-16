#!/usr/bin/env python3
"""Capture the Release performance numbers into bin/TestResults/Perf.json (Docs/Architecture.md §15.1 T6, §15.8; Roadmap
M15; Docs/Decisions/0024-m15-contract.md decision 16; the format is Lib/perf.py's).

On a temporary copy of Projects/FeatureTest, a headless editor of --config (--renderer vulkan, --audio-device none,
--automation) measures:
  - playCopy/1000Entities: a generated 1,000-entity scene and a generated empty one, one warm-up and 5 measured cycles
    of a lockstep play.start of each (the difference of the two round trips; §5.6's 30 ms budget);
  - per scene below Assets/Scenes/: in a lockstep session, the play.step time per tick of 8 calls of 30 ticks with
    render "Every" (frameMs) and with render "None" (simulationMs), stats.get's GPU pass times, and the editor
    process's memory (Lib/process.py);
then a --renderer none editor replays Assets/Tests/Replays/Determinism.replay 3 times (replay/Determinism/tickMs); and
(M32, Docs/Decisions/0047-m32-fog-ssr.md Acceptance 5 and 6) a --headless-game-view editor whose game view renders at
1920 x 1080 measures, over LOOK_SAMPLES single rendered ticks each, the GPU time fog adds to the passes it reaches in
Assets/Scenes/Fog.scene, whose Fog sets every field (distance and height fog, the ambient tint and the sun glow, the
skybox unfogged; fog/Fog/gpuCostMs: each sample with the Fog less the median without it; budget 0.5 ms), and the
SSR row of a generated isometric floor of 35 tiles with 0, 8 and 35 of them glossy, and with 8 at half resolution
(ssr/Floor<n>of35/gpuMs, ssr/Floor8of35Half/gpuMs), whose stats also give the tiles the trace covered.

The numbers are warning-only: an over-budget metric, a scene whose calls failed, or a capture without a Vulkan device
(--gpu-optional: no frame or GPU times) makes the step a warning. Only a capture that cannot run fails it: a missing
editor build (exit 3), an editor that crashes or breaks the protocol, a capture that measured no scene at all, a missing
device without --gpu-optional, or Perf.json not written. No passing outcome depends on how long anything takes (ADR 0008
decision 15).

The capture holds the machine-wide GPU lock (Scripts/Lib/locks.py; Docs/Architecture.md §2.3, ADR 0028 decision 16),
shared by every checkout of the repository, for its whole run: it waits while another GPU run or a gate (CI.py) holds
it, unless it runs inside that run (Hardening.py's perf step), reporting the holder (pid, worktree, command), at most
--lock-timeout seconds (exit code 5); --no-wait fails at once instead (exit code 3).

It works from any directory (Docs/Architecture.md §2.3): a relative path given on its command line is resolved against
the caller's directory, then main() makes the repository root of its own checkout the working directory.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

from Lib import locks, paths, perf, process
from Lib.process import ToolNotFoundError, child_environment, run_captured
from Lib.report import EXIT_INIT_FAILED, Console, Status, Step, configure_stdio, emit_json, overall_exit_code

FEATURE_PROJECT = paths.REPOSITORY_ROOT / "Projects" / "FeatureTest"
PROJECT_CACHE_DIRECTORY = "Library"
DETERMINISM_REPLAY = "Assets/Tests/Replays/Determinism.replay"
GENERATED_SCENE = "Assets/Perf/Entities1000.scene"
GENERATED_EMPTY_SCENE = "Assets/Perf/Empty.scene"
GENERATED_ENTITIES = 1000
BUILTIN_CUBE_MESH = "0000000000000101"  # the engine's built-in cube (Assets/BuiltinAssetCatalog)
PLAY_COPY_CYCLES = 5
SCENE_CHUNKS = 8
SCENE_CHUNK_TICKS = 30
WARM_UP_TICKS = 8
REPLAY_RUNS = 3
# M32: the look costs' game view, samples and scenes (module docstring).
LOOK_WIDTH = 1920
LOOK_HEIGHT = 1080
LOOK_WARM_UP_TICKS = 8
LOOK_SAMPLES = 16
FOG_SCENE = "Assets/Scenes/Fog.scene"
FOG_ENTITY = "/Haze"
# The passes fog reaches (Renderer/Fog.h): their GPU times summed.
FOG_PASSES = ("ForwardOpaque", "TerrainForward", "Skybox", "SSR", "ForwardTransparent", "Sprites")
SSR_SCENE = "Assets/Perf/SsrFloor.scene"
SSR_COLUMNS = 7
SSR_ROWS = 5
SSR_GLOSSY_COUNTS = (0, 8, 35)
DEVICE_PROBE_TIMEOUT_SECONDS = 120.0
CALL_TIMEOUT_SECONDS = 120.0
STEP_TIMEOUT_SECONDS = 600.0
SHUTDOWN_TIMEOUT_SECONDS = 60.0
# The editor's device line (GraphicsDevice::Create):
# "Graphics device: '<name>' (<type>, Vulkan <version>, class <class>".
DEVICE_LINE = re.compile(r"Graphics device: '(?P<name>[^']*)' \([^,]*, Vulkan [^,]*, class (?P<class>[^,)]+)")


class CaptureError(Exception):
    """The capture cannot run: the editor crashed, broke the protocol or could not be started."""


def import_engine_client() -> Any:
    """Tools/Automation/engine_client.py, the standard-library automation client (Architecture §13.2)."""
    tools = str(paths.REPOSITORY_ROOT / "Tools" / "Automation")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import engine_client  # noqa: PLC0415  (Tools/Automation is on sys.path only from here)
    return engine_client


def probe_device(editor: Path) -> tuple[str, str, str | None]:
    """(device name, device class, None) when a headless editor creates its Vulkan device here, else ("", "", why
    not)."""
    with tempfile.TemporaryDirectory(prefix="Perf-GpuProbe-") as directory:
        command = [str(editor), "--headless", "--frames", "1", "--audio-device", "none", f"--user-data-dir={directory}"]
        try:
            result = run_captured(command, cwd=paths.REPOSITORY_ROOT, env=child_environment(),
                                  timeout=DEVICE_PROBE_TIMEOUT_SECONDS)
        except ToolNotFoundError as error:
            return "", "", f"the editor could not run its device probe: {error}"
    match = DEVICE_LINE.search(result.output)
    if result.succeeded and match is not None:
        return match.group("name"), match.group("class").strip(), None
    lines = [line.strip() for line in result.output.splitlines() if line.strip()]
    errors = [line for line in lines if "[error]" in line or "[critical]" in line]
    cause = (errors or lines or ["no output"])[-1]
    return "", "", f"a headless editor {result.describe_exit()} creating its device: {cause}"


def git_commit() -> str:
    try:
        result = run_captured(["git", "rev-parse", "HEAD"], cwd=paths.REPOSITORY_ROOT, timeout=30.0)
    except ToolNotFoundError:
        return "unknown"
    return result.output.strip() if result.succeeded else "unknown"


def copy_project(destination: Path) -> Path:
    """Projects/FeatureTest, with its import cache when one exists, at `destination`; returns the project file."""
    shutil.copytree(FEATURE_PROJECT, destination)
    files = sorted(destination.glob("*.eproj"))
    if len(files) != 1:
        raise CaptureError(f"{paths.display_path(FEATURE_PROJECT)} holds {len(files)} project files, not one")
    return files[0]


def scene_paths(project: Path) -> list[str]:
    """The project-relative scenes below Assets/Scenes/, sorted."""
    return sorted(path.relative_to(project).as_posix() for path in (project / "Assets" / "Scenes").rglob("*.scene"))


def scene_name(path: str) -> str:
    """"Assets/Scenes/Golden/Bloom.scene" -> "Golden/Bloom"."""
    return path.removeprefix("Assets/Scenes/").removesuffix(".scene")


class Capture:
    """One capture run: the samples, scene rows and warnings it gathers."""

    def __init__(self, console: Console, engine_client: Any, renderer: str) -> None:
        self.console = console
        self.engine_client = engine_client
        self.renderer = renderer
        self.metrics: list[perf.MetricSummary] = []
        self.scenes: list[dict[str, Any]] = []
        self.warnings: list[str] = []

    def call(self, client: Any, method: str, params: dict[str, Any], timeout: float = CALL_TIMEOUT_SECONDS) -> Any:
        """client.call; a dropped connection, a protocol error or a timeout ends the capture (CaptureError), an engine
        error is the caller's to handle (EngineError)."""
        engine_client = self.engine_client
        try:
            return client.call(method, params, timeout=timeout)
        except (engine_client.ConnectionClosed, engine_client.ProtocolError, TimeoutError, OSError) as error:
            raise CaptureError(f"{method}: {error}") from error

    def memory(self, pid: int) -> process.ProcessMemory | None:
        try:
            return process.query_process_memory(pid)
        except ProcessLookupError as error:
            raise CaptureError(f"the editor (pid {pid}) exited: {error}") from error
        except OSError as error:
            self.warnings.append(f"the memory of the editor (pid {pid}) could not be read: {error}")
            return None

    def play_start_milliseconds(self, client: Any, scene: str) -> float:
        """Opens `scene` and returns the round trip of a lockstep play.start of it; the session is stopped again."""
        self.call(client, "scene.open", {"path": scene, "discardChanges": True}, timeout=STEP_TIMEOUT_SECONDS)
        started = time.perf_counter()
        self.call(client, "play.start", {"lockstep": True})
        elapsed = time.perf_counter() - started
        self.call(client, "play.stop", {})
        return elapsed * 1000.0

    def play_copy(self, client: Any) -> None:
        """playCopy/1000Entities on generated scenes (module docstring); an engine error makes it a warning."""
        ops = [{"method": "entity.create",
                "params": {"name": f"Entity{index:04}",
                           "components": {"Transform": {"Translation": [float(index % 40), 0.0, float(index // 40)]},
                                          "MeshRenderer": {"Mesh": BUILTIN_CUBE_MESH}}}}
               for index in range(GENERATED_ENTITIES)]
        samples: list[float] = []
        try:
            self.call(client, "scene.new", {"path": GENERATED_EMPTY_SCENE, "template": "Empty", "discardChanges": True})
            self.call(client, "scene.new", {"path": GENERATED_SCENE, "template": "Empty", "discardChanges": True})
            self.call(client, "edit.batch", {"label": "Perf: 1,000 entities", "ops": ops}, timeout=STEP_TIMEOUT_SECONDS)
            self.call(client, "scene.save", {})
            for cycle in range(PLAY_COPY_CYCLES + 1):
                empty = self.play_start_milliseconds(client, GENERATED_EMPTY_SCENE)
                full = self.play_start_milliseconds(client, GENERATED_SCENE)
                if cycle > 0:  # the first cycle warms the caches up
                    samples.append(full - empty)
        except self.engine_client.EngineError as error:
            self.warnings.append(f"playCopy/1000Entities: {error.method}: {error.detail}; not captured")
            return
        metric = perf.summarize("playCopy/1000Entities", "ms", samples)
        self.metrics.append(metric)
        self.console.print(f"  playCopy/1000Entities: median {metric.median:.2f} ms over {metric.samples} cycles")

    def step_chunks(self, client: Any, render: str) -> list[float]:
        """SCENE_CHUNKS play.step calls of SCENE_CHUNK_TICKS ticks; each call's time per tick, in ms."""
        samples: list[float] = []
        for _ in range(SCENE_CHUNKS):
            started = time.perf_counter()
            result = self.call(client, "play.step", {"ticks": SCENE_CHUNK_TICKS, "render": render},
                               timeout=STEP_TIMEOUT_SECONDS)
            elapsed = time.perf_counter() - started
            ticks = int(result.get("ticks", 0))
            if ticks <= 0:
                raise ValueError("play.step ran no tick (the session stopped)")
            samples.append(elapsed * 1000.0 / ticks)
        return samples

    def scene(self, client: Any, pid: int, path: str) -> None:
        """One scene's row and metrics (module docstring); an engine error makes it a warning."""
        name = scene_name(path)
        engine_client = self.engine_client
        memory_samples: list[process.ProcessMemory | None] = []
        frame: perf.MetricSummary | None = None
        simulation: perf.MetricSummary | None = None
        gpu_passes: dict[str, float] = {}
        playing = False
        try:
            self.call(client, "scene.open", {"path": path, "discardChanges": True}, timeout=STEP_TIMEOUT_SECONDS)
            self.call(client, "play.start", {"lockstep": True})
            playing = True
            memory_samples.append(self.memory(pid))
            warm_up = "Every" if self.renderer == "vulkan" else "None"
            self.call(client, "play.step", {"ticks": WARM_UP_TICKS, "render": warm_up}, timeout=STEP_TIMEOUT_SECONDS)
            if self.renderer == "vulkan":
                frame = perf.summarize(f"scene/{name}/frameMs", "ms", self.step_chunks(client, "Every"))
                memory_samples.append(self.memory(pid))
                gpu_passes = self.gpu_pass_times(self.call(client, "stats.get", {}))
            simulation = perf.summarize(f"scene/{name}/simulationMs", "ms", self.step_chunks(client, "None"))
            memory_samples.append(self.memory(pid))
        except (engine_client.EngineError, ValueError) as error:
            detail = f"{error.method}: {error.detail}" if isinstance(error, engine_client.EngineError) else str(error)
            self.warnings.append(f"scene {name}: {detail}; its numbers are not captured")
            frame = simulation = None
        finally:
            if playing:
                try:
                    self.call(client, "play.stop", {})
                except engine_client.EngineError as error:
                    self.warnings.append(f"scene {name}: play.stop failed: {error.detail}")
        if simulation is None:
            return
        self.metrics.append(simulation)
        if frame is not None:
            self.metrics.append(frame)
        readings = [sample for sample in memory_samples if sample is not None]
        memory = None
        if readings and len(readings) == len(memory_samples):
            memory = {"privateStart": readings[0].private_bytes, "privateEnd": readings[-1].private_bytes,
                      "privatePeak": max(sample.private_bytes for sample in readings),
                      "residentPeak": max(sample.peak_resident_bytes for sample in readings)}
        ticks = WARM_UP_TICKS + SCENE_CHUNKS * SCENE_CHUNK_TICKS * (2 if frame is not None else 1)
        self.scenes.append({"scene": name, "ticks": ticks, "frameMs": perf.metric_to_json(frame) if frame else None,
                            "simulationMs": perf.metric_to_json(simulation), "gpuPassMs": gpu_passes, "memory": memory})
        shown = f"frame {frame.median:.3f} ms, " if frame is not None else ""
        self.console.print(f"  {name}: {shown}simulation {simulation.median:.3f} ms per tick")

    def look_samples(self, client: Any, passes: tuple[str, ...]) -> tuple[list[float], dict[str, Any]]:
        """LOOK_SAMPLES rendered single ticks of the playing session: the game view's summed GPU time of `passes` after
        each (a sample only when the view has a completed GPU frame), and the last stats' game view."""
        samples: list[float] = []
        view: dict[str, Any] = {}
        for _ in range(LOOK_SAMPLES):
            self.call(client, "play.step", {"ticks": 1, "render": "Every"}, timeout=STEP_TIMEOUT_SECONDS)
            stats = self.call(client, "stats.get", {})
            view = game_view(stats)
            total = summed_gpu_time(view, passes)
            if total is not None:
                samples.append(total)
        return samples, view

    def play_samples(self, client: Any, passes: tuple[str, ...]) -> tuple[list[float], dict[str, Any]]:
        """look_samples of a lockstep session of the open scene after LOOK_WARM_UP_TICKS rendered ticks."""
        self.call(client, "play.start", {"lockstep": True})
        try:
            self.call(client, "play.step", {"ticks": LOOK_WARM_UP_TICKS, "render": "Every"},
                      timeout=STEP_TIMEOUT_SECONDS)
            return self.look_samples(client, passes)
        finally:
            self.call(client, "play.stop", {})

    def fog_cost(self, client: Any) -> None:
        """fog/Fog/gpuCostMs (module docstring); an engine error or a view without GPU times makes it a
        warning."""
        try:
            self.call(client, "scene.open", {"path": FOG_SCENE, "discardChanges": True}, timeout=STEP_TIMEOUT_SECONDS)
            fogged, _ = self.play_samples(client, FOG_PASSES)
            # The scene stays open without its Fog, unsaved: the editor refuses to open the active scene again, and
            # the SSR floor's scene.new discards the change.
            self.call(client, "entity.update", {"entity": FOG_ENTITY, "removeComponents": ["Fog"]})
            clear, _ = self.play_samples(client, FOG_PASSES)
        except (self.engine_client.EngineError, ValueError) as error:
            detail = (f"{error.method}: {error.detail}" if isinstance(error, self.engine_client.EngineError)
                      else str(error))
            self.warnings.append(f"fog/Fog/gpuCostMs: {detail}; not captured")
            return
        if not fogged or not clear:
            self.warnings.append("fog/Fog/gpuCostMs: the game view reported no GPU times; not captured")
            return
        metric = perf.summarize("fog/Fog/gpuCostMs", "ms", cost_samples(fogged, clear))
        self.metrics.append(metric)
        self.console.print(f"  fog/Fog/gpuCostMs: median {metric.median:.3f} ms at {LOOK_WIDTH} x {LOOK_HEIGHT}")

    def ssr_cost(self, client: Any) -> None:
        """ssr/Floor<n>of35/gpuMs and ssr/Floor8of35Half/gpuMs (module docstring); an engine error or a view without GPU
        times makes it a warning."""
        try:
            self.call(client, "asset.create", {"type": "Material", "path": "Assets/Perf/Glossy.material",
                                               "values": {"BaseColor": [0.3, 0.32, 0.35, 1], "Roughness": 0.25}})
            self.call(client, "asset.create", {"type": "Material", "path": "Assets/Perf/Rough.material",
                                               "values": {"BaseColor": [0.35, 0.3, 0.28, 1], "Roughness": 0.9}})
            self.call(client, "scene.new", {"path": SSR_SCENE, "template": "Empty", "discardChanges": True})
            self.call(client, "edit.batch", {"label": "Perf: SSR floor", "ops": ssr_floor_ops()},
                      timeout=STEP_TIMEOUT_SECONDS)
            self.call(client, "scene.save", {})
            for glossy in SSR_GLOSSY_COUNTS:
                self.set_glossy_tiles(client, glossy)
                self.ssr_metric(client, f"ssr/Floor{glossy}of35/gpuMs")
            self.set_glossy_tiles(client, 8)
            self.call(client, "project.setSettings", {"patch": {"Rendering": {"SsrHalfResolution": True}}})
            self.ssr_metric(client, "ssr/Floor8of35Half/gpuMs")
            self.call(client, "project.setSettings", {"patch": {"Rendering": {"SsrHalfResolution": False}}})
        except (self.engine_client.EngineError, ValueError) as error:
            detail = (f"{error.method}: {error.detail}" if isinstance(error, self.engine_client.EngineError)
                      else str(error))
            self.warnings.append(f"ssr/Floor: {detail}; not captured")

    def set_glossy_tiles(self, client: Any, glossy: int) -> None:
        """Gives the floor's first `glossy` tiles (in ssr_tile_names' order) the glossy material and the rest the
        rough."""
        ops = [{"method": "entity.update",
                "params": {"entity": f"/{name}",
                           "components": {"MeshRenderer": {"Materials": [
                               "Assets/Perf/Glossy.material" if index < glossy else "Assets/Perf/Rough.material"]}}}}
               for index, name in enumerate(ssr_tile_names())]
        self.call(client, "edit.batch", {"label": f"Perf: {glossy} glossy tiles", "ops": ops},
                  timeout=STEP_TIMEOUT_SECONDS)

    def ssr_metric(self, client: Any, name: str) -> None:
        """The SSR row's GPU time of the open floor scene as the metric `name`, with the traced tiles printed."""
        samples, view = self.play_samples(client, ("SSR",))
        if not samples:
            self.warnings.append(f"{name}: the game view reported no SSR GPU time; not captured")
            return
        metric = perf.summarize(name, "ms", samples)
        self.metrics.append(metric)
        self.console.print(f"  {name}: median {metric.median:.3f} ms, {view.get('ssrTracedTiles', 0)} of "
                           f"{view.get('ssrTiles', 0)} tiles traced at {LOOK_WIDTH} x {LOOK_HEIGHT}")

    @staticmethod
    def gpu_pass_times(stats: dict[str, Any]) -> dict[str, float]:
        """The completed GPU pass times of the game view (the scene view's when the game view has none), by pass."""
        views = [view for view in stats.get("views", []) if isinstance(view, dict) and view.get("gpuAvailable")]
        views.sort(key=lambda view: view.get("name") != "game")
        times: dict[str, float] = {}
        for view in views[:1]:
            for entry in view.get("passes", []):
                if isinstance(entry, dict) and entry.get("gpuAvailable") and entry.get("name") not in times:
                    times[str(entry["name"])] = float(entry.get("gpuMilliseconds", 0.0))
        return times

    def replay(self, client: Any) -> None:
        """replay/Determinism/tickMs (module docstring)."""
        samples: list[float] = []
        for _ in range(REPLAY_RUNS):
            started = time.perf_counter()
            try:
                result = self.call(client, "input.replay", {"path": DETERMINISM_REPLAY}, timeout=STEP_TIMEOUT_SECONDS)
            except self.engine_client.EngineError as error:
                self.warnings.append(f"replay {DETERMINISM_REPLAY}: {error.detail}; its numbers are not captured")
                return
            elapsed = time.perf_counter() - started
            ticks = int(result.get("finalTick", 0))
            if ticks <= 0:
                self.warnings.append(f"replay {DETERMINISM_REPLAY} ran no tick; its numbers are not captured")
                return
            samples.append(elapsed * 1000.0 / ticks)
        metric = perf.summarize("replay/Determinism/tickMs", "ms", samples)
        self.metrics.append(metric)
        self.console.print(f"  replay/Determinism/tickMs: median {metric.median:.3f} ms per tick")


def game_view(stats: dict[str, Any]) -> dict[str, Any]:
    """The game view of a stats.get result, or an empty dict without one."""
    return next((view for view in stats.get("views", []) if isinstance(view, dict) and view.get("name") == "game"), {})


def summed_gpu_time(view: dict[str, Any], passes: tuple[str, ...]) -> float | None:
    """The summed GPU time of `passes` in `view` (a pass it did not record counts 0), or None without a completed GPU
    frame."""
    if not view.get("gpuAvailable"):
        return None
    return sum(float(entry.get("gpuMilliseconds", 0.0)) for entry in view.get("passes", [])
               if isinstance(entry, dict) and entry.get("name") in passes and entry.get("gpuAvailable"))


def cost_samples(with_effect: list[float], without: list[float]) -> list[float]:
    """Each sample with an effect less the median of the samples without it: the effect's cost, sample by sample."""
    baseline = perf.summarize("baseline", "ms", without).median
    return [sample - baseline for sample in with_effect]


def ssr_tile_names() -> list[str]:
    """The SSR floor's tiles in row-major order, the glossy ones first by set_glossy_tiles."""
    return [f"Tile{row}{column}" for row in range(SSR_ROWS) for column in range(SSR_COLUMNS)]


def ssr_floor_ops() -> list[dict[str, Any]]:
    """edit.batch ops of the SSR floor: 7 x 5 tiles of 2 m under an isometric orthographic camera, rough columns to
    reflect (so only the floor's glossy tiles trace), a sun, the Studio environment and SSR on (SsrMaxRoughness 0.75, as
    World Tree's Arena 1)."""
    ops: list[dict[str, Any]] = [
        {"method": "entity.create", "params": {"name": "Rendering", "components": {
            "Environment": {"Environment": "engine://Environments/Studio", "Intensity": 0.6},
            "PostProcess": {"SsrEnabled": True, "SsrMaxRoughness": 0.75, "SsaoEnabled": False}}}},
        {"method": "entity.create", "params": {"name": "Camera", "components": {
            "Transform": {"Translation": [10, 10, 10], "EulerAngles": [-35.264, 45, 0]},
            "Camera": {"Primary": True, "Projection": "Orthographic", "OrthographicSize": 6, "FarClip": 60}}}},
        {"method": "entity.create", "params": {"name": "Sun", "components": {
            "Transform": {"EulerAngles": [-50, 30, 0]}, "DirectionalLight": {"Intensity": 2, "CastShadows": False}}}},
    ]
    for index, name in enumerate(ssr_tile_names()):
        row, column = divmod(index, SSR_COLUMNS)
        ops.append({"method": "entity.create", "params": {"name": name, "components": {
            "Transform": {"Translation": [(column - SSR_COLUMNS // 2) * 2.0, 0, (row - SSR_ROWS // 2) * 2.0]},
            "MeshRenderer": {"Mesh": "engine://Meshes/Plane", "Materials": ["Assets/Perf/Rough.material"]}}}})
    for index in range(4):
        ops.append({"method": "entity.create", "params": {"name": f"Column{index}", "components": {
            "Transform": {"Translation": [-4.0 + index * 2.5, 1.5, -3.0], "Scale": [0.6, 3, 0.6]},
            "MeshRenderer": {"Mesh": BUILTIN_CUBE_MESH, "Materials": ["Assets/Perf/Rough.material"]}}}})
    return ops


def run_editor(capture: Capture, editor: Path, project_file: Path, renderer: str, user_data: Path, work: Any,
               extra_arguments: tuple[str, ...] = ()) -> None:
    """Starts a headless editor with `renderer` (and `extra_arguments`) on `project_file`, runs `work(client, pid)`
    and shuts the editor down; the process is always stopped."""
    engine_client = capture.engine_client
    try:
        editor_process = engine_client.launch_editor(
            editor, ["--headless", *extra_arguments, "--renderer", renderer, "--audio-device", "none", "--automation",
                     "--project", str(project_file)], user_data, paths.workspace_name(),
            environment=child_environment())
    except (RuntimeError, TimeoutError, OSError) as error:
        raise CaptureError(f"{editor.name} could not be started: {error}") from error
    try:
        try:
            client = editor_process.connect("perf", call_log=engine_client.CallLog(None))
        except (engine_client.ConnectionClosed, engine_client.ProtocolError, TimeoutError, OSError) as error:
            raise CaptureError(f"{editor.name} could not be reached: {error}") from error
        with client:
            try:
                work(client, editor_process.process.pid)
            except CaptureError as error:
                raise CaptureError(f"{error}\n{editor_process.output()}") from error
            try:
                client.call("session.shutdown", {"force": True}, timeout=SHUTDOWN_TIMEOUT_SECONDS)
            except (engine_client.EngineError, engine_client.ConnectionClosed, TimeoutError, OSError):
                pass  # the process is stopped below either way
        try:
            editor_process.wait(SHUTDOWN_TIMEOUT_SECONDS)
        except TimeoutError:
            capture.warnings.append(f"the {renderer} editor did not exit within {SHUTDOWN_TIMEOUT_SECONDS:.0f} s and "
                                    f"was stopped")
    finally:
        editor_process.kill()


def capture_perf(config: str, output: Path, gpu_optional: bool, console: Console) -> Step:
    started = time.perf_counter()
    name = f"perf capture ({config})"
    editor = paths.output_directory(config) / "Editor" / paths.executable_name("Editor")
    if not editor.is_file():
        return Step(name, Status.FAILED, f"{paths.display_path(editor)} not found: run python Scripts/Build.py "
                                         f"--config {config} --project Editor", exit_code=EXIT_INIT_FAILED)
    device, device_class, no_device = probe_device(editor)
    renderer = "vulkan" if no_device is None else "none"
    data: dict[str, Any] = {"report": paths.display_path(output), "editor": paths.display_path(editor)}
    if no_device is not None:
        data["noDevice"] = no_device
        if not gpu_optional:
            return Step(name, Status.FAILED, f"no usable Vulkan device: {no_device}; --gpu-optional captures the CPU "
                                             f"numbers without one", time.perf_counter() - started, data=data)
    engine_client = import_engine_client()
    capture = Capture(console, engine_client, renderer)
    if no_device is not None:
        capture.warnings.append(f"no usable Vulkan device ({no_device}): frame and GPU pass times are not captured")
    console.heading(f"{name}: {paths.display_path(editor)} on a copy of {paths.display_path(FEATURE_PROJECT)}")
    try:
        with tempfile.TemporaryDirectory(prefix="Perf-") as directory:
            root = Path(directory)
            project_file = copy_project(root / "FeatureTest")
            scenes = scene_paths(project_file.parent)

            def measure(client: Any, pid: int) -> None:
                capture.play_copy(client)
                for path in scenes:
                    capture.scene(client, pid, path)

            run_editor(capture, editor, project_file, renderer, root / "UserData", measure)
            run_editor(capture, editor, project_file, "none", root / "ReplayUserData",
                       lambda client, _pid: capture.replay(client))
            if renderer == "vulkan":
                def look(client: Any, _pid: int) -> None:
                    capture.call(client, "project.setSettings",
                                 {"patch": {"Window": {"Width": LOOK_WIDTH, "Height": LOOK_HEIGHT}}})
                    capture.fog_cost(client)
                    capture.ssr_cost(client)

                run_editor(capture, editor, project_file, renderer, root / "LookUserData", look,
                           ("--headless-game-view",))
    except CaptureError as error:
        return Step(name, Status.FAILED, f"the capture could not run: {error}", time.perf_counter() - started,
                    data=data)
    except OSError as error:
        return Step(name, Status.FAILED, f"the project copy failed: {error}", time.perf_counter() - started, data=data)
    if not capture.scenes:
        reason = capture.warnings[-1] if capture.warnings else "no scene below Assets/Scenes/"
        return Step(name, Status.FAILED, f"the capture could not run: no scene was measured ({reason})",
                    time.perf_counter() - started, data={**data, "warnings": capture.warnings})

    capture.warnings += [f"{metric.name}: median {metric.median:.2f} {metric.unit} is over its {metric.budget:g} "
                         f"{metric.unit} budget" for metric in capture.metrics if metric.over_budget]
    metrics = sorted(capture.metrics, key=lambda metric: metric.name)
    document = {
        "format": perf.PERF_FORMAT, "version": perf.PERF_VERSION, "commit": git_commit(), "config": config,
        "host": {"system": paths.host().system, "machine": paths.host().machine, "device": device,
                 "deviceClass": device_class, "renderer": renderer},
        "metrics": [perf.metric_to_json(metric) for metric in metrics],
        "scenes": sorted(capture.scenes, key=lambda scene: scene["scene"]),
        "warnings": capture.warnings,
    }
    try:
        perf.write_perf_report(output, document)
    except (OSError, ValueError) as error:
        return Step(name, Status.FAILED, f"{paths.display_path(output)} was not written: {error}",
                    time.perf_counter() - started, data=data)
    data.update({"metrics": len(metrics), "scenes": len(capture.scenes), "warnings": capture.warnings})
    for warning in capture.warnings:
        console.print(f"WARNING: {warning}")
    summary = f"{len(metrics)} metrics of {len(capture.scenes)} scenes in {paths.display_path(output)}"
    if capture.warnings:
        return Step(name, Status.WARNING,
                    f"{summary}; {len(capture.warnings)} warning(s), first: {capture.warnings[0]}",
                    time.perf_counter() - started, data=data)
    return Step(name, Status.PASSED, summary, time.perf_counter() - started, data=data)


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Exit codes: 0 captured (possibly with warnings), 1 the capture could not run, 2 usage error, 3 the "
               "editor is not built, or --no-wait found the GPU lock held, 5 the wait for the GPU lock ran out.",
    )
    parser.add_argument("--config", choices=("Debug", "Release"), default="Release",
                        help="the editor configuration to measure (default: Release, the configuration the numbers "
                             "mean something in)")
    parser.add_argument("--output", type=paths.argument_path, default=paths.TEST_RESULTS_ROOT / perf.PERF_REPORT_NAME,
                        help="the report to write (default: bin/TestResults/Perf.json)")
    parser.add_argument("--gpu-optional", action="store_true",
                        help="without a usable Vulkan device, capture the CPU numbers with a warning instead of "
                             "failing")
    parser.add_argument("--json", action="store_true", help="emit a machine-readable result")
    locks.add_wait_arguments(parser)
    options = parser.parse_args(sys.argv[1:] if argv is None else argv)
    paths.enter_repository_root()
    locks.check_wait_arguments(parser, options)
    console = Console(options.json)
    try:
        with locks.acquire_gpu(locks.wait_seconds(options), console.print):
            step = capture_perf(options.config, options.output.resolve(), options.gpu_optional, console)
    except locks.LockUnavailable as error:
        step = locks.unavailable_step(error)
    console.result(step)
    code = overall_exit_code([step])
    if options.json:
        emit_json({"success": code == 0, "exitCode": code, "steps": [step.to_json()]})
    return code


if __name__ == "__main__":
    sys.exit(main())
