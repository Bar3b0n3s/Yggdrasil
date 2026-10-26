"""M10 editor state, camera parity and fresh screenshots through actual host frames."""

from __future__ import annotations

import json
import time
import unittest
from pathlib import Path

from harness import (AutomationTestCase, GPU_EDITOR_ARGUMENTS, app_name, editor_executable,
                     engine_client)

DEFAULT_PANELS = ["SceneHierarchy", "Inspector", "SceneViewport", "GameViewport", "ContentBrowser", "Console"]


class EditorUiTests(AutomationTestCase):
    """All child processes disable audio; windowed coverage requires the platform display."""

    def test_basic3d_template_is_available_through_project_create(self) -> None:
        client = self.connect(self.start_editor())
        root = self.directory / "Basic3D"
        created = client.call("project.create", {"path": str(root), "name": "Basic3D", "template": "Basic3D"})
        self.assertIn("Assets/Scenes/Main.scene", created["createdFiles"])
        client.call("scene.open", {"path": "Assets/Scenes/Main.scene"})
        validation = client.call("project.validate", {"scope": "scene"})
        self.assertEqual(validation["errorCount"], 0, validation)
        self.assertFalse(any(item["code"] == "AUDIO_NO_LISTENER" for item in validation["diagnostics"]))
        self.assertEqual(client.call("entity.get", {"entity": "/Camera"})["entity"]["name"], "Camera")

    def write_automation_preference(self, allowed: bool) -> Path:
        """A real user's preferences, with unrelated state that must survive startup."""
        path = self.user_data / app_name() / "Editor.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"Format": "EditorPreferences", "Version": 1,
                                    "AllowAiAutomation": allowed, "RecentProjects": [],
                                    "FuturePreference": {"Keep": 23}}), encoding="utf-8")
        return path

    def test_attach_to_windowed_editor_with_automation_allowed(self) -> None:
        preferences = self.write_automation_preference(True)
        # Deliberately neither --headless nor --automation: only the saved preference opts in.
        editor = engine_client.launch_editor(
            editor_executable(), ["--renderer", "none", "--audio-device", "none",
                                  self.engine_cache_argument()], self.user_data, app_name())
        self.editors.append(editor)
        client = self.connect(editor)
        self.assertIsNotNone(editor.session)
        self.assertEqual(editor.session.pid, editor.process.pid)
        self.assertFalse(client.call("session.info")["headless"])
        self.create_project(client)
        client.call("scene.new", {"path": "Assets/Scenes/Attached.scene"})
        created = client.call("entity.create", {"name": "AttachedToHumanEditor"})
        self.assertEqual(created["entity"]["name"], "AttachedToHumanEditor")
        saved = json.loads(preferences.read_text(encoding="utf-8"))
        self.assertTrue(saved["AllowAiAutomation"])
        self.assertEqual(saved["FuturePreference"], {"Keep": 23})
        client.call("session.shutdown", {"force": True})
        self.assertEqual(editor.wait(), 0, editor.output())
        self.assertFalse(engine_client.list_sessions(engine_client.sessions_directory(app_name(), self.user_data)))

    def test_attach_to_headless_editor_with_automation_allowed(self) -> None:
        """The headless null-platform editor with the preference set. A headless editor listens whatever the preference
        says (MCP launches rely on it, ADR 0008 decision 14), so this proves attachment and that startup leaves the
        saved preference alone; only the windowed test above can prove that the preference itself opts an editor in."""
        for allowed in (True, False):
            with self.subTest(allowed=allowed):
                preferences = self.write_automation_preference(allowed)
                editor = engine_client.launch_editor(
                    editor_executable(), ["--headless", "--renderer", "none", "--audio-device", "none",
                                          self.engine_cache_argument()], self.user_data, app_name())
                self.editors.append(editor)
                client = self.connect(editor)
                self.assertIsNotNone(editor.session)
                self.assertEqual(editor.session.pid, editor.process.pid)
                self.assertTrue(client.call("session.info")["headless"])
                self.create_project(client, f"Headless{allowed}")
                client.call("scene.new", {"path": "Assets/Scenes/Attached.scene"})
                created = client.call("entity.create", {"name": "AttachedToHeadlessEditor"})
                self.assertEqual(created["entity"]["name"], "AttachedToHeadlessEditor")
                saved = json.loads(preferences.read_text(encoding="utf-8"))
                self.assertEqual(saved["AllowAiAutomation"], allowed)
                self.assertEqual(saved["FuturePreference"], {"Keep": 23})
                client.call("session.shutdown", {"force": True})
                self.assertEqual(editor.wait(), 0, editor.output())
                self.assertFalse(engine_client.list_sessions(engine_client.sessions_directory(app_name(),
                                                                                             self.user_data)))

    def test_explicit_automation_overrides_disabled_preference(self) -> None:
        preferences = self.write_automation_preference(False)
        editor = engine_client.launch_editor(
            editor_executable(), ["--renderer", "none", "--audio-device", "none", "--automation",
                                  self.engine_cache_argument()], self.user_data, app_name())
        self.editors.append(editor)
        client = self.connect(editor)
        self.assertFalse(client.call("session.info")["headless"])
        self.assertFalse(json.loads(preferences.read_text(encoding="utf-8"))["AllowAiAutomation"])
        client.call("session.shutdown")
        self.assertEqual(editor.wait(), 0, editor.output())

    def test_reference_dump_ignores_saved_listener_preference(self) -> None:
        preferences = self.write_automation_preference(True)
        original = preferences.read_bytes()
        # A file in place of the session directory makes an attempted listener startup fail observably.
        # Checking only for absent session files after exit would also pass if a listener started and stopped.
        sessions = engine_client.sessions_directory(app_name(), self.user_data)
        sessions.parent.mkdir(parents=True, exist_ok=True)
        sessions.write_text("listener must not start", encoding="utf-8")
        output = self.directory / "Reference"
        code, error = self.run_editor(["--headless", "--renderer", "none", "--audio-device", "none",
                                       "--dump-reference", str(output)])
        self.assertEqual(code, 0, error)
        self.assertTrue((output / "catalog.json").is_file())
        self.assertEqual(preferences.read_bytes(), original)
        self.assertEqual(sessions.read_text(encoding="utf-8"), "listener must not start")

    def test_windowed_editor_screenshot_with_explicit_automation(self) -> None:
        if not self.require_gpu():
            return
        editor = engine_client.launch_editor(editor_executable(),
            ["--renderer", "vulkan", "--automation", "--audio-device", "none",
             self.engine_cache_argument(), *GPU_EDITOR_ARGUMENTS], self.user_data, app_name())
        self.editors.append(editor)
        client = self.connect(editor)
        self.create_project(client)
        client.call("scene.new", {"path": "Assets/Scenes/Main.scene"})
        state = client.call("editor.state")
        self.assertEqual(state["mode"], "Edit")
        shot = client.call("editor.screenshot")
        self.assertTrue(Path(shot["path"]).is_file())
        self.assertGreater(client.call("editor.state")["uiFrame"], 0)
        client.call("session.shutdown", {"force": True})
        self.assertEqual(editor.wait(), 0)

    def test_editor_state_tracks_panels_selection_and_lockstep(self) -> None:
        client, _ = self.open_editor_with_scene()
        created = client.call("entity.create", {"name": "Selected"})
        client.call("edit.select", {"entities": [created["entity"]["id"]]})
        state = client.call("editor.state")
        # The default layout of an open project, in stable panel order; the launcher closed when the project opened.
        self.assertEqual(state["openPanels"], DEFAULT_PANELS)
        self.assertEqual(state["selection"][0]["name"], "Selected")
        self.assertEqual(state["selectionTarget"], "Edit")
        self.assertEqual(state["uiFrame"], 0)
        self.assertEqual(state["lockstepOwner"], "")
        client.call("play.start", {"lockstep": True})
        playing = client.call("editor.state")
        self.assertEqual(playing["mode"], "Play")
        self.assertTrue(playing["lockstepOwner"])
        self.assertEqual(playing["openPanels"], DEFAULT_PANELS)
        client.call("play.stop")
        before = client.call("viewport.camera")
        changed = client.call("viewport.camera", {"position": [6, 4, 8]})
        self.assertEqual(changed["target"], before["target"])
        framed = client.call("viewport.frame", {"entities": ["/Selected"]})
        self.assertEqual(framed["entities"][0]["name"], "Selected")
        options = client.call("viewport.setOptions", {"grid": False, "colliders": True})
        self.assertFalse(options["grid"])
        self.assertTrue(options["colliders"])
        self.assertTrue(options["icons"])

    def test_host_statistics_report_real_timing_without_a_renderer(self) -> None:
        client, _ = self.open_editor_with_scene()
        deadline = time.monotonic() + 30
        while True:
            stats = client.call("stats.get")
            if stats["fps"] > 0 or time.monotonic() >= deadline:
                break
        self.assertGreater(stats["fps"], 0)
        self.assertGreater(stats["cpuMilliseconds"], 0)
        self.assertEqual(stats["droppedSeconds"], 0)
        self.assertEqual([view["name"] for view in stats["views"]], ["scene", "game"])
        self.assertFalse(any(view["available"] for view in stats["views"]))
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 2, "render": "none"})
        self.assertEqual(client.call("stats.get")["droppedSeconds"], 0)
        client.call("play.stop")
        self.assertEqual(client.call("stats.get")["droppedSeconds"], 0)

    def test_editor_screenshot_after_pipelined_mutation_is_fresh(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        previous = client.call("editor.screenshot")
        prior_frame = client.call("editor.state")["uiFrame"]
        prior_pixels = Path(previous["path"]).read_bytes()
        messages = [
            {"jsonrpc": "2.0", "id": 91001, "method": "entity.create", "params": {"name": "VisibleInThisFrame"}},
            {"jsonrpc": "2.0", "id": 91002, "method": "editor.screenshot", "params": {}},
        ]
        client.send_raw(b"".join(engine_client.encode_frame(json.dumps(message).encode("utf-8"))
                                 for message in messages))
        responses = {}
        while len(responses) < 2:
            response = client.receive_message()
            if response.get("id") in (91001, 91002):
                responses[response["id"]] = response
        for response in responses.values():
            self.assertNotIn("error", response)
        self.assertGreater(client.call("editor.state")["uiFrame"], prior_frame)
        screenshot = responses[91002]["result"]
        self.assertNotEqual(Path(screenshot["path"]).read_bytes(), prior_pixels)

    def test_scene_changed_on_disk_flag_holds_until_reload(self) -> None:
        """The flag the SceneChangedOnDisk banner shows; the banner itself is drawn in EditorLayerTests."""
        client, root = self.open_editor_with_scene()
        source = root / "Assets/Scenes/Main.scene"
        document = json.loads(source.read_text(encoding="utf-8"))
        document["Name"] = "ChangedExternally"
        source.write_text(json.dumps(document), encoding="utf-8")
        # Bounded state wait, not an elapsed-time assertion; detection may happen on any subsequent safe point.
        deadline = time.monotonic() + 30
        while True:
            state = client.call("editor.state")
            if state["sceneChangedOnDisk"] or time.monotonic() >= deadline:
                break
        self.assertTrue(state["sceneChangedOnDisk"])
        self.assertNotEqual(client.call("project.info")["scene"]["name"], "ChangedExternally")
        client.call("scene.open", {"path": "Assets/Scenes/Main.scene", "discardChanges": True, "reload": True})
        self.assertFalse(client.call("editor.state")["sceneChangedOnDisk"])

    def test_editor_state_describes_the_active_scene_tab(self) -> None:
        """The panels show the active scene tab (ADR 0031 decision 20), and so does editor.state, the UI state agents
        read: each open scene keeps its selection and its changed-on-disk flag, which editor.state reports while its tab
        is active; the open panels do not change with the tab."""
        client, root = self.open_editor_with_scene()
        main = "Assets/Scenes/Main.scene"
        second = "Assets/Scenes/Second.scene"
        created = client.call("entity.create", {"name": "SelectedInMain"})
        client.call("edit.select", {"entities": [created["entity"]["id"]]})
        client.call("scene.new", {"path": second, "keepOpen": True})
        state = client.call("editor.state")
        self.assertEqual(state["selection"], [])
        self.assertEqual(state["openPanels"], DEFAULT_PANELS)
        client.call("scene.activate", {"scene": main})
        state = client.call("editor.state")
        self.assertEqual([entity["name"] for entity in state["selection"]], ["SelectedInMain"])
        self.assertEqual(state["selectionTarget"], "Edit")
        self.assertEqual(state["openPanels"], DEFAULT_PANELS)

        # Second's file changes on disk while Main's tab is active: only Second's tab carries the flag.
        source = root / second
        document = json.loads(source.read_text(encoding="utf-8"))
        document["Name"] = "ChangedExternally"
        source.write_text(json.dumps(document), encoding="utf-8")
        # Bounded state wait, not an elapsed-time assertion; detection may happen on any subsequent safe point.
        deadline = time.monotonic() + 30
        while True:
            scenes = {entry["scene"]["path"]: entry for entry in client.call("scene.list")["scenes"]}
            if scenes[second]["changedOnDisk"] or time.monotonic() >= deadline:
                break
        self.assertTrue(scenes[second]["changedOnDisk"])
        self.assertFalse(client.call("editor.state")["sceneChangedOnDisk"])
        client.call("scene.activate", {"scene": second})
        self.assertTrue(client.call("editor.state")["sceneChangedOnDisk"])
        self.assertEqual(client.call("editor.state")["selection"], [])


if __name__ == "__main__":
    unittest.main()
