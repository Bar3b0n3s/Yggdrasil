"""Autosave and recovery through the editor host; CPU interleavings live in AutosaveTests."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from harness import AutomationTestCase, engine_client


class AutosaveTests(AutomationTestCase):
    """A crash must preserve a complete dirty generation without rewriting its source."""

    def make_recovery(self) -> tuple[Path, bytes]:
        client, root = self.open_editor_with_scene(name=f"Recovery{len(self.editors)}")
        source = root / "Assets/Scenes/Main.scene"
        before = source.read_bytes()
        client.call("entity.create", {"name": "RecoverMe"})
        # BeforePlay is synchronous and uses the same writer as the injected periodic clock.
        client.call("play.start", {"lockstep": True})
        manifest = root / "Library/Autosave/Manifest.json"
        self.assertTrue(manifest.is_file())
        self.assertEqual(source.read_bytes(), before)
        client.close()
        self.editors[-1].kill()
        return root, before

    def open_recovery(self, root: Path, recover: bool = True) -> tuple[engine_client.EngineClient, dict]:
        client = self.connect(self.start_editor())
        result = client.call("project.open", {"path": str(root), "recover": recover})
        return client, result

    def test_autosave_and_recovery_after_kill(self) -> None:
        root, before = self.make_recovery()
        client, opened = self.open_recovery(root)
        self.assertTrue(opened["recoveryAvailable"])
        self.assertTrue(opened["recovered"])
        self.assertTrue(client.call("session.info")["_meta"]["dirty"])
        self.assertEqual(client.call("scene.query", {"where": {"name": "RecoverMe"}})["total"], 1)
        self.assertEqual((root / "Assets/Scenes/Main.scene").read_bytes(), before)

    def test_preplay_autosave_failure_keeps_the_edit_scene_and_can_retry(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "KeepThisEdit"})
        source = root / "Assets/Scenes/Main.scene"
        before = source.read_bytes()
        blocker = root / "Library/Autosave"
        blocker.write_text("not a directory", encoding="utf-8")
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("play.start", {"lockstep": True})
        self.assertEqual(refused.exception.data["errorCode"], "AlreadyExists")
        self.assertEqual(client.call("editor.state")["mode"], "Edit")
        self.assertTrue(client.call("session.info")["_meta"]["dirty"])
        self.assertEqual(client.call("scene.query", {"where": {"name": "KeepThisEdit"}})["total"], 1)
        self.assertEqual(source.read_bytes(), before)
        self.assertEqual(blocker.read_text(encoding="utf-8"), "not a directory")
        blocker.unlink()
        client.call("play.start", {"lockstep": True})
        self.assertTrue((blocker / "Manifest.json").is_file())
        client.call("play.stop")

    def test_explicit_save_cleans_only_the_recovered_scene_generation(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "FirstRecovery"})
        client.call("play.start", {"lockstep": True})
        client.call("play.stop")
        manifest = root / "Library/Autosave/Manifest.json"
        first = json.loads(manifest.read_text(encoding="utf-8"))["Generations"][0]["Generation"]
        client.call("scene.new", {"path": "Assets/Scenes/Other.scene", "discardChanges": True})
        client.call("entity.create", {"name": "OtherRecovery"})
        client.call("play.start", {"lockstep": True})
        client.close()
        self.editors[-1].kill()
        recovered, result = self.open_recovery(root)
        self.assertTrue(result["recovered"])
        self.assertEqual(recovered.call("scene.query", {"where": {"name": "OtherRecovery"}})["total"], 1)
        recovered.call("scene.save")
        self.assertFalse(recovered.call("session.info")["_meta"]["dirty"])
        retained = json.loads(manifest.read_text(encoding="utf-8"))["Generations"]
        self.assertEqual([entry["Generation"] for entry in retained], [first])
        self.assertIn("OtherRecovery", (root / "Assets/Scenes/Other.scene").read_text(encoding="utf-8"))
        recovered.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), 0)
        reopened, result = self.open_recovery(root)
        self.assertTrue(result["recovered"])
        self.assertEqual(reopened.call("scene.query", {"where": {"name": "FirstRecovery"}})["total"], 1)

    def test_malformed_recovery_does_not_block_normal_project_launch(self) -> None:
        client, root = self.open_editor_with_scene()
        source = root / "Assets/Scenes/Main.scene"
        before = source.read_bytes()
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), 0)
        manifest = root / "Library/Autosave/Manifest.json"
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text("{broken", encoding="utf-8")
        editor = self.start_editor(project=root)
        opened = self.connect(editor)
        opened.call("scene.open", {"path": "Assets/Scenes/Main.scene"})
        self.assertFalse(opened.call("project.info")["scene"]["dirty"])
        self.assertEqual(source.read_bytes(), before)
        self.assertEqual(manifest.read_text(encoding="utf-8"), "{broken")

    def test_autosave_read_only_writes_nothing(self) -> None:
        root, _ = self.make_recovery()
        before = {path: path.read_bytes() for path in root.rglob("*") if path.is_file()}
        reader = self.connect(self.start_editor(["--read-only"], project=root))
        reader.call("scene.open", {"path": "Assets/Scenes/Main.scene"})
        reader.call("editor.state")
        with self.assertRaises(engine_client.EngineError) as refused:
            reader.call("project.open", {"path": str(root), "recover": True})
        self.assert_engine_error(refused.exception, engine_client.UNAUTHORIZED, "PermissionDenied")
        self.assertEqual({path: path.read_bytes() for path in root.rglob("*") if path.is_file()}, before)

    def test_recovery_refuses_replaced_project_and_escaping_paths(self) -> None:
        root, _ = self.make_recovery()
        project = root / f"{root.name}.eproj"
        original = project.read_bytes()
        project.write_bytes(original + b"\n")
        client = self.connect(self.start_editor())
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("project.open", {"path": str(root), "recover": True})
        self.assertEqual(refused.exception.data["errorCode"], "Conflict")
        self.assertIsNone(engine_client.read_lock_holder(root))
        # A separate project supplies an unchanged fingerprint for the path-confinement check.
        other_root, _ = self.make_recovery()
        manifest = json.loads((other_root / "Library/Autosave/Manifest.json").read_text(encoding="utf-8"))
        generation = manifest["Generations"][0]["Generation"]
        metadata_path = other_root / "Library/Autosave" / generation / "Metadata.json"
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        metadata["ScenePath"] = "../Escape.scene"
        metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
        with self.assertRaises(engine_client.EngineError):
            client.call("project.open", {"path": str(other_root), "recover": True})
        self.assertFalse((self.directory / "Escape.scene").exists())

    def test_device_loss_autosaves_current_dirty_scene(self) -> None:
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene(renderer="vulkan")
        editor = self.editors[-1]
        source = root / "Assets/Scenes/Main.scene"
        before = source.read_bytes()
        client.call("entity.create", {"name": "UnsavedBeforeDeviceLoss"})
        self.assertTrue(client.call("session.info")["_meta"]["dirty"])
        self.assertFalse((root / "Library/Autosave/Manifest.json").exists())
        try:
            queued = client.call("debug.deviceLost")
            self.assertTrue(queued["queued"])
        except engine_client.ConnectionClosed:
            # The fatal submission can precede the I/O thread flushing the queued response. The process exit and
            # recovered bytes below distinguish that permitted ordering from an unrelated disconnect.
            pass
        self.assertEqual(editor.wait(), 4, editor.output())
        self.assertIn("Fatal error (DeviceLost)", editor.output())
        gpu_messages = [line for line in editor.output().splitlines()
                        if ("Vulkan validation:" in line or "NVRHI:" in line)
                        and any(level in line for level in ("[warning]", "[error]", "[critical]"))]
        self.assertFalse(gpu_messages, editor.output())
        self.assertEqual(source.read_bytes(), before)
        restored, result = self.open_recovery(root)
        self.assertTrue(result["recovered"], editor.output())
        self.assertTrue(restored.call("session.info")["_meta"]["dirty"])
        self.assertEqual(restored.call("scene.query", {"where": {"name": "UnsavedBeforeDeviceLoss"}})["total"], 1)
        self.assertEqual(source.read_bytes(), before)

    def test_device_loss_hook_is_unavailable_without_test_hooks_or_renderer(self) -> None:
        editor = self.start_editor(test_hooks=False)
        client = self.connect(editor)
        with self.assertRaises(engine_client.EngineError) as unavailable:
            client.call("debug.deviceLost")
        self.assertEqual(unavailable.exception.code, -32601)
        headless, _ = self.open_editor_with_scene()
        with self.assertRaises(engine_client.EngineError) as unsupported:
            headless.call("debug.deviceLost")
        self.assert_engine_error(unsupported.exception, engine_client.UNSUPPORTED, "Unsupported")

    def test_recovery_freshness_survives_restart_with_equal_timestamps(self) -> None:
        root, before = self.make_recovery()
        source = root / "Assets/Scenes/Main.scene"
        timestamp = source.stat().st_mtime_ns
        client, result = self.open_recovery(root)
        self.assertTrue(result["recovered"])
        self.assertEqual(source.stat().st_mtime_ns, timestamp)
        self.assertEqual(source.read_bytes(), before)
        self.assertTrue(client.call("session.info")["_meta"]["dirty"])

    def test_project_open_recover_false_is_not_an_inspection_only_call(self) -> None:
        root, before = self.make_recovery()
        client, opened = self.open_recovery(root, False)
        self.assertTrue(opened["recoveryAvailable"])
        self.assertFalse(opened["recovered"])
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("project.open", {"path": str(root), "recover": True})
        self.assert_engine_error(refused.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertEqual(engine_client.read_lock_holder(root), client.call("session.info")["pid"])
        self.assertEqual((root / "Assets/Scenes/Main.scene").read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
