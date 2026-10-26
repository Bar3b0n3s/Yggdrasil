"""Provenance and project.upgrade (Docs/Architecture.md §13.4 "Provenance", §13.12, Roadmap M4), and the repository
rules that keep a demo's committed files auditable from a fresh clone (Docs/Decisions/0026-m16-contract.md decision
13)."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness import REPOSITORY_ROOT, AutomationTestCase, read_json

GIT_TIMEOUT_SECONDS = 60.0


def transcript_lines(path: Path) -> list[dict]:
    """The JSON objects of a transcript, split on b"\\n" only like the writers count lines (a raw U+2028 or U+0085
    inside a line must not split it, which str.splitlines would)."""
    data = path.read_bytes()
    return [json.loads(line.decode("utf-8")) for line in data.split(b"\n") if line]


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
        # M16 friction S3-09: each rewritten file comes with its reason.
        self.assertEqual(preview["changes"], [{"path": "Assets/Scenes/Old.scene", "reason": "format version 0 to 1"}])
        self.assertEqual(target.read_text(encoding="utf-8"), old_scene)

        result = client.request("project.upgrade", {}, transcript_line=7)
        self.assertEqual(result["result"]["changedFiles"], ["Assets/Scenes/Old.scene"])
        self.assertEqual(target.read_text(encoding="utf-8"), upgraded)
        entries = {entry["Path"]: entry for entry in read_json(root / "Automation" / "Provenance.json")["Entries"]}
        self.assertEqual(entries["Assets/Scenes/Old.scene"]["Method"], "project.upgrade")
        self.assertEqual(entries["Assets/Scenes/Old.scene"]["TranscriptLine"], 7)
        self.assertEqual(client.call("project.upgrade")["changedFiles"], [])

        # The command-line form appends its own transcript lines (client "cli", §13.12): the deferred asset scan's
        # project.refreshAssets, then project.upgrade, each with its response (ADR 0026 decision 11).
        client.call("session.shutdown", {"force": True})
        self.assertEqual(self.editors[-1].wait(), 0)  # the lock is free once the editor has exited
        project_file = root / f"{root.name}.eproj"
        exit_code, stderr = self.run_editor(
            ["--headless", "--renderer", "none", "--project", str(project_file), "--upgrade"])
        self.assertEqual(exit_code, 0, stderr)
        lines = transcript_lines(root / "Automation" / "BuildLog.jsonl")
        self.assertEqual([line["method"] for line in lines[-4::2]], ["project.refreshAssets", "project.upgrade"])
        for request_line in (len(lines) - 3, len(lines) - 1):
            request = lines[request_line - 1]
            response = lines[request_line]
            self.assertEqual(request["type"], "request")
            self.assertEqual(request["client"], "cli")
            self.assertEqual(response["type"], "response")
            self.assertEqual(response["requestLine"], request_line)
            self.assertTrue(response["ok"])


def run_git(arguments: list[str], cwd: Path, data: bytes | None = None) -> bytes:
    """Runs git with `arguments` in `cwd` and returns its standard output; fails the calling test on an error."""
    completed = subprocess.run(["git", *arguments], cwd=cwd, input=data, capture_output=True,
                               timeout=GIT_TIMEOUT_SECONDS, check=False)
    if completed.returncode != 0:
        raise AssertionError(f"git {' '.join(arguments)} failed: {completed.stderr.decode('utf-8', errors='replace')}")
    return completed.stdout


class RepositoryRulesTests(unittest.TestCase):
    """The repository's rules for game projects (ADR 0026 decision 13), checked without an editor."""

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="EngineRepositoryRules-")).resolve()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)

    def test_demo_engine_files_are_stored_byte_for_byte(self) -> None:
        # G7: the engine-consumed files of a game (the .eproj, the root .luaurc, everything below Assets/ and, since
        # ADR 0030 decision 35, the native module's sources below Native/) are -text, so a fresh clone holds the bytes
        # provenance hashed; the exempt files keep the repository's normalization.
        engine_files = ["Tests/Projects/Breakout/Breakout.eproj", "Tests/Projects/Breakout/.luaurc",
                        "Tests/Projects/Breakout/Assets/Scripts/Ball.luau",
                        "Tests/Projects/Breakout/Assets/Models/A.gltf", "Projects/Tetris/Tetris.eproj",
                        "Projects/Tetris/.luaurc", "Projects/Tetris/Assets/Scenes/Main.scene.meta",
                        "Tests/Projects/NativeTest/Native/Spinner.cpp", "Projects/Tetris/Native/Systems/Move.h"]
        exempt_files = ["Tests/Projects/Breakout/AGENTS.md", "Tests/Projects/Breakout/Automation/BuildLog.jsonl",
                        "Tests/Projects/Breakout/Automation/Provenance.json", "Projects/Tetris/.gitignore"]
        output = run_git(["check-attr", "text", "--", *engine_files, *exempt_files], REPOSITORY_ROOT).decode("utf-8")
        values = {}
        for line in output.splitlines():
            path, _, value = line.rpartition(": ")
            values[path.removesuffix(": text")] = value
        for path in engine_files:
            self.assertEqual(values.get(path), "unset", f"{path}: {output}")
        for path in exempt_files:
            self.assertEqual(values.get(path), "auto", f"{path}: {output}")

        # The effect: staged CR LF text below a demo's Assets/ is checked out unchanged, while an exempt text file is
        # normalized to LF as before. A scratch repository with the repository's .gitattributes stages both and
        # materializes the index the way the audit's index mode does (checkout-index).
        repository = self.directory / "Clone"
        repository.mkdir()
        run_git(["init", "-q"], repository)
        shutil.copyfile(REPOSITORY_ROOT / ".gitattributes", repository / ".gitattributes")
        script = Path("Tests/Projects/Demo/Assets/Scripts/Game.luau")
        notes = Path("Tests/Projects/Demo/AGENTS.md")
        crlf = b"local speed = 8\r\nreturn speed\r\n"
        for relative in (script, notes):
            (repository / relative).parent.mkdir(parents=True, exist_ok=True)
            (repository / relative).write_bytes(crlf)
        run_git(["-c", "core.autocrlf=true", "-c", "core.safecrlf=false", "add", "--", script.as_posix(),
                 notes.as_posix()], repository)
        copy = self.directory / "Copy"
        run_git(["checkout-index", "-f", f"--prefix={copy.as_posix()}/", "--", script.as_posix(), notes.as_posix()],
                repository)
        self.assertEqual((copy / script).read_bytes(), crlf)
        self.assertEqual((copy / notes).read_bytes(), crlf.replace(b"\r\n", b"\n"))

    def test_torn_transcript_is_no_lint_finding(self) -> None:
        # G8: a game's transcript may hold a torn line that a killed writer left and the next writer completed in place
        # (its number must not change); AuditProvenance.py validates transcripts, so Lint's strict JSONL check leaves
        # Automation/BuildLog.jsonl out. Another torn .jsonl file is still a finding, so the check did run.
        project = self.directory / "Tree" / "Tests" / "Projects" / "Demo"
        (project / "Automation").mkdir(parents=True)
        (project / "Automation" / "BuildLog.jsonl").write_bytes(b'{"type": "request", "id": 1}\n{"type": "req\n')
        (project / "Other.jsonl").write_bytes(b'{"a": 1}\n{"b\n')
        completed = subprocess.run(
            [sys.executable, str(REPOSITORY_ROOT / "Scripts" / "Lint.py"), "--steps", "json", "--root",
             str(self.directory / "Tree"), "--json"],
            capture_output=True, timeout=GIT_TIMEOUT_SECONDS, check=False)
        report = json.loads(completed.stdout.decode("utf-8"))
        step = next(step for step in report["steps"] if step["name"] == "json")
        files = sorted({finding["file"] for finding in step["findings"]})
        self.assertEqual(files, ["Tests/Projects/Demo/Other.jsonl"], json.dumps(step, indent=1))
        self.assertEqual(completed.returncode, 1)


if __name__ == "__main__":
    unittest.main()
