"""Launch or attach (Docs/Architecture.md §13.8, Roadmap M4): the bridge attaches to an editor that already serves the
project and refuses to start a second writer."""

from __future__ import annotations

import shutil
import sys
import tempfile
import unittest
from pathlib import Path

MCP_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = MCP_ROOT.parents[1]
sys.path.insert(0, str(MCP_ROOT))
sys.path.insert(0, str(REPOSITORY_ROOT / "Tools" / "Automation"))

import engine_client  # noqa: E402
from engine_mcp.launcher import EditorAlreadyOpen, Launcher, LaunchRequest  # noqa: E402


class LauncherTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="EngineMcp-"))
        # Cleanups run last in, first out: the editors are killed (tearDown) before the directory is removed.
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        self.user_data = self.directory / "UserData"
        self.app_name = engine_client.read_workspace_name(REPOSITORY_ROOT)
        self.editors: list[engine_client.EditorProcess] = []

    def tearDown(self) -> None:
        for editor in self.editors:
            editor.kill()

    def start_project_editor(self, arguments: list[str],
                             headless: bool = True) -> tuple[engine_client.EditorProcess, Path]:
        """An editor on a new project (created through a launcher-state editor first). A headless editor serves
        automation unless the run is one-shot (§13.2, ADR 0008 decision 14); a windowed editor without --automation
        does not. The windowed case needs a display (Xvfb with a window manager on Linux CI, as the unit suite)."""
        executable = engine_client.find_editor_executable(REPOSITORY_ROOT)
        creator = engine_client.launch_editor(executable, ["--headless", "--renderer", "none", "--automation"],
                                              self.user_data, self.app_name)
        self.editors.append(creator)
        with creator.connect() as client:
            client.call("project.create", {"path": str(self.directory / "Game"), "name": "Game"})
            client.call("session.shutdown")
        creator.wait()
        project_file = self.directory / "Game" / "Game.eproj"
        mode = ["--headless"] if headless else []
        editor_arguments = [*mode, "--renderer", "none", "--project", str(project_file), *arguments]
        listens = headless or "--automation" in arguments
        editor = engine_client.launch_editor(executable, editor_arguments, self.user_data, self.app_name,
                                             wait_for_session=listens)
        self.editors.append(editor)
        return editor, project_file

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_launch_attaches_to_running_editor_with_automation(self) -> None:
        editor, project_file = self.start_project_editor(["--automation"])
        launcher = Launcher(REPOSITORY_ROOT, self.user_data)
        connection = launcher.launch(LaunchRequest(project=project_file))
        self.assertFalse(connection.supervised)
        info = connection.call("session.info", {})
        self.assertEqual(info["result"]["pid"], editor.process.pid)
        # Attached editors are never killed: disconnecting leaves it running.
        connection.disconnect()
        self.assertIsNone(editor.process.poll())

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_launch_reports_editor_already_open_without_automation(self) -> None:
        # A windowed editor without --automation holds the lock and listens nowhere; --frames keeps it running
        # meanwhile.
        editor, project_file = self.start_project_editor(["--frames", "100000"], headless=False)
        # The windowed editor takes the lock some time after it starts; wait for it with a deadline (failing if the
        # editor exits first) and confirm the holder before launching.
        holder = engine_client.wait_for_lock_holder(project_file.parent, editor.process)
        self.assertEqual(holder, editor.process.pid)
        launcher = Launcher(REPOSITORY_ROOT, self.user_data)
        with self.assertRaises(EditorAlreadyOpen) as raised:
            launcher.launch(LaunchRequest(project=project_file))
        self.assertEqual(raised.exception.pid, editor.process.pid)
        payload = raised.exception.to_json()
        self.assertEqual(payload["error"], "EditorAlreadyOpen")
        self.assertIn("Allow AI automation", payload["hint"])


if __name__ == "__main__":
    unittest.main()
