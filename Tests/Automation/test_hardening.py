"""The hardening stage's pure parts (Docs/Architecture.md §15.1 T6; Docs/Decisions/0024-m15-contract.md decisions 12 to
15, stream C): the AddressSanitizer options and runtime, the fuzz target list and the evaluation of the fuzz shards' log
lines, the soak scenes and the < 1 % growth rule, and the process memory query the soak and the perf capture share.
These tests need no editor build.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

from Lib import hardening, process  # noqa: E402  (Scripts/ is on sys.path above)


def report_line(case: str, begin: int, end: int, total: int) -> str:
    return f"[info] Fuzz '{case}': iterations {begin}..{end} of {total} (seed 0x00000000000009a4): 1 accepted, 2 rejected"


def sample(tick: int, private_bytes: int, entities: int = 10, script_heap: int = 1000,
           allocations: int = 100) -> hardening.SoakSample:
    """An editor soak sample after `tick` ticks."""
    return hardening.SoakSample(tick / hardening.SOAK_FRAMES, private_bytes, private_bytes, entities, 5, 0, script_heap,
                                allocations)


class AsanTests(unittest.TestCase):
    @unittest.skip("contract stub: hardening (ADR 0024 decision 12, stream C)")
    def test_asan_options_name_the_required_settings(self) -> None:
        options = dict(item.split("=", 1) for item in hardening.asan_options().split(":"))
        self.assertEqual(options["halt_on_error"], "1")
        self.assertEqual(options["allocator_may_return_null"], "0")

    @unittest.skip("contract stub: hardening (ADR 0024 decision 12, stream C)")
    def test_the_environment_puts_the_runtime_first_on_path(self) -> None:
        if sys.platform != "win32":
            return
        environment = hardening.asan_environment({"Path": r"C:\Windows", "OTHER": "kept"})
        self.assertEqual(environment["ASAN_OPTIONS"], hardening.asan_options())
        self.assertEqual(environment["OTHER"], "kept")
        entries = environment["Path"].split(os.pathsep)
        self.assertEqual(Path(entries[0]), hardening.asan_runtime_directory())
        self.assertIn(r"C:\Windows", entries)
        self.assertIn("quarantine_size_mb", hardening.asan_options())

    @unittest.skip("contract stub: hardening (ADR 0024 decision 12, stream C)")
    def test_the_runtime_is_staged_next_to_the_sanitized_executables_only(self) -> None:
        if sys.platform != "win32":
            return
        with tempfile.TemporaryDirectory(prefix="Asan-") as directory:
            workspace = Path(directory)
            output = workspace / "bin" / "Release-windows-x86_64"
            for project in hardening.ASAN_RUNTIME_PROJECTS:
                (output / project).mkdir(parents=True)
            (output / "Runtime" / "Redist").mkdir()
            copied = hardening.stage_asan_runtime(workspace, "Release")
            self.assertEqual(sorted(path.parent.name for path in copied if path.suffix == ".dll"),
                             sorted(hardening.ASAN_RUNTIME_PROJECTS))
            self.assertEqual(list((output / "Runtime" / "Redist").iterdir()), [])


class FuzzTests(unittest.TestCase):
    @unittest.skip("contract stub: hardening (ADR 0024 decision 13, stream C)")
    def test_the_committed_targets_read(self) -> None:
        targets = hardening.read_fuzz_targets()
        readers = [target.reader for target in targets]
        self.assertEqual(readers, sorted(readers))
        self.assertEqual(len(set(readers)), len(readers))
        self.assertTrue(all(target.iterations == hardening.FUZZ_HARDENING_ITERATIONS for target in targets))
        self.assertFalse(any("'" in target.case for target in targets))

    @unittest.skip("contract stub: hardening (ADR 0024 decision 13, stream C)")
    def test_a_target_below_the_hardening_count_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="FuzzTargets-") as directory:
            path = Path(directory) / "Targets.json"
            path.write_text(json.dumps({"Format": "FuzzTargets", "Version": 1, "Targets": [
                {"Reader": "PakReader", "Case": "PakReader: fuzz", "Iterations": 1000}]}), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Iterations"):
                hardening.read_fuzz_targets(path)

    @unittest.skip("contract stub: hardening (ADR 0024 decision 13, stream C)")
    def test_the_process_count_is_bounded_by_cpus_the_cap_and_memory(self) -> None:
        gib = 1 << 30
        self.assertEqual(hardening.fuzz_process_count(24, 64 * gib, gib), 24)
        self.assertEqual(hardening.fuzz_process_count(64, 256 * gib, gib), hardening.FUZZ_MAX_PROCESSES)
        self.assertEqual(hardening.fuzz_process_count(24, 8 * gib, gib), 8)
        self.assertEqual(hardening.fuzz_process_count(24, gib // 2, gib), 1)
        with self.assertRaises(ValueError):
            hardening.fuzz_process_count(0, gib, gib)

    @unittest.skip("contract stub: hardening (ADR 0024 decision 13, stream C)")
    def test_shards_must_cover_every_iteration_exactly_once(self) -> None:
        with tempfile.TemporaryDirectory(prefix="FuzzTargets-") as directory:
            path = Path(directory) / "Targets.json"
            path.write_text(json.dumps({"Format": "FuzzTargets", "Version": 1, "Targets": [
                {"Reader": "PakReader", "Case": "PakReader: fuzz", "Iterations": 10},
                {"Reader": "ZipReader", "Case": "ZipReader: fuzz", "Iterations": 10}]}), encoding="utf-8")
            targets = hardening.read_fuzz_targets(path)
        lines = [report_line("PakReader: fuzz", 0, 5, 10), report_line("PakReader: fuzz", 5, 10, 10),
                 report_line("ZipReader: fuzz", 0, 4, 10), report_line("Other: fuzz", 0, 1, 1)]
        self.assertEqual(hardening.evaluate_fuzz_logs(targets, lines),
                         ["fuzz: ZipReader: ran 4 of 10 iterations", "fuzz: Other: fuzz: not a target"])
        self.assertEqual(hardening.evaluate_fuzz_logs(targets, lines[:2]), ["fuzz: ZipReader: did not run"])


class SoakTests(unittest.TestCase):
    @unittest.skip("contract stub: hardening (ADR 0024 decision 15, stream C)")
    def test_soak_covers_every_scene_of_every_project(self) -> None:
        projects = hardening.soak_projects(REPOSITORY_ROOT)
        self.assertIn(REPOSITORY_ROOT / "Projects" / "FeatureTest", projects)
        self.assertEqual(projects, sorted(projects))
        scenes = hardening.soak_scenes(REPOSITORY_ROOT / "Projects" / "FeatureTest")
        self.assertIn("Assets/Scenes/Physics.scene", scenes)
        self.assertIn("Assets/Scenes/Components/TonemapAces.scene", scenes)
        self.assertIn("Assets/Scenes/Golden/Bloom.scene", scenes)
        self.assertEqual(scenes, sorted(scenes))

    @unittest.skip("contract stub: hardening (ADR 0024 decision 15, stream C)")
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

    @unittest.skip("contract stub: hardening (ADR 0024 decision 15, stream C)")
    def test_garbage_collected_and_small_counters_do_not_fail_without_a_leak(self) -> None:
        ticks = range(0, hardening.SOAK_FRAMES + 1, hardening.SOAK_SAMPLE_INTERVAL)
        # A Luau heap sawtooth between 1 and 2 MB, and an allocation count that wobbles by one: no leak.
        sawtooth = [sample(tick, 100_000_000, script_heap=1_000_000 + (tick % 1000) * 1000,
                           allocations=100 + (tick // 250) % 2) for tick in ticks]
        result = hardening.evaluate_soak("FeatureTest:Assets/Scenes/Scripting.scene", sawtooth)
        self.assertTrue(result.passed, result.failures)
        # The heap's floor rising above the earlier peaks, or allocations climbing, is a leak.
        rising = [sample(tick, 100_000_000, script_heap=1_000_000 + tick * 500) for tick in ticks]
        self.assertFalse(hardening.evaluate_soak("FeatureTest:Assets/Scenes/Scripting.scene", rising).passed)
        climbing = [sample(tick, 100_000_000, allocations=100 + tick // 500) for tick in ticks]
        self.assertFalse(hardening.evaluate_soak("FeatureTest:Assets/Scenes/Scripting.scene", climbing).passed)

    @unittest.skip("contract stub: hardening (ADR 0024 decision 15, stream C)")
    def test_the_runtime_soak_has_only_memory_samples(self) -> None:
        samples = [hardening.SoakSample(index / 100, 80_000_000, 90_000_000) for index in range(101)]
        result = hardening.evaluate_soak("Runtime:FeatureTest", samples)
        self.assertTrue(result.passed, result.failures)
        zero = [hardening.SoakSample(index / 100, 0, 0) for index in range(101)]
        self.assertFalse(hardening.evaluate_soak("Runtime:FeatureTest", zero).passed)

    @unittest.skip("contract stub: process memory (ADR 0024 decision 15, stream C)")
    def test_process_memory_of_this_process(self) -> None:
        memory = process.query_process_memory(os.getpid())
        self.assertGreater(memory.private_bytes, 0)
        self.assertGreater(memory.resident_bytes, 0)
        self.assertGreaterEqual(memory.peak_resident_bytes, memory.resident_bytes)


if __name__ == "__main__":
    unittest.main()
