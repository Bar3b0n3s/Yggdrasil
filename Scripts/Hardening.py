#!/usr/bin/env python3
"""The hardening stage (Docs/Architecture.md §15.1 T6, §15.8; Docs/Decisions/0024-m15-contract.md decisions 12 to 15
and 19): the perf capture, the soak, the AddressSanitizer build and suites, and the long fuzz, which CI.py runs at
milestone gates (CI.py --milestone).

Steps, in this order, failing fast (a step after a failed one is reported as not run):
  perf           perf capture (Release): Scripts/Perf.py --config Release (stream D), warning-only: an over-budget
                 metric or a fallback without a GPU is a warning, only a capture that cannot run fails
  soak           soak Release: every scene of every project below Projects/ (Lib/hardening.py soak_projects and
                 soak_scenes: FeatureTest and the demos built there, not those below Tests/Projects/, the golden scenes
                 included) in one plain Release editor each (--headless --headless-game-view --renderer vulkan
                 --audio-device none --automation, on a temporary copy of the project with Library/, so the editor
                 renders the scene's game view every frame besides its Scene view): scene.open, script.errors {since:
                 "end"} (the error stream's cursor, taken before play.start runs OnStart), play.start {lockstep: true}
                 and N / 250 x play.step {ticks: 250, render: "Every"}, N rendered frames (N = --soak-frames, 10,000 by
                 default), sampling the editor's process memory and stats.get after each step (a sample whose game
                 view did not render fails the subject, hardening.soak_game_view_errors), then script.errors from
                 that cursor, each page read from the file the editor offloads it to when it is over the 48 KB
                 bounded-output limit (Architecture §13.4): any new script error of the play session, OnStart's
                 included, fails the subject with one line counting them and naming the earliest listed, at its latest
                 occurrence (script.errors lists each distinct error once; Docs/Decisions/0027-m17-contract.md
                 decision 4, B4; play.step keeps ticking through a non-fatal script error, so the soak runs all its
                 ticks either way);
                 then the exported Release FeatureTest that the export stage leaves in bin/Exports/Release/FeatureTest
                 (--headless --frames N, its memory sampled from outside every 0.5 s). hardening.evaluate_soak applies
                 the rules of decision 15 to each subject, its windows being fractions of the soak whatever N is;
                 bin/TestResults/Soak.json and Soak-Release.xml record them. A missing or stale package is exit code 3
                 naming Test.py --suite export --config Release --junit. The step has one deadline (soak_timeout(N):
                 TIMEOUTS["soak"] at 10,000 frames, in proportion for a longer soak) for both halves: every call waits
                 at most what is left of it, and at the deadline the subject running fails naming it, the rest do not
                 run and the step times out (exit code 5); Soak.json and the JUnit report are written from a finally
                 block, so the subjects that finished survive any failure
  asan-generate  asan generate: Generate.py --sanitize address --to bin-int/Asan --no-compile-commands
  asan-build     asan build: Build.py --workspace-dir bin-int/Asan --config Release, then MSVC's AddressSanitizer
                 runtime copied next to Tests, Editor and Runtime there (hardening.stage_asan_runtime; exit code 3
                 naming Visual Studio's "C++ AddressSanitizer" component when it is missing)
  asan-unit      unit Release (ASan): Test.py --suite unit --config Release --build-root bin-int/Asan/bin --sanitizer
                 address --exclude-test-suites Fuzz --junit (bin/TestResults/Unit-Release-Asan.xml); the Fuzz suite is
                 left to the fuzz step, which runs its cases over a superset of the same iterations
  asan-feature   feature Release (ASan): the same for the feature suite (Feature-Release-Asan.{xml,json}), with
                 --require-gpu unless --gpu-optional
  debug-heap     gpu Debug (debug heap): the Debug gpu suite under both API caps on the Windows debug heap, with every
                 device's GPU object lifetimes logged (Docs/Decisions/0029-m18-contract.md, R-GPUCRASH: the
                 intermittent crash on a driver thread, Docs/Backlog.md): Test.py --suite gpu --config Debug --junit
                 --debug-heap [--require-gpu unless --gpu-optional], each Tests run on the debug heap
                 (Scripts/Lib/debugheap.py: started under a debugger that detaches at the loader's initial breakpoint,
                 so every heap of the process, the graphics driver's included, checks block tails, fills freed blocks
                 and validates parameters, and its crash handler still reports a crash), with ENGINE_GPU_LIFETIME_LOG
                 naming bin/TestResults/DebugHeap/Run<k>/GpuLifetime/ (one GpuLifetime-<pid>.log per process,
                 Engine/Source/Engine/Graphics/GpuResourceTracker.h) and the run's reports copied beside it
                 (Gpu-Debug-Vulkan1{4,3}-DebugHeap.xml), with the crash reports its Tests runs wrote below their own
                 user data (Test.py's bin/TestResults/<report stem>/UserData) in Crashes/, which the run's detail names
                 instead of the originals that the next run deletes. --debug-heap-runs <n> runs
                 it n times (the crash runs of M18's W18.2: 20, overnight), every run whatever the earlier ones did, and
                 fails naming each failed run; a missing Debug build is exit code 3. Windows only: elsewhere the step
                 is skipped, naming why
  fuzz           fuzz Release (ASan): the sanitized Tests binary with --test-suite=Fuzz --fuzz-iterations=1000000
                 --fuzz-shard=<k>/<n> --test-timeout-scale=<s> in n = hardening.fuzz_process_count(logical CPUs,
                 available memory, FUZZ_PEAK_PROCESS_BYTES) processes, each with its own --user-data-dir and the
                 AddressSanitizer environment. The step is sized from per-target costs (ADR 0028 decision 16,
                 T-FUZZMODEL): each target's seconds per iteration as the last full, passing fuzz step on this machine
                 measured them, in the report it kept in <git common directory>/fuzz-costs/Fuzz-Release-Asan.xml
                 (hardening.machine_fuzz_costs_path, shared by every checkout of the repository and written only by a
                 passing run of 1,000,000 iterations, so a development or failed run never replaces it), or before the
                 first such run as M17's gate measured them (Tests/Data/Hardening/FuzzM17.xml); a target not measured
                 there at the measured average, and without any measurement the reference run's 52 min at 19 shards for
                 35 targets scaled by the target count (hardening.read_fuzz_costs), summed into the step's work W. Below
                 hardening.fuzz_minimum_processes (the fewest shards whose expected time W / n fits in 80 % of the
                 step's budget, 5,400 s per 35 targets: 14 with M17's measured costs and with the reference) the step
                 refuses to start, exit code 3 naming the available memory and logical CPUs it needs: with those costs
                 the stage needs at least 14 logical CPUs and 14 GiB of available memory (14 shards of
                 FUZZ_PEAK_PROCESS_BYTES, 1 GiB) when the fuzz step starts. The step's timeout is W / min(n, 19, logical
                 CPUs) over 80 % (hardening.fuzz_timeout_seconds: shards beyond the reference run's 19 or the logical
                 CPUs run no faster), at least 600 s and at most the budget, so it follows the measured costs and the
                 shard count that runs. Each shard runs 1/n of every target, so its cases' limit grows with that share
                 below the reference run's 19 processes: s = hardening.fuzz_timeout_scale(n) = max(20, ceil(20 x 19 /
                 n)), never below the reference's 20, since shards beyond the physical cores share them, and raised when
                 the dearest target's share needs more. Every target keeps its 1,000,000 iterations (ADR 0024 decision
                 14); only the shards and the timeouts follow the costs. The ReportFuzzRun lines are read from each
                 process's standard error and hardening.evaluate_fuzz_logs checks that every target of
                 Tests/Data/Fuzz/Targets.json covered [0, 1,000,000) exactly once; any process failure fails the step.
                 bin/TestResults/Fuzz-Release-Asan.xml has one test case per target, whose time is its seconds summed
                 over the shards, and per process; the step reports each target's cost per iteration and the peak memory
                 of a process.

--gpu-optional: without a Vulkan device the perf capture falls back, the soak passes without running, and the ASan
feature suite passes without running, each as a warning (otherwise they fail). Development aids, which make a step a
warning because it no longer covers the whole stage: --soak-only <pattern> (fnmatch over the subjects,
"<project>:<scene>" and "Runtime:<project>"), --fuzz-iterations <n> (each target runs n iterations instead of 1,000,000)
and --fuzz-processes <n> (instead of the computed count; a count below the minimum the fuzz step's budget needs for its
iterations, 14 at 1,000,000 with M17's costs, is still refused with exit code 3); a run of fewer iterations, like a
failed one, never replaces the kept measurement, so the run after it is sized as before. --soak-frames <n> soaks every
subject for n ticks or frames instead of 10,000 (at least 10,000, a multiple of 250): a scene whose collection cycle is
longer than about a quarter of the soak can leave the script heap rule inconclusive depending only on the cycle's phase,
and a longer soak of that scene (--soak-only <subject> --soak-frames 40000) confirms it; it is a full soak, not a
development aid.

A run whose steps include perf, soak, asan-feature or debug-heap holds the machine-wide GPU lock (Scripts/Lib/locks.py;
Docs/Architecture.md §2.3, ADR 0028 decision 16), shared by every checkout of the repository, from its start to its end;
its child scripts (Perf.py, Build.py, Test.py) run under that hold, so they never wait for a gate that began after it.
It waits while another GPU run or a gate (CI.py) holds the lock, unless it runs inside that run (CI.py's hardening
stage), reporting the holder (pid, worktree, command), at most --lock-timeout seconds (exit code 5); --no-wait fails at
once instead (exit code 3). A run of the other steps takes no lock of its own; its Build.py takes the build lock.

It works from any directory (Docs/Architecture.md §2.3): once it has parsed its arguments, main() makes the repository
root of its own checkout the working directory.

Exit codes: 0 every step passed (or warned), 1 a step failed, 2 usage error, 3 a build output, the AddressSanitizer
runtime or a current export package is missing, the machine has too little memory or too few CPUs for the fuzz to
finish in time (it needs at least as many logical CPUs and GiB of available memory as its minimum shard count, 14 at
1,000,000 iterations with M17's measured costs), or --no-wait found the GPU lock held, 5 a step timed out, or the wait
for the GPU lock ran out.
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"Hardening.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import dataclasses
import fnmatch
import json
import os
import shutil
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ElementTree
from collections.abc import Callable
from pathlib import Path
from typing import Any

from Lib import debugheap, exports, hardening, locks, paths
from Lib import process as process_lib
from Lib.process import (
    ToolNotFoundError,
    child_environment,
    describe_exit_code,
    format_command,
    run_captured,
    run_streamed,
)
from Lib.report import (
    EXIT_FAILED,
    EXIT_INIT_FAILED,
    EXIT_SUCCESS,
    EXIT_USAGE,
    Console,
    Status,
    Step,
    configure_stdio,
    emit_json,
    overall_exit_code,
    write_junit,
)

STEPS = ("perf", "soak", "asan-generate", "asan-build", "asan-unit", "asan-feature", "debug-heap", "fuzz")
# The steps that run the GPU: a run that includes one holds the machine-wide GPU lock from its start to its end.
GPU_STEPS = frozenset({"perf", "soak", "asan-feature", "debug-heap"})
STEP_NAMES = {
    "perf": "perf capture (Release)",
    "soak": "soak Release",
    "asan-generate": "asan generate",
    "asan-build": "asan build",
    "asan-unit": "unit Release (ASan)",
    "asan-feature": "feature Release (ASan)",
    "debug-heap": "gpu Debug (debug heap)",
    "fuzz": "fuzz Release (ASan)",
}
# Seconds per step (ADR 0024 decision 19's TIMEOUTS as amended at integration: perf 1200 s, the ASan build 7200 s, every
# other step 3600 s). The fuzz step's timeout follows its measured per-target costs and the shard count that runs
# (hardening.fuzz_timeout_seconds; ADR 0028 decision 16, T-FUZZMODEL), within its budget of 5,400 s per 35 targets
# (hardening.fuzz_budget_seconds). Measured on the 24-thread workstation: perf 3 min, soak 16 min, ASan build 20 min
# from scratch and 2 min incremental, ASan unit 21 min, ASan feature 1 min, fuzz 52 min (M17: 55 min at 18 shards).
TIMEOUTS = {"perf": 1200.0, "soak": 3600.0, "asan-generate": 3600.0, "asan-build": 7200.0, "asan-unit": 3600.0,
            "asan-feature": 3600.0, "debug-heap": 7200.0}
# What the sanitized suites' and the debug-heap step's Test.py runs get beyond their own --timeout, to write their
# reports after it.
SUITE_GRACE_SECONDS = 60.0
SANITIZED_SUITES = ("unit", "feature")
# The steps whose Test.py runs get SUITE_GRACE_SECONDS: the sanitized suites and the debug-heap step (one run).
GRACED_STEPS = ("asan-unit", "asan-feature", "debug-heap")
# The debug-heap step (Docs/Decisions/0029-m18-contract.md, R-GPUCRASH): the configuration of its gpu suite, whose Tests
# runs take the API caps below in turn (each bounded by its share of TIMEOUTS["debug-heap"], 3,600 s, under the debug
# heap's slower allocations), the directory where Test.py writes their JUnit reports, the directory of the step's copies
# of them and its lifetime logs, and the variable that turns the logs on.
DEBUG_HEAP_CONFIG = "Debug"
DEBUG_HEAP_CAPS = ("1.4", "1.3")
DEBUG_HEAP_SUITE_REPORTS = paths.TEST_RESULTS_ROOT
DEBUG_HEAP_REPORTS = paths.TEST_RESULTS_ROOT / "DebugHeap"
GPU_LIFETIME_LOG_VARIABLE = "ENGINE_GPU_LIFETIME_LOG"
CONFIG = "Release"
PYTHON = sys.executable
SCRIPTS = paths.REPOSITORY_ROOT / "Scripts"
# The sanitized unit suite's per-test-case limits are multiplied by this (its slowest cases take several times their
# Release time under AddressSanitizer); the fuzz shards' by hardening.fuzz_timeout_scale(<shard count>).
UNIT_TIMEOUT_SCALE = 5
FUZZ_MEMORY_SAMPLE_SECONDS = 2.0
OUTPUT_TAIL_LINES = 40
GPU_PROBE_ARGUMENTS = ("--gpu-validation=sync", "--expect-no-gpu-errors")
GPU_PROBE_TIMEOUT_SECONDS = 120.0
NO_DEVICE_TEXT = "GPU test without a device (passes without running): "
SOAK_STEP_TICKS = hardening.SOAK_SAMPLE_INTERVAL
SOAK_CALL_TIMEOUT_SECONDS = 600.0
SOAK_SHUTDOWN_TIMEOUT_SECONDS = 60.0
# script.errors' cursor for "the stream's current end" and its largest page (ScriptMethods.cpp: limit 1 to 1000), with
# which the soak reads the errors of a scene's play session (Docs/Decisions/0027-m17-contract.md decision 4, B4). A page
# over the 48 KB bounded-output limit (about 200 short errors, fewer with tracebacks) arrives offloaded to a file
# (Architecture §13.4), which new_script_errors reads.
SCRIPT_ERRORS_END = "end"
SCRIPT_ERRORS_PAGE_LIMIT = 1000
SOAK_REPORT = paths.TEST_RESULTS_ROOT / "Soak.json"
SOAK_JUNIT = paths.TEST_RESULTS_ROOT / "Soak-Release.xml"
FUZZ_JUNIT = paths.TEST_RESULTS_ROOT / "Fuzz-Release-Asan.xml"
EXPORT_SUITE_COMMAND = "python Scripts/Test.py --suite export --config Release --junit"


class StepFailure(Exception):
    """A step cannot run; the message is its detail and `exit_code` its exit code."""

    def __init__(self, message: str, exit_code: int = EXIT_FAILED) -> None:
        super().__init__(message)
        self.exit_code = exit_code


def tail(text: str, lines: int = OUTPUT_TAIL_LINES) -> str:
    return "\n".join(text.rstrip().splitlines()[-lines:])


def editor_executable() -> Path:
    return paths.output_directory(CONFIG) / "Editor" / paths.executable_name("Editor")


def asan_output_directory() -> Path:
    return hardening.ASAN_WORKSPACE / "bin" / paths.output_directory_name(CONFIG)


def import_engine_client() -> Any:
    """Tools/Automation/engine_client.py, the standard-library automation client (Architecture §13.2)."""
    tools = str(paths.REPOSITORY_ROOT / "Tools" / "Automation")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import engine_client  # noqa: PLC0415  (Tools/Automation is on sys.path only from here)
    return engine_client


def run_script(name: str, script: str, arguments: list[str], timeout: float, console: Console,
               environment: dict[str, str] | None = None) -> tuple[process_lib.ProcessResult, Step | None]:
    """Runs Scripts/<script> streamed; returns its result and a failed step when it could not be started."""
    command = [PYTHON, str(SCRIPTS / script), *arguments]
    console.heading(f"{name}: {format_command(['python', f'Scripts/{script}', *arguments])}")
    try:
        result = run_streamed(command, cwd=paths.REPOSITORY_ROOT, env=environment or child_environment(),
                              timeout=timeout, echo=console.stream)
    except ToolNotFoundError as error:
        return process_lib.ProcessResult(command, None, False, 0.0, ""), Step(name, Status.FAILED, str(error),
                                                                             exit_code=EXIT_INIT_FAILED)
    return result, None


def script_step(name: str, result: process_lib.ProcessResult, passed: str) -> Step:
    """The step of a child script: passed, timed out, or failed with its exit code (3 stays 3: a missing output)."""
    if result.timed_out:
        return Step(name, Status.TIMEOUT, f"{result.describe_exit()}", result.duration,
                    data={"outputTail": result.tail()})
    if result.exit_code != 0:
        code = result.exit_code if result.exit_code in (EXIT_INIT_FAILED, EXIT_USAGE) else EXIT_FAILED
        lines = [line for line in result.output.splitlines() if line.startswith("[FAILED]") or "FAILED" in line[:12]]
        detail = lines[-1] if lines else f"exit code {describe_exit_code(result.exit_code)}"
        return Step(name, Status.FAILED, detail, result.duration, exit_code=code, data={"outputTail": result.tail()})
    return Step(name, Status.PASSED, passed, result.duration)


@dataclasses.dataclass
class JsonRun:
    """A child script run with --json: its standard output is the JSON document, its human output went to stderr."""

    exit_code: int | None
    timed_out: bool
    duration: float
    document: dict[str, Any]

    def describe_exit(self) -> str:
        return "timed out" if self.timed_out else f"exit code {describe_exit_code(self.exit_code)}"


def run_json_script(name: str, script: Path, arguments: list[str], timeout: float, console: Console,
                    environment: dict[str, str] | None = None) -> JsonRun:
    """Runs a script with --json among `arguments`: its progress (on its stderr) is shown as it runs, its stdout parsed.
    `environment` replaces child_environment(). Raises ToolNotFoundError when it cannot be started."""
    command = [PYTHON, str(script), *arguments]
    console.heading(f"{name}: {format_command(['python', paths.display_path(script), *arguments])}")
    started = time.monotonic()
    try:
        child = subprocess.Popen(command, cwd=paths.REPOSITORY_ROOT, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=None, env=environment if environment is not None else child_environment())
    except OSError as error:
        raise ToolNotFoundError(f"could not start {script.name}: {error}") from None
    timed_out = False
    try:
        output, _ = child.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        process_lib.terminate_tree(child)
        output, _ = child.communicate()
        timed_out = True
    except BaseException:
        process_lib.terminate_tree(child)
        raise
    try:
        document = json.loads(output.decode("utf-8", errors="replace") or "{}")
    except json.JSONDecodeError:
        document = {}
    return JsonRun(None if timed_out else child.returncode, timed_out, time.monotonic() - started,
                   document if isinstance(document, dict) else {})


# --------------------------------------------------------------------------------------------------------------------
# perf (stream D's Scripts/Perf.py)
# --------------------------------------------------------------------------------------------------------------------


def step_perf(arguments: argparse.Namespace, console: Console) -> Step:
    name = STEP_NAMES["perf"]
    script = SCRIPTS / "Perf.py"
    if not script.is_file():
        return Step(name, Status.NOT_AVAILABLE, "Scripts/Perf.py does not exist in this tree (ADR 0024 decision 16)")
    options = ["--config", CONFIG, "--json", *(["--gpu-optional"] if arguments.gpu_optional else [])]
    try:
        run = run_json_script(name, script, options, TIMEOUTS["perf"], console)
    except ToolNotFoundError as error:
        return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)
    data = {"perf": run.document}
    if run.timed_out:
        return Step(name, Status.TIMEOUT, f"Perf.py {run.describe_exit()}", run.duration, data=data)
    if run.exit_code != 0:
        code = run.exit_code if run.exit_code == EXIT_INIT_FAILED else EXIT_FAILED
        return Step(name, Status.FAILED, f"Perf.py {run.describe_exit()}: the capture could not run", run.duration,
                    exit_code=code, data=data)
    reported = [step for step in run.document.get("steps", []) if isinstance(step, dict)]
    warnings = [step for step in reported if step.get("status") == "warning"]
    if warnings:
        return Step(name, Status.WARNING, "; ".join(str(step.get("detail", step.get("name", ""))) for step in warnings),
                    run.duration, data=data)
    return Step(name, Status.PASSED, "Perf.json written; every metric within its budget", run.duration, data=data)


# --------------------------------------------------------------------------------------------------------------------
# soak
# --------------------------------------------------------------------------------------------------------------------


def probe_rendering_editor(editor: Path) -> str | None:
    """None when a headless Release editor creates its device here; otherwise why not (as Test.py's feature suite)."""
    with tempfile.TemporaryDirectory(prefix="Hardening-GpuProbe-") as directory:
        command = [str(editor), "--headless", "--frames", "1", *GPU_PROBE_ARGUMENTS, f"--user-data-dir={directory}"]
        try:
            result = run_captured(command, cwd=paths.REPOSITORY_ROOT, env=child_environment(),
                                  timeout=GPU_PROBE_TIMEOUT_SECONDS)
        except ToolNotFoundError as error:
            return f"the editor could not run its device probe: {error}"
    if result.succeeded:
        return None
    lines = [line.strip() for line in result.output.splitlines() if line.strip()]
    errors = [line for line in lines if "[error]" in line or "[critical]" in line]
    return f"a headless editor {result.describe_exit()} creating its device: {(errors or lines or ['no output'])[-1]}"


def stats_sample(position: float, pid: int, stats: dict[str, Any]) -> hardening.SoakSample:
    memory = process_lib.query_process_memory(pid)

    def integer(field: str) -> int | None:
        value = stats.get(field)
        return int(value) if isinstance(value, (int, float)) and not isinstance(value, bool) else None

    return hardening.SoakSample(position, memory.private_bytes, memory.resident_bytes, integer("entities"),
                                integer("bodies"), integer("voices"),
                                integer("scriptHeapBytes") if stats.get("scriptAvailable", True) else None,
                                integer("memoryAllocationCount"))


class SoakDeadlinePassed(Exception):
    """The soak step's deadline passed before a call could start."""


@dataclasses.dataclass
class SoakDeadline:
    """The soak step's one deadline, `seconds` (soak_timeout) after the step started, shared by the editor scenes and
    the Runtime soak: every call waits at most what is left of it, and no subject starts after it."""

    end: float
    clock: Callable[[], float] = time.monotonic
    seconds: float = TIMEOUTS["soak"]  # the whole step's deadline, which its errors name

    def error(self, where: str = "") -> str:
        return soak_deadline_error(where, self.seconds)

    def remaining(self) -> float:
        return self.end - self.clock()

    def expired(self) -> bool:
        return self.remaining() <= 0.0

    def limit(self, seconds: float) -> float:
        """`seconds`, cut to what is left of the step. Raises SoakDeadlinePassed when nothing is left."""
        remaining = self.remaining()
        if remaining <= 0.0:
            raise SoakDeadlinePassed(self.error())
        return min(seconds, remaining)


def soak_timeout(frames: int) -> float:
    """The soak step's deadline in seconds for a soak of `frames` (--soak-frames): TIMEOUTS["soak"] at the default
    hardening.SOAK_FRAMES, grown in proportion for a longer soak."""
    return TIMEOUTS["soak"] * max(frames, hardening.SOAK_FRAMES) / hardening.SOAK_FRAMES


def soak_deadline_error(where: str = "", seconds: float = TIMEOUTS["soak"]) -> str:
    return f"the soak step's {seconds:.0f} s deadline passed" + (f" {where}" if where else "")


SoakRun = tuple[list[hardening.SoakSample], list[str]]  # a subject's samples and the errors that fail it


def script_error_cursor(client: Any, engine_client: Any, deadline: SoakDeadline) -> tuple[str | None, list[str]]:
    """script.errors {since: "end"}: (its nextCursor, the current end of the editor's error stream, []), or (None, [the
    error line naming the failed call]) when the call fails or returns no cursor. The soak takes it after scene.open and
    before play.start, whose activation runs OnStart, so OnStart's errors come after it
    (Docs/Decisions/0027-m17-contract.md decision 4, B4)."""
    call = f'script.errors {{since: "{SCRIPT_ERRORS_END}"}}'
    try:
        result = client.call("script.errors", {"since": SCRIPT_ERRORS_END},
                             timeout=deadline.limit(SOAK_CALL_TIMEOUT_SECONDS))
    except engine_client.EngineError as error:
        return None, [f"{call} failed: {error}"]
    cursor = result.get("nextCursor") if isinstance(result, dict) else None
    if not isinstance(cursor, str) or not cursor:
        return None, [f"{call} failed: its result has no nextCursor"]
    return cursor, []


def new_script_errors(client: Any, engine_client: Any, cursor: str, deadline: SoakDeadline) -> list[str]:
    """The script errors after `cursor` (script_error_cursor's), read with script.errors {since, limit:
    SCRIPT_ERRORS_PAGE_LIMIT} from the cursor, following nextCursor until a page is empty (ADR 0027 decision 4, B4).
    Every page is read through engine_client.load_offloaded: the soak's connection has offloading on, so a page whose
    JSON is over the 48 KB bounded-output limit comes back as {path, truncated: true, summary} and the page itself is in
    the file it names (Architecture §13.4).
    The editor never clears its error stream and each soak scene starts a fresh editor, so every entry after the cursor
    is an error of this scene's play session (a repeated error included, which the stream moves after the cursor with
    its latest occurrence). The stream lists each distinct error once, at its latest occurrence (its tick, and its count
    of every occurrence, those before the cursor included), and keeps the 256 errors with the latest occurrences, so the
    first occurrence is unknown: the line names the earliest listed entry. Returns one line when there is any, "<n> new
    script error(s) during the soak; the earliest listed: <script>:<line>: <message> (latest at tick <tick>, <count>
    occurrence(s))", <n> counting the entries returned, followed by "; earlier errors were evicted" when any page
    answered evicted: true; and a line naming a script.errors call that failed (or returned no errors array or
    nextCursor, an offloaded page whose file cannot be read, which the line names, or a page that leaves the cursor
    where it was)."""
    entries: list[Any] = []
    evicted = False
    failure: str | None = None
    since = cursor
    while True:
        call = f'script.errors {{since: "{since}"}}'
        try:
            result = client.call("script.errors", {"since": since, "limit": SCRIPT_ERRORS_PAGE_LIMIT},
                                 timeout=deadline.limit(SOAK_CALL_TIMEOUT_SECONDS))
        except engine_client.EngineError as error:
            failure = f"{call} failed: {error}"
            break
        if isinstance(result, dict):
            try:
                result = engine_client.load_offloaded(result)
            except (OSError, ValueError, engine_client.ProtocolError) as error:
                failure = f"{call} failed: its offloaded result {result.get('path')} cannot be read: {error}"
                break
        page = result.get("errors") if isinstance(result, dict) else None
        next_cursor = result.get("nextCursor") if isinstance(result, dict) else None
        if not isinstance(page, list) or not isinstance(next_cursor, str):
            failure = f"{call} failed: its result has no errors array or no nextCursor"
            break
        evicted = evicted or result.get("evicted") is True
        if not page:
            break
        entries += page
        if next_cursor == since:
            failure = f"{call} failed: it returned {len(page)} error(s) without advancing its nextCursor"
            break
        since = next_cursor
    lines: list[str] = []
    if entries:
        earliest = entries[0] if isinstance(entries[0], dict) else {}
        line = (f"{len(entries)} new script error(s) during the soak; the earliest listed: "
                f"{earliest.get('script', '')}:{earliest.get('line', 0)}: {earliest.get('message', '')} "
                f"(latest at tick {earliest.get('tick', 0)}, {earliest.get('count', 0)} occurrence(s))")
        lines.append(f"{line}; earlier errors were evicted" if evicted else line)
    if failure is not None:
        lines.append(failure)
    return lines


def soak_editor_scene(engine_client: Any, editor: Path, project_file: Path, scene: str, user_data: Path,
                      console: Console, deadline: SoakDeadline, frames: int = hardening.SOAK_FRAMES) -> SoakRun:
    """One scene's editor soak of `frames` ticks (frames / SOAK_STEP_TICKS play.step calls): its samples, each at
    position tick / frames, and its errors, which fail the subject: those that ended it early (none when it ran all
    its ticks) and its new script errors (Docs/Decisions/0027-m17-contract.md decision 4, B4). After scene.open it takes
    script.errors' cursor (script_error_cursor), before play.start, whose activation runs OnStart; after the soak's
    last step (all its ticks, or where an error ended it) it reads the errors after that cursor (new_script_errors), so
    any script error of the play session, OnStart's included, adds one line naming how many there were and the earliest
    listed one. play.step keeps ticking through a non-fatal script error (the editor disables the faulting instance), so
    the soak runs all its ticks either way. The editor starts with hardening.SOAK_EDITOR_GAME_VIEW_OPTION, so it renders
    the scene's game view every frame; the first sample after a step whose stats.get reports no rendered game view adds
    one line (hardening.soak_game_view_errors; Docs/Decisions/0030-m19-contract.md decision 11, R-SOAK), and the soak
    goes on. Each call waits at most SOAK_CALL_TIMEOUT_SECONDS and never past `deadline`; at the deadline the scene ends
    with an error naming it, and the editor is killed."""
    samples: list[hardening.SoakSample] = []
    errors: list[str] = []
    game_view_failed = False  # only the first sample whose game view did not render adds a line
    try:
        editor_process = engine_client.launch_editor(
            editor, ["--headless", hardening.SOAK_EDITOR_GAME_VIEW_OPTION, "--renderer", "vulkan", "--audio-device",
                     "none", "--automation", "--project", str(project_file)], user_data, paths.workspace_name(),
            environment=child_environment())
    except (RuntimeError, TimeoutError, OSError) as error:
        errors.append(f"the editor did not start: {error}")
        console.print(f"  {scene}: {errors[0].splitlines()[0]}")
        return samples, errors
    tick = 0
    try:
        client = editor_process.connect("hardening-soak", call_log=engine_client.CallLog(None))
        with client:
            pid = editor_process.process.pid
            cursor: str | None = None
            try:
                client.call("scene.open", {"path": scene}, timeout=deadline.limit(SOAK_CALL_TIMEOUT_SECONDS))
                cursor, cursor_errors = script_error_cursor(client, engine_client, deadline)
                errors += cursor_errors
                client.call("play.start", {"lockstep": True}, timeout=deadline.limit(SOAK_CALL_TIMEOUT_SECONDS))
                stats = client.call("stats.get", {}, timeout=deadline.limit(SOAK_CALL_TIMEOUT_SECONDS))
                samples.append(stats_sample(0.0, pid, stats))
                while tick < frames:
                    try:
                        client.call("play.step", {"ticks": SOAK_STEP_TICKS, "render": "Every"},
                                    timeout=deadline.limit(SOAK_CALL_TIMEOUT_SECONDS))
                    except engine_client.EngineError as error:
                        errors.append(f"play.step after tick {tick} failed: {error}")
                        break
                    tick += SOAK_STEP_TICKS
                    stats = client.call("stats.get", {}, timeout=deadline.limit(SOAK_CALL_TIMEOUT_SECONDS))
                    samples.append(stats_sample(tick / frames, pid, stats))
                    if not game_view_failed:
                        game_view_errors = hardening.soak_game_view_errors(stats, tick)
                        errors += game_view_errors
                        game_view_failed = bool(game_view_errors)
            except engine_client.EngineError as error:
                errors.append(f"{error}")
            if cursor is not None:
                errors += new_script_errors(client, engine_client, cursor, deadline)
            try:
                client.call("session.shutdown", {"force": True}, timeout=deadline.limit(SOAK_SHUTDOWN_TIMEOUT_SECONDS))
            except (engine_client.EngineError, engine_client.ConnectionClosed, TimeoutError):
                pass  # the process is stopped below either way
        exit_code = editor_process.wait(deadline.limit(SOAK_SHUTDOWN_TIMEOUT_SECONDS))
        if exit_code != 0:
            errors.append(f"the editor exited with code {describe_exit_code(exit_code)}:\n{editor_process.output()}")
    except SoakDeadlinePassed:
        errors.append(deadline.error(f"after tick {tick}"))
    except (engine_client.ConnectionClosed, engine_client.ProtocolError, TimeoutError, OSError) as error:
        if deadline.expired():
            errors.append(deadline.error(f"after tick {tick} ({error})"))
        else:
            errors.append(f"the editor stopped answering: {error}\n{editor_process.output()}")
    finally:
        editor_process.kill()
    if errors:
        console.print(f"  {scene}: {errors[0].splitlines()[0]}")
    return samples, errors


def current_package() -> Path:
    """The exported Release FeatureTest's executable, when the export stage of this tree left it (decision 10's rule
    over its manifest: newer than the Release Runtime, the exporting editor and every FeatureTest file outside
    Library/).
    Raises StepFailure with exit code 3 naming the export suite otherwise."""
    package = exports.package_directory(CONFIG)
    executable = exports.package_executable(package)
    if not executable.is_file():
        raise StepFailure(f"the exported Release FeatureTest {paths.display_path(executable)} is missing: run "
                          f"{EXPORT_SUITE_COMMAND}", EXIT_INIT_FAILED)
    if sys.platform == "darwin":
        manifest = package / f"{exports.FEATURE_TEST_NAME}.app" / "Contents" / "Resources" / "Game.json"
    else:
        manifest = package / "Game.json"
    reason = exports.stale_report_reason(manifest, exports.report_inputs(CONFIG, "exported"))
    if reason is not None:
        raise StepFailure(f"the exported Release FeatureTest is not current ({reason}): run {EXPORT_SUITE_COMMAND}",
                          EXIT_INIT_FAILED)
    return executable


def soak_runtime(executable: Path, user_data: Path, console: Console, deadline: SoakDeadline,
                 frames: int = hardening.SOAK_FRAMES) -> SoakRun:
    """The Runtime soak: `<package> --headless --frames <frames>`, its memory sampled from outside every
    SOAK_RUNTIME_SAMPLE_SECONDS; a sample's position is its elapsed time over the run's. At `deadline` the Runtime is
    killed and the soak ends with an error naming it. A memory query that fails other than for an exited process
    (OSError, for example while the process exits) ends the soak as a failure of this subject, as in the editor half."""
    log_path = user_data / "Runtime-output.txt"
    user_data.mkdir(parents=True, exist_ok=True)
    command = [str(executable), "--headless", "--frames", str(frames), "--audio-device", "none",
               f"--user-data-dir={user_data}"]
    console.print(f"  {format_command([paths.display_path(executable), *command[1:]])}")
    timed: list[tuple[float, process_lib.ProcessMemory]] = []
    errors: list[str] = []
    with log_path.open("wb") as log:
        try:
            child = subprocess.Popen(command, cwd=executable.parent, stdin=subprocess.DEVNULL, stdout=log,
                                     stderr=subprocess.STDOUT, env=child_environment())
        except OSError as error:
            return [], [f"the Runtime did not start: {error}"]
        started = time.monotonic()
        try:
            while True:
                try:
                    timed.append((time.monotonic() - started, process_lib.query_process_memory(child.pid)))
                except ProcessLookupError:
                    pass  # it exited between the poll and the query
                except OSError as error:
                    errors.append(f"cannot read the Runtime's memory: {error}")
                    break
                try:
                    child.wait(timeout=deadline.limit(hardening.SOAK_RUNTIME_SAMPLE_SECONDS))
                    break
                except subprocess.TimeoutExpired:
                    continue
                except SoakDeadlinePassed:
                    errors.append(deadline.error(f"before the Runtime finished {frames} frames"))
                    break
        finally:
            if child.poll() is None:
                child.kill()
            child.wait()
        ended = time.monotonic() - started
    output = log_path.read_text(encoding="utf-8", errors="replace")
    if not errors and child.returncode != 0:
        errors.append(f"the Runtime exited with code {describe_exit_code(child.returncode)}:\n{tail(output)}")
    total = max(ended, 1e-9)
    samples = [hardening.SoakSample(min(elapsed / total, 1.0), memory.private_bytes, memory.resident_bytes)
               for elapsed, memory in timed]
    console.print(f"  Runtime: {len(samples)} sample(s) over {ended:.1f} s")
    return samples, errors


@dataclasses.dataclass
class SoakSubject:
    """One subject of the soak step: its name ("<project>:<scene>" or "Runtime:<project>"), what it runs (for its
    heading) and the function that soaks it within the step's deadline."""

    name: str
    description: str
    run: Callable[[SoakDeadline], SoakRun]


@dataclasses.dataclass
class SoakOutcome:
    """What run_soak_subjects soaked: each soaked subject's result, samples and seconds, the subjects the deadline left
    out, and whether the deadline ended the step."""

    results: list[hardening.SoakResult] = dataclasses.field(default_factory=list)
    samples: dict[str, list[hardening.SoakSample]] = dataclasses.field(default_factory=dict)
    seconds: dict[str, float] = dataclasses.field(default_factory=dict)
    not_run: list[str] = dataclasses.field(default_factory=list)
    timed_out: bool = False


def write_soak_reports(outcome: SoakOutcome, report: Path, junit: Path, frames: int = hardening.SOAK_FRAMES) -> None:
    """Soak.json (hardening.soak_report of a soak of `frames`) and the JUnit report of every soaked subject of
    `outcome`."""
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(hardening.soak_report(outcome.results, outcome.samples, frames), indent=2) + "\n",
                      encoding="utf-8")
    write_junit(junit, "Soak.Release", [
        Step(result.subject, Status.PASSED if result.passed else Status.FAILED,
             "\n".join(result.failures) if result.failures else f"growth {result.growth * 100:.3f} %",
             outcome.seconds.get(result.subject, 0.0))
        for result in outcome.results])


def run_soak_subjects(subjects: list[SoakSubject], deadline: SoakDeadline, console: Console,
                      report: Path = SOAK_REPORT, junit: Path = SOAK_JUNIT,
                      frames: int = hardening.SOAK_FRAMES) -> SoakOutcome:
    """Soaks `subjects` in order and evaluates each (hardening.evaluate_soak, after its errors: those that ended it
    early and, for an editor scene, its new script errors, each of which fails it).
    No subject starts after `deadline`: the subject it cut short fails naming it, the rest are listed as not run and the
    outcome is timed out. Soak.json and the JUnit report are written from a finally block, so the subjects that
    finished survive whatever ends the step."""
    outcome = SoakOutcome()
    try:
        for index, subject in enumerate(subjects):
            if deadline.expired():
                outcome.timed_out = True
                outcome.not_run = [entry.name for entry in subjects[index:]]
                break
            console.heading(f"soak {subject.name} ({index + 1}/{len(subjects)}): {subject.description}")
            started = deadline.clock()
            samples, errors = subject.run(deadline)
            result = hardening.evaluate_soak(subject.name, samples)
            result = dataclasses.replace(result, passed=result.passed and not errors,
                                         failures=tuple(errors) + result.failures)
            outcome.results.append(result)
            outcome.samples[subject.name] = samples
            outcome.seconds[subject.name] = deadline.clock() - started
            if errors and deadline.expired():
                outcome.timed_out = True
            console.print(f"  {'passed' if result.passed else 'FAILED'}: growth {result.growth * 100:.3f} % in "
                          f"{outcome.seconds[subject.name]:.0f} s"
                          + (f"; {result.failures[0].splitlines()[0]}" if result.failures else ""))
    finally:
        write_soak_reports(outcome, report, junit, frames)
    return outcome


def soak_step(name: str, outcome: SoakOutcome, duration: float, partial: str | None,
              report: Path = SOAK_REPORT, junit: Path = SOAK_JUNIT, frames: int = hardening.SOAK_FRAMES) -> Step:
    """The soak step of `outcome`, a soak of `frames`: timed out (exit code 5) when the deadline ended it, failed when a
    subject failed, a warning for a partial soak (`partial`, the --soak-only patterns), passed otherwise."""
    results = outcome.results
    failed = [result for result in results if not result.passed]
    data = {"report": paths.display_path(report), "junit": paths.display_path(junit),
            "subjects": [{"subject": result.subject, "growth": result.growth, "passed": result.passed,
                          "seconds": outcome.seconds.get(result.subject, 0.0), "failures": list(result.failures)}
                         for result in results],
            "notRun": outcome.not_run}
    if outcome.timed_out:
        return Step(name, Status.TIMEOUT, f"the soak did not finish within {soak_timeout(frames):.0f} s: "
                                          f"{len(results) - len(failed)} of {len(results)} soaked subject(s) passed "
                                          f"and {len(outcome.not_run)} did not run; {paths.display_path(report)} "
                                          f"holds the soaked ones", duration, data=data)
    if failed:
        return Step(name, Status.FAILED, f"{len(failed)} of {len(results)} subject(s) failed: {failed[0].subject}: "
                                         f"{failed[0].failures[0].splitlines()[0]}", duration, data=data)
    largest = max(results, key=lambda result: result.growth)
    detail = (f"{len(results)} subject(s) passed every rule over {frames} frames; the largest private "
              f"memory growth {largest.growth * 100:.3f} % ({largest.subject})")
    if partial is not None:
        return Step(name, Status.WARNING, f"{detail}; partial soak (--soak-only {partial})", duration, data=data)
    return Step(name, Status.PASSED, detail, duration, data=data)


def step_soak(arguments: argparse.Namespace, console: Console) -> Step:
    name = STEP_NAMES["soak"]
    started = time.monotonic()
    frames = arguments.soak_frames
    # One deadline for the whole step, the editor scenes and the Runtime alike.
    deadline = SoakDeadline(started + soak_timeout(frames), seconds=soak_timeout(frames))
    editor = editor_executable()
    if not editor.is_file():
        return Step(name, Status.FAILED, f"{paths.display_path(editor)} not found: run python Scripts/Build.py "
                                         f"--config {CONFIG} --project Editor", exit_code=EXIT_INIT_FAILED)
    reason = probe_rendering_editor(editor)
    if reason is not None:
        if not arguments.gpu_optional:
            return Step(name, Status.FAILED, f"no usable Vulkan device: {reason} (--gpu-optional lets the soak pass "
                                             f"without running)")
        console.print(f"{NO_DEVICE_TEXT}{name}: {reason}")
        return Step(name, Status.WARNING, f"no usable Vulkan device, so the soak passed without running ({reason})",
                    data={"noDevice": reason})

    scenes: list[tuple[str, Path, str]] = []  # (subject, project, scene)
    for project in hardening.soak_projects(paths.REPOSITORY_ROOT):
        for scene in hardening.soak_scenes(project):
            scenes.append((f"{project.name}:{scene}", project, scene))
    runtime_subject = f"Runtime:{exports.FEATURE_TEST_NAME}"
    patterns = [pattern.strip() for pattern in arguments.soak_only.split(",") if pattern.strip()]
    selected = [entry for entry in scenes if any(fnmatch.fnmatchcase(entry[0], pattern) for pattern in patterns)]
    run_runtime = any(fnmatch.fnmatchcase(runtime_subject, pattern) for pattern in patterns)
    if not selected and not run_runtime:
        return Step(name, Status.FAILED, f"--soak-only {arguments.soak_only} selects no subject")
    executable: Path | None = None
    if run_runtime:
        try:
            executable = current_package()
        except StepFailure as failure:
            return Step(name, Status.FAILED, str(failure), exit_code=failure.exit_code)

    engine_client = import_engine_client()
    with tempfile.TemporaryDirectory(prefix="Hardening-Soak-") as directory:
        root = Path(directory)
        copies: dict[Path, Path] = {}
        for _, project, _ in selected:
            if project in copies:
                continue
            copy = root / project.name
            console.print(f"copying {paths.display_path(project)} with Library/ to {copy}")
            shutil.copytree(project, copy)
            project_files = sorted(copy.glob("*.eproj"))
            if len(project_files) != 1:
                return Step(name, Status.FAILED, f"{paths.display_path(project)} holds {len(project_files)} project "
                                                 f"files, not one")
            copies[project] = project_files[0]

        def editor_scene(project_file: Path, scene: str, user_data: Path) -> Callable[[SoakDeadline], SoakRun]:
            return lambda limit: soak_editor_scene(engine_client, editor, project_file, scene, user_data, console,
                                                   limit, frames)

        def runtime(package: Path) -> Callable[[SoakDeadline], SoakRun]:
            return lambda limit: soak_runtime(package, root / "UserData-Runtime", console, limit, frames)

        subjects = [SoakSubject(subject, f"{frames} rendered lockstep frames",
                                editor_scene(copies[project], scene, root / f"UserData-{index}"))
                    for index, (subject, project, scene) in enumerate(selected)]
        if executable is not None:
            subjects.append(SoakSubject(runtime_subject, f"{frames} headless frames", runtime(executable)))
        outcome = run_soak_subjects(subjects, deadline, console, frames=frames)

    for result in (result for result in outcome.results if not result.passed):
        for failure in result.failures:
            console.print(f"FAILED: {result.subject}: {failure.splitlines()[0]}")
    if outcome.not_run:
        console.print(f"TIMEOUT: not run: {', '.join(outcome.not_run)}")
    return soak_step(name, outcome, time.monotonic() - started,
                     None if arguments.soak_only == "*" else arguments.soak_only, frames=frames)


# --------------------------------------------------------------------------------------------------------------------
# AddressSanitizer: generate, build, unit, feature
# --------------------------------------------------------------------------------------------------------------------


def step_asan_generate(arguments: argparse.Namespace, console: Console) -> Step:
    name = STEP_NAMES["asan-generate"]
    result, failed = run_script(name, "Generate.py", ["--sanitize", "address", "--to",
                                                      str(hardening.ASAN_WORKSPACE), "--no-compile-commands"],
                                TIMEOUTS["asan-generate"], console)
    if failed is not None:
        return failed
    return script_step(name, result, f"Release instrumented with AddressSanitizer in "
                                     f"{paths.display_path(hardening.ASAN_WORKSPACE)}")


def step_asan_build(arguments: argparse.Namespace, console: Console) -> Step:
    name = STEP_NAMES["asan-build"]
    options = ["--workspace-dir", str(hardening.ASAN_WORKSPACE), "--config", CONFIG,
               *(["--jobs", str(arguments.jobs)] if arguments.jobs else [])]
    result, failed = run_script(name, "Build.py", options, TIMEOUTS["asan-build"], console)
    if failed is not None:
        return failed
    step = script_step(name, result, "")
    if step.failed:
        return step
    try:
        copied = hardening.stage_asan_runtime(hardening.ASAN_WORKSPACE, CONFIG)
    except FileNotFoundError as error:
        return Step(name, Status.FAILED, str(error), result.duration, exit_code=EXIT_INIT_FAILED)
    staged = len(hardening.ASAN_RUNTIME_PROJECTS) * len(hardening.ASAN_RUNTIME_FILES)
    return Step(name, Status.PASSED, f"built {paths.display_path(asan_output_directory())}; AddressSanitizer runtime "
                                     f"staged next to {', '.join(hardening.ASAN_RUNTIME_PROJECTS)} ({len(copied)} "
                                     f"copied, {staged - len(copied)} up to date)", result.duration,
                data={"runtimeCopied": [paths.display_path(path) for path in copied]})


def sanitized_suite_step(name: str, suite: str, extra: list[str], arguments: argparse.Namespace,
                         console: Console) -> Step:
    options = ["--suite", suite, "--config", CONFIG, "--build-root", str(hardening.ASAN_WORKSPACE / "bin"),
               "--sanitizer", "address", "--junit", "--timeout", str(int(TIMEOUTS[f"asan-{suite}"])), "--json", *extra]
    try:
        run = run_json_script(name, SCRIPTS / "Test.py", options, TIMEOUTS[f"asan-{suite}"] + SUITE_GRACE_SECONDS,
                              console)
    except ToolNotFoundError as error:
        return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)
    if run.timed_out:
        return Step(name, Status.TIMEOUT, f"Test.py {run.describe_exit()}", run.duration)
    steps = [step for step in run.document.get("steps", []) if isinstance(step, dict)]
    if len(steps) != 1:
        code = run.exit_code if run.exit_code in (EXIT_INIT_FAILED, EXIT_USAGE) else EXIT_FAILED
        return Step(name, Status.FAILED, f"Test.py {run.describe_exit()} and reported {len(steps)} step(s), not one",
                    run.duration, exit_code=code)
    suite_step = steps[0]
    try:
        status = Status(suite_step.get("status", "failed"))
    except ValueError:
        status = Status.FAILED
    return Step(name, status, str(suite_step.get("detail", "")), run.duration, exit_code=suite_step.get("exitCode"),
                data={"suite": suite_step})


def step_asan_unit(arguments: argparse.Namespace, console: Console) -> Step:
    # The Fuzz suite is left out: the fuzz step runs the same cases with the same binary over [0, 1,000,000), a superset
    # of the unit counts' iterations with the same index-derived inputs (ADR 0023: no duplicate work).
    return sanitized_suite_step(STEP_NAMES["asan-unit"], "unit", ["--test-timeout-scale", str(UNIT_TIMEOUT_SCALE),
                                                                  "--exclude-test-suites", hardening.FUZZ_SUITE],
                                arguments, console)


def step_asan_feature(arguments: argparse.Namespace, console: Console) -> Step:
    return sanitized_suite_step(STEP_NAMES["asan-feature"], "feature",
                                [] if arguments.gpu_optional else ["--require-gpu"], arguments, console)


# --------------------------------------------------------------------------------------------------------------------
# debug-heap (Docs/Decisions/0029-m18-contract.md, W18.1 stream C: R-GPUCRASH)
# --------------------------------------------------------------------------------------------------------------------


def debug_heap_run_directory(run: int) -> Path:
    """Where run `run` (from 1) of the debug-heap step keeps its JUnit reports and, in GpuLifetime/, its lifetime
    logs."""
    return DEBUG_HEAP_REPORTS / f"Run{run}"


def debug_heap_suite_reports() -> list[Path]:
    """The JUnit reports one run of the debug-heap step's Test.py writes, one per API cap
    (Gpu-Debug-Vulkan<cap>-DebugHeap.xml in DEBUG_HEAP_SUITE_REPORTS)."""
    return [DEBUG_HEAP_SUITE_REPORTS / f"Gpu-{DEBUG_HEAP_CONFIG}-Vulkan{cap.replace('.', '')}-DebugHeap.xml"
            for cap in DEBUG_HEAP_CAPS]


def debug_heap_suite_crashes() -> list[Path]:
    """The folders of the crash reports one run of the debug-heap step's Tests runs write, one per API cap: the crash
    report folder of the user data Test.py gives each Tests run beside its JUnit report (Lib/paths.py's
    tests_run_user_data and crash_report_folder, which Test.py's run_user_data and crash_reports use). Test.py keeps
    them between its runs, so the step deletes them before each of its runs."""
    return [paths.crash_report_folder(paths.tests_run_user_data(report)) for report in debug_heap_suite_reports()]


def debug_heap_run_options(arguments: argparse.Namespace) -> list[str]:
    """Test.py's options of one run of the debug-heap step: the Debug gpu suite under both API caps on the debug heap,
    each Tests run bounded by its share of the step's timeout."""
    per_tests_run = int(TIMEOUTS["debug-heap"] / len(DEBUG_HEAP_CAPS))
    return ["--suite", "gpu", "--config", DEBUG_HEAP_CONFIG, "--junit", "--debug-heap", "--timeout", str(per_tests_run),
            "--json", *([] if arguments.gpu_optional else ["--require-gpu"])]


def evaluate_debug_heap_run(run: JsonRun) -> tuple[Status, str, int | None]:
    """The outcome of one run of the debug-heap step from Test.py's JSON result: its status, a detail naming every
    failed or warned Tests run, and the exit code a failure keeps (3 when the Debug build is missing)."""
    if run.timed_out:
        return Status.TIMEOUT, f"Test.py {run.describe_exit()}", None
    steps = [step for step in run.document.get("steps", []) if isinstance(step, dict)]
    if not steps:
        code = run.exit_code if run.exit_code in (EXIT_INIT_FAILED, EXIT_USAGE) else EXIT_FAILED
        return Status.FAILED, f"Test.py {run.describe_exit()} without a result", code
    statuses: list[Status] = []
    details: list[str] = []
    exit_code: int | None = None
    for step in steps:
        try:
            status = Status(step.get("status", "failed"))
        except ValueError:
            status = Status.FAILED
        statuses.append(status)
        if status != Status.PASSED:
            details.append(f"{step.get('name', 'gpu')}: {step.get('detail', '')}")
        if status in (Status.FAILED, Status.TIMEOUT) and step.get("exitCode") == EXIT_INIT_FAILED:
            exit_code = EXIT_INIT_FAILED
    for status in (Status.TIMEOUT, Status.FAILED, Status.WARNING):
        if status in statuses:
            return status, "; ".join(details), exit_code
    if run.exit_code != 0:
        return Status.FAILED, f"Test.py {run.describe_exit()}", None
    return Status.PASSED, f"{len(steps)} Tests run(s) passed", None


def count_lifetime_logs(directory: Path) -> int:
    """The lifetime logs (GpuLifetime-<pid>.log) a run left in `directory`."""
    return len(list(directory.glob("GpuLifetime-*.log"))) if directory.is_dir() else 0


def step_debug_heap(arguments: argparse.Namespace, console: Console) -> Step:
    """The Debug gpu suite under both API caps on the Windows debug heap, with every device's GPU object lifetimes
    logged (R-GPUCRASH: the intermittent crash on a driver thread, Docs/Backlog.md), --debug-heap-runs times. Each run
    is `Test.py --suite gpu --config Debug --junit --debug-heap [--require-gpu]` with ENGINE_GPU_LIFETIME_LOG set to its
    GpuLifetime/ directory (Engine/Source/Engine/Graphics/GpuResourceTracker.h), and its JUnit reports are copied
    beside it (DEBUG_HEAP_REPORTS/Run<k>/), with the crash reports and minidumps of its Tests runs in Crashes/, which
    the run's detail and its outcome's "crashReports" name instead of the originals: the reports and the crash folders
    (which Test.py keeps between its runs) are deleted before each run, so a run that wrote none (a Tests run killed by
    its timeout or crashed before its report) gets no earlier run's copy, and only the copies outlive the next run.
    Every run runs, whatever the earlier ones did, so a crash reproduction keeps the evidence of all of them; the step
    fails naming each failed run, and a missing Debug build (exit code 3) stops it. Windows only: elsewhere it is
    skipped, naming why."""
    name = STEP_NAMES["debug-heap"]
    if not debugheap.is_available():
        return Step(name, Status.SKIPPED, "the Windows debug heap exists on Windows only "
                                          "(Docs/Decisions/0028-backlog-program.md decision 10)")
    runs = arguments.debug_heap_runs
    shutil.rmtree(DEBUG_HEAP_REPORTS, ignore_errors=True)
    outcomes: list[dict[str, Any]] = []
    started = time.monotonic()
    for run in range(1, runs + 1):
        directory = debug_heap_run_directory(run)
        lifetime = directory / "GpuLifetime"
        lifetime.mkdir(parents=True, exist_ok=True)
        for report in debug_heap_suite_reports():
            report.unlink(missing_ok=True)
        for crashes in debug_heap_suite_crashes():
            shutil.rmtree(crashes, ignore_errors=True)
        environment = child_environment({GPU_LIFETIME_LOG_VARIABLE: str(lifetime)})
        try:
            result = run_json_script(f"{name}, run {run} of {runs}", SCRIPTS / "Test.py",
                                     debug_heap_run_options(arguments), TIMEOUTS["debug-heap"] + SUITE_GRACE_SECONDS,
                                     console, environment)
        except ToolNotFoundError as error:
            return Step(name, Status.FAILED, str(error), time.monotonic() - started, exit_code=EXIT_INIT_FAILED)
        status, detail, exit_code = evaluate_debug_heap_run(result)
        for report in debug_heap_suite_reports():
            if report.is_file():
                shutil.copy2(report, directory / report.name)
        crash_files = [path for crashes in debug_heap_suite_crashes() if crashes.is_dir()
                       for path in sorted(crashes.iterdir()) if path.is_file()]
        kept_reports: list[str] = []
        if crash_files:
            kept = directory / "Crashes"
            kept.mkdir(exist_ok=True)
            for path in crash_files:
                copy = kept / path.name
                shutil.copy2(path, copy)
                # Test.py's detail names the original, which the next run deletes; the copy stays.
                detail = detail.replace(paths.display_path(path), paths.display_path(copy))
                if path.suffix == ".txt":
                    kept_reports.append(paths.display_path(copy))
        outcomes.append({"run": run, "status": status.value, "detail": detail,
                         "durationSeconds": round(result.duration, 1), "lifetimeLogs": count_lifetime_logs(lifetime),
                         "crashReports": kept_reports, "directory": paths.display_path(directory)})
        console.print(f"{name}, run {run} of {runs}: {status.value}: {detail}")
        if exit_code == EXIT_INIT_FAILED:
            return Step(name, Status.FAILED, f"run {run}: {detail}", time.monotonic() - started,
                        exit_code=EXIT_INIT_FAILED, data={"runs": outcomes})
    duration = time.monotonic() - started
    data = {"runs": outcomes, "reports": paths.display_path(DEBUG_HEAP_REPORTS)}
    failed = [outcome for outcome in outcomes if outcome["status"] in (Status.FAILED.value, Status.TIMEOUT.value)]
    if failed:
        listed = "; ".join(f"run {outcome['run']}: {outcome['detail']}" for outcome in failed)
        timed_out = all(outcome["status"] == Status.TIMEOUT.value for outcome in failed)
        status = Status.TIMEOUT if timed_out else Status.FAILED
        return Step(name, status, f"{len(failed)} of {runs} run(s) failed ({listed}); lifetime logs and reports in "
                                  f"{paths.display_path(DEBUG_HEAP_REPORTS)}", duration, data=data)
    warned = [outcome for outcome in outcomes if outcome["status"] == Status.WARNING.value]
    if warned:
        return Step(name, Status.WARNING, "; ".join(f"run {outcome['run']}: {outcome['detail']}" for outcome in warned),
                    duration, data=data)
    return Step(name, Status.PASSED, f"{runs} run(s) of the {DEBUG_HEAP_CONFIG} gpu suite on the debug heap passed "
                                     f"under both API caps; lifetime logs in {paths.display_path(DEBUG_HEAP_REPORTS)}",
                duration, data=data)


# --------------------------------------------------------------------------------------------------------------------
# fuzz
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class FuzzShard:
    index: int
    count: int
    process: subprocess.Popen[bytes]
    standard_output: Path
    standard_error: Path
    junit: Path
    peak_private_bytes: int = 0

    @property
    def label(self) -> str:
        return f"shard {self.index}/{self.count}"


def case_seconds(junit: Path) -> dict[str, float]:
    """{test case: seconds} of a doctest JUnit report (empty when it cannot be read)."""
    try:
        root = ElementTree.parse(junit).getroot()
    except (OSError, ElementTree.ParseError):
        return {}
    seconds: dict[str, float] = {}
    for case in root.iter("testcase"):
        try:
            seconds[case.get("name", "")] = seconds.get(case.get("name", ""), 0.0) + float(case.get("time", "0") or 0)
        except ValueError:
            continue
    return seconds


def junit_failures(junit: Path) -> list[str]:
    try:
        root = ElementTree.parse(junit).getroot()
    except (OSError, ElementTree.ParseError):
        return []
    failures = []
    for case in root.iter("testcase"):
        for problem in case.findall("failure") + case.findall("error"):
            text = (problem.get("message", "") or problem.text or "").strip().splitlines()
            failures.append(f"{case.get('name')}: {text[0] if text else 'failed'}")
    return failures


def fuzz_costs_path() -> Path:
    """Where the fuzz step keeps, and reads, the last full, passing run's report (hardening.machine_fuzz_costs_path)."""
    return hardening.machine_fuzz_costs_path()


def fuzz_costs(targets: list[hardening.FuzzTarget],
               sources: tuple[Path, ...]) -> tuple[dict[str, float], Path | None, str]:
    """(the per-target costs, the report they came from, how the step is sized): the first of `sources` that measures
    one of `targets` (a missing, unreadable or failed report, or one of a development run, measures none); without one,
    no costs, no report and the reference run."""
    problems: list[str] = []
    for source in sources:
        try:
            costs = hardening.read_fuzz_costs(source)
        except ValueError as error:
            problems.append(str(error))
            continue
        measured = sum(1 for target in targets if hardening.fuzz_target_case_name(target) in costs)
        if measured:
            return costs, source, (f"sized from {paths.display_path(source)}: {measured} of {len(targets)} target(s) "
                                   f"measured, the rest at their average")
        problems.append(f"{paths.display_path(source)} measures no target of Targets.json" if source.is_file()
                        else f"no {paths.display_path(source)}")
    return {}, None, f"sized from the reference run scaled by the target count: {'; '.join(problems)}"


def step_fuzz(arguments: argparse.Namespace, console: Console) -> Step:
    name = STEP_NAMES["fuzz"]
    started = time.monotonic()
    tests = asan_output_directory() / "Tests" / paths.executable_name("Tests")
    if not tests.is_file():
        return Step(name, Status.FAILED, f"{paths.display_path(tests)} not found: run python Scripts/Hardening.py "
                                         f"--steps asan-generate,asan-build", exit_code=EXIT_INIT_FAILED)
    try:
        targets = hardening.read_fuzz_targets()
    except ValueError as error:
        return Step(name, Status.FAILED, str(error))
    iterations = arguments.fuzz_iterations or hardening.FUZZ_HARDENING_ITERATIONS
    if iterations != hardening.FUZZ_HARDENING_ITERATIONS:
        targets = [dataclasses.replace(target, iterations=iterations) for target in targets]
    # The per-target costs of the last full, passing run on this machine size the step, else M17's committed ones;
    # without either the reference run, scaled by the target count, does.
    kept = fuzz_costs_path()
    measured_costs, source, sizing = fuzz_costs(targets, (kept, hardening.FUZZ_COMMITTED_COSTS))
    target_seconds = hardening.fuzz_target_seconds(targets, measured_costs)
    measured = sum(1 for target in targets if hardening.fuzz_target_case_name(target) in measured_costs)
    work = sum(target_seconds.values())
    budget = hardening.fuzz_budget_seconds(len(targets))
    try:
        available = hardening.available_memory_bytes()
    except OSError as error:
        return Step(name, Status.FAILED, f"cannot read the available memory: {error}")
    logical_cpus = os.cpu_count() or 1
    count = arguments.fuzz_processes or hardening.fuzz_process_count(logical_cpus, available,
                                                                      hardening.FUZZ_PEAK_PROCESS_BYTES)
    # Refuse a shard count too small for the step's budget before anything starts, rather than time out after an
    # hour on the machine's free memory alone; each case's limit grows with its shard's share of the work.
    minimum = hardening.fuzz_minimum_processes(work, budget)
    model = {"workSeconds": work, "budgetSeconds": budget, "minimumProcesses": minimum, "measuredTargets": measured,
             "costs": paths.display_path(source) if source is not None else "reference"}
    problem = hardening.fuzz_capacity_problem(count, minimum, logical_cpus, available,
                                              hardening.FUZZ_PEAK_PROCESS_BYTES, budget)
    if problem is not None:
        return Step(name, Status.FAILED, f"{problem} ({sizing})", exit_code=EXIT_INIT_FAILED,
                    data={"processes": count, "availableBytes": available, "logicalCpus": logical_cpus, **model})
    timeout = hardening.fuzz_timeout_seconds(work, count, budget, logical_cpus)
    timeout_scale = hardening.fuzz_timeout_scale(count, iterations, max(target_seconds.values()))
    try:
        environment = hardening.asan_environment(child_environment())
    except FileNotFoundError as error:
        return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)
    console.heading(f"{name}: {len(targets)} target(s) x {iterations} iteration(s) in {count} process(es) "
                    f"({logical_cpus} logical CPUs, {available >> 20} MB available; each case's limit scaled by "
                    f"{timeout_scale}; about {hardening.fuzz_expected_seconds(work, count) / 60:.0f} min expected, "
                    f"timeout {timeout:.0f} s; {sizing})")

    shards: list[FuzzShard] = []
    lines: list[str] = []
    failures: list[str] = []
    seconds: dict[str, float] = {}
    with tempfile.TemporaryDirectory(prefix="Hardening-Fuzz-") as directory:
        root = Path(directory)
        try:
            for index in range(count):
                user_data = root / f"Shard{index}"
                user_data.mkdir()
                junit = root / f"Shard{index}.xml"
                command = [str(tests), f"--test-suite={hardening.FUZZ_SUITE}",
                           f"--fuzz-iterations={iterations}", f"--fuzz-shard={index}/{count}",
                           f"--test-timeout-scale={timeout_scale}", f"--user-data-dir={user_data}",
                           "--reporters=junit", f"--out={junit}"]
                if index == 0:
                    console.print(format_command([paths.display_path(tests), *command[1:]]))
                standard_output = root / f"Shard{index}.out.txt"
                standard_error = root / f"Shard{index}.err.txt"
                with standard_output.open("wb") as output, standard_error.open("wb") as error_output:
                    child = subprocess.Popen(command, cwd=paths.REPOSITORY_ROOT, stdin=subprocess.DEVNULL,
                                             stdout=output, stderr=error_output, env=environment)
                shards.append(FuzzShard(index, count, child, standard_output, standard_error, junit))
            deadline = started + timeout
            timed_out = False
            while any(shard.process.poll() is None for shard in shards):
                for shard in shards:
                    if shard.process.poll() is None:
                        try:
                            memory = process_lib.query_process_memory(shard.process.pid)
                            shard.peak_private_bytes = max(shard.peak_private_bytes, memory.private_bytes)
                        except (ProcessLookupError, OSError):
                            pass
                if time.monotonic() >= deadline:
                    timed_out = True
                    break
                time.sleep(FUZZ_MEMORY_SAMPLE_SECONDS)
        finally:
            for shard in shards:
                if shard.process.poll() is None:
                    shard.process.kill()
                shard.process.wait()
        for shard in shards:
            text = shard.standard_error.read_text(encoding="utf-8", errors="replace")
            shard_lines = text.splitlines()
            lines += shard_lines
            sanitizer = [line.strip() for line in shard_lines if "ERROR: AddressSanitizer:" in line]
            for case, case_time in case_seconds(shard.junit).items():
                seconds[case] = seconds.get(case, 0.0) + case_time
            if shard.process.returncode != 0 or sanitizer:
                output = shard.standard_output.read_text(encoding="utf-8", errors="replace")
                detail = "; ".join(junit_failures(shard.junit)[:3]) or tail(text, 10) or tail(output, 10)
                failures.append(f"fuzz: {shard.label} exited with code {describe_exit_code(shard.process.returncode)}"
                                + (f"; {sanitizer[0]}" if sanitizer else "") + (f": {detail}" if detail else ""))
                console.print(f"FAILED: {shard.label}:\n{tail(text, 60)}")
    if timed_out:
        failures.insert(0, f"fuzz: the shards did not finish within {timeout:.0f} s")
    coverage_failures = hardening.evaluate_fuzz_logs(targets, lines)
    failures += coverage_failures
    duration = time.monotonic() - started
    costs = {target.reader: seconds.get(target.case, 0.0) / target.iterations for target in targets}
    peak = max((shard.peak_private_bytes for shard in shards), default=0)
    reports = hardening.parse_fuzz_reports(lines)
    counts = {target.reader: {"accepted": sum(report.accepted for report in reports if report.case == target.case),
                              "rejected": sum(report.rejected for report in reports if report.case == target.case)}
              for target in targets}
    for target in targets:
        console.print(f"  {target.reader}: {costs[target.reader] * 1e6:.1f} us per iteration (summed over shards), "
                      f"{counts[target.reader]['accepted']} accepted, {counts[target.reader]['rejected']} rejected")
    console.print(f"  peak private memory of a shard process: {peak >> 20} MB")
    for failure in failures:
        console.print(f"FAILED: {failure.splitlines()[0]}")

    junit_steps = [Step(hardening.fuzz_target_case_name(target),
                        Status.FAILED if any(line.startswith(f"fuzz: {target.reader}:") for line in coverage_failures)
                        else Status.PASSED,
                        "; ".join(line for line in coverage_failures if line.startswith(f"fuzz: {target.reader}:"))
                        or hardening.fuzz_target_detail(target.iterations, costs[target.reader]),
                        seconds.get(target.case, 0.0)) for target in targets]
    junit_steps += [Step(shard.label,
                         Status.FAILED if any(line.startswith(f"fuzz: {shard.label} ") for line in failures)
                         else Status.PASSED,
                         next((line for line in failures if line.startswith(f"fuzz: {shard.label} ")),
                              f"peak private memory {shard.peak_private_bytes >> 20} MB")) for shard in shards]
    write_junit(FUZZ_JUNIT, "Fuzz.Release.Asan", junit_steps)
    # Only a full, passing run measures every target's cost: it becomes the measurement every checkout's next fuzz step
    # reads, and a development or failed run leaves the kept one alone.
    kept_costs: str | None = None
    keep_problem = ""
    if not failures and iterations == hardening.FUZZ_HARDENING_ITERATIONS:
        try:
            hardening.keep_fuzz_costs(FUZZ_JUNIT, kept)
            kept_costs = paths.display_path(kept)
        except OSError as error:
            keep_problem = f"the measured costs could not be kept in {paths.display_path(kept)}: {error}"
            console.print(f"WARNING: {keep_problem}")
    data = {"junit": paths.display_path(FUZZ_JUNIT), "processes": count, "iterations": iterations,
            "timeoutScale": timeout_scale, "timeoutSeconds": timeout, "targets": len(targets),
            "secondsPerIteration": costs, "peakProcessPrivateBytes": peak, "counts": counts, "failures": failures,
            "keptCosts": kept_costs, **model}
    if failures:
        return Step(name, Status.TIMEOUT if timed_out else Status.FAILED,
                    f"{len(failures)} failure(s): {failures[0].splitlines()[0]}", duration, data=data)
    detail = (f"{len(targets)} target(s) ran {iterations} iterations each in {count} process(es), clean; the peak "
              f"private memory of a process {peak >> 20} MB")
    if iterations != hardening.FUZZ_HARDENING_ITERATIONS or arguments.fuzz_processes:
        return Step(name, Status.WARNING, f"{detail}; development run (--fuzz-iterations or --fuzz-processes)",
                    duration, data=data)
    if keep_problem:
        # The next fuzz step would be sized from an older measurement: the run passed, but not as a measurement.
        return Step(name, Status.WARNING, f"{detail}; {keep_problem}", duration, data=data)
    return Step(name, Status.PASSED, detail, duration, data=data)


# --------------------------------------------------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------------------------------------------------


STEP_FUNCTIONS = {
    "perf": step_perf,
    "soak": step_soak,
    "asan-generate": step_asan_generate,
    "asan-build": step_asan_build,
    "asan-unit": step_asan_unit,
    "asan-feature": step_asan_feature,
    "debug-heap": step_debug_heap,
    "fuzz": step_fuzz,
}


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the hardening stage: perf capture, soak, AddressSanitizer build and suites, the gpu suite on "
                    "the debug heap, long fuzz.",
        epilog="Exit codes: 0 every step passed (or warned), 1 a step failed, 2 usage error, 3 a build output, the "
               "AddressSanitizer runtime or a current export package is missing, or the machine has too little memory "
               "or too few CPUs for the fuzz to finish in time (it needs as many logical CPUs and GiB of available "
               "memory as its minimum shard count, which its measured per-target costs set: 14 at 1,000,000 "
               "iterations with M17's), or --no-wait found the GPU lock held, 5 a step timed out or the wait for the "
               "GPU lock ran out.",
    )
    parser.add_argument("--steps", default=",".join(STEPS),
                        help=f"comma-separated steps, run in the stage's order: {', '.join(STEPS)} (default: all)")
    parser.add_argument("--gpu-optional", action="store_true",
                        help="without a Vulkan device the perf capture falls back and the soak and the ASan feature "
                             "suite pass without running, as warnings (otherwise they fail)")
    parser.add_argument("--jobs", type=int, help="asan-build: parallel build jobs (Build.py --jobs)")
    parser.add_argument("--soak-only", default="*", metavar="PATTERNS",
                        help="soak: only the subjects matching one of these comma-separated fnmatch patterns "
                             "('<project>:<scene>', 'Runtime:<project>'); a development aid that makes the step a "
                             "warning")
    parser.add_argument("--fuzz-iterations", type=int,
                        help="fuzz: iterations per target instead of 1,000,000; a development aid that makes the step "
                             "a warning")
    parser.add_argument("--soak-frames", type=int, default=hardening.SOAK_FRAMES, metavar="N",
                        help=f"soak: ticks per editor scene and Runtime frames (default {hardening.SOAK_FRAMES}; at "
                             f"least that, a multiple of {hardening.SOAK_SAMPLE_INTERVAL}); a longer soak confirms a "
                             f"script heap result that was inconclusive because a collection cycle is longer than "
                             f"about a quarter of the soak; the step's deadline grows in proportion")
    parser.add_argument("--debug-heap-runs", type=int, default=1, metavar="N",
                        help="debug-heap: run the Debug gpu suite on the debug heap N times (default 1; the crash runs "
                             "of M18's W18.2 take 20), each run's reports and lifetime logs in "
                             "bin/TestResults/DebugHeap/Run<k>/; every run runs, and the step's deadline grows in "
                             "proportion")
    parser.add_argument("--fuzz-processes", type=int,
                        help="fuzz: processes instead of the computed count; a development aid that makes the step a "
                             "warning, and a count below the minimum the step's budget needs (14 at 1,000,000 "
                             "iterations with M17's measured costs) is refused with exit code 3")
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    locks.add_wait_arguments(parser)
    arguments = parser.parse_args(argv)
    locks.check_wait_arguments(parser, arguments)
    try:
        arguments.steps = paths.parse_name_list(arguments.steps, STEPS, "step")
    except ValueError as error:
        parser.error(str(error))
    for option in ("jobs", "fuzz_iterations", "fuzz_processes", "debug_heap_runs"):
        value = getattr(arguments, option)
        if value is not None and value < 1:
            parser.error(f"--{option.replace('_', '-')} must be at least 1")
    if arguments.fuzz_processes is not None and arguments.fuzz_processes > hardening.FUZZ_MAX_PROCESSES:
        parser.error(f"--fuzz-processes must be at most {hardening.FUZZ_MAX_PROCESSES}")
    if arguments.soak_frames < hardening.SOAK_FRAMES or arguments.soak_frames % hardening.SOAK_SAMPLE_INTERVAL:
        parser.error(f"--soak-frames must be at least {hardening.SOAK_FRAMES} and a multiple of "
                     f"{hardening.SOAK_SAMPLE_INTERVAL}")
    return arguments


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    paths.enter_repository_root()
    console = Console(arguments.json)
    try:
        host = paths.host()
    except paths.UnsupportedHostError as error:
        console.print(f"Hardening.py: error: {error}")
        return EXIT_USAGE
    console.heading(f"Hardening ({host.system}): {', '.join(arguments.steps)}")
    try:
        hold = take_machine_locks(arguments, console)
    except locks.LockUnavailable as error:
        step = locks.unavailable_step(error)
        console.result(step)
        console.summary("Hardening summary", [step])
        if arguments.json:
            emit_json({"success": False, "steps": [step.to_json()]})
        return error.exit_code
    try:
        steps = run_steps(arguments, console)
    finally:
        if hold is not None:
            hold.release()
    exit_code = overall_exit_code(steps)
    console.summary("Hardening summary", steps)
    if arguments.json:
        emit_json({"success": exit_code == EXIT_SUCCESS, "steps": [step.to_json() for step in steps]})
    return exit_code


def uses_gpu(steps: list[str]) -> bool:
    """Whether a run of `steps` runs the GPU (GPU_STEPS), so it holds the machine-wide GPU lock."""
    return bool(GPU_STEPS & set(steps))


def take_machine_locks(arguments: argparse.Namespace, console: Console) -> locks.Hold | None:
    """The machine-wide GPU lock for a run whose steps run the GPU, held to its end; any other run takes no lock of its
    own (its Build.py takes the build lock). Raises locks.LockUnavailable."""
    if uses_gpu(arguments.steps):
        return locks.acquire_gpu(locks.wait_seconds(arguments), console.print)
    return None


def longest_run_seconds(target_count: int) -> float:
    """The longest a run of every step can take with `target_count` fuzz targets and one run of the debug-heap step:
    every step's timeout, the graces of the sanitized suites and the debug-heap step (GRACED_STEPS) and the fuzz step's
    budget (hardening.fuzz_budget_seconds), which its timeout never exceeds. CI.py's bound on the whole hardening run
    must exceed it (Tests/Automation/test_hardening.py)."""
    return (sum(TIMEOUTS.values()) + SUITE_GRACE_SECONDS * len(GRACED_STEPS)
            + hardening.fuzz_budget_seconds(target_count))


def run_steps(arguments: argparse.Namespace, console: Console) -> list[Step]:
    """The selected steps in the stage's order, failing fast: a step after a failed one is reported as not run."""
    steps: list[Step] = []
    failed: Step | None = None
    for key in arguments.steps:
        if failed is not None:
            step = Step(STEP_NAMES[key], Status.NOT_RUN, f"{failed.name} failed")
        else:
            step_started = time.monotonic()
            step = STEP_FUNCTIONS[key](arguments, console)
            if step.duration == 0.0:
                step.duration = time.monotonic() - step_started
            if step.failed:
                failed = step
        steps.append(step)
        console.result(step)
    return steps


if __name__ == "__main__":
    sys.exit(main())
