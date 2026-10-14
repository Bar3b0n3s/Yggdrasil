"""The hardening stage's pure parts (Docs/Architecture.md §15.1 T6; Docs/Decisions/0024-m15-contract.md decisions 12 to
15, stream C): the AddressSanitizer options and runtime, the fuzz target list and the evaluation of the fuzz shards' log
lines, the soak scenes and the < 1 % growth rule, and the process memory query the soak and the perf capture share; and
the scripts around them: Hardening.py's step order and fail-fast summary, Test.py's sanitized runs, Generate.py's
--sanitize and CheckBuildConfig.py's AddressSanitizer rules. These tests need no editor build.
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
import xml.etree.ElementTree as ElementTree
from collections.abc import Callable
from pathlib import Path
from typing import Any
from unittest import mock

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import CheckBuildConfig as check_build_config  # noqa: E402  (Scripts/ is on sys.path above)
import Generate as generate_script  # noqa: E402
import Hardening as hardening_script  # noqa: E402
import Test as test_script  # noqa: E402
from Lib import hardening, process  # noqa: E402
from Lib.report import Status, Step  # noqa: E402


RECORDED_SOAKS = REPOSITORY_ROOT / "Tests" / "Data" / "Hardening"


def recorded_soak(name: str) -> dict[str, list[hardening.SoakSample]]:
    """{subject: samples} of a recorded Soak.json subset under Tests/Data/Hardening/ (Tests/Data/LICENSES.md)."""
    document = json.loads((RECORDED_SOAKS / name).read_text(encoding="utf-8"))
    return {subject["Subject"]: [hardening.SoakSample(**sample) for sample in subject["Samples"]]
            for subject in document["Subjects"]}


def report_line(case: str, begin: int, end: int, total: int) -> str:
    return (f"[info] Fuzz '{case}': iterations {begin}..{end} of {total} (seed 0x00000000000009a4): 1 accepted, "
            f"2 rejected")


def sample(tick: int, private_bytes: int, entities: int = 10, script_heap: int = 1000,
           allocations: int = 100) -> hardening.SoakSample:
    """An editor soak sample after `tick` ticks."""
    return hardening.SoakSample(tick / hardening.SOAK_FRAMES, private_bytes, private_bytes, entities, 5, 0, script_heap,
                                allocations)


class AsanTests(unittest.TestCase):
    """Hermetic: the runtime lookup is patched to a temporary directory holding placeholder ASAN_RUNTIME_FILES, so these
    tests need neither Windows nor Visual Studio's optional "C++ AddressSanitizer" component, which only the hardening
    stage needs (Setup.py merely warns when it is missing)."""

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="Asan-")).resolve()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        self.runtime = self.directory / "Hostx64" / "x64"
        self.runtime.mkdir(parents=True)
        for name in hardening.ASAN_RUNTIME_FILES:
            (self.runtime / name).write_bytes(f"placeholder {name}".encode("utf-8"))
        patcher = mock.patch.object(hardening, "asan_runtime_directory", return_value=self.runtime)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_asan_options_name_the_required_settings(self) -> None:
        options = dict(item.split("=", 1) for item in hardening.asan_options().split(":"))
        self.assertEqual(options["halt_on_error"], "1")
        self.assertEqual(options["allocator_may_return_null"], "0")
        self.assertIn("quarantine_size_mb", options)

    def test_the_environment_puts_the_runtime_first_on_path(self) -> None:
        key = "Path" if os.name == "nt" else "PATH"
        existing = os.pathsep.join([str(self.directory / "First"), str(self.directory / "Second")])
        environment = hardening.asan_environment({key: existing, "OTHER": "kept"})
        self.assertEqual(environment["ASAN_OPTIONS"], hardening.asan_options())
        self.assertEqual(environment["OTHER"], "kept")
        self.assertEqual(environment[key].split(os.pathsep),
                         [str(self.runtime), str(self.directory / "First"), str(self.directory / "Second")])

    def test_the_runtime_is_staged_next_to_the_sanitized_executables_only(self) -> None:
        workspace = self.directory / "Workspace"
        output = workspace / "bin" / test_script.paths.output_directory_name("Release")
        for project in hardening.ASAN_RUNTIME_PROJECTS:
            (output / project).mkdir(parents=True)
        (output / "Runtime" / "Redist").mkdir()
        copied = hardening.stage_asan_runtime(workspace, "Release")
        self.assertEqual(sorted((path.parent.name, path.name) for path in copied),
                         sorted((project, name) for project in hardening.ASAN_RUNTIME_PROJECTS
                                for name in hardening.ASAN_RUNTIME_FILES))
        self.assertEqual(list((output / "Runtime" / "Redist").iterdir()), [])
        # An unchanged copy is kept.
        self.assertEqual(hardening.stage_asan_runtime(workspace, "Release"), [])


class FuzzTests(unittest.TestCase):
    def test_the_committed_targets_read(self) -> None:
        targets = hardening.read_fuzz_targets()
        readers = [target.reader for target in targets]
        self.assertEqual(readers, sorted(readers))
        self.assertEqual(len(set(readers)), len(readers))
        self.assertTrue(all(target.iterations == hardening.FUZZ_HARDENING_ITERATIONS for target in targets))
        self.assertFalse(any("'" in target.case for target in targets))

    def test_a_target_below_the_hardening_count_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="FuzzTargets-") as directory:
            path = Path(directory) / "Targets.json"
            path.write_text(json.dumps({"Format": "FuzzTargets", "Version": 1, "Targets": [
                {"Reader": "PakReader", "Case": "PakReader: fuzz", "Iterations": 1000}]}), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Iterations"):
                hardening.read_fuzz_targets(path)

    def test_the_process_count_is_bounded_by_cpus_the_cap_and_memory(self) -> None:
        gib = 1 << 30
        self.assertEqual(hardening.fuzz_process_count(24, 64 * gib, gib), 24)
        self.assertEqual(hardening.fuzz_process_count(64, 256 * gib, gib), hardening.FUZZ_MAX_PROCESSES)
        self.assertEqual(hardening.fuzz_process_count(24, 8 * gib, gib), 8)
        self.assertEqual(hardening.fuzz_process_count(24, gib // 2, gib), 1)
        with self.assertRaises(ValueError):
            hardening.fuzz_process_count(0, gib, gib)

    def test_the_case_limit_grows_with_a_shards_share_and_never_drops_below_the_reference(self) -> None:
        # The reference run: 19 shards with each case's limit scaled by 20. Fewer shards each run a larger share.
        self.assertEqual(hardening.fuzz_timeout_scale(hardening.FUZZ_REFERENCE_PROCESSES),
                         hardening.FUZZ_REFERENCE_TIMEOUT_SCALE)
        self.assertEqual(hardening.fuzz_timeout_scale(11), 35)  # ceil(20 * 19 / 11)
        self.assertEqual(hardening.fuzz_timeout_scale(1), 380)
        # More shards each run a smaller share, but not faster: beyond the physical cores they share them (SMT), so the
        # limit stays the reference's 20 (the M15 fix review's P2; ceil(20 * 19 / 32) would be 12).
        self.assertEqual(hardening.fuzz_timeout_scale(24), 20)
        self.assertEqual(hardening.fuzz_timeout_scale(32), 20)
        # A development run of fewer iterations needs a smaller limit, never below the unscaled one.
        self.assertEqual(hardening.fuzz_timeout_scale(19, 100_000), 2)
        self.assertEqual(hardening.fuzz_timeout_scale(32, 1_000), 1)
        # The Bindings case (27,600 CPU-s over every shard at 1,000,000 iterations) stays within its scaled limit
        # (the default 120 s per case) at every count the step accepts.
        minimum = hardening.fuzz_minimum_processes(hardening_script.TIMEOUTS["fuzz"])
        for processes in range(minimum, hardening.FUZZ_MAX_PROCESSES + 1):
            with self.subTest(processes=processes):
                self.assertLess(27_600 / processes, 120 * hardening.fuzz_timeout_scale(processes))

    def test_the_shard_count_must_fit_the_step_timeout(self) -> None:
        budget = hardening_script.TIMEOUTS["fuzz"]
        minimum = hardening.fuzz_minimum_processes(budget)
        self.assertEqual(minimum, 14)  # 52 min x 19 processes in 80 % of 5,400 s
        self.assertLessEqual(hardening.fuzz_expected_seconds(minimum), budget * hardening.FUZZ_BUDGET_SHARE)
        self.assertGreater(hardening.fuzz_expected_seconds(minimum - 1), budget * hardening.FUZZ_BUDGET_SHARE)
        self.assertEqual(hardening.fuzz_minimum_processes(budget, 1_000), 1)
        gib = 1 << 30
        self.assertIsNone(hardening.fuzz_capacity_problem(19, minimum, 24, 19 * gib, gib, budget))
        memory = hardening.fuzz_capacity_problem(11, minimum, 24, 11 * gib, gib, budget)
        self.assertIn("at least 14 processes", memory or "")
        self.assertIn("at least 14336 MB of available memory", memory or "")
        self.assertNotIn("logical CPUs", memory or "")
        cpus = hardening.fuzz_capacity_problem(8, minimum, 8, 64 * gib, gib, budget)
        self.assertIn("at least 14 logical CPUs (8 here)", cpus or "")
        self.assertNotIn("available memory", cpus or "")
        forced = hardening.fuzz_capacity_problem(4, minimum, 24, 64 * gib, gib, budget)
        self.assertIn("--fuzz-processes of at least 14", forced or "")

    def test_the_fuzz_step_refuses_to_start_without_the_memory_its_timeout_needs(self) -> None:
        # 11 GiB available on a 24-CPU machine gives 11 shards, below the 14 the 5,400 s timeout needs: exit code 3
        # naming the memory, before any process starts or the AddressSanitizer runtime is looked up.
        arguments = quiet_parse(hardening_script.parse_arguments, ["--steps", "fuzz"])
        with tempfile.TemporaryDirectory(prefix="FuzzStep-") as directory:
            output = Path(directory)
            tests = output / "Tests" / test_script.paths.executable_name("Tests")
            tests.parent.mkdir()
            tests.write_bytes(b"")
            with (mock.patch.object(hardening_script, "asan_output_directory", return_value=output),
                  mock.patch.object(hardening, "available_memory_bytes", return_value=11 << 30),
                  mock.patch.object(hardening_script.os, "cpu_count", return_value=24),
                  mock.patch.object(hardening, "asan_environment", side_effect=AssertionError("looked up")),
                  mock.patch.object(hardening_script.subprocess, "Popen", side_effect=AssertionError("started")),
                  contextlib.redirect_stderr(io.StringIO())):
                step = hardening_script.step_fuzz(arguments, hardening_script.Console(True))
        self.assertEqual(step.status, Status.FAILED)
        self.assertEqual(step.exit_code, hardening_script.EXIT_INIT_FAILED)
        self.assertIn("at least 14 processes", step.detail)
        self.assertIn("MB of available memory", step.detail)
        self.assertEqual((step.data["processes"], step.data["minimumProcesses"]), (11, 14))

    def test_each_shard_runs_with_the_case_limit_of_its_share(self) -> None:
        # 64 GiB and 24 CPUs give 24 shards, each with --test-timeout-scale=max(20, ceil(20 * 19 / 24)) = 20.
        commands: list[list[str]] = []

        class FinishedShard:
            pid = 1
            returncode = 0

            def __init__(self, command: list[str], **options: object) -> None:
                commands.append(command)

            def poll(self) -> int:
                return 0

            def wait(self, timeout: float | None = None) -> int:
                return 0

            def kill(self) -> None:
                pass

        arguments = quiet_parse(hardening_script.parse_arguments, ["--steps", "fuzz"])
        with tempfile.TemporaryDirectory(prefix="FuzzStep-") as directory:
            output = Path(directory)
            tests = output / "Tests" / test_script.paths.executable_name("Tests")
            tests.parent.mkdir()
            tests.write_bytes(b"")
            with (mock.patch.object(hardening_script, "asan_output_directory", return_value=output),
                  mock.patch.object(hardening_script, "FUZZ_JUNIT", output / "Fuzz-Release-Asan.xml"),
                  mock.patch.object(hardening, "available_memory_bytes", return_value=64 << 30),
                  mock.patch.object(hardening_script.os, "cpu_count", return_value=24),
                  mock.patch.object(hardening, "asan_environment", return_value={}),
                  mock.patch.object(hardening_script.subprocess, "Popen", FinishedShard),
                  contextlib.redirect_stderr(io.StringIO())):
                step = hardening_script.step_fuzz(arguments, hardening_script.Console(True))
        self.assertEqual(len(commands), 24)
        self.assertEqual(hardening.fuzz_timeout_scale(24), 20)
        for index, command in enumerate(commands):
            self.assertIn("--test-timeout-scale=20", command)
            self.assertIn(f"--fuzz-shard={index}/24", command)
        self.assertEqual(step.data["timeoutScale"], 20)
        self.assertEqual(step.status, Status.FAILED)  # the shards wrote no report line

    def test_shards_must_cover_every_iteration_exactly_once(self) -> None:
        # Small counts keep the lines short; read_fuzz_targets refuses any count but 1,000,000 in a file, so the targets
        # are built directly.
        targets = [hardening.FuzzTarget("PakReader", "PakReader: fuzz", 10),
                   hardening.FuzzTarget("ZipReader", "ZipReader: fuzz", 10)]
        lines = [report_line("PakReader: fuzz", 0, 5, 10), report_line("PakReader: fuzz", 5, 10, 10),
                 report_line("ZipReader: fuzz", 0, 4, 10), report_line("Other: fuzz", 0, 1, 1)]
        self.assertEqual(hardening.evaluate_fuzz_logs(targets, lines),
                         ["fuzz: ZipReader: ran 4 of 10 iterations", "fuzz: Other: fuzz: not a target"])
        self.assertEqual(hardening.evaluate_fuzz_logs(targets, lines[:2]), ["fuzz: ZipReader: did not run"])
        # A shard reported twice counts twice, and a report of another total does not cover the target.
        twice = lines[:2] + [lines[1], report_line("ZipReader: fuzz", 0, 10, 10)]
        self.assertEqual(hardening.evaluate_fuzz_logs(targets, twice), ["fuzz: PakReader: ran 15 of 10 iterations"])
        other_total = lines[:2] + [report_line("ZipReader: fuzz", 0, 10, 20)]
        self.assertEqual(hardening.evaluate_fuzz_logs(targets, other_total),
                         ["fuzz: ZipReader: ran 10 of 10 iterations"])

    def test_target_files_are_refused_with_the_problem_named(self) -> None:
        good = {"Reader": "PakReader", "Case": "PakReader: fuzz", "Iterations": hardening.FUZZ_HARDENING_ITERATIONS}
        documents = {
            "Format": {"Format": "Targets", "Version": 1, "Targets": [good]},
            "sorted": {"Format": "FuzzTargets", "Version": 1,
                       "Targets": [{**good, "Reader": "Zip", "Case": "Zip"}, good]},
            "duplicate Case": {"Format": "FuzzTargets", "Version": 1, "Targets": [good, {**good, "Reader": "Zip"}]},
            "apostrophe": {"Format": "FuzzTargets", "Version": 1, "Targets": [{**good, "Case": "Pak's fuzz"}]},
            "exactly Reader": {"Format": "FuzzTargets", "Version": 1, "Targets": [{**good, "Extra": 1}]},
            "Iterations": {"Format": "FuzzTargets", "Version": 1, "Targets": [{**good, "Iterations": True}]},
        }
        with tempfile.TemporaryDirectory(prefix="FuzzTargets-") as directory:
            for problem, document in documents.items():
                with self.subTest(problem=problem):
                    path = Path(directory) / "Targets.json"
                    path.write_text(json.dumps(document), encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, problem):
                        hardening.read_fuzz_targets(path)


class SoakTests(unittest.TestCase):
    def test_soak_covers_every_scene_of_every_project(self) -> None:
        projects = hardening.soak_projects(REPOSITORY_ROOT)
        self.assertIn(REPOSITORY_ROOT / "Projects" / "FeatureTest", projects)
        self.assertEqual(projects, sorted(projects))
        scenes = hardening.soak_scenes(REPOSITORY_ROOT / "Projects" / "FeatureTest")
        self.assertIn("Assets/Scenes/Physics.scene", scenes)
        self.assertIn("Assets/Scenes/Components/TonemapAces.scene", scenes)
        self.assertIn("Assets/Scenes/Golden/Bloom.scene", scenes)
        self.assertEqual(scenes, sorted(scenes))

    def test_growth_compares_the_last_window_maximum_with_the_baseline_median(self) -> None:
        ticks = range(0, hardening.SOAK_FRAMES + 1, hardening.SOAK_SAMPLE_INTERVAL)
        flat = [sample(tick, 100_000_000 + (tick % 3) * 1000) for tick in ticks]
        result = hardening.evaluate_soak("Assets/Scenes/Physics.scene", flat)
        self.assertTrue(result.passed, result.failures)
        self.assertLess(result.growth, hardening.SOAK_GROWTH_LIMIT)
        leaking = [sample(tick, 100_000_000 + tick * 300) for tick in ticks]
        result = hardening.evaluate_soak("Assets/Scenes/Physics.scene", leaking)
        self.assertFalse(result.passed)
        spawning = [sample(tick, 100_000_000, entities=10 + tick // 1000) for tick in ticks]
        self.assertFalse(hardening.evaluate_soak("Assets/Scenes/Physics.scene", spawning).passed)
        self.assertFalse(hardening.evaluate_soak("Assets/Scenes/Physics.scene", flat[:10]).passed)

    def test_garbage_collected_and_small_counters_do_not_fail_without_a_leak(self) -> None:
        ticks = range(0, hardening.SOAK_FRAMES + 1, hardening.SOAK_SAMPLE_INTERVAL)
        # A Luau heap sawtooth between 1 and 2 MB, and an allocation count that wobbles by one: no leak.
        sawtooth = [sample(tick, 100_000_000, script_heap=1_000_000 + (tick % 1000) * 1000,
                           allocations=100 + (tick // 250) % 2) for tick in ticks]
        result = hardening.evaluate_soak("FeatureTest:Assets/Scenes/Scripting.scene", sawtooth)
        self.assertTrue(result.passed, result.failures)
        # A heap that only grows, never collected, is inconclusive, a failure; allocations climbing are a leak.
        rising = [sample(tick, 100_000_000, script_heap=1_000_000 + tick * 500) for tick in ticks]
        self.assertFalse(hardening.evaluate_soak("FeatureTest:Assets/Scenes/Scripting.scene", rising).passed)
        climbing = [sample(tick, 100_000_000, allocations=100 + tick // 500) for tick in ticks]
        self.assertFalse(hardening.evaluate_soak("FeatureTest:Assets/Scenes/Scripting.scene", climbing).passed)

    def test_a_long_cycle_collected_in_both_heap_windows_is_judged_by_its_floors(self) -> None:
        # Input.scene's former heap: a sawtooth whose cycle lasts about 3,800 ticks, collected at ticks 200, 4,000 and
        # 7,800 here, so both the reference window and the last quarter hold a collection. Its last 5 % sits higher in
        # its cycle than the 5 % after the half (the windows of the private memory rule), which is no leak: the
        # post-collection floors agree. A floor that rises with a leak still fails. At another phase the same cycle can
        # leave the last quarter without a collection (the next case).
        ticks = range(0, hardening.SOAK_FRAMES + 1, hardening.SOAK_SAMPLE_INTERVAL)
        long_cycle = [sample(tick, 100_000_000, script_heap=3_600_000 + ((tick - 4000) % 3800) * 500) for tick in ticks]
        result = hardening.evaluate_soak("FeatureTest:Assets/Scenes/Input.scene", long_cycle)
        self.assertTrue(result.passed, result.failures)
        rising = [sample(tick, 100_000_000, script_heap=3_600_000 + ((tick - 4000) % 3800) * 500 + tick * 1000)
                  for tick in ticks]
        result = hardening.evaluate_soak("FeatureTest:Assets/Scenes/Input.scene", rising)
        self.assertFalse(result.passed)
        self.assertIn("script heap's floor rose", result.failures[0])

    def test_a_cycle_longer_than_a_quarter_of_the_soak_can_be_inconclusive_until_a_longer_soak(self) -> None:
        # The same 3,800-tick sawtooth, collected at ticks 3,400 and 7,200: the next collection falls after tick 10,000,
        # so the last quarter of the default soak holds none and climbs 1.4 MB above the reference floor, which the
        # rule cannot tell from a leak. A cycle longer than about a quarter of the soak (2,750 ticks here: a collection
        # after tick 7,250 shows in the last quarter's first sample) can do this, depending only on its phase (the M15
        # fix review's P1). The failure names the longer soak that settles it, and over 20,000 ticks the same heap
        # collects in both windows and passes.
        def heap(tick: int) -> int:
            return 3_600_000 + ((tick - 3400) % 3800) * 500

        def soak(frames: int) -> list[hardening.SoakSample]:
            return [hardening.SoakSample(tick / frames, 100_000_000, 100_000_000, 10, 5, 0, heap(tick), 100)
                    for tick in range(0, frames + 1, hardening.SOAK_SAMPLE_INTERVAL)]

        subject = "FeatureTest:Assets/Scenes/Input.scene"
        result = hardening.evaluate_soak(subject, soak(hardening.SOAK_FRAMES))
        self.assertFalse(result.passed)
        self.assertEqual(len(result.failures), 1, result.failures)
        self.assertIn("script heap inconclusive: no collection in the last quarter", result.failures[0])
        self.assertIn(hardening.SOAK_LONGER_COMMAND, result.failures[0])
        self.assertIn("--soak-frames", hardening.SOAK_LONGER_COMMAND)
        result = hardening.evaluate_soak(subject, soak(2 * hardening.SOAK_FRAMES))
        self.assertTrue(result.passed, result.failures)

    def test_stream_c_leaking_callbacks_scene_fails_the_heap_rule(self) -> None:
        # Stream C's Callbacks.scene kept every per-tick journal entry: its heap's floor rose from 3.40 MB (lowest
        # trough of 0.25-0.75) to 5.87 MB (lowest trough of the last quarter) while the sawtooth's ceiling rose with it,
        # which the former rule (last quarter's minimum against the second quarter's maximum) let pass. Private memory,
        # the only other rule it broke, is not what fails it here.
        samples = recorded_soak("SoakStreamC.json")["FeatureTest:Assets/Scenes/Callbacks.scene"]
        flat_private = [dataclasses.replace(sample, private_bytes=300_000_000) for sample in samples]
        result = hardening.evaluate_soak("FeatureTest:Assets/Scenes/Callbacks.scene", flat_private)
        self.assertFalse(result.passed)
        self.assertEqual(len(result.failures), 1, result.failures)
        self.assertIn("script heap's floor rose", result.failures[0])
        self.assertIn("5871582 bytes", result.failures[0])
        self.assertIn("3398150 bytes", result.failures[0])
        heap = [(sample.position, sample.script_heap_bytes or 0) for sample in samples]
        self.assertEqual(hardening.evaluate_script_heap(heap), result.failures[0])

    def test_a_heap_that_never_collects_while_it_grows_is_inconclusive(self) -> None:
        # Stream C's Input.scene appended a reading every tick and never collected: 6.9 MB at a quarter, 26.8 MB at the
        # end, no trough anywhere.
        samples = recorded_soak("SoakStreamC.json")["FeatureTest:Assets/Scenes/Input.scene"]
        failure = hardening.evaluate_script_heap([(sample.position, sample.script_heap_bytes or 0)
                                                  for sample in samples])
        self.assertIn("script heap inconclusive: no collection in the last quarter", failure or "")

    def test_every_recorded_soak_that_does_not_leak_passes(self) -> None:
        # Every sawtooth of stream C's soak but the two leaking scenes, its script-less Physics.scene (a constant heap,
        # never collected), and the integration run's bounded Callbacks.scene and Input.scene.
        leaking = {"FeatureTest:Assets/Scenes/Callbacks.scene", "FeatureTest:Assets/Scenes/Input.scene"}
        subjects = [(f"SoakStreamC.json {name}", samples) for name, samples in recorded_soak("SoakStreamC.json").items()
                    if name not in leaking]
        subjects += [(f"SoakIntegration.json {name}", samples)
                     for name, samples in recorded_soak("SoakIntegration.json").items()]
        self.assertEqual(len(subjects), 11)
        for name, samples in subjects:
            with self.subTest(subject=name):
                result = hardening.evaluate_soak(name.split(" ", 1)[1], samples)
                self.assertTrue(result.passed, result.failures)

    def test_a_window_without_a_collection_passes_only_when_its_samples_bound_the_growth(self) -> None:
        ticks = range(0, hardening.SOAK_FRAMES + 1, hardening.SOAK_SAMPLE_INTERVAL)

        def heap(values: Callable[[int], int]) -> list[tuple[float, int]]:
            return [(tick / hardening.SOAK_FRAMES, values(tick)) for tick in ticks]

        floor = 1_000_000
        tolerance = hardening.soak_heap_tolerance(floor)
        self.assertEqual(tolerance, max(hardening.SOAK_HEAP_TOLERANCE_BYTES, floor // 10))
        # A 1,000-tick sawtooth whose floor rises within, then beyond, the tolerance.
        self.assertIsNone(hardening.evaluate_script_heap(heap(lambda tick: floor + (tick % 1000) * 1000
                                                              + (tolerance if tick >= 7500 else 0))))
        self.assertIn("floor rose", hardening.evaluate_script_heap(
            heap(lambda tick: floor + (tick % 1000) * 1000 + (tolerance + 1 if tick >= 7500 else 0))) or "")
        # The last collection at tick 7,000: a last quarter that only climbs passes while its highest sample stays
        # within the tolerance of the reference floor, and is inconclusive beyond it.
        self.assertIsNone(hardening.evaluate_script_heap(
            heap(lambda tick: floor + (tick % 1000) * 100 if tick < 7000 else floor + (tick - 7000) * 30)))
        climbing = hardening.evaluate_script_heap(
            heap(lambda tick: floor + (tick % 1000) * 100 if tick < 7000 else floor + (tick - 7000) * 100)) or ""
        self.assertIn("inconclusive: no collection in the last quarter", climbing)
        self.assertIn(hardening.SOAK_LONGER_COMMAND, climbing)
        # A collection only in the last quarter leaves no reference floor.
        late = hardening.evaluate_script_heap(heap(lambda tick: floor + tick * 100 if tick < 8000 else floor)) or ""
        self.assertIn("inconclusive: no collection in the reference window", late)
        self.assertIn(hardening.SOAK_LONGER_COMMAND, late)
        # Without any collection the heap only grew by allocation: within the tolerance (a constant heap) it passes.
        self.assertIsNone(hardening.evaluate_script_heap(heap(lambda tick: 324_112)))
        self.assertIsNone(hardening.evaluate_script_heap(heap(lambda tick: floor + tick * 10)))
        self.assertIn("with no collection since", hardening.evaluate_script_heap(
            heap(lambda tick: floor + tick * 100)) or "")

    def test_the_runtime_soak_has_only_memory_samples(self) -> None:
        samples = [hardening.SoakSample(index / 100, 80_000_000, 90_000_000) for index in range(101)]
        result = hardening.evaluate_soak("Runtime:FeatureTest", samples)
        self.assertTrue(result.passed, result.failures)
        zero = [hardening.SoakSample(index / 100, 0, 0) for index in range(101)]
        self.assertFalse(hardening.evaluate_soak("Runtime:FeatureTest", zero).passed)

    def test_process_memory_of_this_process(self) -> None:
        memory = process.query_process_memory(os.getpid())
        self.assertGreater(memory.private_bytes, 0)
        self.assertGreater(memory.resident_bytes, 0)
        self.assertGreaterEqual(memory.peak_resident_bytes, memory.resident_bytes)

    def test_process_memory_of_an_exited_process_is_a_lookup_error(self) -> None:
        # The Popen object keeps its process handle open, so on Windows the exited child's id cannot be given to another
        # process before the query; elsewhere ids are handed out in sequence, so the reaped id is not reused at once.
        child = subprocess.Popen([sys.executable, "-c", "pass"])
        self.assertEqual(child.wait(timeout=60), 0)
        with self.assertRaises(ProcessLookupError):
            process.query_process_memory(child.pid)
        with self.assertRaises(ProcessLookupError):
            process.query_process_memory(0)

    def test_the_report_lists_every_subject_with_its_samples(self) -> None:
        samples = {"Runtime:FeatureTest": [hardening.SoakSample(index / 100, 80_000_000, 90_000_000)
                                           for index in range(101)]}
        result = hardening.evaluate_soak("Runtime:FeatureTest", samples["Runtime:FeatureTest"])
        report = hardening.soak_report([result], samples)
        self.assertEqual((report["Format"], report["Version"], report["Frames"]), ("SoakReport", 1, 10_000))
        subject = report["Subjects"][0]
        self.assertEqual((subject["Subject"], subject["Passed"], subject["Failures"]),
                         ("Runtime:FeatureTest", True, []))
        self.assertEqual(len(subject["Samples"]), 101)
        self.assertIsNone(subject["Samples"][0]["entities"])
        json.dumps(report)  # the report is plain JSON
        self.assertEqual(hardening.soak_report([result], samples, 20_000)["Frames"], 20_000)


def quiet_parse(parse: object, argv: list[str]) -> argparse.Namespace:
    """Calls an argparse-based parse_arguments, keeping its usage text out of the test output."""
    with contextlib.redirect_stderr(io.StringIO()):
        return parse(argv)  # type: ignore[operator]


class HardeningScriptTests(unittest.TestCase):
    def test_the_steps_run_in_the_stage_order(self) -> None:
        self.assertEqual(hardening_script.STEPS, ("perf", "soak", "asan-generate", "asan-build", "asan-unit",
                                                  "asan-feature", "fuzz"))
        arguments = quiet_parse(hardening_script.parse_arguments, ["--steps", "fuzz,soak"])
        self.assertEqual(arguments.steps, ["soak", "fuzz"])
        for argv in (["--steps", "lint"], ["--fuzz-processes", "0"], ["--fuzz-processes", "33"], ["--jobs", "0"]):
            with self.subTest(argv=argv), self.assertRaises(SystemExit):
                quiet_parse(hardening_script.parse_arguments, argv)

    def test_a_failed_step_stops_the_stage_and_names_the_failure(self) -> None:
        ran: list[str] = []

        def fake(key: str, status: Status,
                 exit_code: int | None = None) -> Callable[[argparse.Namespace, object], Step]:
            def step(_arguments: argparse.Namespace, _console: object) -> Step:
                ran.append(key)
                return Step(hardening_script.STEP_NAMES[key], status, f"{key} {status.value}", exit_code=exit_code)
            return step

        fakes = {"asan-generate": fake("asan-generate", Status.PASSED),
                 "asan-build": fake("asan-build", Status.FAILED, hardening_script.EXIT_INIT_FAILED),
                 "asan-unit": fake("asan-unit", Status.PASSED)}
        original = dict(hardening_script.STEP_FUNCTIONS)
        hardening_script.STEP_FUNCTIONS.update(fakes)
        try:
            output = io.StringIO()
            with contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
                exit_code = hardening_script.main(["--steps", "asan-generate,asan-build,asan-unit", "--json"])
        finally:
            hardening_script.STEP_FUNCTIONS.clear()
            hardening_script.STEP_FUNCTIONS.update(original)
        self.assertEqual(ran, ["asan-generate", "asan-build"])
        self.assertEqual(exit_code, hardening_script.EXIT_INIT_FAILED)
        document = json.loads(output.getvalue())
        self.assertFalse(document["success"])
        self.assertEqual([step["status"] for step in document["steps"]], ["passed", "failed", "not-run"])
        self.assertEqual(document["steps"][2]["detail"], "asan build failed")

    def test_the_perf_capture_runs_perf_py_and_only_a_capture_that_cannot_run_fails(self) -> None:
        # Stream D's Scripts/Perf.py (ADR 0024 decision 16) through its documented interface: --config Release --json
        # [--gpu-optional], a document {"success", "exitCode", "steps": [...]}; an over-budget metric is a warning step.
        fake = ("import json, sys\n"
                "status, code = '{status}', {code}\n"
                "print(json.dumps({{'success': code == 0, 'exitCode': code, 'steps': [{{'name': "
                "'perf capture (Release)', 'status': status, 'detail': ' '.join(sys.argv[1:])}}]}}))\n"
                "sys.exit(code)\n")
        arguments = quiet_parse(hardening_script.parse_arguments, ["--steps", "perf", "--gpu-optional"])
        original = hardening_script.SCRIPTS
        with tempfile.TemporaryDirectory(prefix="Perf-") as directory, contextlib.redirect_stderr(io.StringIO()):
            hardening_script.SCRIPTS = Path(directory)
            try:
                console = hardening_script.Console(True)
                missing = hardening_script.step_perf(arguments, console)
                self.assertEqual(missing.status, Status.NOT_AVAILABLE)
                script = Path(directory) / "Perf.py"
                script.write_text(fake.format(status="warning", code=0), encoding="utf-8")
                warned = hardening_script.step_perf(arguments, console)
                self.assertEqual(warned.status, Status.WARNING)
                self.assertEqual(warned.detail, "--config Release --json --gpu-optional")
                script.write_text(fake.format(status="passed", code=0), encoding="utf-8")
                self.assertEqual(hardening_script.step_perf(arguments, console).status, Status.PASSED)
                script.write_text(fake.format(status="failed", code=3), encoding="utf-8")
                failed = hardening_script.step_perf(arguments, console)
                self.assertEqual(failed.status, Status.FAILED)
                self.assertEqual(failed.exit_code, hardening_script.EXIT_INIT_FAILED)
            finally:
                hardening_script.SCRIPTS = original

    def test_a_shard_report_gives_each_case_its_time_and_its_failures(self) -> None:
        with tempfile.TemporaryDirectory(prefix="FuzzShard-") as directory:
            junit = Path(directory) / "Shard0.xml"
            junit.write_text(
                '<?xml version="1.0"?><testsuites><testsuite name="Tests">'
                '<testcase name="PakReader: fuzz" time="1.5"/>'
                '<testcase name="ZipReader: fuzz" time="0.25">'
                '<failure message="CHECK( ok ) is NOT correct!"/></testcase>'
                '</testsuite></testsuites>', encoding="utf-8")
            self.assertEqual(hardening_script.case_seconds(junit), {"PakReader: fuzz": 1.5, "ZipReader: fuzz": 0.25})
            self.assertEqual(hardening_script.junit_failures(junit), ["ZipReader: fuzz: CHECK( ok ) is NOT correct!"])
            self.assertEqual(hardening_script.case_seconds(Path(directory) / "Missing.xml"), {})


class FakeClock:
    """A monotonic clock the fake editor advances."""

    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now


class FakeEngineError(Exception):
    pass


class FakeClient:
    """An engine client whose play.step takes `step_seconds` of the fake clock; a call whose timeout is shorter than its
    cost uses up the timeout and raises TimeoutError, as engine_client does."""

    def __init__(self, clock: FakeClock, step_seconds: float) -> None:
        self.clock = clock
        self.step_seconds = step_seconds
        self.calls: list[tuple[str, float]] = []

    def __enter__(self) -> FakeClient:
        return self

    def __exit__(self, *exception: object) -> None:
        return None

    def call(self, method: str, params: dict[str, Any], timeout: float) -> dict[str, Any]:
        self.calls.append((method, timeout))
        cost = self.step_seconds if method == "play.step" else 0.0
        if cost > timeout:
            self.clock.now += timeout
            raise TimeoutError(f"no message from the editor within {timeout:.1f} s")
        self.clock.now += cost
        return {"entities": 3, "bodies": 1, "voices": 0, "scriptHeapBytes": 500_000, "memoryAllocationCount": 9}


class FakeEditorProcess:
    def __init__(self, client: FakeClient) -> None:
        self.client = client
        self.process = types.SimpleNamespace(pid=4242)
        self.killed = False

    def connect(self, client_name: str, call_log: object) -> FakeClient:
        return self.client

    def wait(self, timeout: float) -> int:
        return 0

    def kill(self) -> None:
        self.killed = True

    def output(self) -> str:
        return ""


def fake_engine_client(editor_process: FakeEditorProcess) -> types.SimpleNamespace:
    return types.SimpleNamespace(EngineError=FakeEngineError, ConnectionClosed=ConnectionError,
                                 ProtocolError=ValueError, CallLog=lambda path: None,
                                 launch_editor=lambda *arguments, **options: editor_process)


def flat_samples() -> list[hardening.SoakSample]:
    return [sample(tick, 100_000_000) for tick in range(0, hardening.SOAK_FRAMES + 1, hardening.SOAK_SAMPLE_INTERVAL)]


class SoakDeadlineTests(unittest.TestCase):
    """The soak step's one deadline over its editor and Runtime halves, and its reports on every way out."""

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="SoakStep-")).resolve()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        silence = contextlib.redirect_stderr(io.StringIO())
        silence.__enter__()
        self.addCleanup(silence.__exit__, None, None, None)
        self.console = hardening_script.Console(True)
        memory = process.ProcessMemory(100_000_000, 90_000_000, 95_000_000)
        patcher = mock.patch.object(hardening_script.process_lib, "query_process_memory", return_value=memory)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_every_editor_call_waits_at_most_what_is_left_of_the_step(self) -> None:
        clock = FakeClock()
        client = FakeClient(clock, step_seconds=400.0)
        editor = FakeEditorProcess(client)
        deadline = hardening_script.SoakDeadline(1000.0, clock)
        samples, errors = hardening_script.soak_editor_scene(fake_engine_client(editor), self.directory / "Editor",
                                                             self.directory / "P.eproj", "Assets/Scenes/A.scene",
                                                             self.directory / "UserData", self.console, deadline)
        steps = [timeout for method, timeout in client.calls if method == "play.step"]
        self.assertEqual(steps, [600.0, 600.0, 200.0])  # 1,000 s left, then 600, then 200
        self.assertEqual(len(samples), 3)  # the start and two steps
        self.assertEqual(len(errors), 1)
        self.assertIn("deadline passed after tick 500", errors[0])
        self.assertTrue(editor.killed)
        self.assertTrue(all(timeout <= hardening_script.SOAK_CALL_TIMEOUT_SECONDS for _, timeout in client.calls))

    def test_the_deadline_ends_the_step_as_a_timeout_and_keeps_the_finished_subjects(self) -> None:
        clock = FakeClock()
        deadline = hardening_script.SoakDeadline(1000.0, clock)
        ran: list[str] = []

        def subject(name: str, seconds: float, errors: list[str]) -> hardening_script.SoakSubject:
            def run(limit: hardening_script.SoakDeadline) -> hardening_script.SoakRun:
                ran.append(name)
                clock.now += seconds
                return flat_samples(), errors
            return hardening_script.SoakSubject(name, "test", run)

        subjects = [subject("FeatureTest:A.scene", 600.0, []),
                    subject("FeatureTest:B.scene", 500.0, [hardening_script.soak_deadline_error("after tick 2500")]),
                    subject("Runtime:FeatureTest", 10.0, [])]
        report, junit = self.directory / "Soak.json", self.directory / "Soak-Release.xml"
        outcome = hardening_script.run_soak_subjects(subjects, deadline, self.console, report, junit)
        self.assertEqual(ran, ["FeatureTest:A.scene", "FeatureTest:B.scene"])
        self.assertTrue(outcome.timed_out)
        self.assertEqual(outcome.not_run, ["Runtime:FeatureTest"])
        document = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual([(entry["Subject"], entry["Passed"]) for entry in document["Subjects"]],
                         [("FeatureTest:A.scene", True), ("FeatureTest:B.scene", False)])
        self.assertEqual([case.get("name") for case in ElementTree.parse(junit).getroot().iter("testcase")],
                         ["FeatureTest:A.scene", "FeatureTest:B.scene"])
        step = hardening_script.soak_step("soak Release", outcome, 1100.0, None, report, junit)
        self.assertEqual(step.status, Status.TIMEOUT)
        self.assertIn("did not finish within 3600 s", step.detail)
        self.assertEqual(step.data["notRun"], ["Runtime:FeatureTest"])

    def test_the_reports_are_written_when_a_subject_raises(self) -> None:
        deadline = hardening_script.SoakDeadline(1000.0, FakeClock())

        def broken(limit: hardening_script.SoakDeadline) -> hardening_script.SoakRun:
            raise RuntimeError("unexpected")

        subjects = [hardening_script.SoakSubject("FeatureTest:A.scene", "test", lambda limit: (flat_samples(), [])),
                    hardening_script.SoakSubject("FeatureTest:B.scene", "test", broken)]
        report, junit = self.directory / "Soak.json", self.directory / "Soak-Release.xml"
        with self.assertRaises(RuntimeError):
            hardening_script.run_soak_subjects(subjects, deadline, self.console, report, junit)
        document = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual([entry["Subject"] for entry in document["Subjects"]], ["FeatureTest:A.scene"])
        self.assertTrue(junit.is_file())

    def test_a_failed_runtime_memory_query_fails_the_runtime_subject(self) -> None:
        # K32GetProcessMemoryInfo can fail with ERROR_PARTIAL_COPY while the Runtime exits: an OSError that is not a
        # ProcessLookupError. It must fail the subject, not escape and lose Soak.json and the step results.
        class FakeRuntime:
            pid = 4343
            returncode: int | None = None

            def __init__(self, *arguments: object, **options: object) -> None:
                self.waits = 0

            def wait(self, timeout: float | None = None) -> int:
                self.waits += 1
                if timeout is not None and self.waits == 1:
                    raise subprocess.TimeoutExpired("Runtime", timeout)
                self.returncode = 0
                return 0

            def poll(self) -> int | None:
                return self.returncode

            def kill(self) -> None:
                self.returncode = 1

        clock = FakeClock()
        deadline = hardening_script.SoakDeadline(1000.0, clock)
        failing = OSError(299, "cannot read the memory of process 4343: ERROR_PARTIAL_COPY")
        with (mock.patch.object(hardening_script.subprocess, "Popen", FakeRuntime),
              mock.patch.object(hardening_script.process_lib, "query_process_memory", side_effect=failing)):
            samples, errors = hardening_script.soak_runtime(self.directory / "Package" / "FeatureTest.exe",
                                                            self.directory / "UserData", self.console, deadline)
        self.assertEqual(samples, [])
        self.assertEqual(len(errors), 1)
        self.assertIn("cannot read the Runtime's memory", errors[0])
        self.assertIn("ERROR_PARTIAL_COPY", errors[0])

    def test_the_runtime_soak_stops_at_the_deadline(self) -> None:
        class SlowRuntime:
            pid = 4444
            returncode: int | None = None

            def __init__(self, *arguments: object, **options: object) -> None:
                pass

            def wait(self, timeout: float | None = None) -> int:
                if timeout is not None:
                    clock.now += timeout
                    raise subprocess.TimeoutExpired("Runtime", timeout)
                return self.returncode or 0

            def poll(self) -> int | None:
                return self.returncode

            def kill(self) -> None:
                self.returncode = 1

        clock = FakeClock()
        deadline = hardening_script.SoakDeadline(2.0, clock)
        with mock.patch.object(hardening_script.subprocess, "Popen", SlowRuntime):
            samples, errors = hardening_script.soak_runtime(self.directory / "Package" / "FeatureTest.exe",
                                                            self.directory / "UserData", self.console, deadline)
        self.assertEqual(len(samples), 5)  # at 0, 0.5, 1.0, 1.5 and 2.0 s of the fake clock
        self.assertEqual(len(errors), 1)
        self.assertIn("deadline passed before the Runtime finished", errors[0])


class SoakFramesTests(unittest.TestCase):
    """Hardening.py --soak-frames (the M15 fix review's P1): a longer soak of the same subjects, its windows still
    fractions of the soak, so a scene whose script heap rule was inconclusive can be confirmed."""

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="SoakFrames-")).resolve()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        silence = contextlib.redirect_stderr(io.StringIO())
        silence.__enter__()
        self.addCleanup(silence.__exit__, None, None, None)
        self.console = hardening_script.Console(True)
        memory = process.ProcessMemory(100_000_000, 90_000_000, 95_000_000)
        patcher = mock.patch.object(hardening_script.process_lib, "query_process_memory", return_value=memory)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_the_soak_length_defaults_to_10000_and_takes_only_longer_multiples_of_the_sample_interval(self) -> None:
        self.assertEqual(quiet_parse(hardening_script.parse_arguments, []).soak_frames, hardening.SOAK_FRAMES)
        self.assertEqual(quiet_parse(hardening_script.parse_arguments, ["--soak-frames", "40000"]).soak_frames, 40_000)
        for value in ("9750", "10100", "0", "-250"):
            with self.subTest(value=value), self.assertRaises(SystemExit) as raised:
                quiet_parse(hardening_script.parse_arguments, ["--soak-frames", value])
            self.assertEqual(raised.exception.code, 2)

    def test_a_longer_editor_soak_steps_frames_over_250_times_and_ends_at_position_one(self) -> None:
        clock = FakeClock()
        client = FakeClient(clock, step_seconds=1.0)
        editor = FakeEditorProcess(client)
        deadline = hardening_script.SoakDeadline(1_000_000.0, clock)
        samples, errors = hardening_script.soak_editor_scene(fake_engine_client(editor), self.directory / "Editor",
                                                             self.directory / "P.eproj", "Assets/Scenes/A.scene",
                                                             self.directory / "UserData", self.console, deadline,
                                                             20_000)
        self.assertEqual(errors, [])
        self.assertEqual(len([method for method, _ in client.calls if method == "play.step"]), 80)
        self.assertEqual(len(samples), 81)
        self.assertEqual((samples[0].position, samples[40].position, samples[-1].position), (0.0, 0.5, 1.0))

    def test_a_longer_runtime_soak_runs_that_many_frames(self) -> None:
        commands: list[list[str]] = []

        class FinishedRuntime:
            pid = 4545
            returncode = 0

            def __init__(self, command: list[str], **options: object) -> None:
                commands.append(command)

            def wait(self, timeout: float | None = None) -> int:
                return 0

            def poll(self) -> int:
                return 0

            def kill(self) -> None:
                pass

        deadline = hardening_script.SoakDeadline(1000.0, FakeClock())
        with mock.patch.object(hardening_script.subprocess, "Popen", FinishedRuntime):
            samples, errors = hardening_script.soak_runtime(self.directory / "Package" / "FeatureTest.exe",
                                                            self.directory / "UserData", self.console, deadline,
                                                            30_000)
        self.assertEqual(errors, [])
        self.assertEqual(len(commands), 1)
        frames = commands[0].index("--frames")
        self.assertEqual(commands[0][frames + 1], "30000")

    def test_a_longer_soak_gets_a_deadline_in_proportion_and_its_reports_name_its_length(self) -> None:
        self.assertEqual(hardening_script.soak_timeout(hardening.SOAK_FRAMES), hardening_script.TIMEOUTS["soak"])
        self.assertEqual(hardening_script.soak_timeout(40_000), 4 * hardening_script.TIMEOUTS["soak"])
        deadline = hardening_script.SoakDeadline(0.0, FakeClock(), hardening_script.soak_timeout(20_000))
        self.assertIn("7200 s deadline passed after tick 250", deadline.error("after tick 250"))
        samples = [hardening.SoakSample(tick / 20_000, 100_000_000, 100_000_000, 10, 5, 0, 1000, 100)
                   for tick in range(0, 20_001, hardening.SOAK_SAMPLE_INTERVAL)]
        subjects = [hardening_script.SoakSubject("FeatureTest:A.scene", "test", lambda limit: (samples, []))]
        report, junit = self.directory / "Soak.json", self.directory / "Soak-Release.xml"
        outcome = hardening_script.run_soak_subjects(subjects, hardening_script.SoakDeadline(1000.0, FakeClock()),
                                                     self.console, report, junit, frames=20_000)
        self.assertEqual(json.loads(report.read_text(encoding="utf-8"))["Frames"], 20_000)
        step = hardening_script.soak_step("soak Release", outcome, 10.0, None, report, junit, frames=20_000)
        self.assertEqual(step.status, Status.PASSED, step.detail)
        self.assertIn("over 20000 frames", step.detail)
        outcome.timed_out = True
        step = hardening_script.soak_step("soak Release", outcome, 10.0, None, report, junit, frames=20_000)
        self.assertIn("did not finish within 7200 s", step.detail)


class TestScriptSanitizerTests(unittest.TestCase):
    def test_a_sanitized_run_names_its_steps_and_reports_apart(self) -> None:
        build_root = REPOSITORY_ROOT / "bin-int" / "Asan" / "bin"
        arguments = quiet_parse(test_script.parse_arguments, [
            "--suite", "unit", "--config", "Release", "--build-root", str(build_root), "--sanitizer", "address",
            "--test-timeout-scale", "5", "--fuzz-scale", "0.1"])
        [run] = test_script.plan_runs("unit", "Release", arguments)
        self.assertEqual(run.name, "unit Release (ASan)")
        self.assertEqual(run.report_stem, "Unit-Release-Asan")
        self.assertEqual(run.junit_suite_name, "Unit.Release.Asan")
        self.assertIn("--test-timeout-scale=5", run.options)
        self.assertIn("--fuzz-scale=0.1", run.options)
        self.assertEqual(test_script.tests_executable("Release", arguments.build_root).parent.parent.parent,
                         build_root.resolve())
        # A plain run keeps its names and takes the repository's bin/.
        plain = quiet_parse(test_script.parse_arguments, ["--suite", "unit", "--config", "Debug"])
        [plain_run] = test_script.plan_runs("unit", "Debug", plain)
        self.assertEqual((plain_run.name, plain_run.report_stem), ("unit Debug", "Unit-Debug"))
        self.assertEqual(test_script.tests_executable("Debug", plain.build_root),
                         test_script.paths.output_directory("Debug") / "Tests"
                         / test_script.paths.executable_name("Tests"))

    def test_the_debug_unit_run_executes_a_tenth_of_each_fuzz_case_and_release_every_count(self) -> None:
        # ADR 0024 decision 14 (approved by the product owner): without --fuzz-scale, Debug passes 0.1 and Release
        # nothing.
        plain = quiet_parse(test_script.parse_arguments, ["--suite", "unit", "--config", "Debug,Release"])
        [debug_run] = test_script.plan_runs("unit", "Debug", plain)
        [release_run] = test_script.plan_runs("unit", "Release", plain)
        self.assertIn("--fuzz-scale=0.1", debug_run.options)
        self.assertFalse(any(option.startswith("--fuzz-scale") for option in release_run.options))
        # An explicit scale applies to every configuration, so --fuzz-scale 1 runs the whole count in Debug too.
        whole = quiet_parse(test_script.parse_arguments, ["--suite", "unit", "--config", "Debug", "--fuzz-scale", "1"])
        [whole_run] = test_script.plan_runs("unit", "Debug", whole)
        self.assertIn("--fuzz-scale=1", whole_run.options)
        self.assertNotIn("--fuzz-scale=0.1", whole_run.options)
        # The other suites never pass a fuzz scale.
        for suite in ("gpu", "golden"):
            for run in test_script.plan_runs(suite, "Debug", plain):
                self.assertFalse(any(option.startswith("--fuzz-scale") for option in run.options), run.name)

    def test_the_sanitized_unit_run_leaves_the_fuzz_suite_to_the_fuzz_step(self) -> None:
        # The fuzz step runs every Fuzz case with the same sanitized binary over [0, 1,000,000), a superset of the unit
        # counts' iterations, so the sanitized unit run leaves the suite out (ADR 0023: no duplicate work).
        excluded = quiet_parse(test_script.parse_arguments, ["--suite", "unit", "--config", "Release",
                                                             "--exclude-test-suites", hardening.FUZZ_SUITE])
        [run] = test_script.plan_runs("unit", "Release", excluded)
        self.assertEqual(run.filters, ("--test-suite-exclude=GPU,Golden,Fuzz",))
        plain = quiet_parse(test_script.parse_arguments, ["--suite", "unit", "--config", "Release"])
        [plain_run] = test_script.plan_runs("unit", "Release", plain)
        self.assertEqual(plain_run.filters, ("--test-suite-exclude=GPU,Golden",))
        document = {"steps": [{"name": "unit Release (ASan)", "status": "passed", "detail": "3,500 passed"}]}
        arguments = quiet_parse(hardening_script.parse_arguments, ["--steps", "asan-unit"])
        with mock.patch.object(hardening_script, "run_json_script",
                               return_value=hardening_script.JsonRun(0, False, 1.0, document)) as run_json:
            step = hardening_script.step_asan_unit(arguments, hardening_script.Console(True))
        self.assertEqual(step.status, Status.PASSED)
        options = run_json.call_args.args[2]
        index = options.index("--exclude-test-suites")
        self.assertEqual(options[index + 1], hardening.FUZZ_SUITE)
        self.assertEqual(options[options.index("--suite") + 1], "unit")

    def test_sanitized_options_are_refused_where_they_mean_nothing(self) -> None:
        for argv in (["--suite", "automation", "--sanitizer", "address"],
                     ["--suite", "determinism", "--build-root", str(REPOSITORY_ROOT / "bin-int")],
                     ["--suite", "unit", "--fuzz-scale", "0"], ["--suite", "unit", "--fuzz-scale", "1.5"],
                     ["--suite", "unit", "--test-timeout-scale", "0.5"], ["--suite", "unit", "--sanitizer", "thread"],
                     ["--suite", "gpu", "--exclude-test-suites", "Fuzz"],
                     ["--suite", "unit", "--exclude-test-suites", "Fuzz,"]):
            with self.subTest(argv=argv), self.assertRaises(SystemExit):
                quiet_parse(test_script.parse_arguments, argv)

    def test_sanitizer_reports_are_found_in_a_run_output(self) -> None:
        lines = ["[info] ordinary output",
                 "==1234==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x12 at pc 0x34",
                 "SUMMARY: AddressSanitizer: heap-buffer-overflow Foo.cpp:12 in Engine::Read"]
        self.assertEqual([line for line in lines if test_script.SANITIZER_ERROR_PATTERN.search(line)], [lines[1]])
        self.assertEqual(test_script.describe_sanitizer_errors(lines),
                         "1 AddressSanitizer report(s), first: ERROR: AddressSanitizer: heap-buffer-overflow on "
                         "address 0x12 at pc 0x34")
        self.assertEqual(test_script.describe_sanitizer_errors(lines[:1]), "")


class GenerateAndBuildConfigTests(unittest.TestCase):
    def test_the_sanitized_workspace_is_generated_apart(self) -> None:
        host = test_script.paths.host()
        location = REPOSITORY_ROOT / "bin-int" / "Asan"
        if host.is_windows:
            arguments = quiet_parse(lambda argv: generate_script.parse_arguments(argv, host),
                                    ["--sanitize", "address", "--to", str(location)])
            self.assertEqual(arguments.sanitize, "address")
            self.assertIn("--sanitize=address", generate_script.premake_arguments("vs2026", "windows", None, location,
                                                                                  "address"))
        with self.assertRaises(SystemExit):
            quiet_parse(lambda argv: generate_script.parse_arguments(argv, host), ["--sanitize", "address"])
        self.assertNotIn("--sanitize=address",
                         " ".join(generate_script.premake_arguments("vs2026", "windows", None, None)))

    def test_the_asan_target_instruments_every_compiling_project_in_release_only(self) -> None:
        def project(name: str, asan: set[str], utility: bool = False) -> check_build_config.Project:
            return check_build_config.Project(name, asan_settings=set(asan), utility=utility)

        asan_target = check_build_config.TARGET_BY_NAME["windows-asan"]
        plain_target = check_build_config.TARGET_BY_NAME["windows"]
        workspace = check_build_config.GeneratedWorkspace(asan_target, {
            "Engine": project("Engine", {"Release"}), "Shaders": project("Shaders", set(), utility=True),
            "Tests": project("Tests", set()), "Runtime": project("Runtime", {"Release", "Dist"})})
        findings: list[str] = []
        check_build_config.check_address_sanitizer(workspace, findings)
        self.assertEqual(len(findings), 2)
        self.assertIn("Dist: Runtime is built with AddressSanitizer", findings[0])
        self.assertIn("Release: Tests is not built with AddressSanitizer", findings[1])
        # The compiler option counts as much as the MSBuild setting, and a plain target instruments nothing.
        unit = check_build_config.CompileUnit([], ["/O2", "/fsanitize=address"])
        option = check_build_config.Project("Editor", units={"Debug": [unit]})
        findings = []
        check_build_config.check_address_sanitizer(
            check_build_config.GeneratedWorkspace(plain_target, {"Editor": option}), findings)
        self.assertEqual(findings, ["[windows] Debug: Editor is built with AddressSanitizer in a plain workspace; only "
                                    "premake5.lua's --sanitize=address workspace instruments code"])

    def test_the_annotation_macros_are_banned_in_defines_and_sources(self) -> None:
        unit = check_build_config.CompileUnit(["NOMINMAX", "_DISABLE_VECTOR_ANNOTATION"], [])
        engine = check_build_config.Project("Engine", units={"Release": [unit]})
        workspace = check_build_config.GeneratedWorkspace(check_build_config.TARGET_BY_NAME["windows"],
                                                          {"Engine": engine})
        findings: list[str] = []
        check_build_config.check_banned_annotation_macros(workspace, findings)
        self.assertEqual(len(findings), 1)
        self.assertIn("Engine defines _DISABLE_VECTOR_ANNOTATION", findings[0])
        with tempfile.TemporaryDirectory(prefix="BannedMacro-") as directory:
            source = Path(directory) / "Container.cpp"
            source.write_text("#define _DISABLE_STRING_ANNOTATION\nint value = 0;\n", encoding="utf-8")
            clean = Path(directory) / "Clean.cpp"
            clean.write_text("// _DISABLE_STRING_ANNOTATIONS is another name\n", encoding="utf-8")
            findings = []
            check_build_config.check_banned_macro_sources([source, clean], findings)
        self.assertEqual(len(findings), 1)
        self.assertIn("Container.cpp:1: names _DISABLE_STRING_ANNOTATION", findings[0])


if __name__ == "__main__":
    unittest.main()
