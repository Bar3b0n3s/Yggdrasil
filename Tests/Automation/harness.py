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
"""

from __future__ import annotations

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

    def start_editor(self, arguments: list[str] | None = None, project: Path | None = None, test_hooks: bool = True,
                     wait_for_session: bool = True) -> engine_client.EditorProcess:
        """Starts `Editor --headless --renderer none --automation [--automation-test-hooks] [--project <project>]
        <arguments>` with this test's user-data root and remembers it for tearDown."""
        command = ["--headless", "--renderer", "none", "--automation"]
        if test_hooks:
            command.append("--automation-test-hooks")
        if project is not None:
            command += ["--project", str(project)]
        command += arguments or []
        editor = engine_client.launch_editor(editor_executable(), command, self.user_data, app_name(),
                                             wait_for_session=wait_for_session)
        self.editors.append(editor)
        return editor

    def run_editor(self, arguments: list[str], timeout: float = 120.0) -> tuple[int, str]:
        """Runs a one-shot editor (--batch, --upgrade, --dump-reference, a locked project) to completion and returns its
        exit code and standard error."""
        command = [str(editor_executable()), *arguments, f"--user-data-dir={self.user_data}"]
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

    def open_editor_with_scene(self, name: str = DEFAULT_PROJECT_NAME,
                               scene: str = "Assets/Scenes/Main.scene") -> tuple[engine_client.EngineClient, Path]:
        """The common start: an editor in the launcher state, project.create, scene.new; returns the client and the
        project root."""
        client = self.connect(self.start_editor())
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
