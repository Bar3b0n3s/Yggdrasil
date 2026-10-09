"""Basic3D creates a runnable built-in scene through the real project API."""

from __future__ import annotations

import unittest
from pathlib import Path

from harness import AutomationTestCase, read_json


class Basic3dTests(AutomationTestCase):
    def test_basic3d_template_validates_clean(self) -> None:
        client = self.connect(self.start_editor())
        created = client.call("project.create", {
            "path": str(self.directory / "BasicGame"),
            "name": "BasicGame",
            "template": "Basic3D",
        })
        root = Path(created["project"]["root"])
        settings = client.call("project.getSettings")["settings"]
        self.assertEqual(settings["StartScene"], "Assets/Scenes/Main.scene")
        self.assertIn(settings["StartScene"], settings["Export"]["BuildScenes"])
        self.assertIn(settings["StartScene"], created["createdFiles"])
        client.call("scene.open", {"path": settings["StartScene"]})
        report = client.call("project.validate")
        self.assertEqual(report["errorCount"], 0, report["diagnostics"])
        codes = {item["code"] for item in report["diagnostics"]}
        self.assertNotIn("AUDIO_NO_LISTENER", codes)
        self.assertNotIn("AUDIO_MULTIPLE_PRIMARY_LISTENERS", codes)
        scene = read_json(root / settings["StartScene"])
        components = [entity["Components"] for entity in scene["Entities"]]
        for component in ("Camera", "AudioListener", "DirectionalLight", "Environment",
                          "PostProcess", "MeshRenderer", "RigidBody", "BoxCollider"):
            self.assertTrue(any(component in entry for entry in components), component)
        self.assertFalse(any("Script" in entry for entry in components))
        provenance = read_json(root / "Automation/Provenance.json")
        recorded = {entry["Path"]: entry for entry in provenance["Entries"]}
        for path in ("BasicGame.eproj", settings["StartScene"]):
            self.assertEqual(recorded[path]["Method"], "project.create")
        self.assertEqual(list((root / "Assets/Textures").iterdir()), [])
        client.call("session.shutdown", {"force": True})
        self.assertEqual(self.editors[-1].wait(), 0)


if __name__ == "__main__":
    unittest.main()
