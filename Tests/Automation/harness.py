"""Shared setup of the Python automation suite (Docs/Architecture.md §15.7, Roadmap M4): headless editors on temporary
projects, driven through Tools/Automation/engine_client.py.

Every test case gets its own temporary directory holding the editor's user-data root (--user-data-dir, so session files,
logs and crash reports never touch the real profile) and its projects, removed afterwards. Editors run headless with
--renderer none and the debug.* test hooks (--automation-test-hooks), and are killed in tearDown if a test left one
running. The transcript of the projects these tests create is redirected with ENGINE_MCP_TRANSCRIPT only by the MCP
tests; the suite itself sends params._meta.transcriptLine explicitly where a test needs it.

Rendering editors (the screenshot methods, Docs/Decisions/0009-m5-decisions.md decision 33) run with the Vulkan renderer
and the GPU test options (GPU_EDITOR_ARGUMENTS: validation with synchronization validation, and a failing exit code
for any GPU error or warning, like the C++ GPU tests' processes). A test that needs one calls require_gpu() first: it
probes once per run whether a headless editor can create its device here; without one the test prints NO_DEVICE_PREFIX
with the reason and returns, passing without running (Scripts/Test.py counts those lines and ends the run as a warning),
unless ENGINE_AUTOMATION_REQUIRE_GPU=1 (Test.py --require-gpu), which fails it instead.

The editor binary comes from ENGINE_AUTOMATION_CONFIG (Release by default, the configuration the CI automation stage
uses, §15.8; Debug when only that one is built). Calls are recorded for method coverage (§15.6 gate 5) through
engine_client.CallLog and ENGINE_AUTOMATION_COVERAGE, which Scripts/Test.py sets for the suite. Test modules import
engine_client from this module (`from harness import engine_client`), which puts Tools/Automation on sys.path first, so
the suite runs in any discovery order.
"""

from __future__ import annotations

import functools
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT / "Tools" / "Automation"))

import engine_client  # noqa: E402  (the path above makes the shared client importable)

DEFAULT_PROJECT_NAME = "AutomationProject"
# The options of every rendering editor a test starts (Test::GetGpuApplicationArguments in the C++ suite).
GPU_EDITOR_ARGUMENTS = ("--gpu-validation=sync", "--expect-no-gpu-errors")
REQUIRE_GPU_VARIABLE = "ENGINE_AUTOMATION_REQUIRE_GPU"
# The line Scripts/Test.py counts (its NO_DEVICE_PATTERN, shared with the C++ GPU tests' HeadlessGpuFixture).
NO_DEVICE_PREFIX = "GPU test without a device (passes without running): "
GPU_PROBE_TIMEOUT_SECONDS = 120.0
EXIT_SUCCESS = 0
EXIT_FAILED = 1
EXIT_USAGE = 2
EXIT_INIT_FAILED = 3


def editor_executable() -> Path:
    """The editor of ENGINE_AUTOMATION_CONFIG (or Release, then Debug: engine_client.configurations_from_environment)."""
    return engine_client.find_editor_executable(REPOSITORY_ROOT, engine_client.configurations_from_environment())


def app_name() -> str:
    """The editor's application name (the workspace name of premake5.lua), the user-data folder below
    --user-data-dir."""
    return engine_client.read_workspace_name(REPOSITORY_ROOT)


@functools.cache
def probe_rendering_editor() -> str | None:
    """None when a headless editor can create its device here (`Editor --headless --frames 1` with the GPU options
    exits 0); otherwise why not: its exit code and the last error line it printed. Probed once per run (cached)."""
    with tempfile.TemporaryDirectory(prefix="EngineGpuProbe-") as directory:
        command = [str(editor_executable()), "--headless", "--frames", "1", *GPU_EDITOR_ARGUMENTS,
                   f"--user-data-dir={directory}"]
        try:
            completed = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                                       timeout=GPU_PROBE_TIMEOUT_SECONDS, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            return f"the editor could not run its device probe: {error}"
    if completed.returncode == EXIT_SUCCESS:
        return None
    # The last error line names the cause ("No Vulkan loader found", no suitable device, a missing validation layer);
    # without one, the last line.
    lines = [line.strip() for line in completed.stderr.decode("utf-8", errors="replace").splitlines() if line.strip()]
    errors = [line for line in lines if "[error]" in line or "[critical]" in line]
    cause = (errors or lines or ["no output"])[-1]
    return f"a headless editor exited with code {completed.returncode} creating its device: {cause}"


class AutomationTestCase(unittest.TestCase):
    """Base class of every automation test: a temporary directory per test case and helpers that start editors in it."""

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="EngineAutomation-")).resolve()
        # Cleanups run after tearDown, which kills the editors this test left running.
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        self.user_data = self.directory / "UserData"
        self.editors: list[engine_client.EditorProcess] = []
        self.clients: list[engine_client.EngineClient] = []
        self.call_log = engine_client.CallLog(path=_coverage_path())

    def tearDown(self) -> None:
        for client in self.clients:
            client.close()
        for editor in self.editors:
            editor.kill()

    def require_gpu(self) -> bool:
        """Whether this test can start rendering editors (see the module comment). False after printing the
        no-device line, which makes the test pass without running; the caller returns at once. With
        ENGINE_AUTOMATION_REQUIRE_GPU=1 a missing device fails the test instead."""
        reason = probe_rendering_editor()
        if reason is None:
            return True
        if os.environ.get(REQUIRE_GPU_VARIABLE) == "1":
            self.fail(f"no usable Vulkan device for a rendering editor ({REQUIRE_GPU_VARIABLE}=1): {reason}")
        print(f"{NO_DEVICE_PREFIX}{self.id()}: {reason}", file=sys.stderr, flush=True)
        return False

    def start_editor(self, arguments: list[str] | None = None, project: Path | None = None, test_hooks: bool = True,
                     wait_for_session: bool = True, renderer: str = "none") -> engine_client.EditorProcess:
        """Starts `Editor --headless --renderer none --automation [--automation-test-hooks] [--project <project>]
        --engine-cache-dir <dir> <arguments>` with this test's user-data root and remembers it for tearDown. With renderer "vulkan" the editor
        renders, with the GPU test options (GPU_EDITOR_ARGUMENTS); call require_gpu() first."""
        if renderer not in ("none", "vulkan"):
            raise ValueError(f"renderer must be none or vulkan, not {renderer!r}")
        command = ["--headless", "--renderer", renderer, "--automation"]
        if renderer == "vulkan":
            command += GPU_EDITOR_ARGUMENTS
        if test_hooks:
            command.append("--automation-test-hooks")
        if project is not None:
            command += ["--project", str(project)]
        command.append(self.engine_cache_argument())
        command += arguments or []
        editor = engine_client.launch_editor(editor_executable(), command, self.user_data, app_name(),
                                             wait_for_session=wait_for_session)
        self.editors.append(editor)
        return editor

    def engine_cache_argument(self) -> str:
        """--engine-cache-dir below this test's user-data root, so no editor a test starts writes the checkout's engine
        cooked cache (bin/EngineCache, ADR 0010 decision 13)."""
        return f"--engine-cache-dir={self.user_data / 'EngineCache'}"

    def run_editor(self, arguments: list[str], timeout: float = 120.0) -> tuple[int, str]:
        """Runs a one-shot editor (--batch, --upgrade, --dump-reference, a locked project) to completion and returns its
        exit code and standard error."""
        command = [str(editor_executable()), *arguments, f"--user-data-dir={self.user_data}", self.engine_cache_argument()]
        completed = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout,
                                   check=False)
        return completed.returncode, completed.stderr.decode("utf-8", errors="replace")

    def connect(self, editor: engine_client.EditorProcess,
                client_name: str = "engine-tests") -> engine_client.EngineClient:
        """An authenticated client of `editor` that records its calls in this test's call log; closed in tearDown."""
        client = editor.connect(client_name, call_log=self.call_log)
        self.clients.append(client)
        return client

    def create_project(self, client: engine_client.EngineClient, name: str = DEFAULT_PROJECT_NAME) -> Path:
        """project.create in this test's directory through `client`; returns the project root."""
        result = client.call("project.create", {"path": str(self.directory / name), "name": name})
        return Path(result["project"]["root"])

    def open_editor_with_scene(self, name: str = DEFAULT_PROJECT_NAME, scene: str = "Assets/Scenes/Main.scene",
                               renderer: str = "none") -> tuple[engine_client.EngineClient, Path]:
        """The common start: an editor in the launcher state (rendering with renderer "vulkan"), project.create,
        scene.new; returns the client and the project root."""
        client = self.connect(self.start_editor(renderer=renderer))
        root = self.create_project(client, name)
        client.call("scene.new", {"path": scene})
        return client, root

    def assert_engine_error(self, error: engine_client.EngineError, code: int, error_code: str | None = None) -> None:
        """Fails unless `error` has the JSON-RPC `code` (and the engine `error_code` when given), quoting the error."""
        quoted = f"{error} (errorCode {error.error_code!r}, data {json.dumps(error.data)})"
        self.assertEqual(error.code, code, quoted)
        if error_code is not None:
            self.assertEqual(error.error_code, error_code, quoted)


def read_json(path: Path) -> Any:
    """The JSON value of a UTF-8 file."""
    return json.loads(path.read_text(encoding="utf-8"))


def _coverage_path() -> Path | None:
    value = os.environ.get(engine_client.COVERAGE_ENVIRONMENT_VARIABLE, "")
    return Path(value) if value else None
