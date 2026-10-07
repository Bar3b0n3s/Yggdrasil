"""Provenance and project.upgrade (Docs/Architecture.md §13.4 "Provenance", §13.12, Roadmap M4)."""

from __future__ import annotations

import json
import unittest

from harness import REPOSITORY_ROOT, AutomationTestCase, read_json


class ProvenanceTests(AutomationTestCase):
    def test_provenance_records_method_request_and_transcript_line(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Board"})
        response = client.request("scene.save", {}, transcript_line=42)
        self.assertIn("result", response)
        provenance = read_json(root / "Automation" / "Provenance.json")
        self.assertEqual(provenance["Format"], "Provenance")
        entries = {entry["Path"]: entry for entry in provenance["Entries"]}
        entry = entries["Assets/Scenes/Main.scene"]
        self.assertEqual(entry["Method"], "scene.save")
        self.assertEqual(entry["RequestId"], response["id"])
        self.assertEqual(entry["Client"], "engine-tests")
        self.assertEqual(entry["TranscriptLine"], 42)
        self.assertEqual(len(entry["XXH64"]), 16)
        self.assertEqual([entry["Path"] for entry in provenance["Entries"]], sorted(entries))
        # Dry runs never record.
        client.call("project.setSettings", {"patch": {"Window": {"Title": "Dry"}}, "dryRun": True})
        self.assertEqual(read_json(root / "Automation" / "Provenance.json"), provenance)

    def test_upgrade_rewrites_and_records_provenance(self) -> None:
        client, root = self.open_editor_with_scene()
        formats = REPOSITORY_ROOT / "Tests" / "Data" / "Formats" / "Scene"
        old_scene = (formats / "v0.scene").read_text(encoding="utf-8")
        upgraded = (formats / "v0.upgraded.scene").read_text(encoding="utf-8")
        target = root / "Assets" / "Scenes" / "Old.scene"
        target.write_bytes(old_scene.encode("utf-8"))

        preview = client.call("project.upgrade", {"dryRun": True})
        self.assertEqual(preview["changedFiles"], ["Assets/Scenes/Old.scene"])
        self.assertEqual(target.read_text(encoding="utf-8"), old_scene)

        result = client.request("project.upgrade", {}, transcript_line=7)
        self.assertEqual(result["result"]["changedFiles"], ["Assets/Scenes/Old.scene"])
        self.assertEqual(target.read_text(encoding="utf-8"), upgraded)
        entries = {entry["Path"]: entry for entry in read_json(root / "Automation" / "Provenance.json")["Entries"]}
        self.assertEqual(entries["Assets/Scenes/Old.scene"]["Method"], "project.upgrade")
        self.assertEqual(entries["Assets/Scenes/Old.scene"]["TranscriptLine"], 7)
        self.assertEqual(client.call("project.upgrade")["changedFiles"], [])

        # The command-line form appends its own transcript lines (client "cli", §13.12).
        client.call("session.shutdown", {"force": True})
        self.assertEqual(self.editors[-1].wait(), 0)  # the lock is free once the editor has exited
        project_file = root / f"{root.name}.eproj"
        exit_code, stderr = self.run_editor(
            ["--headless", "--renderer", "none", "--project", str(project_file), "--upgrade"])
        self.assertEqual(exit_code, 0, stderr)
        transcript = (root / "Automation" / "BuildLog.jsonl").read_text(encoding="utf-8")
        lines = [json.loads(line) for line in transcript.splitlines()]
        self.assertEqual(lines[-2]["type"], "request")
        self.assertEqual(lines[-2]["method"], "project.upgrade")
        self.assertEqual(lines[-2]["client"], "cli")
        self.assertEqual(lines[-1]["type"], "response")
        self.assertEqual(lines[-1]["requestLine"], len(lines) - 1)


if __name__ == "__main__":
    unittest.main()
