"""M18's golden tooling (Docs/Decisions/0029-m18-contract.md, W18.2 stream D2; Roadmap M18's acceptance): the golden
scaffolds and the kept content of Scripts/Scaffold.py, the provenance of Projects/FeatureTest after T-FTPROV's recorded
regeneration, and Scripts/Test.py's report of the device-class check. No editor runs."""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import Scaffold as scaffold  # noqa: E402  (Scripts/ is on sys.path above)
import Test as test_script  # noqa: E402
from Lib import xxh64  # noqa: E402
from Lib.report import Console  # noqa: E402

RENDERER_II_SCAFFOLD = "GoldenRendererII.jsonl"
RENDERER_II_DIRECTORIES = ("Assets/Materials/GoldenRendererII", "Assets/Scenes/GoldenRendererII")


def temporary_project(case: unittest.TestCase, prefix: str) -> Path:
    project = Path(tempfile.mkdtemp(prefix=prefix))
    case.addCleanup(shutil.rmtree, project, ignore_errors=True)
    return project


class GoldenScaffoldManifestTests(unittest.TestCase):
    """Scaffold.py's reading of golden scaffolds in Scaffold/Scaffolds.json."""

    def write_manifest(self, entries: list[dict[str, object]]) -> Path:
        project = temporary_project(self, "GoldenManifest-")
        (project / "Scaffold").mkdir()
        for entry in entries:
            (project / "Scaffold" / str(entry["File"])).write_text("", encoding="utf-8")
        (project / scaffold.MANIFEST).write_text(json.dumps({"Format": scaffold.MANIFEST_FORMAT,
                                                             "Version": scaffold.MANIFEST_VERSION,
                                                             "Scaffolds": entries}), encoding="utf-8")
        return project

    def assert_refused(self, entries: list[dict[str, object]], *named: str) -> None:
        project = self.write_manifest(entries)
        with self.assertRaises(scaffold.ManifestError) as raised:
            scaffold.read_manifest(project)
        for text in named:
            self.assertIn(text, str(raised.exception))

    def test_the_committed_golden_scaffold_owns_its_directories_outside_the_kept_ones(self) -> None:
        golden = {entry.file: entry for entry in scaffold.read_manifest(scaffold.PROJECT) if entry.golden_directories}
        self.assertIn(RENDERER_II_SCAFFOLD, golden)
        # Docs/Decisions/0029-m18-contract.md decision 22: W18.2's golden scaffolds take IdSeeds 1610 to 1619.
        self.assertTrue(1610 <= golden[RENDERER_II_SCAFFOLD].id_seed <= 1619)
        self.assertEqual(golden[RENDERER_II_SCAFFOLD].golden_directories, RENDERER_II_DIRECTORIES)
        for entry in golden.values():
            for directory in entry.golden_directories:
                for kept in scaffold.KEPT_ASSET_DIRECTORIES:
                    self.assertFalse(scaffold.overlaps(directory, kept), f"{entry.file}: {directory} and {kept}")

    def test_a_golden_scaffold_reads_with_its_directories_and_any_other_without(self) -> None:
        project = self.write_manifest([
            {"File": "A.jsonl", "IdSeed": 1, "Renderer": "none"},
            {"File": "B.jsonl", "IdSeed": 2, "Renderer": "vulkan",
             "GoldenDirectories": ["Assets/Scenes/GoldenB", "Assets/Textures/GoldenB.Set"]}])
        scaffolds = scaffold.read_manifest(project)
        self.assertEqual(scaffolds[0].golden_directories, ())
        self.assertEqual(scaffolds[1].golden_directories, ("Assets/Scenes/GoldenB", "Assets/Textures/GoldenB.Set"))

    def test_a_golden_directory_that_overlaps_a_kept_directory_is_refused(self) -> None:
        # Golden.jsonl's kept outputs are never removed, so a golden scaffold could not be regenerated there; letter
        # case does not tell directories apart on a case-insensitive file system.
        for directory, kept in (("Assets/Scenes/Golden", "Assets/Scenes/Golden"),
                                ("Assets/Materials/Golden/More", "Assets/Materials/Golden"),
                                ("Assets/Textures", "Assets/Textures/Golden"),
                                ("Assets/scenes/golden", "Assets/Scenes/Golden")):
            with self.subTest(directory=directory):
                self.assert_refused([{"File": "A.jsonl", "IdSeed": 1, "Renderer": "none",
                                      "GoldenDirectories": [directory]}], "Scaffolds[0]", directory, kept)

    def test_golden_directories_of_two_scaffolds_that_overlap_are_refused_naming_both(self) -> None:
        self.assert_refused([{"File": "A.jsonl", "IdSeed": 1, "Renderer": "none",
                              "GoldenDirectories": ["Assets/Scenes/GoldenA"]},
                             {"File": "B.jsonl", "IdSeed": 2, "Renderer": "none"},
                             {"File": "C.jsonl", "IdSeed": 3, "Renderer": "none",
                              "GoldenDirectories": ["Assets/Materials/GoldenC", "Assets/Scenes/GoldenA/C"]}],
                            "Scaffolds[0]", "A.jsonl", "Scaffolds[2]", "C.jsonl", "Assets/Scenes/GoldenA")

    def test_overlapping_directories_of_one_golden_scaffold_are_refused(self) -> None:
        for directories in (["Assets/Scenes/GoldenA", "Assets/Scenes/GoldenA"],
                            ["Assets/Scenes/GoldenA", "Assets/Scenes/GoldenA/Nested"],
                            ["Assets/Scenes/GoldenA", "Assets/Scenes/goldena"]):
            with self.subTest(directories=directories):
                self.assert_refused([{"File": "A.jsonl", "IdSeed": 1, "Renderer": "none",
                                      "GoldenDirectories": directories}], "Scaffolds[0]", "overlap")

    def test_malformed_golden_directories_are_refused(self) -> None:
        for value in ([], "Assets/Scenes/GoldenA", ["Scenes/GoldenA"], ["Assets"], ["Assets/"], ["Assets/../Escape"],
                      ["Assets/.Hidden"], ["Assets/Scenes//GoldenA"], ["Assets/Scenes/GoldenA/"],
                      ["Assets\\Scenes\\GoldenA"], [7]):
            with self.subTest(value=value):
                self.assert_refused([{"File": "A.jsonl", "IdSeed": 1, "Renderer": "none",
                                      "GoldenDirectories": value}], "Scaffolds[0]", "GoldenDirectories")

    def test_a_member_other_than_golden_directories_is_still_refused(self) -> None:
        self.assert_refused([{"File": "A.jsonl", "IdSeed": 1, "Renderer": "none",
                              "Golden": ["Assets/Scenes/GoldenA"]}], "Scaffolds[0]", "exactly")


class KeptGoldenContentTests(unittest.TestCase):
    """Golden.jsonl's kept outputs: the files below Assets/ that Scaffold/Baseline/Provenance.json records."""

    def make_project(self, files: dict[str, bytes], recorded: list[str]) -> Path:
        project = temporary_project(self, "GoldenKept-")
        for relative, data in files.items():
            (project / relative).parent.mkdir(parents=True, exist_ok=True)
            (project / relative).write_bytes(data)
        entries = [{"Path": relative, "XXH64": xxh64.xxh64_hex(files[relative]), "Method": "asset.create",
                    "RequestId": index + 1, "Client": "batch", "TranscriptLine": None}
                   for index, relative in enumerate(sorted(recorded))]
        baseline = project / scaffold.BASELINE_PROVENANCE
        baseline.parent.mkdir(parents=True)
        baseline.write_text(json.dumps({"Format": "Provenance", "Version": 1, "Entries": entries}), encoding="utf-8")
        return project

    def test_the_committed_kept_files_are_the_recorded_files_of_the_kept_directories_with_their_bytes(self) -> None:
        # T-FTPROV moved the 24 M9 files, which the baseline never recorded, out of the kept directories.
        kept = scaffold.read_kept_files(scaffold.PROJECT)
        on_disk = sorted(path.relative_to(scaffold.PROJECT).as_posix()
                         for directory in scaffold.KEPT_ASSET_DIRECTORIES
                         for path in (scaffold.PROJECT / directory).rglob("*") if path.is_file())
        self.assertEqual(on_disk, sorted(kept))
        self.assertEqual(scaffold.check_kept_files(scaffold.PROJECT, kept), [])

    def test_regeneration_keeps_the_recorded_files_and_removes_every_other_one(self) -> None:
        project = self.make_project({"Assets/Scenes/Golden/Kept.scene": b"kept",
                                     "Assets/Scenes/Golden/Stale.scene": b"stale",
                                     "Assets/Materials/Golden/Old/Only.material": b"old",
                                     "Assets/Scenes/Main.scene": b"main",
                                     "FeatureTest.eproj": b"project"},
                                    ["Assets/Scenes/Golden/Kept.scene", "FeatureTest.eproj"])
        kept = scaffold.read_kept_files(project)
        self.assertEqual(list(kept), ["Assets/Scenes/Golden/Kept.scene"])
        removed = scaffold.remove_scaffold_outputs(project, kept)
        self.assertEqual(removed, ["Assets/Materials/Golden/Old/Only.material", "Assets/Scenes/Golden/Stale.scene",
                                   "Assets/Scenes/Main.scene"])
        self.assertTrue((project / "Assets/Scenes/Golden/Kept.scene").is_file())
        self.assertFalse((project / "Assets/Materials").exists(), "a directory left empty is removed")
        self.assertTrue((project / "Assets").is_dir())
        self.assertTrue((project / "FeatureTest.eproj").is_file())

    def test_a_baseline_entry_below_assets_outside_the_kept_directories_is_refused(self) -> None:
        project = self.make_project({"Assets/Scenes/Main.scene": b"main"}, ["Assets/Scenes/Main.scene"])
        with self.assertRaises(scaffold.ManifestError) as raised:
            scaffold.read_kept_files(project)
        self.assertIn("Assets/Scenes/Main.scene", str(raised.exception))

    def test_a_kept_file_that_does_not_hold_its_recorded_bytes_is_a_difference(self) -> None:
        project = self.make_project({"Assets/Scenes/Golden/A.scene": b"a", "Assets/Scenes/Golden/B.scene": b"b"},
                                    ["Assets/Scenes/Golden/A.scene", "Assets/Scenes/Golden/B.scene"])
        kept = scaffold.read_kept_files(project)
        self.assertEqual(scaffold.check_kept_files(project, kept), [])
        (project / "Assets/Scenes/Golden/A.scene").write_bytes(b"edited by hand")
        (project / "Assets/Scenes/Golden/B.scene").unlink()
        self.assertEqual(scaffold.check_kept_files(project, kept),
                         ["kept-differs: Assets/Scenes/Golden/A.scene", "kept-missing: Assets/Scenes/Golden/B.scene"])


class ScaffoldIsolationTests(unittest.TestCase):
    """What a scaffold's run may touch (Scaffold.py's step 3)."""

    GOLDEN = scaffold.Scaffold("G.jsonl", 2, "none", ("Assets/Scenes/GoldenG", "Assets/Materials/GoldenG"))
    PLAIN = scaffold.Scaffold("P.jsonl", 1, "none")
    BEFORE = {"Assets/Scenes/Golden/Kept.scene": b"kept", "FeatureTest.eproj": b"project",
              "Automation/Provenance.json": b"provenance", "Assets/Scenes/GoldenG/Old.scene": b"old"}

    def violations(self, scaffold_entry: scaffold.Scaffold, after: dict[str, bytes]) -> list[str]:
        return scaffold.find_isolation_violations(scaffold_entry, self.BEFORE, after, [self.PLAIN, self.GOLDEN])

    def test_a_golden_scaffold_may_write_only_below_its_directories(self) -> None:
        inside = {**self.BEFORE, "Assets/Scenes/GoldenG/A.scene": b"a", "Assets/Materials/GoldenG/A.material": b"m",
                  "Automation/Provenance.json": b"recorded"}
        self.assertEqual(self.violations(self.GOLDEN, inside), [])
        outside = {**self.BEFORE, "Assets/Scenes/Other.scene": b"other", "FeatureTest.eproj": b"settings"}
        self.assertEqual([line.split(": ", 1)[0] for line in self.violations(self.GOLDEN, outside)],
                         ["Assets/Scenes/Other.scene", "FeatureTest.eproj"])

    def test_no_other_scaffold_may_touch_a_golden_scaffolds_directories(self) -> None:
        self.assertEqual(self.violations(self.PLAIN, {**self.BEFORE, "Assets/Scenes/Other.scene": b"other",
                                                      "FeatureTest.eproj": b"settings"}), [])
        removed = {path: data for path, data in self.BEFORE.items() if path != "Assets/Scenes/GoldenG/Old.scene"}
        violations = self.violations(self.PLAIN, removed)
        self.assertEqual(len(violations), 1)
        self.assertIn("Assets/Scenes/GoldenG/Old.scene", violations[0])
        self.assertIn("G.jsonl", violations[0])

    def test_no_scaffold_may_touch_anything_below_a_kept_directory(self) -> None:
        # A kept file rewritten, or a new file beside the kept ones, which the next regeneration would remove and Test::
        # ListGoldenScenes would list as Golden.jsonl's.
        removed = {path: data for path, data in self.BEFORE.items() if path != "Assets/Scenes/Golden/Kept.scene"}
        for after, path in (({**self.BEFORE, "Assets/Scenes/Golden/Kept.scene": b"rewritten"},
                             "Assets/Scenes/Golden/Kept.scene"),
                            (removed, "Assets/Scenes/Golden/Kept.scene"),
                            ({**self.BEFORE, "Assets/Materials/Golden/New.material": b"new"},
                             "Assets/Materials/Golden/New.material")):
            for entry in (self.PLAIN, self.GOLDEN):
                with self.subTest(scaffold=entry.file, path=path):
                    violations = self.violations(entry, after)
                    self.assertEqual(len(violations), 1)
                    self.assertTrue(violations[0].startswith(f"{path}: below the kept directory"), violations[0])

    def test_regeneration_fails_the_step_of_a_scaffold_that_writes_where_it_may_not(self) -> None:
        # regenerate() snapshots the project around each scaffold's run and checks it with find_isolation_violations: a
        # run that breaks the rules fails its own step, naming the files, and the regeneration stops there. The editor's
        # runs are replaced by writes of the files each scaffold would make.
        project = temporary_project(self, "GoldenRegenerate-")
        baseline = project / scaffold.BASELINE_PROVENANCE
        baseline.parent.mkdir(parents=True)
        baseline.write_text(json.dumps({"Format": "Provenance", "Version": 1, "Entries": []}), encoding="utf-8")
        (baseline.parent / scaffold.PROJECT_FILE_NAME).write_text("{}", encoding="utf-8")
        golden = scaffold.Scaffold("G.jsonl", 2, "none", ("Assets/Scenes/GoldenG",))
        plain = scaffold.Scaffold("P.jsonl", 1, "none")
        later = scaffold.Scaffold("L.jsonl", 3, "none")
        written = {plain.file: "Assets/Scenes/Plain.scene", golden.file: "Assets/Scenes/Stray.scene",
                   later.file: "Assets/Scenes/Later.scene"}
        ran: list[str] = []

        def run_scaffold(editor: Path, root: Path, entry: scaffold.Scaffold, console: Console,
                         timeout: float) -> scaffold.Step:
            ran.append(entry.file)
            target = root / written[entry.file]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(entry.file, encoding="utf-8")
            return scaffold.Step(f"scaffold {entry.file}", scaffold.Status.PASSED, "written", data={"file": entry.file})

        with mock.patch.object(scaffold, "run_scaffold", run_scaffold):
            steps = scaffold.regenerate(Path("Editor"), project, [plain, golden, later], Console(True))
        self.assertEqual([step.name for step in steps], ["clean", "scaffold P.jsonl", "scaffold G.jsonl"])
        self.assertEqual(ran, ["P.jsonl", "G.jsonl"])
        self.assertFalse(steps[1].failed, steps[1].detail)
        self.assertTrue(steps[2].failed)
        self.assertEqual([line.split(": ", 1)[0] for line in steps[2].data["violations"]],
                         ["Assets/Scenes/Stray.scene"])


class FeatureTestProvenanceTests(unittest.TestCase):
    """T-FTPROV: Projects/FeatureTest's provenance after the recorded regeneration of its M9 golden content."""

    def test_every_project_file_and_file_below_assets_has_its_provenance_entry(self) -> None:
        # Roadmap M18's acceptance: FeatureTest's 24 golden files have Provenance.json entries from one recorded run. Up
        # to M17 they had none: they were copied in from a unit test's replay (ADR 0022 review fix M11).
        project = scaffold.PROJECT
        document = json.loads((project / scaffold.PROVENANCE).read_text(encoding="utf-8"))
        entries = {entry["Path"]: entry for entry in document["Entries"]}
        assets = sorted(path for path in (project / "Assets").rglob("*")
                        if path.is_file() and path.suffix != scaffold.BACKUP_SUFFIX)
        files = [project / scaffold.PROJECT_FILE_NAME, *assets]
        unrecorded = []
        differing = []
        for file in files:
            relative = file.relative_to(project).as_posix()
            if relative not in entries:
                unrecorded.append(relative)
            elif entries[relative]["XXH64"] != xxh64.xxh64_hex(file.read_bytes()):
                differing.append(relative)
        self.assertEqual(unrecorded, [])
        self.assertEqual(differing, [])
        self.assertEqual([path for path in entries if not (project / path).is_file()], [])

    def test_the_golden_scaffold_files_name_the_requests_of_their_recorded_run(self) -> None:
        project = scaffold.PROJECT
        document = json.loads((project / scaffold.PROVENANCE).read_text(encoding="utf-8"))
        requests = [json.loads(line) for line in
                    (project / "Scaffold" / RENDERER_II_SCAFFOLD).read_text(encoding="utf-8").splitlines()]
        recorded = [entry for entry in document["Entries"]
                    if any(scaffold.is_below(entry["Path"], directory) for directory in RENDERER_II_DIRECTORIES)]
        # The nine M9 scenes and three materials, each with its .meta.
        self.assertEqual(len(recorded), 24)
        for entry in recorded:
            with self.subTest(path=entry["Path"]):
                self.assertEqual(entry["Client"], "batch")
                self.assertIsInstance(entry["RequestId"], int)
                self.assertTrue(1 <= entry["RequestId"] <= len(requests))
                # A batch run's request id is its line in the scaffold (BatchRunner).
                self.assertEqual(entry["Method"], requests[entry["RequestId"] - 1]["method"])


class GoldenDeviceClassReportTests(unittest.TestCase):
    """Test.py's reading of the golden harness's lines (GOLDEN_PATTERN) for the device-class check."""

    def test_a_changed_device_class_is_reported_apart_from_the_smoke_mode(self) -> None:
        lines = [
            "[warn] Golden image 'Bloom' on device class 'nvidia-62x': device class changed: no "
            "'Tests/Golden/nvidia-62x/Bloom.png', but the same vendor's device class 'nvidia-61x' has this golden",
            "[info] Golden image 'Text' on device class 'nvidia-62x': matched 'Tests/Golden/nvidia-62x/Text.png'",
            "[warn] Golden image 'Other' on device class 'nvidia-62x': goldens missing (no "
            "'Tests/Golden/nvidia-62x/Other.png'), smoke checks passed",
        ]
        report = test_script.DeviceReport.parse(lines)
        self.assertEqual(report.class_changed, {"Bloom"})
        self.assertEqual(report.goldens_missing, {"Other"})
        self.assertEqual(report.matched, {"Text"})
        self.assertEqual(report.candidates_written, set())
        self.assertEqual(report.device_classes, {"nvidia-62x"})
        self.assertEqual(report.to_json()["deviceClassChanged"], ["Bloom"])


if __name__ == "__main__":
    unittest.main()
