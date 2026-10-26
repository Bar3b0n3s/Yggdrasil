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
import functools
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

from harness import REPOSITORY_ROOT, engine_client

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import CheckBuildConfig as check_build_config  # noqa: E402  (Scripts/ is on sys.path above)
import CI as ci_script  # noqa: E402
import Generate as generate_script  # noqa: E402
import Hardening as hardening_script  # noqa: E402
import Test as test_script  # noqa: E402
from Lib import hardening, process  # noqa: E402
from Lib.report import Status, Step, write_junit  # noqa: E402


RECORDED_SOAKS = REPOSITORY_ROOT / "Tests" / "Data" / "Hardening"
# The fuzz step's JUnit report of M17's milestone gate (Tests/Data/LICENSES.md).
RECORDED_FUZZ = RECORDED_SOAKS / "FuzzM17.xml"


def write_fuzz_report(path: Path, seconds: dict[hardening.FuzzTarget, float],
                      failed: tuple[hardening.FuzzTarget, ...] = (),
                      shards: tuple[Status, ...] = (Status.PASSED, Status.PASSED)) -> None:
    """A fuzz step's JUnit report as Hardening.py's step_fuzz writes it: one test case per target, passed with its
    `seconds` summed over the shards, or failed, then one per shard with the given status."""
    steps = [Step(hardening.fuzz_target_case_name(target), Status.PASSED,
                  hardening.fuzz_target_detail(target.iterations, value / target.iterations), value)
             for target, value in seconds.items()]
    steps += [Step(hardening.fuzz_target_case_name(target), Status.FAILED,
                   f"fuzz: {target.reader}: did not run") for target in failed]
    steps += [Step(f"shard {index}/{len(shards)}", status, "peak private memory 900 MB")
              for index, status in enumerate(shards)]
    write_junit(path, "Fuzz.Release.Asan", steps)


def recorded_fuzz_targets() -> list[hardening.FuzzTarget]:
    """The 35 targets RECORDED_FUZZ measured, at 1,000,000 iterations, from its "<Reader> (<Case>)" test case names, so
    the tests that read it do not depend on Targets.json, which the program's milestones extend."""
    root = ElementTree.parse(RECORDED_FUZZ).getroot()
    names = [case.get("name", "") for case in root.iter("testcase") if not case.get("name", "").startswith("shard ")]
    return [hardening.FuzzTarget(name.split(" (", 1)[0], name.split(" (", 1)[1][:-1],
                                 hardening.FUZZ_HARDENING_ITERATIONS) for name in names]


def reference_targets(count: int) -> list[hardening.FuzzTarget]:
    """`count` targets at 1,000,000 iterations that no report measures."""
    return [hardening.FuzzTarget(f"Reader{index:02}", f"Reader{index:02}: fuzz", hardening.FUZZ_HARDENING_ITERATIONS)
            for index in range(count)]


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
        # The Bindings case (27,600 CPU-s over every shard at 1,000,000 iterations, M17's gate measured 26,708) stays
        # within its scaled limit (the default 120 s per case) at every count the step accepts.
        targets = reference_targets(hardening.FUZZ_REFERENCE_TARGETS)
        work = sum(hardening.fuzz_target_seconds(targets, {}).values())
        minimum = hardening.fuzz_minimum_processes(work, hardening.fuzz_budget_seconds(len(targets)))
        for processes in range(minimum, hardening.FUZZ_MAX_PROCESSES + 1):
            with self.subTest(processes=processes):
                scale = hardening.fuzz_timeout_scale(processes, dearest_seconds=26_708)
                self.assertLess(27_600 / processes, 120 * scale)
                self.assertEqual(scale, hardening.fuzz_timeout_scale(processes))

    def test_a_target_dearer_than_the_reference_allows_raises_the_case_limit(self) -> None:
        # A target of 60,000 s summed over the shards needs 60,000 / 19 = 3,158 s per shard, more than the reference's
        # 2,400 s limit: its share must fit 80 % of the limit, ceil(3,158 / (120 x 0.8)) = 33; counted, like the
        # reference's limit, as if more shards than 19 ran no faster.
        self.assertEqual(hardening.fuzz_timeout_scale(19, dearest_seconds=60_000), 33)
        self.assertEqual(hardening.fuzz_timeout_scale(24, dearest_seconds=60_000), 33)
        self.assertEqual(hardening.fuzz_timeout_scale(14, dearest_seconds=60_000), 45)  # ceil(60,000 / 14 / 96)
        for processes in range(1, hardening.FUZZ_MAX_PROCESSES + 1):
            with self.subTest(processes=processes):
                share = 60_000 / min(processes, hardening.FUZZ_REFERENCE_PROCESSES)
                limit = hardening.FUZZ_CASE_LIMIT_SECONDS * hardening.fuzz_timeout_scale(processes,
                                                                                         dearest_seconds=60_000)
                self.assertLessEqual(share, limit * hardening.FUZZ_BUDGET_SHARE)

    def test_the_shard_count_must_fit_the_step_budget(self) -> None:
        # Without a measurement the work is the reference run's: 52 min x 19 processes for M15's 35 targets.
        targets = reference_targets(hardening.FUZZ_REFERENCE_TARGETS)
        work = sum(hardening.fuzz_target_seconds(targets, {}).values())
        self.assertAlmostEqual(work, 52 * 60 * 19)
        budget = hardening.fuzz_budget_seconds(len(targets))
        self.assertEqual(budget, hardening.FUZZ_REFERENCE_TIMEOUT_SECONDS)
        minimum = hardening.fuzz_minimum_processes(work, budget)
        self.assertEqual(minimum, 14)  # 52 min x 19 processes in 80 % of 5,400 s
        self.assertLessEqual(hardening.fuzz_expected_seconds(work, minimum), budget * hardening.FUZZ_BUDGET_SHARE)
        self.assertGreater(hardening.fuzz_expected_seconds(work, minimum - 1), budget * hardening.FUZZ_BUDGET_SHARE)
        self.assertEqual(hardening.fuzz_minimum_processes(work / 1_000, budget), 1)  # 1,000 iterations per target
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

    def test_the_measured_report_sizes_the_step(self) -> None:
        # M17's gate (Tests/Data/Hardening/FuzzM17.xml, its Fuzz-Release-Asan.xml verbatim): 35 targets measured, their
        # times summed over 18 shards 58,583.848 s; the step took 3,312 s, 1.7 % above the 3,255 s the model expects.
        targets = recorded_fuzz_targets()
        self.assertEqual(len(targets), 35)
        costs = hardening.read_fuzz_costs(RECORDED_FUZZ)
        self.assertEqual(set(costs), {hardening.fuzz_target_case_name(target) for target in targets})
        seconds = hardening.fuzz_target_seconds(targets, costs)
        self.assertAlmostEqual(seconds["Bindings"], 26_708.090, places=3)
        self.assertAlmostEqual(seconds["ProjectLock"], 10.443, places=3)
        work = sum(seconds.values())
        self.assertAlmostEqual(work, 58_583.848, places=2)
        self.assertLess(abs(hardening.fuzz_expected_seconds(work, 18) - 3_312) / 3_312, 0.02)
        budget = hardening.fuzz_budget_seconds(len(targets))
        minimum = hardening.fuzz_minimum_processes(work, budget)
        self.assertEqual(minimum, 14)  # the resource check stays 14 logical CPUs and 14 GiB with M17's costs
        # The timeout follows the costs and the shard count that runs, and never exceeds the budget.
        share = hardening.FUZZ_BUDGET_SHARE
        self.assertAlmostEqual(hardening.fuzz_timeout_seconds(work, 18, budget), work / 18 / share)
        # Shards beyond the reference run's 19, or beyond the logical CPUs, shorten nothing: the timeout counts at most
        # that many, so 24 shards on this 24-CPU machine get the timeout of 19 (3,854 s, where 24 would give 3,051 s
        # against the 3,083 s the step needs if the five extra shards run no faster).
        self.assertAlmostEqual(hardening.fuzz_timeout_seconds(work, 24, budget, 24), work / 19 / share)
        self.assertAlmostEqual(hardening.fuzz_timeout_seconds(work, 32, budget, 24), work / 19 / share)
        self.assertAlmostEqual(hardening.fuzz_timeout_seconds(work, 18, budget, 16), work / 16 / share)
        for processes in range(minimum, hardening.FUZZ_MAX_PROCESSES + 1):
            with self.subTest(processes=processes):
                timeout = hardening.fuzz_timeout_seconds(work, processes, budget, 24)
                self.assertLessEqual(timeout, budget)
                self.assertGreaterEqual(timeout, work / min(processes, 24) / share)
        # The dearest measured target, Bindings, fits the reference's case limit, so the limit stays the reference's.
        self.assertEqual(hardening.fuzz_timeout_scale(18, dearest_seconds=max(seconds.values())), 22)

    def test_dearer_measured_targets_raise_the_minimum_and_the_timeout(self) -> None:
        # Every target twice as dear as in M17's report: the work doubles, so the minimum shard count rises from 14 to
        # ceil(2 x 58,583.848 / 4,320) = 28, and the timeout at a given count doubles until the budget bounds it.
        targets = recorded_fuzz_targets()
        measured = hardening.read_fuzz_costs(RECORDED_FUZZ)
        work = sum(hardening.fuzz_target_seconds(targets, measured).values())
        dearer = sum(hardening.fuzz_target_seconds(targets, {name: 2 * cost for name, cost in measured.items()})
                     .values())
        budget = hardening.fuzz_budget_seconds(len(targets))
        self.assertEqual(hardening.fuzz_minimum_processes(dearer, budget), 28)
        self.assertAlmostEqual(hardening.fuzz_timeout_seconds(1.25 * work, 19, budget),
                               1.25 * hardening.fuzz_timeout_seconds(work, 19, budget))
        self.assertEqual(hardening.fuzz_timeout_seconds(dearer, 28, budget), budget)

    def test_a_target_not_yet_measured_costs_the_measured_average(self) -> None:
        targets = [hardening.FuzzTarget("PakReader", "PakReader: fuzz", hardening.FUZZ_HARDENING_ITERATIONS),
                   hardening.FuzzTarget("SpriteSheet", "SpriteSheet: fuzz", hardening.FUZZ_HARDENING_ITERATIONS),
                   hardening.FuzzTarget("ZipReader", "ZipReader: fuzz", hardening.FUZZ_HARDENING_ITERATIONS)]
        with tempfile.TemporaryDirectory(prefix="FuzzCosts-") as directory:
            report = Path(directory) / "Fuzz-Release-Asan.xml"
            write_fuzz_report(report, {targets[0]: 100.0, targets[2]: 300.0})
            costs = hardening.read_fuzz_costs(report)
        seconds = hardening.fuzz_target_seconds(targets, costs)
        self.assertAlmostEqual(seconds["PakReader"], 100.0)
        self.assertAlmostEqual(seconds["SpriteSheet"], 200.0)  # the average of 100 and 300 per 1,000,000 iterations
        self.assertAlmostEqual(seconds["ZipReader"], 300.0)
        # A development run of fewer iterations of the same targets is priced from the same costs.
        fewer = [dataclasses.replace(target, iterations=1_000) for target in targets]
        self.assertAlmostEqual(sum(hardening.fuzz_target_seconds(fewer, costs).values()), 0.6)

    def test_without_a_measurement_the_reference_scaled_by_the_target_count_sizes_the_step(self) -> None:
        with tempfile.TemporaryDirectory(prefix="FuzzCosts-") as directory:
            self.assertEqual(hardening.read_fuzz_costs(Path(directory) / "Fuzz-Release-Asan.xml"), {})
        reference = 52 * 60 * 19 / 35
        for count in (35, 46, 70):
            with self.subTest(targets=count):
                targets = reference_targets(count)
                work = sum(hardening.fuzz_target_seconds(targets, {}).values())
                self.assertAlmostEqual(work, reference * count)
                budget = hardening.fuzz_budget_seconds(count)
                self.assertAlmostEqual(budget, 5_400 * count / 35)
                # Each target brings its share of the budget, so targets of the reference's average cost keep the
                # minimum at 14 while the timeout grows with them.
                self.assertEqual(hardening.fuzz_minimum_processes(work, budget), 14)
                self.assertAlmostEqual(hardening.fuzz_timeout_seconds(work, 18, budget), reference * count / 18 / 0.8)
        # A measurement of none of the targets (every one renamed or new) is no measurement either.
        targets = [hardening.FuzzTarget("New", "New: fuzz", hardening.FUZZ_HARDENING_ITERATIONS)]
        self.assertAlmostEqual(hardening.fuzz_target_seconds(targets, {"Old (Old: fuzz)": 1.0})["New"], reference)

    def test_a_failed_development_or_damaged_report_measures_nothing(self) -> None:
        target = hardening.FuzzTarget("PakReader", "PakReader: fuzz", hardening.FUZZ_HARDENING_ITERATIONS)
        other = hardening.FuzzTarget("ZipReader", "ZipReader: fuzz", hardening.FUZZ_HARDENING_ITERATIONS)
        with tempfile.TemporaryDirectory(prefix="FuzzCosts-") as directory:
            report = Path(directory) / "Fuzz-Release-Asan.xml"
            # A failed run: a shard that crashed or was stopped wrote none of its times, so no target's time counts.
            write_fuzz_report(report, {target: 100.0}, failed=(other,))
            with self.assertRaisesRegex(ValueError, "failed run"):
                hardening.read_fuzz_costs(report)
            write_fuzz_report(report, {target: 100.0, other: 300.0}, shards=(Status.PASSED, Status.TIMEOUT))
            with self.assertRaisesRegex(ValueError, "failed run"):
                hardening.read_fuzz_costs(report)
            # A development run of 1,000 iterations counts each case's fixed setup a thousand times over.
            write_fuzz_report(report, {dataclasses.replace(target, iterations=1_000): 0.5})
            self.assertEqual(hardening.read_fuzz_costs(report), {})
            # A target with a time of 0 measured nothing.
            write_fuzz_report(report, {target: 0.0, other: 300.0})
            self.assertEqual(hardening.read_fuzz_costs(report), {hardening.fuzz_target_case_name(other): 300e-6})
            report.write_text("<testsuites><testsuite", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "cannot be read"):
                hardening.read_fuzz_costs(report)

    def test_ci_bounds_the_hardening_run_above_its_steps_timeouts(self) -> None:
        # CI.py bounds the whole Hardening.py run above the sum of its steps' timeouts (ADR 0024 "Integration"), the
        # sanitized suites' 60 s graces included; the fuzz step's timeout is at most its budget, which grows with the
        # targets, so the committed targets must fit.
        self.assertEqual(hardening_script.longest_run_seconds(35),
                         sum(hardening_script.TIMEOUTS.values()) + 2 * 60.0 + hardening.fuzz_budget_seconds(35))
        bound = ci_script.TIMEOUTS["hardening"]
        self.assertGreater(bound, hardening_script.longest_run_seconds(len(hardening.read_fuzz_targets())))
        # The limit the documents state (ADR 0028 decision 16, CI.py's TIMEOUTS): up to 49 targets fit, 50 do not.
        self.assertGreater(bound, hardening_script.longest_run_seconds(49))
        self.assertLessEqual(bound, hardening_script.longest_run_seconds(50))

    def test_the_fuzz_step_refuses_to_start_without_the_memory_its_budget_needs(self) -> None:
        # 11 GiB available on a 24-CPU machine gives 11 shards, below the 14 the 5,400 s budget needs without a
        # measurement: exit code 3 naming the memory, before any process starts or the AddressSanitizer runtime is
        # looked up.
        arguments = quiet_parse(hardening_script.parse_arguments, ["--steps", "fuzz"])
        with tempfile.TemporaryDirectory(prefix="FuzzStep-") as directory:
            output = Path(directory)
            tests = output / "Tests" / test_script.paths.executable_name("Tests")
            tests.parent.mkdir()
            tests.write_bytes(b"")
            with (mock.patch.object(hardening_script, "asan_output_directory", return_value=output),
                  mock.patch.object(hardening_script, "FUZZ_JUNIT", output / "Fuzz-Release-Asan.xml"),
                  mock.patch.object(hardening_script, "fuzz_costs_path", return_value=output / "Kept.xml"),
                  mock.patch.object(hardening, "FUZZ_COMMITTED_COSTS", output / "Committed.xml"),
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

    def run_fuzz_step(self, previous: Callable[[Path], None] | None = None, committed: Path | None = None,
                      argv: tuple[str, ...] = (), passing: bool = False) -> tuple[Step, list[list[str]], Path, Path]:
        """Runs the fuzz step over the 35 targets of M17's report (recorded_fuzz_targets, so the committed Targets.json,
        which the program's milestones extend, changes nothing here) on a machine of 24 logical CPUs and 64 GiB of
        available memory with shards that finish at once. `previous` writes the kept measurement (the last full,
        passing run's report, machine-wide) at the path it is given first; `committed` stands for M17's committed
        measurement (none by default); `argv` adds options. The shards report nothing, or with `passing` cover their
        share of every target and record 1 s per target. Returns the step, each shard's command, the step's report
        path and the kept measurement's path."""
        commands: list[list[str]] = []
        targets = recorded_fuzz_targets()

        class FinishedShard:
            pid = 1
            returncode = 0

            def __init__(self, command: list[str], **options: Any) -> None:
                commands.append(command)
                if not passing:
                    return
                iterations = int(next(option for option in command if option.startswith("--fuzz-iterations="))
                                 .split("=", 1)[1])
                index, count = (int(value) for value in next(option for option in command
                                                             if option.startswith("--fuzz-shard="))
                                .split("=", 1)[1].split("/"))
                begin, end = iterations * index // count, iterations * (index + 1) // count
                options["stderr"].write("".join(f"{report_line(target.case, begin, end, iterations)}\n"
                                                for target in targets).encode("utf-8"))
                junit = Path(next(option for option in command if option.startswith("--out=")).split("=", 1)[1])
                cases = "".join(f'<testcase name="{target.case}" time="1.0"/>' for target in targets)
                junit.write_text(f"<testsuites><testsuite>{cases}</testsuite></testsuites>", encoding="utf-8")

            def poll(self) -> int:
                return 0

            def wait(self, timeout: float | None = None) -> int:
                return 0

            def kill(self) -> None:
                pass

        arguments = quiet_parse(hardening_script.parse_arguments, ["--steps", "fuzz", *argv])
        directory = Path(tempfile.mkdtemp(prefix="FuzzStep-"))
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        tests = directory / "Tests" / test_script.paths.executable_name("Tests")
        tests.parent.mkdir()
        tests.write_bytes(b"")
        report = directory / "Fuzz-Release-Asan.xml"
        kept = directory / "Machine" / "fuzz-costs" / "Fuzz-Release-Asan.xml"
        if previous is not None:
            kept.parent.mkdir(parents=True)
            previous(kept)
        with (mock.patch.object(hardening_script, "asan_output_directory", return_value=directory),
              mock.patch.object(hardening_script, "FUZZ_JUNIT", report),
              mock.patch.object(hardening_script, "fuzz_costs_path", return_value=kept),
              mock.patch.object(hardening, "FUZZ_COMMITTED_COSTS", committed or directory / "NoCommittedCosts.xml"),
              mock.patch.object(hardening, "read_fuzz_targets", return_value=targets),
              mock.patch.object(hardening, "available_memory_bytes", return_value=64 << 30),
              mock.patch.object(hardening_script.os, "cpu_count", return_value=24),
              mock.patch.object(hardening, "asan_environment", return_value={}),
              mock.patch.object(hardening_script.subprocess, "Popen", FinishedShard),
              contextlib.redirect_stderr(io.StringIO())):
            step = hardening_script.step_fuzz(arguments, hardening_script.Console(True))
        return step, commands, report, kept

    def test_each_shard_runs_with_the_case_limit_of_its_share(self) -> None:
        # 64 GiB and 24 CPUs give 24 shards, each with --test-timeout-scale=max(20, ceil(20 * 19 / 24)) = 20.
        step, commands, _, _ = self.run_fuzz_step()
        self.assertEqual(len(commands), 24)
        self.assertEqual(hardening.fuzz_timeout_scale(24), 20)
        for index, command in enumerate(commands):
            self.assertIn("--test-timeout-scale=20", command)
            self.assertIn(f"--fuzz-shard={index}/24", command)
        self.assertEqual(step.data["timeoutScale"], 20)
        self.assertEqual(step.status, Status.FAILED)  # the shards wrote no report line

    def test_the_fuzz_step_is_sized_from_the_previous_runs_report(self) -> None:
        # Without a measurement the reference run sizes the step (52 min x 19 shards for the 35 targets); with the
        # kept one (M17's here), its measured costs do (58,583.848 s summed over the shards). 24 shards on 24 CPUs get
        # the timeout of 19, beyond which more shards run no faster.
        unmeasured, _, _, _ = self.run_fuzz_step()
        self.assertEqual((unmeasured.data["measuredTargets"], unmeasured.data["costs"]), (0, "reference"))
        self.assertAlmostEqual(unmeasured.data["workSeconds"], 52 * 60 * 19)
        self.assertAlmostEqual(unmeasured.data["timeoutSeconds"], 52 * 60 * 19 / 19 / hardening.FUZZ_BUDGET_SHARE)
        measured, commands, report, kept = self.run_fuzz_step(lambda path: shutil.copyfile(RECORDED_FUZZ, path))
        self.assertEqual(len(commands), 24)
        self.assertEqual((measured.data["measuredTargets"], measured.data["costs"]),
                         (35, test_script.paths.display_path(kept)))
        self.assertAlmostEqual(measured.data["workSeconds"], 58_583.848, places=2)
        self.assertEqual(measured.data["minimumProcesses"], 14)
        self.assertAlmostEqual(measured.data["timeoutSeconds"], 58_583.848 / 19 / hardening.FUZZ_BUDGET_SHARE,
                               places=2)
        # The step wrote its own report with this run, whose shards reported nothing; that failed run measured
        # nothing, so the kept measurement stays M17's for the next run, in this checkout or any other.
        with self.assertRaisesRegex(ValueError, "failed run"):
            hardening.read_fuzz_costs(report)
        self.assertEqual(kept.read_bytes(), RECORDED_FUZZ.read_bytes())
        self.assertIsNone(measured.data["keptCosts"])

    def test_before_the_first_kept_measurement_the_committed_one_sizes_the_step(self) -> None:
        # A slot that never ran a hardened gate, or a machine whose kept report is damaged, is sized from M17's
        # committed measurement, not from the reference.
        for name, previous in (("no kept measurement", None),
                               ("a damaged one", lambda path: path.write_text("<testsuites", encoding="utf-8"))):
            with self.subTest(name):
                step, _, _, _ = self.run_fuzz_step(previous, committed=RECORDED_FUZZ)
                self.assertEqual((step.data["measuredTargets"], step.data["costs"]),
                                 (35, test_script.paths.display_path(RECORDED_FUZZ)))
                self.assertAlmostEqual(step.data["workSeconds"], 58_583.848, places=2)

    def test_a_full_passing_run_becomes_the_kept_measurement_and_a_development_run_does_not(self) -> None:
        # Each shard records 1 s per target over 24 shards: 24 s per target, so 24 us per iteration at 1,000,000.
        keep_m17 = functools.partial(shutil.copyfile, RECORDED_FUZZ)
        step, _, report, kept = self.run_fuzz_step(keep_m17, passing=True)
        self.assertEqual(step.status, Status.PASSED, step.detail)
        self.assertEqual(kept.read_bytes(), report.read_bytes())
        self.assertEqual(step.data["keptCosts"], test_script.paths.display_path(kept))
        costs = hardening.read_fuzz_costs(kept)
        self.assertEqual(len(costs), 35)
        self.assertTrue(all(abs(cost - 24e-6) < 1e-12 for cost in costs.values()), costs)
        # A development run of fewer iterations passes as a warning and keeps no measurement, wherever it runs.
        development, _, report, kept = self.run_fuzz_step(keep_m17, argv=("--fuzz-iterations", "1000"), passing=True)
        self.assertEqual(development.status, Status.WARNING, development.detail)
        self.assertNotEqual(report.read_bytes(), RECORDED_FUZZ.read_bytes())
        self.assertEqual(kept.read_bytes(), RECORDED_FUZZ.read_bytes())
        self.assertIsNone(development.data["keptCosts"])

    def test_more_shards_than_cpus_get_a_timeout_the_machine_can_meet(self) -> None:
        # --fuzz-processes 32 on 24 logical CPUs: perfect scaling on 24 cores needs W / 24, so the timeout must be at
        # least W / 24 over 80 % (it was W / 32 over 80 %, 2,288 s, below the 2,441 s the machine can deliver).
        step, commands, _, _ = self.run_fuzz_step(lambda path: shutil.copyfile(RECORDED_FUZZ, path),
                                                  argv=("--fuzz-processes", "32"))
        self.assertEqual(len(commands), 32)
        self.assertGreaterEqual(step.data["timeoutSeconds"], 58_583.848 / 24 / hardening.FUZZ_BUDGET_SHARE)
        self.assertLessEqual(step.data["timeoutSeconds"], step.data["budgetSeconds"])

    def test_the_fuzz_step_refuses_a_machine_too_small_for_its_measured_costs(self) -> None:
        # Every target twice as dear as M17 measured needs 28 shards, more than this machine's 24 logical CPUs: exit
        # code 3 naming them, before any shard starts.
        def dearer(path: Path) -> None:
            measured = hardening.read_fuzz_costs(RECORDED_FUZZ)
            write_fuzz_report(path, {target: 2 * measured[hardening.fuzz_target_case_name(target)] * target.iterations
                                     for target in recorded_fuzz_targets()})

        step, commands, _, _ = self.run_fuzz_step(dearer)
        self.assertEqual(commands, [])
        self.assertEqual(step.status, Status.FAILED)
        self.assertEqual(step.exit_code, hardening_script.EXIT_INIT_FAILED)
        self.assertIn("at least 28 logical CPUs (24 here)", step.detail)
        self.assertEqual((step.data["processes"], step.data["minimumProcesses"]), (24, 28))

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
    cost uses up the timeout and raises TimeoutError, as engine_client does. Its script.errors reports an empty error
    stream (ScriptErrorClient serves one with errors), which the soak reads before play.start and after its last step
    (Docs/Decisions/0027-m17-contract.md decision 4, B4); its stats.get reports the scene and game views rendered, as
    the soak's --headless-game-view editor does (Docs/Decisions/0030-m19-contract.md decision 11)."""

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
        if method == "script.errors":
            return {"errors": [], "nextCursor": "0", "evicted": False}
        return {"entities": 3, "bodies": 1, "voices": 0, "scriptHeapBytes": 500_000, "memoryAllocationCount": 9,
                "views": [{"name": "scene", "available": True, "width": 983, "height": 446},
                          {"name": "game", "available": True, "width": 1600, "height": 900}]}


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


class ScriptErrorClient(FakeClient):
    """A FakeClient that serves script.errors as the editor does (ScriptMethods.cpp over ScriptErrorStream,
    ScriptError.cpp). The editor's stream already holds three distinct errors (ids 1 to 3, raised before the soak takes
    its cursor). `error`, one error or a list of them, is added in order when the method `raised_by` is first called
    (play.start for an OnStart error, play.step for one raised while the soak plays); each one added takes the next id
    with count 1, unless the stream holds an error with the same script, jsonPointer, line and message, which it
    replaces at the end with the new id, its own tick and that error's count + 1 (a repeated error moves to its latest
    occurrence). The stream keeps the MAX_ENTRIES distinct errors with the latest occurrences, evicting the oldest.
    Since "end" returns no entry and the current cursor; a decimal since (or none, read as 0) returns the entries with a
    larger id, at most `limit`, with the last one's id as nextCursor, and evicted true when an error whose latest
    occurrence was newer than since has been evicted. Like the editor, it refuses with an EngineError a since that is
    neither empty, "end" nor a decimal cursor, and a limit outside 1 to 1000 (100 when absent), so a page the editor
    would refuse fails the soak here too. Each entry carries the editor's fields, the entity {id, name, path} included
    (ScriptMethods.cpp fills its id and name; its path stays empty), so a page has the editor's size, and every result
    goes through the Dispatcher's bounded output as a socket client's does (respond: over 48 KB it is offloaded to a
    file in `offload_directory`). It records the since of every script.errors call and the files it offloaded."""

    MAX_ENTRIES = 256  # MaxScriptErrorEntries, ScriptLimits.h
    OFFLOAD_THRESHOLD_BYTES = 48 * 1024  # DefaultOffloadThresholdBytes, ResultOffload.h
    SUMMARY_VERBATIM_BYTES = 512  # MaxOffloadSummaryVerbatimBytes, ResultOffload.h
    ENTITY = {"id": "5c1e9a77d0f4b312", "name": "Board", "path": ""}

    def __init__(self, clock: FakeClock, error: dict[str, Any] | list[dict[str, Any]] | None,
                 raised_by: str = "play.step", offload_directory: Path | None = None) -> None:
        super().__init__(clock, step_seconds=0.0)
        self.errors = [] if error is None else error if isinstance(error, list) else [error]
        self.raised_by = raised_by
        self.offload_directory = offload_directory
        self.offloaded: list[Path] = []
        self.stream: list[dict[str, Any]] = []
        self.cursor = 0
        self.evicted_through = 0
        for line in (1, 2, 3):
            self.add({"kind": "runtime", "script": "Assets/Scripts/Menu.luau", "line": line, "column": 1,
                      "message": "an error raised before the soak", "callback": "", "tick": 0, "traceback": [],
                      "jsonPointer": ""})
        self.raised = False
        self.since: list[str] = []

    def add(self, error: dict[str, Any]) -> None:
        """ScriptErrorStream::Add: the error with the next id and its count, replacing the same error or evicting the
        error whose latest occurrence is the oldest when the stream is full."""
        self.cursor += 1
        added = {"entity": self.ENTITY, **error, "id": self.cursor, "count": 1}
        key = ("script", "jsonPointer", "line", "message")
        previous = next((entry for entry in self.stream if all(entry[k] == added[k] for k in key)), None)
        if previous is not None:
            added["count"] = previous["count"] + 1
            self.stream.remove(previous)
        elif len(self.stream) == self.MAX_ENTRIES:
            self.evicted_through = self.stream.pop(0)["id"]
        self.stream.append(added)

    def call(self, method: str, params: dict[str, Any], timeout: float) -> dict[str, Any]:
        if method != "script.errors":
            if method == self.raised_by and not self.raised:
                self.raised = True
                for error in self.errors:
                    self.add(error)
            return super().call(method, params, timeout)
        self.calls.append((method, timeout))
        since = str((params or {}).get("since", ""))
        self.since.append(since)
        if since not in ("", "end") and not (since.isascii() and since.isdecimal()):
            raise FakeEngineError("expected an unsigned decimal cursor or end")
        limit = (params or {}).get("limit", 100)
        if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= 1000:
            raise FakeEngineError("limit must be from 1 to 1000")
        if since == "end":
            return self.respond({"errors": [], "nextCursor": str(self.cursor), "evicted": False})
        after = int(since) if since else 0
        page = [entry for entry in self.stream if entry["id"] > after][:limit]
        return self.respond({"errors": page, "nextCursor": str(page[-1]["id"] if page else after),
                             "evicted": self.evicted_through > after})

    def respond(self, result: dict[str, Any]) -> dict[str, Any]:
        """The Dispatcher's bounded output for a client with offloading on, as every socket client has (§13.4,
        Dispatcher.cpp, ResultOffload.h): a result whose minified JSON is over OFFLOAD_THRESHOLD_BYTES is written,
        indented, to a new file in the offload directory, and the response carries {path, truncated: true, summary}
        instead, the summary repeating each member whose minified JSON is at most SUMMARY_VERBATIM_BYTES and giving the
        type and count of a longer array (MakeOffloadSummary). A result to offload without an offload directory is an
        AssertionError, which no soak catches, so a test whose pages grow past the limit cannot skip the offload."""
        def minified_size(value: Any) -> int:
            return len(json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))

        if minified_size(result) <= self.OFFLOAD_THRESHOLD_BYTES:
            return result
        if self.offload_directory is None:
            raise AssertionError(f"a {minified_size(result)}-byte script.errors result needs an offload directory")
        self.offload_directory.mkdir(parents=True, exist_ok=True)
        path = self.offload_directory / f"4242-1791244800-{len(self.offloaded) + 1:08d}.json"
        path.write_text(json.dumps(result, ensure_ascii=False, indent="\t"), encoding="utf-8")
        self.offloaded.append(path)
        summary = {name: value if minified_size(value) <= self.SUMMARY_VERBATIM_BYTES
                   else {"type": "array", "count": len(value)} for name, value in result.items()}
        return {"path": str(path), "truncated": True, "summary": summary}


class FailingScriptErrorClient(ScriptErrorClient):
    """A ScriptErrorClient whose script.errors fails with an EngineError for the since `failing` (recorded like the
    others), as a host without a script error service answers (Unsupported); the calls with any other since are
    served, so a failure on the page after one that returned errors is reachable."""

    def __init__(self, clock: FakeClock, error: dict[str, Any] | None, failing: str) -> None:
        super().__init__(clock, error)
        self.failing = failing

    def call(self, method: str, params: dict[str, Any], timeout: float) -> dict[str, Any]:
        if method == "script.errors" and str((params or {}).get("since", "")) == self.failing:
            self.calls.append((method, timeout))
            self.since.append(self.failing)
            raise FakeEngineError("no script error service")
        return super().call(method, params, timeout)


class EndingScriptErrorClient(ScriptErrorClient):
    """A ScriptErrorClient whose method `raised_by` fails with an EngineError the first time it is called, after the
    error it raises has joined the stream (play.start refusing after an OnStart error, or play.step after an error
    raised while playing), so the soak ends early with that error and must still read the script errors."""

    def __init__(self, clock: FakeClock, error: dict[str, Any] | None, raised_by: str) -> None:
        super().__init__(clock, error, raised_by)
        self.failed = False

    def call(self, method: str, params: dict[str, Any], timeout: float) -> dict[str, Any]:
        result = super().call(method, params, timeout)
        if method == self.raised_by and not self.failed:
            self.failed = True
            raise FakeEngineError("the play session stopped")
        return result


class CannedScriptErrorClient(ScriptErrorClient):
    """A ScriptErrorClient whose script.errors returns `answers[since]` for the since values it lists (recorded like the
    others), the malformed results and stuck pages a misbehaving editor could give; any other since is served. More
    than MAX_READS script.errors calls is an AssertionError, which no soak catches, so a reading loop that never ends
    fails the test instead of hanging it."""

    MAX_READS = 10

    def __init__(self, clock: FakeClock, error: dict[str, Any] | None, answers: dict[str, Any]) -> None:
        super().__init__(clock, error)
        self.answers = answers

    def call(self, method: str, params: dict[str, Any], timeout: float) -> dict[str, Any]:
        if method != "script.errors":
            return super().call(method, params, timeout)
        if len(self.since) >= self.MAX_READS:
            raise AssertionError(f"script.errors was called more than {self.MAX_READS} times: {self.since}")
        since = str((params or {}).get("since", ""))
        if since not in self.answers:
            return super().call(method, params, timeout)
        self.calls.append((method, timeout))
        self.since.append(since)
        return self.answers[since]


class DamagedOffloadScriptErrorClient(ScriptErrorClient):
    """A ScriptErrorClient that damages each file it offloads before answering with it, as a file removed or cut short
    before the soak reads it leaves it: `damage` "missing" removes it, "not JSON" cuts it short and "not an object"
    replaces it with a JSON array."""

    def __init__(self, clock: FakeClock, error: list[dict[str, Any]], offload_directory: Path, damage: str) -> None:
        super().__init__(clock, error, offload_directory=offload_directory)
        self.damage = damage

    def respond(self, result: dict[str, Any]) -> dict[str, Any]:
        response = super().respond(result)
        if response.get("truncated") is True:
            path = Path(response["path"])
            if self.damage == "missing":
                path.unlink()
            else:
                path.write_text('{"errors": [' if self.damage == "not JSON" else "[]", encoding="utf-8")
        return response


def distinct_errors(count: int) -> list[dict[str, Any]]:
    """`count` distinct short runtime errors of Assets/Scripts/Game.luau, lines 100 onwards at ticks 10 onwards, for a
    ScriptErrorClient to raise."""
    return [{"kind": "runtime", "script": "Assets/Scripts/Game.luau", "line": 100 + index, "column": 9,
             "message": f"attempt to index nil (line {100 + index})", "callback": "OnFixedUpdate", "tick": 10 + index,
             "traceback": [], "jsonPointer": ""} for index in range(count)]


def fake_engine_client(editor_process: FakeEditorProcess) -> types.SimpleNamespace:
    """The engine_client module as the soak uses it, around `editor_process`: its exceptions (the real ProtocolError,
    which the real load_offloaded raises for a file that is not an object) and the real load_offloaded, which reads an
    offloaded result's file (§13.4)."""
    return types.SimpleNamespace(EngineError=FakeEngineError, ConnectionClosed=ConnectionError,
                                 ProtocolError=engine_client.ProtocolError, CallLog=lambda path: None,
                                 launch_editor=lambda *arguments, **options: editor_process,
                                 load_offloaded=engine_client.load_offloaded)


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

    def test_a_soak_subject_whose_scripts_raise_an_error_fails(self) -> None:
        # ADR 0027 decision 4, B4 (M17 gameplay map GDF-11): play.step keeps ticking through a non-fatal script error
        # (the editor only disables the faulting instance), so the soak passed a scene whose game had stopped with an
        # error at top-out. The soak takes script.errors' cursor after scene.open and before play.start, so an error
        # in OnStart (which play.start runs) counts, and reads from that cursor after its last step, following
        # nextCursor until a page is empty: a new error adds one line counting them and naming the earliest listed
        # error, at its latest tick and count, to the subject's errors, which fails the subject. Errors from before the
        # cursor never count.
        late = {"kind": "runtime", "script": "Assets/Scripts/Game.luau", "line": 42, "column": 9,
                "message": "attempt to index nil with 'Transform'", "callback": "OnFixedUpdate", "tick": 6012,
                "count": 1, "traceback": [], "jsonPointer": ""}
        start = {**late, "line": 17, "message": "attempt to call a nil value (field 'Instantiate')",
                 "callback": "OnStart", "tick": 0}
        for name, error, raised_by in (("none", None, "play.step"), ("while playing", late, "play.step"),
                                       ("in OnStart", start, "play.start")):
            with self.subTest(error=name):
                clock = FakeClock()
                client = ScriptErrorClient(clock, error, raised_by)
                editor = FakeEditorProcess(client)
                deadline = hardening_script.SoakDeadline(1000.0, clock)
                samples, errors = hardening_script.soak_editor_scene(
                    fake_engine_client(editor), self.directory / "Editor", self.directory / "P.eproj",
                    "Assets/Scenes/Main.scene", self.directory / "UserData", self.console, deadline)
                # Every step ran either way: the error does not end the soak early, it fails the subject after it.
                self.assertEqual(len(samples), hardening.SOAK_FRAMES // hardening_script.SOAK_STEP_TICKS + 1)
                methods = [method for method, _ in client.calls]
                self.assertLess(methods.index("scene.open"), methods.index("script.errors"))
                self.assertLess(methods.index("script.errors"), methods.index("play.start"))
                if error is None:
                    self.assertEqual(client.since, ["end", "3"])
                    self.assertEqual(errors, [])
                    continue
                self.assertEqual(client.since, ["end", "3", "4"])
                self.assertEqual(len(errors), 1, errors)
                for text in ("1 new script error(s) during the soak; the earliest listed: ",
                             f"{error['script']}:{error['line']}", error["message"],
                             f"(latest at tick {error['tick']}, 1 occurrence(s))"):
                    self.assertIn(text, errors[0])
                self.assertNotIn("an error raised before the soak", errors[0])
                subject = hardening_script.SoakSubject("Tetris:Assets/Scenes/Main.scene", "test",
                                                       lambda limit: (samples, errors))
                outcome = hardening_script.run_soak_subjects([subject], deadline, self.console,
                                                             self.directory / "Soak.json",
                                                             self.directory / "Soak-Release.xml")
                self.assertFalse(outcome.results[0].passed)
                self.assertIn(error["message"], outcome.results[0].failures[0])

    def test_a_soak_subject_whose_game_view_does_not_render_fails(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 11 (R-SOAK; ADR 0027 friction F08): the editor soak starts its
        # editor with --headless-game-view, so the game camera's passes and post-processing run while the scene plays,
        # and a subject whose stats.get reports no rendered game view fails, naming the tick.
        game = {"name": "game", "available": True, "width": 1600, "height": 900}
        scene = {"name": "scene", "available": True, "width": 983, "height": 446}
        self.assertEqual(hardening.soak_game_view_errors({"views": [scene, game]}, 250), [])
        hidden = hardening.soak_game_view_errors({"views": [scene, {"name": "game", "available": False, "width": 0,
                                                                       "height": 0}]}, 250)
        self.assertEqual(len(hidden), 1)
        self.assertIn("the game view did not render at tick 250", hidden[0])
        self.assertIn(hardening.SOAK_EDITOR_GAME_VIEW_OPTION, hidden[0])
        self.assertEqual(len(hardening.soak_game_view_errors({"views": [scene]}, 500)), 1)

        class HiddenGameClient(FakeClient):
            def call(self, method: str, params: dict[str, Any], timeout: float) -> dict[str, Any]:
                result = super().call(method, params, timeout)
                if method == "stats.get":
                    result = {**result, "views": [scene, {"name": "game", "available": False, "width": 0, "height": 0}]}
                return result

        for name, client_type in (("rendered", None), ("hidden", HiddenGameClient)):
            with self.subTest(game_view=name):
                clock = FakeClock()
                client = FakeClient(clock, 0.0) if client_type is None else client_type(clock, 0.0)
                if client_type is None:
                    original = client.call

                    def rendered(method: str, params: dict[str, Any], timeout: float,
                                 original: Callable[..., dict[str, Any]] = original) -> dict[str, Any]:
                        result = original(method, params, timeout)
                        return {**result, "views": [scene, game]} if method == "stats.get" else result

                    client.call = rendered  # type: ignore[method-assign]
                editor = FakeEditorProcess(client)
                launches: list[list[str]] = []
                engine = fake_engine_client(editor)

                def launch(executable: Path, arguments: list[str], *rest: object, editor: FakeEditorProcess = editor,
                           launches: list[list[str]] = launches, **options: object) -> FakeEditorProcess:
                    launches.append(list(arguments))
                    return editor

                engine.launch_editor = launch
                samples, errors = hardening_script.soak_editor_scene(
                    engine, self.directory / "Editor", self.directory / "P.eproj", "Assets/Scenes/Main.scene",
                    self.directory / "UserData", self.console, hardening_script.SoakDeadline(1000.0, clock))
                self.assertEqual(len(launches), 1)
                self.assertIn(hardening.SOAK_EDITOR_GAME_VIEW_OPTION, launches[0])
                if client_type is None:
                    self.assertEqual(errors, [])
                else:
                    self.assertEqual(len(errors), 1, errors)
                    self.assertIn("the game view did not render", errors[0])

    def test_the_soak_line_names_the_earliest_listed_error_with_its_latest_tick_and_count(self) -> None:
        # ADR 0027 Integration, stream B (review finding P-01): script.errors lists each distinct error once, at its
        # latest occurrence (a repeated error moves to the end with a new id, its latest tick and its count + 1, the
        # occurrences before the cursor included), and keeps the 256 errors with the latest occurrences, answering
        # evicted: true on a page after whose since one was evicted. The soak cannot know the first occurrence, so its
        # line names the earliest listed error with its latest tick and count, and says when earlier errors were
        # evicted. M17 milestone review P-01: the soak's connection has offloading on, so a page over 48 KB (the 256
        # retained errors, or 200 short distinct ones) comes back as {path, truncated: true, summary}; the soak reads it
        # from that file and still counts and names its errors.
        def game(line: int, tick: int) -> dict[str, Any]:
            return {"kind": "runtime", "script": "Assets/Scripts/Game.luau", "line": line, "column": 9,
                    "message": f"attempt to index nil (line {line})", "callback": "OnFixedUpdate", "tick": tick,
                    "traceback": [], "jsonPointer": ""}

        menu = {"kind": "runtime", "script": "Assets/Scripts/Menu.luau", "line": 2, "column": 1,
                "message": "an error raised before the soak", "callback": "OnFixedUpdate", "tick": 1000,
                "traceback": [], "jsonPointer": ""}
        listed = "new script error(s) during the soak; the earliest listed:"
        cases = {
            # Line 20 at tick 250, line 30 at 500, line 20 again at 750: line 30 is listed first.
            "a repeated error": ([game(20, 250), game(30, 500), game(20, 750)], ["end", "3", "6"],
                                 f"2 {listed} Assets/Scripts/Game.luau:30: attempt to index nil (line 30) (latest at "
                                 "tick 500, 1 occurrence(s))", 0),
            # An error raised before the cursor and again while the soak plays counts both occurrences.
            "an error raised before the soak too": ([menu], ["end", "3", "4"],
                                                    f"1 {listed} Assets/Scripts/Menu.luau:2: an error raised before "
                                                    "the soak (latest at tick 1000, 2 occurrence(s))", 0),
            # 200 distinct errors (ids 4 to 203), none evicted: their page is over 48 KB, so it is offloaded.
            "an offloaded page": (distinct_errors(200), ["end", "3", "203"],
                                  f"200 {listed} Assets/Scripts/Game.luau:100: attempt to index nil (line 100) (latest "
                                  "at tick 10, 1 occurrence(s))", 1),
            # 300 distinct errors (ids 4 to 303): the stream keeps ids 48 to 303, so 44 of them were evicted; the page
            # of the 256 retained ones is offloaded.
            "evicted errors": (distinct_errors(300), ["end", "3", "303"],
                               f"256 {listed} Assets/Scripts/Game.luau:144: attempt to index nil (line 144) (latest "
                               "at tick 54, 1 occurrence(s)); earlier errors were evicted", 1),
        }
        for name, (raised, since, expected, offloads) in cases.items():
            with self.subTest(case=name):
                clock = FakeClock()
                client = ScriptErrorClient(clock, raised, offload_directory=Path(tempfile.mkdtemp(dir=self.directory)))
                samples, errors = self.soak_scene(client, clock)
                self.assertEqual(len(samples), hardening.SOAK_FRAMES // hardening_script.SOAK_STEP_TICKS + 1)
                self.assertEqual(client.since, since)
                self.assertEqual(len(client.offloaded), offloads)
                self.assertEqual(errors, [expected])

    def test_the_fake_script_error_stream_merges_and_evicts_as_the_editor_does(self) -> None:
        # ScriptErrorStream::Add (ScriptError.cpp): a repeated error (the same script, jsonPointer, line and message)
        # moves to the end with the next id, its own tick and the count + 1; beyond MaxScriptErrorEntries (256) the
        # error whose latest occurrence is the oldest is evicted, and HasEvictedAfter(since) holds while an evicted id
        # is greater than since. The P-01 test relies on ScriptErrorClient doing the same. Pages of the full stream are
        # over 48 KB, so they are read through the offload as the soak reads them.
        client = ScriptErrorClient(FakeClock(), None, offload_directory=self.directory / "Out")
        client.add({**client.stream[0], "tick": 7})
        self.assertEqual([(entry["id"], entry["line"], entry["tick"], entry["count"]) for entry in client.stream],
                         [(2, 2, 0, 1), (3, 3, 0, 1), (4, 1, 7, 2)])
        for line in range(10, 10 + ScriptErrorClient.MAX_ENTRIES - 1):
            client.add({**client.stream[0], "line": line})
        self.assertEqual(len(client.stream), ScriptErrorClient.MAX_ENTRIES)
        self.assertEqual(client.stream[0]["id"], 4)  # ids 2 and 3 evicted

        def read(since: str) -> dict[str, Any]:
            return engine_client.load_offloaded(client.call("script.errors", {"since": since, "limit": 1000}, 1.0))

        self.assertTrue(read("2")["evicted"])
        self.assertFalse(read("3")["evicted"])
        self.assertEqual(len(read("")["errors"]), ScriptErrorClient.MAX_ENTRIES)

    def test_the_fake_script_error_stream_offloads_a_large_page_as_the_dispatcher_does(self) -> None:
        # Dispatcher.cpp and ResultOffload.h (§13.4): every socket client has offloading on, and a result whose
        # minified JSON, "_meta" excluded, is over 48 KB (49,152 bytes) is written to a file, the response carrying
        # {path, truncated: true, summary}. Entries with the editor's fields, the entity {id, name, path} included, put
        # the limit between 182 and 183 short errors, so the soak's tests meet the offload the real editor gives them.
        directory = self.directory / "Out"
        for count, offloaded in ((182, False), (183, True)):
            with self.subTest(count=count):
                client = ScriptErrorClient(FakeClock(), distinct_errors(count), offload_directory=directory)
                client.call("play.step", {"ticks": 250}, 1.0)
                response = client.call("script.errors", {"since": "3", "limit": 1000}, 1.0)
                full = engine_client.load_offloaded(response)
                self.assertEqual(len(full["errors"]), count)
                self.assertEqual(full["errors"][0]["entity"], ScriptErrorClient.ENTITY)
                size = len(json.dumps(full, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
                self.assertEqual(size > 49_152, offloaded, size)
                if not offloaded:
                    self.assertIs(full, response)
                    continue
                self.assertEqual(set(response), {"path", "truncated", "summary"})
                self.assertTrue(response["truncated"])
                self.assertEqual(Path(response["path"]).parent, directory)
                self.assertEqual(response["summary"], {"errors": {"type": "array", "count": count},
                                                       "nextCursor": str(3 + count), "evicted": False})
        client = ScriptErrorClient(FakeClock(), distinct_errors(200))
        client.call("play.step", {"ticks": 250}, 1.0)
        with self.assertRaisesRegex(AssertionError, "needs an offload directory"):
            client.call("script.errors", {"since": "3", "limit": 1000}, 1.0)

    def test_an_offloaded_script_errors_page_that_cannot_be_read_fails_the_soak_subject_naming_its_file(self) -> None:
        # M17 milestone review P-01: an offloaded page whose file is missing, is not JSON or is not an object is one
        # error line naming the call and the file, never the "no errors array" line the offloaded result's own members
        # would give; the reading stops there, and the soak still runs all its ticks.
        for damage in ("missing", "not JSON", "not an object"):
            with self.subTest(damage=damage):
                clock = FakeClock()
                client = DamagedOffloadScriptErrorClient(clock, distinct_errors(200),
                                                         Path(tempfile.mkdtemp(dir=self.directory)), damage)
                samples, errors = self.soak_scene(client, clock)
                self.assertEqual(len(samples), hardening.SOAK_FRAMES // hardening_script.SOAK_STEP_TICKS + 1)
                self.assertEqual(client.since, ["end", "3"])
                self.assertEqual(len(client.offloaded), 1)
                self.assertEqual(len(errors), 1, errors)
                self.assertTrue(errors[0].startswith(f'script.errors {{since: "3"}} failed: its offloaded result '
                                                     f"{client.offloaded[0]} cannot be read: "), errors[0])

    def test_a_failed_script_errors_call_fails_the_soak_subject_naming_it(self) -> None:
        # ADR 0027 decision 4, B4: a script.errors call that fails is an error line naming it, so a soak that cannot
        # read its scene's errors never passes as one without errors. The soak still runs all its ticks; without a
        # cursor it reads nothing after them.
        late = {"kind": "runtime", "script": "Assets/Scripts/Game.luau", "line": 42, "column": 9,
                "message": "attempt to index nil with 'Transform'", "callback": "OnFixedUpdate", "tick": 6012,
                "count": 1, "traceback": [], "jsonPointer": ""}
        cases = {"end": (["end"], ['script.errors {since: "end"} failed: no script error service']),
                 "3": (["end", "3"], ['script.errors {since: "3"} failed: no script error service']),
                 "4": (["end", "3", "4"], ["1 new script error(s) during the soak; the earliest listed: "
                                           "Assets/Scripts/Game.luau:42: attempt to index nil with 'Transform' (latest "
                                           "at tick 6012, 1 occurrence(s))",
                                           'script.errors {since: "4"} failed: no script error service'])}
        for failing, (since, expected) in cases.items():
            with self.subTest(failing=failing):
                clock = FakeClock()
                client = FailingScriptErrorClient(clock, late, failing)
                deadline = hardening_script.SoakDeadline(1000.0, clock)
                samples, errors = hardening_script.soak_editor_scene(
                    fake_engine_client(FakeEditorProcess(client)), self.directory / "Editor",
                    self.directory / "P.eproj", "Assets/Scenes/Main.scene", self.directory / "UserData", self.console,
                    deadline)
                self.assertEqual(len(samples), hardening.SOAK_FRAMES // hardening_script.SOAK_STEP_TICKS + 1)
                self.assertEqual(client.since, since)
                self.assertEqual(errors, expected)

    def soak_scene(self, client: FakeClient, clock: FakeClock) -> hardening_script.SoakRun:
        """soak_editor_scene of Assets/Scenes/Main.scene on `client`, with 1,000 s of `clock` left."""
        return hardening_script.soak_editor_scene(
            fake_engine_client(FakeEditorProcess(client)), self.directory / "Editor", self.directory / "P.eproj",
            "Assets/Scenes/Main.scene", self.directory / "UserData", self.console,
            hardening_script.SoakDeadline(1000.0, clock))

    def test_a_soak_that_ends_early_still_reads_its_script_errors(self) -> None:
        # ADR 0027 decision 4, B4 ("Frozen interfaces"): the errors are read after the soak's last step, all its ticks
        # or where an error ended it. A play.start that fails after an OnStart error, or a play.step that fails after an
        # error raised while playing, ends the soak with that error, and the script error that explains it still adds
        # its line after it.
        late = {"kind": "runtime", "script": "Assets/Scripts/Game.luau", "line": 42, "column": 9,
                "message": "attempt to index nil with 'Transform'", "callback": "OnFixedUpdate", "tick": 6012,
                "count": 1, "traceback": [], "jsonPointer": ""}
        start = {**late, "line": 17, "message": "attempt to call a nil value (field 'Instantiate')",
                 "callback": "OnStart", "tick": 0}
        cases = {"play.start": (start, 0, "the play session stopped",
                                "Assets/Scripts/Game.luau:17: attempt to call a nil value (field 'Instantiate') "
                                "(latest at tick 0, 1 occurrence(s))"),
                 "play.step": (late, 1, "play.step after tick 0 failed: the play session stopped",
                               "Assets/Scripts/Game.luau:42: attempt to index nil with 'Transform' (latest at tick "
                               "6012, 1 occurrence(s))")}
        for raised_by, (error, sampled, ended, earliest) in cases.items():
            with self.subTest(raised_by=raised_by):
                clock = FakeClock()
                client = EndingScriptErrorClient(clock, error, raised_by)
                samples, errors = self.soak_scene(client, clock)
                self.assertEqual(len(samples), sampled)
                self.assertEqual(client.since, ["end", "3", "4"])
                self.assertEqual(errors, [ended, f"1 new script error(s) during the soak; the earliest listed: "
                                                 f"{earliest}"])

    def test_a_malformed_or_stuck_script_errors_result_fails_the_soak_subject_naming_the_call(self) -> None:
        # ADR 0027 decision 4, B4: a script.errors result without the cursor the soak needs, without an errors array, or
        # with a page of errors that leaves nextCursor where it was (which would read the same page forever) is an error
        # line naming the call, after the count of what was read; the reading stops there, and the soak still runs all
        # its ticks.
        late = {"kind": "runtime", "script": "Assets/Scripts/Game.luau", "line": 42, "column": 9,
                "message": "attempt to index nil with 'Transform'", "callback": "OnFixedUpdate", "tick": 6012,
                "count": 1, "traceback": [], "jsonPointer": ""}
        counted = ("Assets/Scripts/Game.luau:42: attempt to index nil with 'Transform' (latest at tick 6012, "
                   "1 occurrence(s))")
        no_cursor = 'script.errors {since: "end"} failed: its result has no nextCursor'
        no_page = 'script.errors {since: "3"} failed: its result has no errors array or no nextCursor'
        stuck = 'script.errors {{since: "{since}"}} failed: it returned 1 error(s) without advancing its nextCursor'
        cases: dict[str, tuple[dict[str, Any], list[str], list[str]]] = {
            "no cursor at the end": ({"end": {}}, ["end"], [no_cursor]),
            "an empty cursor at the end": ({"end": {"errors": [], "nextCursor": "", "evicted": False}}, ["end"],
                                           [no_cursor]),
            "no nextCursor": ({"3": {"errors": [{**late, "id": 4}]}}, ["end", "3"], [no_page]),
            "no errors array": ({"3": {"nextCursor": "4", "evicted": False}}, ["end", "3"], [no_page]),
            "a stuck first page": ({"3": {"errors": [{**late, "id": 4}], "nextCursor": "3", "evicted": False}},
                                   ["end", "3"], [f"1 new script error(s) during the soak; the earliest listed: "
                                                  f"{counted}", stuck.format(since="3")]),
            "a stuck later page": ({"4": {"errors": [{**late, "id": 4}], "nextCursor": "4", "evicted": False}},
                                   ["end", "3", "4"], [f"2 new script error(s) during the soak; the earliest listed: "
                                                       f"{counted}", stuck.format(since="4")]),
        }
        for name, (answers, since, expected) in cases.items():
            with self.subTest(case=name):
                clock = FakeClock()
                client = CannedScriptErrorClient(clock, late, answers)
                samples, errors = self.soak_scene(client, clock)
                self.assertEqual(len(samples), hardening.SOAK_FRAMES // hardening_script.SOAK_STEP_TICKS + 1)
                self.assertEqual(client.since, since)
                self.assertEqual(errors, expected)

    def test_the_fake_script_error_stream_refuses_what_the_editor_refuses(self) -> None:
        # ScriptMethods.cpp refuses a limit outside 1 to 1000 and a since that is not a cursor; ScriptErrorClient does
        # too, so a soak that asked for a page the editor refuses would fail the B4 tests, not only the real soak.
        client = ScriptErrorClient(FakeClock(), None)
        for params in ({"since": "3", "limit": 0}, {"since": "3", "limit": 1001}, {"since": "end", "limit": 5000},
                       {"since": "-1"}, {"since": "three"}):
            with self.subTest(params=params), self.assertRaises(FakeEngineError):
                client.call("script.errors", params, 1.0)
        self.assertEqual(client.call("script.errors", {"since": "1", "limit": 1}, 1.0)["nextCursor"], "2")
        self.assertEqual(len(client.call("script.errors", {"since": "", "limit": 1000}, 1.0)["errors"]), 3)

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
