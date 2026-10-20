"""The hardening stage's AddressSanitizer build, long fuzz and soak (Docs/Architecture.md §15.1 T6, §15.8;
Docs/Decisions/0024-m15-contract.md decisions 12 to 15). Scripts/Hardening.py runs it; CI.py's hardening stage runs that
at milestone gates (CI.py --milestone).

AddressSanitizer: premake --sanitize=address instruments the Release configuration of every project; the workspace is
generated into ASAN_WORKSPACE (Generate.py --sanitize address --to bin-int/Asan) and built there, so its binaries live
in bin-int/Asan/bin/Release-<system>-<arch>/ and never replace the plain Release build. The MSVC runtime
clang_rt.asan_dynamic-x86_64.dll is copied next to Tests, Editor and Runtime there (never into Runtime/Redist, which
exports copy), and every process the sanitized suites start gets asan_environment(): ASAN_OPTIONS, and the runtime's
directory first on PATH, so the instrumented Runtime copies that tests export into temporary packages (the resource and
user-data acceptance tests) load it too.

Fuzz: Tests/Data/Fuzz/Targets.json lists every reader of external input with its fuzz case (§15.2), each at
FUZZ_HARDENING_ITERATIONS, and the stage runs the sanitized Tests binary with --test-suite=Fuzz
--fuzz-iterations=1000000 --fuzz-shard=<k>/<n> in fuzz_process_count() processes. Each case logs one
FUZZ_REPORT_PATTERN line per process (Tests/Source/Support/Fuzz.h, ReportFuzzRun) through the console sink, which writes
to the process's standard error at every level; the stage reads those lines from each process's captured standard error,
never from Tests.log, whose rotation could drop them. It fails when a target's shards do not cover [0, its iterations)
exactly once, or when any process fails.

The fuzz step is sized from per-target costs (Docs/Decisions/0028-backlog-program.md decision 16, T-FUZZMODEL), never by
changing a target's iterations: read_fuzz_costs reads each target's measured seconds per iteration from the JUnit report
of the last full, passing fuzz step on this machine, which every checkout of the repository shares
(machine_fuzz_costs_path: <git common directory>/fuzz-costs/Fuzz-Release-Asan.xml, written only by a passing run of
FUZZ_HARDENING_ITERATIONS, so a development or failed run in any checkout never replaces it), or, before the first such
run, from M17's committed report (FUZZ_COMMITTED_COSTS). fuzz_target_seconds prices every target from them (a target not
yet measured at the measured targets' average; without any measurement, the reference run scaled by the target count),
and their sum, the step's work, sets the fewest shards whose expected time fits the step's budget
(fuzz_minimum_processes, fuzz_budget_seconds, fuzz_capacity_problem), the step's timeout at the shard count that runs
(fuzz_timeout_seconds) and each case's limit (fuzz_timeout_scale). Each shard runs 1/n of every target, but beyond the
reference run's 19 shards (or the logical CPUs) more shards run no faster, so the timeout and each case's limit count at
most that many (fuzz_effective_processes); the limit is raised when the dearest target needs more.

Soak: every scene of every project soak_projects() names (Projects/FeatureTest, the golden scenes included, and each
demo project once it exists) plays SOAK_FRAMES lockstep ticks (Hardening.py --soak-frames sets a longer soak) with
rendering in the plain Release editor, sampled every SOAK_SAMPLE_INTERVAL ticks (Lib/process.py query_process_memory and
stats.get); and the exported Release FeatureTest (bin/Exports/Release/FeatureTest, which the export stage leaves) runs
its start scene for as many frames headless, its memory sampled from outside every SOAK_RUNTIME_SAMPLE_SECONDS, so the
Runtime-only paths (the pak mounts, the Runtime's asset manager and frame loop) are soaked too. evaluate_soak applies
the rules of decision 15 to each, the script heap's floors-to-floors rule included (evaluate_script_heap).
"""

from __future__ import annotations

import dataclasses
import json
import math
import os
import re
import shutil
import statistics
import xml.etree.ElementTree as ElementTree
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from Lib import paths, vendor_cache

ASAN_WORKSPACE = paths.BIN_INT_ROOT / "Asan"
ASAN_RUNTIME_FILES = ("clang_rt.asan_dynamic-x86_64.dll", "clang_rt.asan_dynamic-x86_64.pdb")
# The projects whose output directories receive the runtime.
ASAN_RUNTIME_PROJECTS = ("Tests", "Editor", "Runtime")
# ASan's quarantine (freed memory held back to catch use-after-free) per process, bounded so FUZZ_MAX_PROCESSES
# sanitized processes fit in the memory of the machine that runs the stage (ASan's own default is 256 MB).
ASAN_QUARANTINE_MB = 64
# The handler settings the crash-handler tests need (stream C's spike, ADR 0024 risk 2): handle_segv=0 leaves access
# violations to the engine's own unhandled-exception filter (Platform/CrashHandler), which writes the crash report and
# exits with code 4, instead of ASan's handler, which would print its own report and abort; ASan still reports every
# memory error it detects through its instrumentation.
ASAN_HANDLER_OPTIONS = ("handle_segv=0",)
FUZZ_SUITE = "Fuzz"  # the doctest suite of the fuzz cases (Test::FuzzSuite, Tests/Source/Support/Fuzz.h)
FUZZ_TARGETS_FILE = paths.REPOSITORY_ROOT / "Tests" / "Data" / "Fuzz" / "Targets.json"
FUZZ_TARGETS_FORMAT = "FuzzTargets"
FUZZ_TARGETS_VERSION = 1
FUZZ_HARDENING_ITERATIONS = 1_000_000
FUZZ_MAX_PROCESSES = 32
# The private memory one sanitized fuzz process holds (stream C's measurement of a --test-suite=Fuzz shard, rounded up),
# which fuzz_process_count divides the available memory by: 19 shards of 1,000,000 iterations held 16 to 18 GB together
# and at most 965 MB each, sampled every five minutes. A single decode (a PNG or a texture whose header asks for the
# largest image its bytes allow) briefly raises one process to about 2.8 GB; those spikes last under a second and do not
# coincide across shards, so they are not counted (the machine kept more than 5 GB free throughout).
FUZZ_PEAK_PROCESS_BYTES = 1 << 30
# The reference fuzz run (Docs/Decisions/0024-m15-contract.md "Budgets and timeouts"): FUZZ_REFERENCE_PROCESSES shards
# of FUZZ_HARDENING_ITERATIONS iterations of each of M15's FUZZ_REFERENCE_TARGETS targets took FUZZ_REFERENCE_SECONDS in
# a step whose timeout was FUZZ_REFERENCE_TIMEOUT_SECONDS, with each case's limit scaled by FUZZ_REFERENCE_TIMEOUT_SCALE
# (FUZZ_CASE_LIMIT_SECONDS, so 2,400 s, against the Bindings case's 27,600 CPU-s / 19 = 1,453 s per shard). Without a
# measured report the step's work is this run's, scaled by the target count. Each shard runs 1/n of every target, so the
# limit its cases need grows as the reference's times FUZZ_REFERENCE_PROCESSES / n below the reference's count; above it
# the limit stays the reference's, since logical CPUs beyond the physical cores do not run a shard's share any faster.
FUZZ_REFERENCE_PROCESSES = 19
FUZZ_REFERENCE_SECONDS = 52 * 60.0
FUZZ_REFERENCE_TARGETS = 35
FUZZ_REFERENCE_TIMEOUT_SECONDS = 5400.0
FUZZ_REFERENCE_TIMEOUT_SCALE = 20
# The Tests binary's default per-case limit (--test-timeout, Tests/Source/Support/TestOptions.h), which no fuzz case
# replaces and --test-timeout-scale multiplies.
FUZZ_CASE_LIMIT_SECONDS = 120.0
# The share of the fuzz step's budget, and of its timeout, the expected run may take; the rest absorbs the run-to-run
# variance (stream C's run came within 8 minutes of the former 3,600 s timeout; M17's gate ran 3,312 s at 18 shards,
# 1.7 % above the 3,255 s its measured costs give).
FUZZ_BUDGET_SHARE = 0.8
# The shortest timeout the fuzz step gets: per-iteration costs leave out each shard's fixed costs (starting a sanitized
# process, each case's setup), which dominate a development run of few iterations.
FUZZ_MINIMUM_TIMEOUT_SECONDS = 600.0
# Where the last full, passing fuzz step's report is kept for the next one, in every checkout of the repository
# (machine_fuzz_costs_path), and the committed measurement read before the first such run (M17's gate,
# Tests/Data/LICENSES.md).
FUZZ_COSTS_DIRECTORY_NAME = "fuzz-costs"
FUZZ_COSTS_FILE_NAME = "Fuzz-Release-Asan.xml"
FUZZ_COMMITTED_COSTS = paths.REPOSITORY_ROOT / "Tests" / "Data" / "Hardening" / "FuzzM17.xml"
# How the output of a passed target's test case in the fuzz step's JUnit report starts (fuzz_target_detail), which
# read_fuzz_costs reads back.
FUZZ_JUNIT_ITERATIONS_PATTERN = re.compile(r"^(?P<iterations>\d+) iterations;")
# ReportFuzzRun's line (Tests/Source/Support/Fuzz.h). Fuzz case names contain no apostrophe.
FUZZ_REPORT_PATTERN = re.compile(
    r"Fuzz '(?P<case>[^']+)': iterations (?P<begin>\d+)\.\.(?P<end>\d+) of (?P<total>\d+) "
    r"\(seed 0x(?P<seed>[0-9a-f]{16})\): (?P<accepted>\d+) accepted, (?P<rejected>\d+) rejected"
)
# The soak's length in ticks (editor) and frames (Runtime): Hardening.py --soak-frames, at least this default and a
# multiple of SOAK_SAMPLE_INTERVAL. A longer soak confirms a scene whose script heap rule was inconclusive.
SOAK_FRAMES = 10_000
# M19 (Docs/Decisions/0030-m19-contract.md decision 11, R-SOAK): the option every editor soak subject's editor starts
# with, so the headless editor renders the Game view (the game camera and its post-processing) while the scene plays.
SOAK_EDITOR_GAME_VIEW_OPTION = "--headless-game-view"
SOAK_SAMPLE_INTERVAL = 250  # ticks between the samples of an editor scene soak
# What an inconclusive script heap result suggests: the same subject soaked longer.
SOAK_LONGER_COMMAND = "Hardening.py --steps soak --soak-only <subject> --soak-frames <n>"
SOAK_RUNTIME_SAMPLE_SECONDS = 0.5  # seconds between the samples of the Runtime soak
# The rule's windows, as fractions of the soak (position = tick / soak frames in the editor, elapsed / total run time in
# the Runtime), whatever its length: the first half absorbs warm-up, buffers that fill and the Luau GC reaching its
# steady state.
SOAK_BASELINE_WINDOW = (0.50, 0.55)  # inclusive
SOAK_FINAL_WINDOW = (0.95, 1.00)  # inclusive
# The script heap rule compares floors with floors. A Luau heap is a garbage-collected sawtooth (LUAI_GCGOAL 200, so
# its ceiling sits near twice the live data and rises with a leak too), whose collection cycle can last almost half the
# soak (Input.scene's once lasted 4,000 to 5,000 ticks; Docs/Decisions/0024-m15-contract.md "Integration"). Only the
# samples taken just after a collection show the live data: a trough is a sample whose heap is lower than the previous
# sample's. The reference floor is the lowest trough of the reference window, after the first quarter's warm-up and
# before the last quarter; the final floor the lowest trough of the last quarter. The windows bound the cycles the rule
# can judge: a cycle longer than about a quarter of the soak (2,500 to 2,750 ticks of the default 10,000; the trough is
# the sample after a collection, every 250 ticks) can leave the last quarter without a collection, and one longer than
# half the soak the reference window, so whether such a scene passes or is inconclusive depends only on its cycle's
# phase. An inconclusive scene is confirmed with a longer soak (SOAK_LONGER_COMMAND), whose windows hold more ticks; no
# recorded non-leaking soak comes near it (troughs at most 1,250 ticks apart, Tests/Data/Hardening/).
SOAK_HEAP_REFERENCE_WINDOW = (0.25, 0.75)  # 0.25 <= position < 0.75
SOAK_HEAP_FINAL_WINDOW = (0.75, 1.00)  # inclusive
# How far the final floor may lie above the reference floor: the larger of 128 KiB and 10 % of the reference floor.
# A trough is a snapshot taken every 250 ticks of an incremental collector, so it holds the floor plus what the sweep
# had not freed yet and what was allocated since. Over the soaks that do not leak (stream C's eight scripted scenes and
# the integration run's bounded Callbacks.scene and Input.scene, Tests/Data/Hardening/), the final floor lay at most
# 32,720 bytes (2.5 %) above the reference floor; stream C's leaking Callbacks.scene lay 2,473,432 bytes (73 %) above
# it. The tolerance is four times the largest noise seen at today's 0.5 to 2 MB floors and grows with larger heaps;
# with the two floors about half the soak (5,000 ticks) apart it flags a leak of 26 to 40 bytes per tick at those
# floors (6 to 9 MB per hour of play at 60 Hz), far below the private memory rule's reach (1 % of a 265 MB editor,
# about 530 bytes per tick).
SOAK_HEAP_TOLERANCE_BYTES = 128 * 1024
SOAK_HEAP_TOLERANCE_FRACTION = 0.10
SOAK_GROWTH_LIMIT = 0.01  # private-memory growth must stay below 1 %
# The project directory, scene directory and project file the soak reads.
SOAK_PROJECTS_DIRECTORY = "Projects"
SOAK_SCENES_DIRECTORY = "Assets/Scenes"
# The stats.get fields of the absolute counter rules, with the SoakSample attribute each fills.
SOAK_COUNTER_FIELDS = (("entities", "entities"), ("bodies", "bodies"), ("voices", "voices"),
                       ("memory allocation count", "memory_allocation_count"))


@dataclasses.dataclass(frozen=True)
class FuzzTarget:
    """One entry of Targets.json: {"Reader", "Case", "Iterations"}."""

    reader: str  # the reader of external input, as the module docstring of its header names it ("PakReader")
    case: str  # the doctest case name of its fuzz case (in the Fuzz suite)
    iterations: int  # what the hardening stage runs: FUZZ_HARDENING_ITERATIONS for every target, without exception


@dataclasses.dataclass(frozen=True)
class SoakSample:
    """One soak sample. The stats.get fields are None for the Runtime soak, which has no automation."""

    position: float  # how far into the soak, 0.0 to 1.0
    private_bytes: int
    resident_bytes: int
    entities: int | None = None
    bodies: int | None = None
    voices: int | None = None
    script_heap_bytes: int | None = None
    memory_allocation_count: int | None = None


@dataclasses.dataclass(frozen=True)
class SoakResult:
    subject: str  # "<project>:<scene>" for an editor soak, "Runtime:<project>" for the Runtime soak
    growth: float  # (final - baseline) / baseline of private bytes; 0.0 when it could not be computed
    passed: bool
    failures: tuple[str, ...]  # one line per broken rule


# --------------------------------------------------------------------------------------------------------------------
# AddressSanitizer
# --------------------------------------------------------------------------------------------------------------------


def asan_options() -> str:
    """The ASAN_OPTIONS every sanitized process gets: colon-separated, always with halt_on_error=1,
    allocator_may_return_null=0 and quarantine_size_mb=<n> (bounded so FUZZ_MAX_PROCESSES processes fit in memory), plus
    the handler settings the crash-handler tests need (ASAN_HANDLER_OPTIONS)."""
    return ":".join(("halt_on_error=1", "allocator_may_return_null=0", f"quarantine_size_mb={ASAN_QUARANTINE_MB}",
                     *ASAN_HANDLER_OPTIONS))


def asan_runtime_directory() -> Path:
    """The MSVC tools' bin/Hostx64/x64 directory holding ASAN_RUNTIME_FILES (Lib/toolchain.py's Visual Studio).
    Raises FileNotFoundError naming Visual Studio's "C++ AddressSanitizer" component when the runtime is missing."""
    from Lib import toolchain  # noqa: PLC0415  (vswhere runs only when a sanitized step needs the runtime)

    if os.name != "nt":
        raise FileNotFoundError("the hardening stage's AddressSanitizer runtime is MSVC's (Windows only; "
                                "Docs/Decisions/0024-m15-contract.md decision 12)")
    try:
        return toolchain.find_asan_runtime(toolchain.find_visual_studio())
    except toolchain.ToolchainError as error:
        raise FileNotFoundError(str(error)) from None


def asan_environment(base: Mapping[str, str]) -> dict[str, str]:
    """A copy of `base` (an environment) for a process of the sanitized suites: ASAN_OPTIONS set to asan_options(), and
    asan_runtime_directory() put first on PATH (on Windows the variable's existing spelling, Path or PATH, is kept), so
    every child the process starts, an exported Runtime copy included, finds the runtime. Raises FileNotFoundError as
    asan_runtime_directory."""
    environment = dict(base)
    runtime = str(asan_runtime_directory())
    environment["ASAN_OPTIONS"] = asan_options()
    if os.name == "nt":
        key = next((name for name in environment if name.upper() == "PATH"), "Path")
    else:
        key = "PATH"
    existing = [entry for entry in environment.get(key, "").split(os.pathsep) if entry]
    environment[key] = os.pathsep.join([runtime, *existing])
    return environment


def stage_asan_runtime(workspace: Path, config: str) -> list[Path]:
    """Copies ASAN_RUNTIME_FILES next to each of ASAN_RUNTIME_PROJECTS in <workspace>/bin/<OutputDir>/ (unchanged files
    are kept) and returns the copied paths. Raises FileNotFoundError as asan_runtime_directory, or when a project's
    output directory does not exist."""
    runtime = asan_runtime_directory()
    output = workspace / "bin" / paths.output_directory_name(config)
    destinations = [output / project for project in ASAN_RUNTIME_PROJECTS]
    for destination in destinations:
        if not destination.is_dir():
            raise FileNotFoundError(f"{paths.display_path(destination)} does not exist: build the sanitized workspace "
                                    f"first (Build.py --workspace-dir {paths.display_path(workspace)} --config "
                                    f"{config})")
    copied: list[Path] = []
    for destination in destinations:
        for name in ASAN_RUNTIME_FILES:
            source = runtime / name
            target = destination / name
            if target.is_file() and _same_file_contents(source, target):
                continue
            shutil.copy2(source, target)
            copied.append(target)
    return copied


def _same_file_contents(first: Path, second: Path) -> bool:
    first_status, second_status = first.stat(), second.stat()
    if first_status.st_size != second_status.st_size:
        return False
    if int(first_status.st_mtime) == int(second_status.st_mtime):
        return True  # copy2 keeps the modification time, so an unchanged copy has the source's
    with first.open("rb") as left, second.open("rb") as right:
        while True:
            left_block, right_block = left.read(1 << 20), right.read(1 << 20)
            if left_block != right_block:
                return False
            if not left_block:
                return True


# --------------------------------------------------------------------------------------------------------------------
# Fuzz
# --------------------------------------------------------------------------------------------------------------------


def read_fuzz_targets(path: Path = FUZZ_TARGETS_FILE) -> list[FuzzTarget]:
    """Targets.json ({"Format": "FuzzTargets", "Version": 1, "Targets": [...]}, sorted by Reader, unique Reader and
    Case). Raises ValueError naming the file and the problem, including a target whose Iterations is not
    FUZZ_HARDENING_ITERATIONS or whose Case holds an apostrophe."""
    where = paths.display_path(path)
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"{where}: cannot be read: {error}") from None
    if not isinstance(document, dict):
        raise ValueError(f"{where}: the document is not an object")
    if document.get("Format") != FUZZ_TARGETS_FORMAT or document.get("Version") != FUZZ_TARGETS_VERSION:
        raise ValueError(f"{where}: Format must be \"{FUZZ_TARGETS_FORMAT}\" and Version {FUZZ_TARGETS_VERSION}")
    entries = document.get("Targets")
    if not isinstance(entries, list) or not entries:
        raise ValueError(f"{where}: Targets must be a non-empty array")
    targets: list[FuzzTarget] = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict) or set(entry) != {"Reader", "Case", "Iterations"}:
            raise ValueError(f"{where}: Targets[{index}] must be an object with exactly Reader, Case and Iterations")
        reader, case, iterations = entry["Reader"], entry["Case"], entry["Iterations"]
        if not isinstance(reader, str) or not reader or not isinstance(case, str) or not case:
            raise ValueError(f"{where}: Targets[{index}]: Reader and Case must be non-empty strings")
        if "'" in case:
            raise ValueError(f"{where}: Targets[{index}] ({reader}): Case holds an apostrophe, which the fuzz report "
                             f"line quotes")
        if isinstance(iterations, bool) or not isinstance(iterations, int) or iterations != FUZZ_HARDENING_ITERATIONS:
            raise ValueError(f"{where}: Targets[{index}] ({reader}): Iterations is {iterations!r}; every reader runs "
                             f"exactly {FUZZ_HARDENING_ITERATIONS} iterations in the hardening stage (ADR 0024 "
                             f"decision 13)")
        targets.append(FuzzTarget(reader, case, iterations))
    readers = [target.reader for target in targets]
    if readers != sorted(readers):
        raise ValueError(f"{where}: Targets must be sorted by Reader")
    for kind, values in (("Reader", readers), ("Case", [target.case for target in targets])):
        duplicates = sorted({value for value in values if values.count(value) > 1})
        if duplicates:
            raise ValueError(f"{where}: duplicate {kind} {', '.join(duplicates)}")
    return targets


def fuzz_process_count(logical_cpus: int, available_bytes: int, peak_bytes_per_process: int) -> int:
    """How many sanitized fuzz processes run at once: min(logical_cpus, FUZZ_MAX_PROCESSES,
    available_bytes // peak_bytes_per_process), at least 1. The stage passes the machine's logical CPUs, its available
    physical memory and FUZZ_PEAK_PROCESS_BYTES. Raises ValueError for a count or size below 1."""
    if logical_cpus < 1 or available_bytes < 1 or peak_bytes_per_process < 1:
        raise ValueError(f"fuzz_process_count needs positive values, got {logical_cpus} CPUs, {available_bytes} "
                         f"available bytes and {peak_bytes_per_process} bytes per process")
    return max(1, min(logical_cpus, FUZZ_MAX_PROCESSES, available_bytes // peak_bytes_per_process))


def fuzz_target_case_name(target: FuzzTarget) -> str:
    """The name of `target`'s test case in the fuzz step's JUnit report: "<Reader> (<Case>)"."""
    return f"{target.reader} ({target.case})"


def fuzz_target_detail(iterations: int, seconds_per_iteration: float) -> str:
    """The output of a passed target's test case in the fuzz step's JUnit report, whose time is the target's seconds
    summed over the shards: "<iterations> iterations; <microseconds> us per iteration" (FUZZ_JUNIT_ITERATIONS_PATTERN
    reads the count back)."""
    return f"{iterations} iterations; {seconds_per_iteration * 1e6:.1f} us per iteration"


def machine_fuzz_costs_path() -> Path:
    """Where the last full, passing fuzz step's JUnit report is kept: <git common directory>/fuzz-costs/
    Fuzz-Release-Asan.xml, beside the vendor cache and the machine-wide locks, so every checkout of the repository sizes
    its fuzz step from the same measurement whichever checkout ran the last hardened gate; bin-int/fuzz-costs/ of this
    checkout when git cannot name the common directory."""
    try:
        directory = vendor_cache.git_common_directory() / FUZZ_COSTS_DIRECTORY_NAME
    except vendor_cache.VendorCacheError:
        directory = paths.BIN_INT_ROOT / FUZZ_COSTS_DIRECTORY_NAME
    return directory / FUZZ_COSTS_FILE_NAME


def keep_fuzz_costs(report: Path, destination: Path) -> None:
    """Copies the fuzz step's report `report` to `destination` (machine_fuzz_costs_path) through a temporary file and a
    rename, so a reader sees the previous measurement or this one, never a partial file. Only a full, passing run calls
    it. Raises OSError."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f"{destination.name}.{os.getpid()}.tmp")
    try:
        shutil.copyfile(report, temporary)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def read_fuzz_costs(path: Path) -> dict[str, float]:
    """The per-target costs the fuzz step's JUnit report `path` (machine_fuzz_costs_path, or FUZZ_COMMITTED_COSTS)
    measured:
    {test case name: seconds per iteration, summed over the shards}, the name being fuzz_target_case_name's. A target's
    cost is its test case's time over the FUZZ_HARDENING_ITERATIONS its output names (fuzz_target_detail); a test case
    of a development run of fewer iterations is not read, since it counts each case's fixed setup many times over, nor
    is one with a time of 0. Empty when the report does not exist. Raises ValueError naming the file when it cannot be
    read or parsed, or when it records a failed or timed-out test case: a shard that crashed or was stopped wrote none
    of its cases' times, so even a passed target's time can fall short of its iterations."""
    where = paths.display_path(path)
    try:
        root = ElementTree.parse(path).getroot()
    except FileNotFoundError:
        return {}
    except (OSError, ElementTree.ParseError) as error:
        raise ValueError(f"{where}: cannot be read: {error}") from None
    costs: dict[str, float] = {}
    for case in root.iter("testcase"):
        name = case.get("name", "")
        if case.find("failure") is not None or case.find("error") is not None:
            raise ValueError(f"{where}: records a failed run ({name} did not pass), whose times can fall short")
        output = case.find("system-out")
        match = FUZZ_JUNIT_ITERATIONS_PATTERN.match((output.text or "") if output is not None else "")
        if match is None or int(match.group("iterations")) != FUZZ_HARDENING_ITERATIONS:
            continue
        try:
            seconds = float(case.get("time", ""))
        except ValueError:
            raise ValueError(f"{where}: test case {name!r} has no numeric time") from None
        if math.isfinite(seconds) and seconds > 0.0:
            costs[name] = seconds / FUZZ_HARDENING_ITERATIONS
    return costs


def fuzz_target_seconds(targets: list[FuzzTarget], costs: Mapping[str, float]) -> dict[str, float]:
    """Each target's expected seconds, summed over the shards, at its iterations, by reader: its cost per iteration
    from `costs` (read_fuzz_costs, looked up by fuzz_target_case_name) times its iterations. A target `costs` lacks (one
    added or renamed since the measurement, or one measured at a time of 0) costs the average of the measured targets'
    costs per iteration. When `costs` measures none of `targets`, every target costs the reference run's average per
    iteration (FUZZ_REFERENCE_SECONDS x FUZZ_REFERENCE_PROCESSES over FUZZ_REFERENCE_TARGETS x
    FUZZ_HARDENING_ITERATIONS): the reference run scaled by the target count."""
    measured = {target.reader: costs[fuzz_target_case_name(target)] for target in targets
                if fuzz_target_case_name(target) in costs}
    if measured:
        fallback = statistics.fmean(measured.values())
    else:
        fallback = (FUZZ_REFERENCE_SECONDS * FUZZ_REFERENCE_PROCESSES
                    / (FUZZ_REFERENCE_TARGETS * FUZZ_HARDENING_ITERATIONS))
    return {target.reader: measured.get(target.reader, fallback) * target.iterations for target in targets}


def fuzz_budget_seconds(target_count: int) -> float:
    """The longest the fuzz step may run for `target_count` targets: FUZZ_REFERENCE_TIMEOUT_SECONDS for the reference's
    FUZZ_REFERENCE_TARGETS, and as much again per target in proportion (about 154 s each), since every target keeps its
    1,000,000 iterations (ADR 0024 decision 14). The step's timeout never exceeds it (fuzz_timeout_seconds), so CI.py's
    bound on the whole hardening run must exceed the other steps' timeouts and graces plus this budget (Hardening.py
    longest_run_seconds; Tests/Automation/test_hardening.py checks it for the committed targets and that it holds up to
    49 targets)."""
    return FUZZ_REFERENCE_TIMEOUT_SECONDS * target_count / FUZZ_REFERENCE_TARGETS


def fuzz_expected_seconds(work_seconds: float, processes: int) -> float:
    """The expected wall time of a fuzz run whose targets take `work_seconds` summed over the shards (the sum of
    fuzz_target_seconds) in `processes` shards: each shard runs 1/processes of every target, so the work over the shard
    count, as the reference run and M17's gate measured it within 2 %."""
    return work_seconds / processes


def fuzz_effective_processes(processes: int, logical_cpus: int | None = None) -> int:
    """How many shards' speed `processes` shards deliver, as far as the model trusts it: at most
    FUZZ_REFERENCE_PROCESSES, since beyond the reference run's count more shards ran no faster (they share the
    machine's cores), and at most `logical_cpus` when given; at least 1."""
    return max(1, min(processes, FUZZ_REFERENCE_PROCESSES, logical_cpus if logical_cpus is not None else processes))


def fuzz_minimum_processes(work_seconds: float, budget_seconds: float) -> int:
    """The fewest shards whose expected wall time (fuzz_expected_seconds of `work_seconds`) fits in FUZZ_BUDGET_SHARE of
    the step's budget `budget_seconds` (fuzz_budget_seconds), at least 1: 14 for the reference run and for M17's
    measured costs alike, so a machine needs as many logical CPUs and GiB of available memory
    (fuzz_capacity_problem)."""
    return max(1, math.ceil(work_seconds / (budget_seconds * FUZZ_BUDGET_SHARE)))


def fuzz_timeout_seconds(work_seconds: float, processes: int, budget_seconds: float,
                         logical_cpus: int | None = None) -> float:
    """The fuzz step's timeout in `processes` shards on a machine of `logical_cpus`: the expected wall time at the
    speed the model trusts (fuzz_expected_seconds of `work_seconds` in fuzz_effective_processes shards, so a count
    beyond the reference's 19 or the logical CPUs shortens nothing) over FUZZ_BUDGET_SHARE, the margin the budget
    leaves the minimum shard count; at most the step's budget `budget_seconds` (fuzz_budget_seconds), which CI.py's
    bound on the whole hardening run covers, and at least FUZZ_MINIMUM_TIMEOUT_SECONDS."""
    expected = fuzz_expected_seconds(work_seconds, fuzz_effective_processes(processes, logical_cpus))
    return max(FUZZ_MINIMUM_TIMEOUT_SECONDS, min(budget_seconds, expected / FUZZ_BUDGET_SHARE))


def fuzz_timeout_scale(processes: int, iterations: int = FUZZ_HARDENING_ITERATIONS,
                       dearest_seconds: float = 0.0) -> int:
    """The --test-timeout-scale of a fuzz shard, at least 1:
      - the reference run's limit, FUZZ_REFERENCE_TIMEOUT_SCALE grown with the shard's share of the work below the
        reference run's shard count and never below the reference's scale above it, ceil(FUZZ_REFERENCE_TIMEOUT_SCALE *
        FUZZ_REFERENCE_PROCESSES / min(processes, FUZZ_REFERENCE_PROCESSES) * iterations / 1,000,000):
        max(20, ceil(20 * 19 / processes)) at 1,000,000 iterations. A shard's share shrinks with more shards, but its
        speed does not follow beyond the reference's count: more shards than physical cores share them through SMT, so
        each case keeps at least the reference run's limit;
      - raised when the dearest target, `dearest_seconds` summed over the shards at `iterations` (the largest
        fuzz_target_seconds), needs more: its share of a shard, counted as above, must fit in FUZZ_BUDGET_SHARE of
        the scaled FUZZ_CASE_LIMIT_SECONDS, the margin the step's timeout gives the whole run."""
    shards = min(processes, FUZZ_REFERENCE_PROCESSES)
    reference = -(-FUZZ_REFERENCE_TIMEOUT_SCALE * FUZZ_REFERENCE_PROCESSES * iterations
                  // (shards * FUZZ_HARDENING_ITERATIONS))
    dearest = math.ceil(dearest_seconds / (shards * FUZZ_CASE_LIMIT_SECONDS * FUZZ_BUDGET_SHARE))
    return max(1, reference, dearest)


def fuzz_capacity_problem(processes: int, minimum: int, logical_cpus: int, available_bytes: int,
                          peak_bytes_per_process: int, budget_seconds: float) -> str | None:
    """None when `processes` shards reach `minimum` (fuzz_minimum_processes); otherwise why the fuzz cannot finish in
    time: what falls short of `minimum` shards, the available memory (minimum x peak_bytes_per_process), the logical
    CPUs, or else the explicitly requested count. `budget_seconds` is the step's budget (fuzz_budget_seconds)."""
    if processes >= minimum:
        return None
    needs: list[str] = []
    if available_bytes // peak_bytes_per_process < minimum:
        needs.append(f"at least {minimum * peak_bytes_per_process >> 20} MB of available memory ({minimum} x "
                     f"{peak_bytes_per_process >> 20} MB per process; {available_bytes >> 20} MB are available)")
    if logical_cpus < minimum:
        needs.append(f"at least {minimum} logical CPUs ({logical_cpus} here)")
    if not needs:
        needs.append(f"--fuzz-processes of at least {minimum}")
    return (f"the fuzz needs at least {minimum} processes to finish within {FUZZ_BUDGET_SHARE:.0%} of its "
            f"{budget_seconds:.0f} s budget, and it would run {processes}: it needs {' and '.join(needs)}")


def available_memory_bytes() -> int:
    """The machine's available physical memory: GlobalMemoryStatusEx on Windows, MemAvailable of /proc/meminfo on
    Linux, otherwise the physical memory (sysconf). Raises OSError when it cannot be read."""
    if os.name == "nt":
        import ctypes  # noqa: PLC0415  (Windows only)
        from ctypes import wintypes  # noqa: PLC0415

        class MemoryStatusEx(ctypes.Structure):
            _fields_ = [("dwLength", wintypes.DWORD), ("dwMemoryLoad", wintypes.DWORD),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

        status = MemoryStatusEx()
        status.dwLength = ctypes.sizeof(MemoryStatusEx)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GlobalMemoryStatusEx.argtypes = (ctypes.POINTER(MemoryStatusEx),)
        kernel32.GlobalMemoryStatusEx.restype = wintypes.BOOL
        if not kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            raise OSError(ctypes.get_last_error(), "GlobalMemoryStatusEx failed")
        return int(status.ullAvailPhys)
    meminfo = Path("/proc/meminfo")
    if meminfo.is_file():
        match = re.search(r"^MemAvailable:\s+(\d+)\s+kB", meminfo.read_text(encoding="utf-8"), re.MULTILINE)
        if match:
            return int(match.group(1)) * 1024
    return int(os.sysconf("SC_PAGE_SIZE")) * int(os.sysconf("SC_PHYS_PAGES"))


@dataclasses.dataclass(frozen=True)
class FuzzReport:
    """One FUZZ_REPORT_PATTERN line."""

    case: str
    begin: int
    end: int
    total: int
    accepted: int
    rejected: int


def parse_fuzz_reports(lines: list[str]) -> list[FuzzReport]:
    """The ReportFuzzRun lines among `lines`, in order."""
    reports: list[FuzzReport] = []
    for line in lines:
        match = FUZZ_REPORT_PATTERN.search(line)
        if match:
            reports.append(FuzzReport(match.group("case"), int(match.group("begin")), int(match.group("end")),
                                      int(match.group("total")), int(match.group("accepted")),
                                      int(match.group("rejected"))))
    return reports


def evaluate_fuzz_logs(targets: list[FuzzTarget], lines: list[str]) -> list[str]:
    """The failures of a fuzz run whose processes wrote `lines` to their standard error: "fuzz: <reader>: did not run"
    for a target without a report, "fuzz: <reader>: ran <n> of <iterations> iterations" when its shards' ranges do not
    cover [0, iterations) exactly once (a shard reported twice counts twice), in the order of `targets`; then
    "fuzz: <case>: not a target" for each reported case no target lists, sorted."""
    by_case: dict[str, list[FuzzReport]] = {}
    for report in parse_fuzz_reports(lines):
        by_case.setdefault(report.case, []).append(report)
    failures: list[str] = []
    for target in targets:
        reports = by_case.get(target.case)
        if not reports:
            failures.append(f"fuzz: {target.reader}: did not run")
            continue
        ran = sum(report.end - report.begin for report in reports)
        position = 0
        covered = True
        for report in sorted(reports, key=lambda item: (item.begin, item.end)):
            if report.total != target.iterations or report.begin != position or report.end < report.begin:
                covered = False
                break
            position = report.end
        if not covered or position != target.iterations:
            failures.append(f"fuzz: {target.reader}: ran {ran} of {target.iterations} iterations")
    listed = {target.case for target in targets}
    failures += [f"fuzz: {case}: not a target" for case in sorted(set(by_case) - listed)]
    return failures


# --------------------------------------------------------------------------------------------------------------------
# Soak
# --------------------------------------------------------------------------------------------------------------------


def soak_game_view_errors(stats: Mapping[str, Any], tick: int) -> list[str]:
    """M19 (Docs/Decisions/0030-m19-contract.md decision 11, R-SOAK): the failure of one editor soak sample's stats.get
    result `stats`, taken after the play.step that reached `tick`: none when its "views" list holds the view named
    "game" with available true and a positive width and height, else one line: "the game view did not render at tick
    <tick> (stats.get's game view: <the entry as JSON, or 'missing'>): the editor soak starts its editor with
    --headless-game-view, which renders it". Hardening.py's soak_editor_scene adds the first such line to the
    subject's errors, so a subject whose game view never rendered, or stopped rendering, fails."""
    raise NotImplementedError("contract stub: hardening (ADR 0030 decision 11, R-SOAK, stream E)")


def soak_projects(repository_root: Path) -> list[Path]:
    """The projects the soak covers: every directory of <repository_root>/Projects/ that holds a .eproj file
    (FeatureTest from M15, each demo from its milestone, §15.1 T6), sorted by name."""
    root = repository_root / SOAK_PROJECTS_DIRECTORY
    if not root.is_dir():
        return []
    return sorted((directory for directory in root.iterdir()
                   if directory.is_dir() and any(directory.glob("*.eproj"))), key=lambda directory: directory.name)


def soak_scenes(project: Path) -> list[str]:
    """The project-relative paths of every .scene below <project>/Assets/Scenes/, the golden scenes included, sorted."""
    scenes = project / SOAK_SCENES_DIRECTORY
    if not scenes.is_dir():
        return []
    return sorted(path.relative_to(project).as_posix() for path in scenes.rglob("*.scene") if path.is_file())


def _in_window(sample: SoakSample, window: tuple[float, float]) -> bool:
    return window[0] <= sample.position <= window[1]


def _describe_window(window: tuple[float, float]) -> str:
    return f"{window[0]:.2f}-{window[1]:.2f}"


def soak_heap_tolerance(reference_floor: int) -> int:
    """The script heap rule's tolerance over a reference floor of `reference_floor` bytes: the larger of
    SOAK_HEAP_TOLERANCE_BYTES and SOAK_HEAP_TOLERANCE_FRACTION of the floor."""
    return max(SOAK_HEAP_TOLERANCE_BYTES, int(reference_floor * SOAK_HEAP_TOLERANCE_FRACTION))


def evaluate_script_heap(heap: list[tuple[float, int]]) -> str | None:
    """The script heap rule over `heap`, the (position, script heap bytes) samples of a soak in order, or None when it
    holds; the failure line otherwise. A trough is a sample whose heap is lower than the previous sample's, the only
    samples that show the live data after a collection:
      - with a trough in SOAK_HEAP_REFERENCE_WINDOW and one in SOAK_HEAP_FINAL_WINDOW, the final window's lowest trough
        must not exceed the reference window's lowest trough by more than soak_heap_tolerance() ("floor rose");
      - a window without a collection shows no floor, so the rule cannot pass on floors and is inconclusive, a
        failure, unless its samples bound the growth: without a trough in the final window, its highest sample (all
        the heap holds, live or not) must stay within the tolerance of the reference floor; without a trough in
        either window, the heap only grew by allocation from the reference window's first sample on, so that growth
        bounds what the live data gained and must stay within the tolerance (the heap of a scene whose scripts
        allocate nothing never changes). A trough only in the final window leaves no reference floor: inconclusive.
    Positions are fractions of the soak, so the rule reads a soak of any length; both inconclusive lines name
    SOAK_LONGER_COMMAND, since a collection cycle longer than about a quarter of the soak can leave the last quarter
    without a collection."""
    def in_reference(position: float) -> bool:
        return SOAK_HEAP_REFERENCE_WINDOW[0] <= position < SOAK_HEAP_REFERENCE_WINDOW[1]

    def in_final(position: float) -> bool:
        return SOAK_HEAP_FINAL_WINDOW[0] <= position <= SOAK_HEAP_FINAL_WINDOW[1]

    reference = [value for position, value in heap if in_reference(position)]
    final = [value for position, value in heap if in_final(position)]
    if not reference or not final:
        return "no script heap sample in the reference window or the last quarter of the soak"
    troughs = [(position, value) for (_, previous), (position, value) in zip(heap, heap[1:]) if value < previous]
    reference_troughs = [value for position, value in troughs if in_reference(position)]
    final_troughs = [value for position, value in troughs if in_final(position)]
    reference_name = f"the reference window ({_describe_window(SOAK_HEAP_REFERENCE_WINDOW)})"
    final_name = f"the last quarter ({_describe_window(SOAK_HEAP_FINAL_WINDOW)})"
    if reference_troughs:
        reference_floor = min(reference_troughs)
    elif final_troughs:
        return (f"script heap inconclusive: no collection in {reference_name}, so the soak shows no reference floor; "
                f"a collection cycle longer than half the soak does this, so confirm with a longer soak "
                f"({SOAK_LONGER_COMMAND})")
    else:
        reference_floor = reference[0]
    tolerance = soak_heap_tolerance(reference_floor)
    if final_troughs:
        final_floor = min(final_troughs)
        if final_floor - reference_floor > tolerance:
            return (f"script heap's floor rose: the lowest trough of {final_name}, {final_floor} bytes, exceeds the "
                    f"lowest trough of {reference_name}, {reference_floor} bytes, by {final_floor - reference_floor} "
                    f"bytes (tolerance {tolerance} bytes)")
        return None
    highest = max(final)
    if highest - reference_floor > tolerance:
        since = (f"its lowest trough in {reference_name}" if reference_troughs
                 else f"its first sample in {reference_name}, with no collection since")
        return (f"script heap inconclusive: no collection in {final_name}, and its heap reached {highest} bytes, "
                f"{highest - reference_floor} bytes above {since} ({reference_floor} bytes; tolerance {tolerance} "
                f"bytes); a collection cycle longer than a quarter of the soak can leave it without one, so confirm "
                f"with a longer soak ({SOAK_LONGER_COMMAND})")
    return None


def evaluate_soak(subject: str, samples: list[SoakSample]) -> SoakResult:
    """The soak rules of decision 15 over `samples`:
      - private memory: the baseline is the median private_bytes of the samples in SOAK_BASELINE_WINDOW, the final value
        the maximum in SOAK_FINAL_WINDOW, and growth must stay below SOAK_GROWTH_LIMIT;
      - script heap (a garbage-collected sawtooth): floors compared with floors, evaluate_script_heap (the lowest
        post-collection trough of the last quarter against that of 0.25 to 0.75, within soak_heap_tolerance(); a
        window without a collection is inconclusive, a failure, unless its samples bound the growth within it);
      - entities, bodies, voices and the device's memory allocation count (small integers): the final window's maximum
        must not exceed the baseline window's maximum.
    A rule whose field is None in every sample is not applied (the Runtime soak). A window without a sample, or a
    baseline private_bytes of 0, is a failure naming it; no rule divides by zero."""
    baseline = [sample for sample in samples if _in_window(sample, SOAK_BASELINE_WINDOW)]
    final = [sample for sample in samples if _in_window(sample, SOAK_FINAL_WINDOW)]
    failures: list[str] = []
    growth = 0.0
    windows = (("baseline", SOAK_BASELINE_WINDOW, baseline), ("final", SOAK_FINAL_WINDOW, final))
    for name, window, chosen in windows:
        if not chosen:
            failures.append(f"no sample in the {name} window ({_describe_window(window)} of the soak; "
                            f"{len(samples)} sample(s) in all)")
    if baseline and final:
        baseline_private = statistics.median(sample.private_bytes for sample in baseline)
        final_private = max(sample.private_bytes for sample in final)
        if baseline_private <= 0:
            failures.append(f"the baseline window's private memory is {baseline_private} bytes, so its growth cannot "
                            f"be computed")
        else:
            growth = (final_private - baseline_private) / baseline_private
            if growth >= SOAK_GROWTH_LIMIT:
                failures.append(f"private memory grew {growth * 100:.2f} % (baseline median {int(baseline_private)} "
                                f"bytes in {_describe_window(SOAK_BASELINE_WINDOW)}, final maximum {final_private} "
                                f"bytes in {_describe_window(SOAK_FINAL_WINDOW)}; the limit is < "
                                f"{SOAK_GROWTH_LIMIT * 100:.0f} %)")

        def values(window: list[SoakSample], attribute: str) -> list[int]:
            return [value for value in (getattr(sample, attribute) for sample in window) if value is not None]

        def applies(attribute: str) -> bool:
            return any(getattr(sample, attribute) is not None for sample in samples)

        if applies("script_heap_bytes"):
            heap = [(sample.position, sample.script_heap_bytes) for sample in samples
                    if sample.script_heap_bytes is not None]
            heap_failure = evaluate_script_heap(heap)
            if heap_failure is not None:
                failures.append(heap_failure)
        for label, attribute in SOAK_COUNTER_FIELDS:
            if not applies(attribute):
                continue
            before, after = values(baseline, attribute), values(final, attribute)
            if not before or not after:
                failures.append(f"no {label} sample in the baseline or the final window")
            elif max(after) > max(before):
                failures.append(f"{label} grew: the final window's maximum {max(after)} exceeds the baseline window's "
                                f"maximum {max(before)}")
    return SoakResult(subject, growth, not failures, tuple(failures))


def soak_report(results: list[SoakResult], samples: dict[str, list[SoakSample]],
                frames: int = SOAK_FRAMES) -> dict[str, object]:
    """bin/TestResults/Soak.json's document ({"Format": "SoakReport", "Version": 1, "Frames": <frames>, "Subjects":
    [{"Subject", "Samples", "Growth", "Passed", "Failures"}]}), one subject per result in order, with its samples;
    `frames` is the soak's length (Hardening.py --soak-frames)."""
    subjects = []
    for result in results:
        subjects.append({
            "Subject": result.subject,
            "Samples": [dataclasses.asdict(sample) for sample in samples.get(result.subject, [])],
            "Growth": result.growth,
            "Passed": result.passed,
            "Failures": list(result.failures),
        })
    return {"Format": "SoakReport", "Version": 1, "Frames": frames, "Subjects": subjects}
