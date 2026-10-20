"""Scripts/AuditProvenance.py and Scripts/Lib/provenance.py, the demo projects' provenance audit (Docs/Architecture.md
§13.12, §2.3; Roadmap M16; Docs/Decisions/0026-m16-contract.md decisions 3 to 6 and 9, stream A): the XXH64 port, the
readers of the two committed files, the engine-consumed files, every finding code over the seeded fixtures of
Tests/Data/Provenance/, the exit codes, the static-check wiring, the fresh-clone (index) mode in a temporary git
repository, and that nothing below Library/ is ever read. Only the last class starts an editor: it builds a project
through the MCP bridge's launcher, the regression test of stream C's fixes G1, G2a, G2b and G5 (decisions 10, 11 and
13).
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from harness import EXIT_INIT_FAILED, EXIT_USAGE, REPOSITORY_ROOT, AutomationTestCase, engine_client

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import AuditProvenance  # noqa: E402  (Scripts/ is on sys.path above)
from Lib import games, paths, provenance, scripts, xxh64  # noqa: E402
from Lib.report import Console, Status, Step  # noqa: E402

AUDIT_SCRIPT = REPOSITORY_ROOT / "Scripts" / "AuditProvenance.py"
FIXTURES = REPOSITORY_ROOT / "Tests" / "Data" / "Provenance"
CLEAN = "Clean"
MCP_ROOT = REPOSITORY_ROOT / "Tools" / "MCP"


def run_audit(*arguments: str, cwd: Path = REPOSITORY_ROOT,
              environment: dict[str, str] | None = None) -> tuple[int, dict]:
    """AuditProvenance.py with `arguments` and --json: its exit code and its JSON document. GITHUB_ACTIONS is cleared,
    so a failing step's workflow annotation never precedes the document on stdout (Lib/report.py)."""
    child = {**(os.environ if environment is None else environment), "GITHUB_ACTIONS": ""}
    completed = subprocess.run([sys.executable, str(AUDIT_SCRIPT), *arguments, "--json"], cwd=cwd, capture_output=True,
                               text=True, encoding="utf-8", env=child, timeout=300)
    document = json.loads(completed.stdout) if completed.stdout.strip() else {}
    return completed.returncode, document


def run_main(*arguments: str) -> tuple[int, dict]:
    """AuditProvenance.main in this process with `arguments` and --json, for the tests that patch the library: its exit
    code and its JSON document."""
    output = io.StringIO()
    with mock.patch.dict(os.environ, {"GITHUB_ACTIONS": ""}), contextlib.redirect_stdout(output), \
            contextlib.redirect_stderr(io.StringIO()):
        code = AuditProvenance.main([*arguments, "--json"])
    return code, json.loads(output.getvalue())


def copy_fixture(case: str, destination: Path) -> Path:
    """A copy of Tests/Data/Provenance/<case>/Project at <destination>/<case>; returns the copy's directory."""
    target = destination / case
    shutil.copytree(FIXTURES / case / "Project", target)
    return target


def git(repository: Path, *arguments: str) -> str:
    """git with a fixed identity and no system or user configuration (deterministic, no network)."""
    environment = {**os.environ, "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
                   "GIT_AUTHOR_NAME": "Test", "GIT_AUTHOR_EMAIL": "test@example.invalid",
                   "GIT_COMMITTER_NAME": "Test", "GIT_COMMITTER_EMAIL": "test@example.invalid"}
    completed = subprocess.run(["git", "-C", str(repository), *arguments], capture_output=True, text=True,
                               encoding="utf-8", env=environment, check=True, timeout=120)
    return completed.stdout


def codes(findings: list[provenance.Finding]) -> list[tuple[str, str]]:
    return sorted((finding.code, finding.path) for finding in findings)


class Xxh64Tests(unittest.TestCase):
    def test_xxh64_matches_the_engine_reference_vectors(self) -> None:
        # The independent expected values of Tests/Source/Engine/Core/HashTests.cpp, every tail path included (the same
        # vectors as Tools/MCP/tests/test_autosave_probe.py).
        generator = 2654435761
        buffer = bytearray()
        for _ in range(2367):
            buffer.append(generator >> 56)
            generator = (generator * 11400714785074694797) & ((1 << 64) - 1)
        vectors = {0: 0xEF46DB3751D8E999, 1: 0xE934A84ADB052768, 14: 0x8282DCC4994E35C8,
                   100: 0x4BFE019CD91D9EA4, 222: 0xB641AE8CB691C174, 2367: 0xA82418DDEC0EA581}
        for size, expected in vectors.items():
            with self.subTest(size=size):
                self.assertEqual(xxh64.xxh64(bytes(buffer[:size])), expected)
        self.assertEqual(xxh64.xxh64(b"abc"), 0x44BC2CF5AD770999)

    def test_xxh64_hex_is_sixteen_lowercase_digits(self) -> None:
        self.assertEqual(xxh64.xxh64_hex(b""), "ef46db3751d8e999")
        self.assertEqual(xxh64.xxh64_hex(b"abc"), "44bc2cf5ad770999")


class ReaderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="Provenance-"))
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)

    def test_provenance_entries_read_in_the_editors_pascal_case_spelling(self) -> None:
        path = self.directory / "Provenance.json"
        path.write_text(json.dumps({"Format": "Provenance", "Version": 1, "Entries": [
            {"Path": "Assets/A.scene", "XXH64": "00000000000000aa", "Method": "scene.save", "RequestId": 4,
             "Client": "engine-mcp", "TranscriptLine": 7},
            {"Path": "Game.eproj", "XXH64": "00000000000000bb", "Method": "project.upgrade", "RequestId": None,
             "Client": "cli", "TranscriptLine": None}]}, indent="\t"), encoding="utf-8")
        entries = provenance.load_provenance(path)
        self.assertEqual(entries[0], provenance.ProvenanceEntry("Assets/A.scene", "00000000000000aa", "scene.save", 4,
                                                                "engine-mcp", 7))
        self.assertEqual(entries[1].request_id, None)
        self.assertEqual(entries[1].transcript_line, None)
        with self.assertRaises(FileNotFoundError):
            provenance.load_provenance(self.directory / "Missing.json")

    def test_malformed_provenance_documents_are_refused_naming_a_pointer(self) -> None:
        entry = {"Path": "Assets/A.scene", "XXH64": "00000000000000aa", "Method": "scene.save", "RequestId": 4,
                 "Client": "engine-mcp", "TranscriptLine": 7}
        later = {**entry, "Path": "Assets/B.scene"}
        documents = {
            "format": {"Format": "Other", "Version": 1, "Entries": []},
            "version": {"Format": "Provenance", "Version": 2, "Entries": []},
            "unknown member": {"Format": "Provenance", "Version": 1, "Entries": [{**entry, "Extra": 1}]},
            "missing member": {"Format": "Provenance", "Version": 1, "Entries": [
                {key: value for key, value in entry.items() if key != "Client"}]},
            "boolean id": {"Format": "Provenance", "Version": 1, "Entries": [{**entry, "RequestId": True}]},
            "short hash": {"Format": "Provenance", "Version": 1, "Entries": [{**entry, "XXH64": "aa"}]},
            "line zero": {"Format": "Provenance", "Version": 1, "Entries": [{**entry, "TranscriptLine": 0}]},
            "unsorted": {"Format": "Provenance", "Version": 1, "Entries": [later, entry]},
            "duplicate path": {"Format": "Provenance", "Version": 1, "Entries": [entry, entry]},
        }
        texts = {name: json.dumps(document) for name, document in documents.items()}
        texts["duplicate key"] = '{"Format": "Provenance", "Format": "Provenance", "Version": 1, "Entries": []}'
        texts["NaN"] = ('{"Format": "Provenance", "Version": NaN, "Entries": []}')
        for name, text in texts.items():
            with self.subTest(defect=name):
                path = self.directory / "Provenance.json"
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError) as raised:
                    provenance.load_provenance(path)
                self.assertIn("Provenance.json", str(raised.exception))

    def test_transcript_lines_are_split_on_newline_bytes_only(self) -> None:
        path = self.directory / "BuildLog.jsonl"
        request = {"type": "request", "time": "2026-01-01T00:00:00.000Z", "client": "engine-mcp", "id": 1,
                   "method": "script.write", "params": {"source": "-- a b c\u0085d"}}
        body = json.dumps(request, ensure_ascii=False).encode("utf-8")
        path.write_bytes(body + b"\n" + b'{"type": "resp' + b"\n" + body + b"\n" + b'{"torn')
        transcript = provenance.load_transcript(path)
        self.assertEqual([line.number for line in transcript.lines], [1, 2, 3, 4])
        self.assertEqual(transcript.lines[0].record, request)
        self.assertIsNone(transcript.lines[1].record)
        self.assertTrue(transcript.lines[1].terminated)
        self.assertEqual(transcript.lines[2].record, request)
        self.assertIsNone(transcript.lines[3].record)
        self.assertFalse(transcript.lines[3].terminated)
        path.write_bytes(b"")
        self.assertEqual(provenance.load_transcript(path).lines, ())

    def test_engine_consumed_files_are_the_root_project_file_luaurc_and_assets(self) -> None:
        project = self.directory / "Game"
        for relative in ("Game.eproj", ".luaurc", ".gitignore", "AGENTS.md", "README.md", "Notes.txt",
                         "Assets/Scenes/Main.scene", "Assets/Scenes/Main.scene.meta", "Assets/Scenes/Main.scene.bak",
                         "Automation/Provenance.json", "Automation/BuildLog.jsonl", "Library/Cache/x.bin",
                         "Build/Windows-Dist/Game/Game.exe", "Sub/Other.eproj"):
            (project / relative).parent.mkdir(parents=True, exist_ok=True)
            (project / relative).write_bytes(b"x")
        self.assertEqual(provenance.enumerate_engine_files(project, "disk"),
                         [".luaurc", "Assets/Scenes/Main.scene", "Assets/Scenes/Main.scene.meta", "Game.eproj"])
        self.assertEqual(provenance.enumerate_engine_files(project, "index"),
                         [".luaurc", "Assets/Scenes/Main.scene", "Assets/Scenes/Main.scene.bak",
                          "Assets/Scenes/Main.scene.meta", "Game.eproj"])
        with self.assertRaises(ValueError):
            provenance.enumerate_engine_files(project, "git")

    @unittest.skip("contract stub: the audit of Native/ (Docs/Decisions/0030-m19-contract.md decision 35, stream N-D)")
    def test_native_sources_are_engine_consumed(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 35 (the milestone's acceptance: "the provenance audit covers the
        # module's sources"): every file below Native/ is engine-consumed like every file below Assets/, so a source the
        # editor did not write (native.write) is a finding; the builds below Library/Native/ are never read.
        project = self.directory / "Game"
        for relative in ("Game.eproj", "Assets/Scenes/Main.scene", "Native/Spinner.cpp", "Native/Systems/Move.h",
                         "Native/Notes.txt", "Library/Native/Builds/00000001/NativeBuild.json"):
            (project / relative).parent.mkdir(parents=True, exist_ok=True)
            (project / relative).write_bytes(b"x")
        self.assertEqual(provenance.enumerate_engine_files(project, "disk"),
                         ["Assets/Scenes/Main.scene", "Game.eproj", "Native/Notes.txt", "Native/Spinner.cpp",
                          "Native/Systems/Move.h"])


class FixtureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="ProvenanceFixtures-"))
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)

    def test_the_clean_fixture_passes_with_its_counts(self) -> None:
        project = FIXTURES / CLEAN / "Project"
        audit = provenance.audit_project(project, "disk")
        self.assertEqual(audit.findings, [])
        self.assertEqual(audit.warnings, [])
        self.assertEqual(provenance.audit_exit_code(audit), provenance.EXIT_PASSED)
        self.assertEqual(sorted(audit.counts), ["entries", "files", "requests", "transcriptLines"])
        self.assertGreater(audit.counts["files"], 0)
        self.assertEqual(audit.counts["files"], audit.counts["entries"])
        # Clean holds every passing behaviour of decision 4 (decision 6), so an audit that refused one fails here: raw
        # U+2028 and U+0085 inside a line, a request without a response line (a killed bridge), a failed edit.batch
        # whose writes are recorded (rolled back), and a cli project.upgrade entry without a transcript line.
        transcript = provenance.load_transcript(project / provenance.TRANSCRIPT_FILE)
        raw = b"\n".join(line.raw for line in transcript.lines)
        self.assertIn("\u2028".encode("utf-8"), raw)
        self.assertIn("\u0085".encode("utf-8"), raw)
        requests = {line.number: line.record for line in transcript.lines
                    if line.record is not None and line.record.get("type") == "request"}
        responses = [line.record for line in transcript.lines
                     if line.record is not None and line.record.get("type") == "response"]
        self.assertTrue(set(requests) - {response.get("requestLine") for response in responses})
        failed_batches = {response["requestLine"] for response in responses if response.get("ok") is False
                          and requests.get(response.get("requestLine"), {}).get("method") == "edit.batch"}
        entries = provenance.load_provenance(project / provenance.PROVENANCE_FILE)
        self.assertTrue(any(entry.transcript_line in failed_batches for entry in entries))
        self.assertTrue(any(entry.method == "project.upgrade" and entry.client == "cli"
                            and entry.transcript_line is None for entry in entries))

    def test_each_seeded_fixture_has_exactly_its_findings_and_warnings(self) -> None:
        cases = sorted(path.name for path in FIXTURES.iterdir() if (path / "Expected.json").is_file())
        self.assertIn(CLEAN, cases)
        for case in cases:
            with self.subTest(case=case):
                expected = json.loads((FIXTURES / case / "Expected.json").read_text(encoding="utf-8"))
                self.assertEqual((expected["Format"], expected["Version"]), ("ProvenanceAuditFixture", 1))
                audit = provenance.audit_project(FIXTURES / case / "Project", "disk")
                self.assertEqual(provenance.audit_exit_code(audit), expected["ExitCode"])
                self.assertEqual(codes(audit.findings), sorted((item["Code"], item["Path"])
                                                               for item in expected["Findings"]))
                self.assertEqual(codes(audit.warnings), sorted((item["Code"], item["Path"])
                                                               for item in expected["Warnings"]))
                if case != CLEAN:
                    self.assertTrue(audit.findings or audit.warnings, "a seeded fixture must have a finding")

    def test_every_finding_and_warning_code_has_a_seeded_fixture(self) -> None:
        seen: set[str] = set()
        for expected_path in FIXTURES.glob("*/Expected.json"):
            expected = json.loads(expected_path.read_text(encoding="utf-8"))
            seen.update(item["Code"] for item in expected["Findings"] + expected["Warnings"])
        # crlf cannot be committed (.gitattributes normalizes it): test_crlf_text_is_a_warning builds it. The index-only
        # path-collision cannot be audited in disk mode: FreshCloneTests builds it.
        self.assertEqual(provenance.INDEX_ONLY_CODES, ("path-collision",))
        self.assertEqual(sorted(set(provenance.FINDING_CODES + provenance.WARNING_CODES) - seen),
                         ["crlf", "path-collision"])

    def test_the_self_test_passes(self) -> None:
        exit_code, document = run_audit("--self-test")
        self.assertEqual(exit_code, 0, document)
        self.assertTrue(document["success"])
        self.assertTrue(all(step["status"] == "passed" for step in document["steps"]))

    def test_the_self_test_fails_on_a_changed_expectation_and_on_a_code_without_a_case(self) -> None:
        fixtures = self.directory / "Fixtures"
        shutil.copytree(FIXTURES, fixtures)
        cases_by_code: dict[str, list[str]] = {}
        for expected_path in sorted(fixtures.glob("*/Expected.json")):
            expected = json.loads(expected_path.read_text(encoding="utf-8"))
            for code in {item["Code"] for item in expected["Findings"]}:
                cases_by_code.setdefault(code, []).append(expected_path.parent.name)
        # One finding code's only case removed, and Clean's expectation changed.
        code, cases = next((code, cases) for code, cases in sorted(cases_by_code.items()) if len(cases) == 1)
        shutil.rmtree(fixtures / cases[0])
        clean = fixtures / CLEAN / "Expected.json"
        expected = json.loads(clean.read_text(encoding="utf-8"))
        expected["ExitCode"] = provenance.EXIT_FINDINGS
        clean.write_text(json.dumps(expected, indent="\t") + "\n", encoding="utf-8")
        steps = AuditProvenance.self_test(fixtures)
        failed = [step for step in steps if step.status == Status.FAILED]
        self.assertTrue(any(step.name == f"provenance self-test {CLEAN}" for step in failed), steps)
        coverage = next(step for step in failed if step.name == "provenance self-test coverage")
        self.assertIn(code, coverage.detail)
        self.assertTrue(all(step.status == Status.PASSED for step in steps if step not in failed))

    def test_missing_required_files_exit_3(self) -> None:
        project = copy_fixture(CLEAN, self.directory)
        cases = {"provenance-missing": project / provenance.PROVENANCE_FILE,
                 "transcript-missing": project / provenance.TRANSCRIPT_FILE}
        for code, path in cases.items():
            with self.subTest(code=code):
                saved = path.read_bytes()
                path.unlink()
                exit_code, document = run_audit("--project", str(project), "--files", "disk")
                path.write_bytes(saved)
                self.assertEqual(exit_code, EXIT_INIT_FAILED)
                self.assertIn(code, [finding["code"] for finding in document["projects"][0]["findings"]])
        for missing in (self.directory / "Nowhere", self.directory / "Empty"):
            with self.subTest(project=missing.name):
                if missing.name == "Empty":
                    missing.mkdir()
                exit_code, document = run_audit("--project", str(missing), "--files", "disk")
                self.assertEqual(exit_code, EXIT_INIT_FAILED)
                self.assertEqual([finding["code"] for finding in document["projects"][0]["findings"]],
                                 ["project-missing"])

    def test_several_projects_exit_with_the_first_failing_projects_code(self) -> None:
        missing = self.directory / "Nowhere"
        hash_mismatch = copy_fixture("HashMismatch", self.directory)
        for order, expected in (((missing, hash_mismatch), EXIT_INIT_FAILED), ((hash_mismatch, missing), 1)):
            with self.subTest(first=order[0].name):
                arguments = [argument for project in order for argument in ("--project", str(project))]
                exit_code, document = run_audit(*arguments, "--files", "disk")
                self.assertEqual(exit_code, expected)
                self.assertEqual([step["status"] for step in document["steps"]], ["failed", "failed"])

    def test_a_directory_with_several_project_files_fails_the_demos_step(self) -> None:
        with mock.patch.object(games, "discover_projects",
                               side_effect=ValueError("Projects/Twice holds 2 *.eproj files")):
            exit_code, document = run_main("--demos")
        self.assertEqual(exit_code, 1)
        self.assertFalse(document["success"])
        self.assertEqual([(step["name"], step["status"]) for step in document["steps"]],
                         [("provenance demos", "failed")])
        self.assertIn("Projects/Twice", document["steps"][0]["detail"])

    def test_usage_errors_exit_2(self) -> None:
        for arguments in ([], ["--self-test", "--project", "X"], ["--demos", "--self-test"],
                          ["--self-test", "--files", "disk"], ["--project", "X", "--files", "git"]):
            with self.subTest(arguments=arguments):
                completed = subprocess.run([sys.executable, str(AUDIT_SCRIPT), *arguments], cwd=REPOSITORY_ROOT,
                                           capture_output=True, text=True, timeout=120)
                self.assertEqual(completed.returncode, EXIT_USAGE)

    def test_library_is_never_read(self) -> None:
        project = copy_fixture(CLEAN, self.directory)
        baseline_code, baseline = run_audit("--project", str(project), "--files", "disk")
        # Decoys that would change the result if anything below Library/ were read.
        for relative, content in (("Library/Automation/Provenance.json", "{}"),
                                  ("Library/Automation/BuildLog.jsonl", "not json\n"),
                                  ("Library/Assets/Extra.scene", "{}"),
                                  ("Library/Cache/AssetLocations.json", "garbage")):
            (project / relative).parent.mkdir(parents=True, exist_ok=True)
            (project / relative).write_text(content, encoding="utf-8")
        exit_code, document = run_audit("--project", str(project), "--files", "disk")
        self.assertEqual((exit_code, document["projects"]), (baseline_code, baseline["projects"]))

    def test_crlf_text_is_a_warning(self) -> None:
        project = copy_fixture(CLEAN, self.directory)
        audit = provenance.audit_project(project, "disk")
        script = next(path for path in provenance.enumerate_engine_files(project, "disk") if path.endswith(".luau"))
        # The file rewritten with CR LF and its entry re-hashed to match: rule 1 holds, and only the warning remains.
        data = (project / script).read_bytes().replace(b"\n", b"\r\n")
        (project / script).write_bytes(data)
        document = json.loads((project / provenance.PROVENANCE_FILE).read_text(encoding="utf-8"))
        for entry in document["Entries"]:
            if entry["Path"] == script:
                entry["XXH64"] = xxh64.xxh64_hex(data)
        (project / provenance.PROVENANCE_FILE).write_text(json.dumps(document, indent="\t") + "\n", encoding="utf-8")
        audit = provenance.audit_project(project, "disk")
        self.assertEqual(audit.findings, [])
        self.assertEqual(codes(audit.warnings), [("crlf", script)])

    def test_a_torn_line_is_a_warning_until_an_entry_references_it(self) -> None:
        project = copy_fixture(CLEAN, self.directory)
        transcript = project / provenance.TRANSCRIPT_FILE
        transcript.write_bytes(transcript.read_bytes() + b'{"type": "request", "ti')
        audit = provenance.audit_project(project, "disk")
        self.assertEqual(audit.findings, [])
        self.assertEqual(codes(audit.warnings), [("torn-line", provenance.TRANSCRIPT_FILE)])
        torn = len(provenance.load_transcript(transcript).lines)
        document = json.loads((project / provenance.PROVENANCE_FILE).read_text(encoding="utf-8"))
        document["Entries"][0]["TranscriptLine"] = torn
        (project / provenance.PROVENANCE_FILE).write_text(json.dumps(document, indent="\t") + "\n", encoding="utf-8")
        audit = provenance.audit_project(project, "disk")
        self.assertIn(("torn-line", document["Entries"][0]["Path"]), codes(audit.findings))

    def test_the_json_report_lists_each_project_with_its_counts_and_findings(self) -> None:
        clean = copy_fixture(CLEAN, self.directory)
        hash_mismatch = copy_fixture("HashMismatch", self.directory)
        exit_code, document = run_audit("--project", str(clean), "--project", str(hash_mismatch), "--files", "disk")
        self.assertEqual(exit_code, 1)
        self.assertFalse(document["success"])
        self.assertEqual([project["passed"] for project in document["projects"]], [True, False])
        for project in document["projects"]:
            self.assertEqual(sorted(project), ["counts", "files", "findings", "passed", "project", "warnings"])
            self.assertEqual(project["files"], "disk")
        self.assertEqual([finding["code"] for finding in document["projects"][1]["findings"]], ["hash-mismatch"])
        self.assertEqual([step["status"] for step in document["steps"]], ["passed", "failed"])

    def test_demos_audits_every_staged_game_project_and_names_the_unstaged_ones(self) -> None:
        # Docs/Decisions/0027-m17-contract.md decision 4, B3 (M17 pipeline map G3): the case this one replaced,
        # test_demos_audits_every_discovered_game_project, audited the working tree (--demos --files disk), so a
        # half-built demo that is not staged (Tetris during M17) failed every commit's automation suite on its
        # working-tree state, against ADR 0026 decisions 3, 7 and 9. --demos in its default index mode audits the staged
        # projects, each of which must pass, and names each project found only on disk in one passed step.
        exit_code, document = run_audit("--demos")
        self.assertEqual(exit_code, 0, document)
        staged = games.discover_projects(REPOSITORY_ROOT, "index")
        directories = {project.directory for project in staged}
        unstaged = [project for project in games.discover_projects(REPOSITORY_ROOT, "disk")
                    if project.directory not in directories]
        self.assertEqual(len(document["projects"]), len(staged))
        self.assertTrue(all(project["passed"] and project["files"] == "index" for project in document["projects"]),
                        document["projects"])
        for project in unstaged:
            with self.subTest(unstaged=project.name):
                name = f"provenance {paths.display_path(project.directory)}"
                notes = [step for step in document["steps"] if step["name"] == name]
                self.assertEqual([step["status"] for step in notes], ["passed"])
                self.assertIn("not staged", notes[0]["detail"])
        if not staged and not unstaged:
            self.assertEqual([step["name"] for step in document["steps"]], ["provenance demos"])


class StaticCheckTests(unittest.TestCase):
    def test_the_static_checks_run_the_audit_self_test_and_not_the_demo_audit(self) -> None:
        calls: list[tuple[str, str, list[str]]] = []

        def recorded(name: str, script: str, arguments: list[str], *_: object, **__: object) -> Step:
            calls.append((name, script, list(arguments)))
            return Step(name, Status.PASSED)

        with mock.patch.object(scripts, "run_script", recorded):
            scripts.run_static_checks(Console(json_mode=True), lambda step: step)
        self.assertIn(("provenance audit self-test", "AuditProvenance.py", ["--self-test"]), calls)
        self.assertFalse([call for call in calls if call[1] == "AuditProvenance.py" and "--demos" in call[2]])


class FreshCloneTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="ProvenanceClone-"))
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        self.repository = self.directory / "Repository"
        self.repository.mkdir()
        git(self.repository, "init", "--quiet")
        (self.repository / ".gitattributes").write_text("* text=auto eol=lf\n", encoding="utf-8")
        (self.repository / ".gitignore").write_text("*.bak\nLibrary/\nBuild/\n", encoding="utf-8")
        self.project = copy_fixture(CLEAN, self.repository)
        git(self.repository, "add", "--all")
        git(self.repository, "commit", "--quiet", "--message", "Clean project")

    def test_index_mode_audits_the_staged_blobs_not_the_working_tree(self) -> None:
        self.assertEqual(provenance.audit_project(self.project, "index").findings, [])
        # Ignored and untracked files are not in a fresh clone, so they change nothing.
        scene = next(path for path in provenance.enumerate_engine_files(self.project, "disk")
                     if path.endswith(".scene"))
        (self.project / (scene + ".bak")).write_bytes(b"backup")
        (self.project / "Library" / "x.bin").parent.mkdir(parents=True, exist_ok=True)
        (self.project / "Library" / "x.bin").write_bytes(b"cache")
        (self.project / "Assets" / "Untracked.material").write_text("{}", encoding="utf-8")
        self.assertEqual(provenance.audit_project(self.project, "index").findings, [])
        self.assertIn(("entry-missing", "Assets/Untracked.material"),
                      codes(provenance.audit_project(self.project, "disk").findings))
        # A tracked file edited but not staged: the index still holds the recorded bytes.
        (self.project / scene).write_bytes((self.project / scene).read_bytes() + b"\n")
        self.assertEqual(provenance.audit_project(self.project, "index").findings, [])
        git(self.repository, "add", "--", str(self.project / scene))
        self.assertEqual(codes(provenance.audit_project(self.project, "index").findings), [("hash-mismatch", scene)])

    def test_index_mode_holds_what_a_fresh_clone_checks_out(self) -> None:
        files = provenance.enumerate_engine_files(self.project, "disk")
        script = next(path for path in files if path.endswith(".luau"))
        # Bytes the editor wrote with CR LF, recorded as written: eol=lf normalizes them in the index, so a clone (and
        # index mode) holds other bytes than provenance hashed ...
        data = (self.project / script).read_bytes().replace(b"\n", b"\r\n")
        (self.project / script).write_bytes(data)
        document = json.loads((self.project / provenance.PROVENANCE_FILE).read_text(encoding="utf-8"))
        for entry in document["Entries"]:
            if entry["Path"] == script:
                entry["XXH64"] = xxh64.xxh64_hex(data)
        (self.project / provenance.PROVENANCE_FILE).write_text(json.dumps(document, indent="\t") + "\n",
                                                               encoding="utf-8")
        git(self.repository, "add", "--all")
        git(self.repository, "commit", "--quiet", "--message", "CRLF script")
        self.assertEqual(codes(provenance.audit_project(self.project, "index").findings), [("hash-mismatch", script)])
        clone = self.directory / "Clone"
        subprocess.run(["git", "clone", "--quiet", str(self.repository), str(clone)], check=True, capture_output=True,
                       timeout=120)
        cloned = clone / self.project.name
        self.assertEqual(codes(provenance.audit_project(cloned, "disk").findings), [("hash-mismatch", script)])
        # ... unless .gitattributes keeps the project's files byte for byte (-text), as the demo projects' rules do.
        (self.repository / ".gitattributes").write_text(f"* text=auto eol=lf\n/{self.project.name}/** -text\n",
                                                        encoding="utf-8")
        git(self.repository, "add", "--renormalize", "--all")
        git(self.repository, "commit", "--quiet", "--message", "Keep the project's bytes")
        self.assertEqual(provenance.audit_project(self.project, "index").findings, [])

    def test_index_mode_reads_the_staged_provenance_and_transcript(self) -> None:
        # Unstaged changes to the two committed files are not what a fresh clone holds, so index mode ignores them.
        (self.project / provenance.PROVENANCE_FILE).write_text("{}", encoding="utf-8")
        transcript = self.project / provenance.TRANSCRIPT_FILE
        transcript.write_bytes(transcript.read_bytes() + b'{"type": "request", "ti')
        audit = provenance.audit_project(self.project, "index")
        self.assertEqual((audit.findings, audit.warnings), ([], []))
        self.assertIn("provenance-invalid",
                      [finding.code for finding in provenance.audit_project(self.project, "disk").findings])

    def test_the_cli_audits_the_index_by_default_and_takes_a_project_file(self) -> None:
        project_file = next(self.project.glob("*.eproj"))
        self.assertEqual(AuditProvenance.resolve_project(str(project_file)), self.project.resolve())
        self.assertEqual(AuditProvenance.resolve_project(str(self.project)), self.project.resolve())
        exit_code, document = run_audit("--project", str(project_file))
        self.assertEqual(exit_code, 0, document)
        self.assertEqual([project["files"] for project in document["projects"]], ["index"])
        # The index holds the recorded bytes, so an unstaged hand edit changes nothing by default.
        scene = next(path for path in provenance.enumerate_engine_files(self.project, "disk")
                     if path.endswith(".scene"))
        (self.project / scene).write_bytes((self.project / scene).read_bytes() + b"\n")
        self.assertEqual(run_audit("--project", str(self.project))[0], 0)

    def test_a_missing_or_unstaged_project_is_project_missing_in_index_mode(self) -> None:
        untracked = copy_fixture("HashMismatch", self.repository)
        for project in (self.repository / "Nowhere", untracked):
            with self.subTest(project=project.name):
                exit_code, document = run_audit("--project", str(project))
                self.assertEqual(exit_code, EXIT_INIT_FAILED)
                self.assertEqual([finding["code"] for finding in document["projects"][0]["findings"]],
                                 ["project-missing"])

    def test_index_mode_outside_a_repository_exits_3(self) -> None:
        outside = copy_fixture(CLEAN, self.directory / "Outside")
        # No repository above the copy is found, whatever holds the temporary directory.
        environment = {**os.environ, "GIT_CEILING_DIRECTORIES": str(self.directory)}
        exit_code, document = run_audit("--project", str(outside), environment=environment)
        self.assertEqual(exit_code, EXIT_INIT_FAILED)
        self.assertEqual([step["status"] for step in document["steps"]], ["failed"])
        self.assertEqual(run_audit("--project", str(outside), "--files", "disk", environment=environment)[0], 0)

    def test_demos_audit_only_staged_projects_in_index_mode_and_name_the_others(self) -> None:
        staged = copy_fixture(CLEAN, self.repository / "Tests" / "Projects")
        git(self.repository, "add", "--all")
        unstaged = copy_fixture("HashMismatch", self.repository / "Projects")
        discover = games.discover_projects

        def discover_in_repository(root: Path, files: str = "disk") -> list[games.GameProject]:
            return discover(self.repository, files)

        with mock.patch.object(games, "discover_projects", side_effect=discover_in_repository):
            exit_code, document = run_main("--demos")
            self.assertEqual(exit_code, 0, document)
            self.assertEqual([project["passed"] for project in document["projects"]], [True])
            notes = [step for step in document["steps"] if unstaged.name in step["name"]]
            self.assertEqual([step["status"] for step in notes], ["passed"])
            self.assertIn("not staged", notes[0]["detail"])
            self.assertTrue(any(staged.name in step["name"] and step["status"] == "passed"
                                for step in document["steps"]))
            # Disk mode audits every project on disk, the unstaged one included.
            exit_code, document = run_main("--demos", "--files", "disk")
            self.assertEqual(exit_code, 1)
            self.assertEqual(sorted(project["passed"] for project in document["projects"]), [False, True])

    def test_staged_paths_that_differ_only_in_letter_case_are_a_path_collision(self) -> None:
        # The milestone review's R07: on a case-insensitive file system the materialized copy holds one file for both
        # paths, so a hand-added Ball.luau staged beside the recorded ball.luau was never audited. Both are staged here
        # through the index alone (update-index --cacheinfo), as a commit made on Linux would bring them.
        files = provenance.enumerate_engine_files(self.project, "disk")
        script = next(path for path in files if path.endswith(".luau"))
        folded = Path(script).with_name(Path(script).name.swapcase()).as_posix()
        blob = git(self.repository, "hash-object", "-w", str(self.project / script)).strip()
        # Paths that differ only in Unicode normalization (NFC "é" against NFD "e" and a combining accent) collide too:
        # a macOS file system holds one file for them.
        # Escapes keep the two spellings visible; precomposition is off because git on macOS would otherwise turn the
        # NFD command-line path into NFC and stage one path twice.
        nfc, nfd = "Assets/Caf\u00e9.material", "Assets/Cafe\u0301.material"
        for relative in (folded, nfc, nfd):
            git(self.repository, "-c", "core.precomposeUnicode=false", "update-index", "--add", "--cacheinfo",
                f"100644,{blob},{self.project.name}/{relative}")
        audit = provenance.audit_project(self.project, "index")
        collisions = {finding.path: finding.detail for finding in audit.findings if finding.code == "path-collision"}
        self.assertEqual(sorted(collisions), sorted([script, folded, nfc, nfd]))
        for path, other in ((script, folded), (folded, script), (nfc, nfd), (nfd, nfc)):
            self.assertIn(other, collisions[path])
            self.assertIn("case-insensitive file system", collisions[path])
        self.assertEqual(provenance.audit_exit_code(audit), provenance.EXIT_FINDINGS)
        # Disk mode audits the working tree, which holds one spelling only.
        self.assertNotIn("path-collision",
                         [finding.code for finding in provenance.audit_project(self.project, "disk").findings])

    def test_materialize_index_writes_only_the_projects_staged_files(self) -> None:
        (self.repository / "Other.txt").write_text("outside the project", encoding="utf-8")
        git(self.repository, "add", "Other.txt")
        (self.project / "Untracked.txt").write_text("untracked", encoding="utf-8")
        destination = self.directory / "Copy"
        copy = provenance.materialize_index(self.repository, self.project, destination)
        self.assertEqual(copy, destination / self.project.name)
        listed = sorted(path.relative_to(destination).as_posix() for path in destination.rglob("*") if path.is_file())
        tracked = sorted(git(self.repository, "ls-files", "--", self.project.name).splitlines())
        self.assertEqual(listed, tracked)
        with self.assertRaises(ValueError):
            provenance.materialize_index(self.repository, self.directory, self.directory / "Copy2")

    def test_materialize_index_applies_only_the_gitattributes_conversions_whatever_the_git_configuration(self) -> None:
        # .gitattributes' own conversions apply: text=auto without an eol everywhere, eol=crlf for one file.
        notes = self.project / "Notes.txt"
        notes.write_bytes(b"first\nsecond\n")
        (self.repository / ".gitattributes").write_text(f"* text=auto\n/{self.project.name}/Notes.txt text eol=crlf\n",
                                                        encoding="utf-8")
        git(self.repository, "add", "--all")
        # Configuration that converts line endings on checkout (here the repository's own, which outranks the user's
        # and the system's: Git for Windows' system configuration sets core.autocrlf=true): materialize_index
        # overrides all of it.
        user_attributes = self.directory / "UserAttributes"
        user_attributes.write_text("* text eol=crlf\n", encoding="utf-8")
        for key, value in (("core.autocrlf", "true"), ("core.eol", "crlf"),
                           ("core.attributesFile", user_attributes.as_posix())):
            git(self.repository, "config", key, value)
        destination = self.directory / "Copy"
        copy = provenance.materialize_index(self.repository, self.project, destination)
        relative = self.project.relative_to(self.repository).as_posix()
        staged = git(self.repository, "ls-files", "-z", "--", relative).split("\0")
        for path in filter(None, staged):
            with self.subTest(path=path):
                written = (destination / path).read_bytes()
                if path == f"{relative}/Notes.txt":
                    self.assertEqual(written, b"first\r\nsecond\r\n")
                    continue
                blob = subprocess.run(["git", "-C", str(self.repository), "cat-file", "blob", f":{path}"],
                                      capture_output=True, check=True, timeout=120).stdout
                self.assertEqual(written, blob)
        self.assertEqual(provenance.audit_project(self.project, "index").findings, [])
        self.assertTrue((copy / "Notes.txt").is_file())

    def test_materialize_index_ignores_the_system_wide_gitattributes_file(self) -> None:
        # The system-wide $(prefix)/etc/gitattributes cannot be replaced without administrator rights, so the test
        # checks that both git commands run with GIT_ATTR_NOSYSTEM=1, the variable that makes git skip it.
        git(self.repository, "add", "--all")
        real_run = subprocess.run
        environments: list[dict[str, str] | None] = []

        def recorded(command: list[str], *arguments: object, **options: object) -> subprocess.CompletedProcess:
            if command[:1] == ["git"]:
                environments.append(options.get("env"))  # type: ignore[arg-type]
            return real_run(command, *arguments, **options)  # type: ignore[arg-type]

        with mock.patch.object(provenance.subprocess, "run", recorded):
            provenance.materialize_index(self.repository, self.project, self.directory / "Copy")
        self.assertEqual(len(environments), 2)
        for environment in environments:
            self.assertIsNotNone(environment)
            assert environment is not None
            self.assertEqual(environment.get("GIT_ATTR_NOSYSTEM"), "1")


class BridgeAuditTests(AutomationTestCase):
    """A project built through the bridge's launcher, with the real transcript (ENGINE_MCP_TRANSCRIPT unset), passes the
    audit: the regression test of decision 10 (G1: a glTF's .meta rewritten after asset.import returned), decision 11
    (G2a: the bridge reopens an existing project through project.open; G2b: --upgrade's deferred scan) and decision 13's
    G5 (the Empty template's root .luaurc, which project.create writes, is recorded). Without G1 or G5 the audit fails.
    G2a and G2b attribute the .meta files an open's scan writes, so build_project copies a source without a .meta into
    Assets/ before --upgrade and another before the bridge reopens the project, and checks that each .meta is recorded
    under the transcribed request that wrote it (cli project.refreshAssets, project.open); without G2a or G2b that check
    fails. The copied sources themselves were written by hand, so the reopened session deletes them through automation
    before the audit."""

    def setUp(self) -> None:
        super().setUp()
        sys.path.insert(0, str(MCP_ROOT))
        sys.path.insert(0, str(REPOSITORY_ROOT / "Tools" / "Automation"))
        environment = {key: value for key, value in os.environ.items() if key != "ENGINE_MCP_TRANSCRIPT"}
        patch = mock.patch.dict(os.environ, environment, clear=True)
        patch.start()
        self.addCleanup(patch.stop)

    def build_project(self) -> Path:
        from engine_mcp.launcher import Launcher, LaunchRequest  # Tools/MCP is on sys.path (setUp)

        launcher = Launcher(REPOSITORY_ROOT, self.user_data, engine_client.configurations_from_environment())
        project = self.directory / "Game"
        gltf = (REPOSITORY_ROOT / "Tests" / "Data" / "Assets" / "Gltf" / "Textured.gltf").as_posix()
        connection = launcher.launch(LaunchRequest(project=project, create=True))
        try:
            for method, params in (
                    ("scene.new", {"path": "Assets/Scenes/Main.scene"}),
                    ("entity.create", {"name": "Paddle"}),
                    ("asset.create", {"type": "material", "path": "Assets/Materials/Red.material"}),
                    ("script.write", {"path": "Assets/Scripts/Ball.luau", "source": "--!strict\nreturn {}\n"}),
                    ("asset.import", {"source": gltf, "destDir": "Assets/Models"}),
                    ("asset.move", {"asset": "Assets/Materials/Red.material", "path": "Assets/Red.material"}),
                    ("project.setSettings", {"patch": {"StartScene": "Assets/Scenes/Main.scene"}}),
                    ("scene.save", {})):
                self.assertIn("result", connection.call(method, params), method)
        finally:
            self.assertTrue(connection.shutdown(force=True)["shutDown"])
        # G2b: --upgrade defers its asset scan to the transcribed cli project.refreshAssets, which writes the .meta of a
        # source that has none.
        upgraded = self.copy_source(project, "Assets/Materials/Upgraded.material")
        exit_code, output = self.run_editor(["--headless", "--renderer", "none", "--project",
                                             str(project / "Game.eproj"), "--upgrade"])
        self.assertEqual(exit_code, 0, output)
        self.assert_recorded_by(project, upgraded + ".meta", "project.refreshAssets", "cli")
        # G2a: reopening the existing project goes through project.open, whose open-time writes it transcribes.
        reopened = self.copy_source(project, "Assets/Materials/Reopened.material")
        connection = launcher.launch(LaunchRequest(project=project, create=False))
        try:
            self.assert_recorded_by(project, reopened + ".meta", "project.open", "engine-mcp")
            for source in (upgraded, reopened):
                self.assertIn("result", connection.call("asset.delete", {"asset": source}), source)
        finally:
            self.assertTrue(connection.shutdown(force=True)["shutDown"])
        return project

    @staticmethod
    def copy_source(project: Path, relative: str) -> str:
        """A copy of the project's Assets/Red.material at `relative`, made outside the editor and without a .meta, so
        the next open's asset scan writes its .meta; returns `relative`."""
        target = project / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(project / "Assets" / "Red.material", target)
        return relative

    def assert_recorded_by(self, project: Path, relative: str, method: str, client: str) -> None:
        """`relative` has a provenance entry of `method` and `client` whose transcript line is that request's."""
        entries = {entry.path: entry for entry in provenance.load_provenance(project / provenance.PROVENANCE_FILE)}
        self.assertIn(relative, entries)
        entry = entries[relative]
        self.assertEqual((entry.method, entry.client), (method, client), entry)
        self.assertIsNotNone(entry.transcript_line, entry)
        transcript = provenance.load_transcript(project / provenance.TRANSCRIPT_FILE)
        record = transcript.lines[(entry.transcript_line or 0) - 1].record or {}
        self.assertEqual((record.get("type"), record.get("method"), record.get("client")),
                         ("request", method, client), record)

    def test_a_project_built_through_the_bridge_passes_the_audit(self) -> None:
        project = self.build_project()
        audit = provenance.audit_project(project, "disk")
        self.assertEqual(audit.findings, [])
        entries = provenance.load_provenance(project / provenance.PROVENANCE_FILE)
        self.assertNotIn("ui", {entry.method for entry in entries})
        self.assertIn("asset.import", {entry.method for entry in entries if entry.path.endswith(".gltf.meta")})
        # --upgrade adds its own cli lines and keeps the audit green.
        exit_code, output = self.run_editor(["--headless", "--renderer", "none", "--project",
                                             str(project / "Game.eproj"), "--upgrade"])
        self.assertEqual(exit_code, 0, output)
        self.assertEqual(provenance.audit_project(project, "disk").findings, [])

    def test_a_hand_edit_after_the_build_fails_with_hash_mismatch(self) -> None:
        project = self.build_project()
        material = project / "Assets" / "Red.material"
        material.write_text(material.read_text(encoding="utf-8").replace("\t", "  "), encoding="utf-8")
        exit_code, document = run_audit("--project", str(project), "--files", "disk")
        self.assertEqual(exit_code, 1)
        self.assertEqual([(finding["code"], finding["path"]) for finding in document["projects"][0]["findings"]],
                         [("hash-mismatch", "Assets/Red.material")])


if __name__ == "__main__":
    unittest.main()
