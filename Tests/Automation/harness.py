"""Shared setup of the Python automation suite (Docs/Architecture.md §15.7, Roadmap M4): headless editors on temporary
projects, driven through Tools/Automation/engine_client.py.

Every test case gets its own temporary directory holding the editor's user-data root (--user-data-dir, so session files,
logs and crash reports never touch the real profile) and its projects, removed afterwards. Editors run headless with
--renderer none and the debug.* test hooks (--automation-test-hooks), and are killed in tearDown if a test left one
running. The transcript of the projects these tests create is redirected with ENGINE_MCP_TRANSCRIPT only by the MCP
tests; the suite itself sends params._meta.transcriptLine explicitly where a test needs it.

The editor binary comes from ENGINE_AUTOMATION_CONFIG (Release by default, the configuration the CI automation stage
uses, §15.8; Debug when only that one is built). Calls are recorded for method coverage (§15.6 gate 5) through
engine_client.CallLog and ENGINE_AUTOMATION_COVERAGE, which Scripts/Test.py sets for the suite. Test modules import
engine_client from this module (`from harness import engine_client`), which puts Tools/Automation on sys.path first, so
the suite runs in any discovery order.

The helpers are contract stubs (M4 stream D) until implemented.
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT / "Tools" / "Automation"))

import engine_client  # noqa: E402  (the path above makes the shared client importable)

CONFIG_ENVIRONMENT_VARIABLE = "ENGINE_AUTOMATION_CONFIG"
DEFAULT_PROJECT_NAME = "AutomationProject"
EXIT_SUCCESS = 0
EXIT_FAILED = 1
EXIT_USAGE = 2
EXIT_INIT_FAILED = 3


def editor_executable() -> Path:
    """The editor of ENGINE_AUTOMATION_CONFIG (or Release, then Debug: engine_client.find_editor_executable)."""
    raise NotImplementedError("contract stub: harness.editor_executable (M4 stream D)")


def app_name() -> str:
    """The editor's application name (the workspace name of premake5.lua), the user-data folder below
    --user-data-dir."""
    raise NotImplementedError("contract stub: harness.app_name (M4 stream D)")


class AutomationTestCase(unittest.TestCase):
    """Base class of every automation test: a temporary directory per test case and helpers that start editors in it."""

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="EngineAutomation-"))
        # Cleanups run after tearDown, which kills the editors this test left running.
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        self.user_data = self.directory / "UserData"
        self.editors: list[engine_client.EditorProcess] = []
        self.call_log = engine_client.CallLog(path=_coverage_path())

    def tearDown(self) -> None:
        raise NotImplementedError("contract stub: AutomationTestCase.tearDown (M4 stream D)")

    def start_editor(self, arguments: list[str] | None = None, project: Path | None = None, test_hooks: bool = True,
                     wait_for_session: bool = True) -> engine_client.EditorProcess:
        """Starts `Editor --headless --renderer none --automation [--automation-test-hooks] [--project <project>]
        <arguments>` with this test's user-data root and remembers it for tearDown."""
        raise NotImplementedError("contract stub: AutomationTestCase.start_editor (M4 stream D)")

    def run_editor(self, arguments: list[str], timeout: float = 120.0) -> tuple[int, str]:
        """Runs a one-shot editor (--batch, --upgrade, --dump-reference, a locked project) to completion and returns its
        exit code and standard error."""
        raise NotImplementedError("contract stub: AutomationTestCase.run_editor (M4 stream D)")

    def connect(self, editor: engine_client.EditorProcess,
                client_name: str = "engine-tests") -> engine_client.EngineClient:
        """An authenticated client of `editor` that records its calls in this test's call log; closed in tearDown."""
        raise NotImplementedError("contract stub: AutomationTestCase.connect (M4 stream D)")

    def create_project(self, client: engine_client.EngineClient, name: str = DEFAULT_PROJECT_NAME) -> Path:
        """project.create in this test's directory through `client`; returns the project root."""
        raise NotImplementedError("contract stub: AutomationTestCase.create_project (M4 stream D)")

    def open_editor_with_scene(self, name: str = DEFAULT_PROJECT_NAME,
                               scene: str = "Assets/Scenes/Main.scene") -> tuple[engine_client.EngineClient, Path]:
        """The common start: an editor in the launcher state, project.create, scene.new; returns the client and the
        project root."""
        raise NotImplementedError("contract stub: AutomationTestCase.open_editor_with_scene (M4 stream D)")

    def assert_engine_error(self, error: engine_client.EngineError, code: int, error_code: str | None = None) -> None:
        """Fails unless `error` has the JSON-RPC `code` (and the engine `error_code` when given), quoting the error."""
        raise NotImplementedError("contract stub: AutomationTestCase.assert_engine_error (M4 stream D)")


def read_json(path: Path) -> Any:
    """The JSON value of a UTF-8 file."""
    raise NotImplementedError("contract stub: harness.read_json (M4 stream D)")


def _coverage_path() -> Path | None:
    value = os.environ.get(engine_client.COVERAGE_ENVIRONMENT_VARIABLE, "")
    return Path(value) if value else None
