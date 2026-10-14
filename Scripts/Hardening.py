#!/usr/bin/env python3
"""The hardening stage (Docs/Architecture.md §15.1 T6, §15.8; Docs/Decisions/0024-m15-contract.md decisions 12 to 15
and 19): the perf capture, the soak, the AddressSanitizer build and suites, and the long fuzz, which CI.py runs at
milestone gates (CI.py --milestone).

Steps, in this order, failing fast (a step after a failed one is reported as not run):
  perf           perf capture (Release): Scripts/Perf.py --config Release (stream D), warning-only: an over-budget
                 metric or a fallback without a GPU is a warning, only a capture that cannot run fails
  soak           soak Release: every scene of every project (Lib/hardening.py soak_projects and soak_scenes, the
                 golden scenes included) in one plain Release editor each (--headless --renderer vulkan --audio-device
                 none --automation, on a temporary copy of the project with Library/): scene.open, play.start
                 {lockstep: true} and N / 250 x play.step {ticks: 250, render: "Every"}, N rendered frames (N =
                 --soak-frames, 10,000 by default), sampling the editor's process memory and stats.get after each step;
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
  fuzz           fuzz Release (ASan): the sanitized Tests binary with --test-suite=Fuzz --fuzz-iterations=1000000
                 --fuzz-shard=<k>/<n> --test-timeout-scale=<s> in n = hardening.fuzz_process_count(logical CPUs,
                 available memory, FUZZ_PEAK_PROCESS_BYTES) processes, each with its own --user-data-dir and the
                 AddressSanitizer environment. Each shard runs 1/n of every target, so its cases' limit grows with
                 that share below the measured run's 19 processes: s = hardening.fuzz_timeout_scale(n) =
                 max(20, ceil(20 x 19 / n)), never below the measured run's 20, since shards beyond the physical cores
                 share them. Below hardening.fuzz_minimum_processes (14, the count whose expected time fits in 80 % of
                 the step's 5400 s) the step refuses to start, exit code 3 naming the available memory and logical
                 CPUs it needs: the stage needs at least 14 logical CPUs and 14 GiB of available memory (14 shards of
                 FUZZ_PEAK_PROCESS_BYTES, 1 GiB) when the fuzz step starts. The
                 ReportFuzzRun lines are read from each process's standard error and hardening.evaluate_fuzz_logs
                 checks that every target of Tests/Data/Fuzz/Targets.json covered [0, 1,000,000) exactly once; any
                 process failure fails the step. bin/TestResults/Fuzz-Release-Asan.xml has one test case per target
                 and per process; the step reports each target's cost per iteration and the peak memory of a process.

--gpu-optional: without a Vulkan device the perf capture falls back, the soak passes without running, and the ASan
feature suite passes without running, each as a warning (otherwise they fail). Development aids, which make a step a
warning because it no longer covers the whole stage: --soak-only <pattern> (fnmatch over the subjects,
"<project>:<scene>" and "Runtime:<project>"), --fuzz-iterations <n> (each target runs n iterations instead of
1,000,000) and --fuzz-processes <n> (instead of the computed count; a count below the minimum the fuzz step's timeout
needs for its iterations, 14 at 1,000,000, is still refused with exit code 3). --soak-frames <n> soaks every subject
for n ticks or frames instead of 10,000 (at least 10,000, a multiple of 250): a scene whose collection cycle is longer
than about a quarter of the soak can leave the script heap rule inconclusive depending only on the cycle's phase, and
a longer soak of that scene (--soak-only <subject> --soak-frames 40000) confirms it; it is a full soak, not a
development aid.

Exit codes: 0 every step passed (or warned), 1 a step failed, 2 usage error, 3 a build output, the AddressSanitizer
runtime or a current export package is missing, or the machine has too little memory or too few CPUs for the fuzz to
finish in time (it needs at least 14 logical CPUs and 14 GiB of available memory at 1,000,000 iterations), 5 a step
timed out.
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

from Lib import exports, hardening, paths
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

STEPS = ("perf", "soak", "asan-generate", "asan-build", "asan-unit", "asan-feature", "fuzz")
STEP_NAMES = {
    "perf": "perf capture (Release)",
    "soak": "soak Release",
    "asan-generate": "asan generate",
    "asan-build": "asan build",
    "asan-unit": "unit Release (ASan)",
    "asan-feature": "feature Release (ASan)",
    "fuzz": "fuzz Release (ASan)",
}
# Seconds per step (ADR 0024 decision 19's TIMEOUTS as amended at integration: perf 1200 s, the ASan build 7200 s, the
# fuzz 5400 s, every other step 3600 s). Measured on the 24-thread workstation: perf 3 min, soak 16 min, ASan build
# 20 min from scratch and 2 min incremental, ASan unit 21 min, ASan feature 1 min, fuzz 52 min.
TIMEOUTS = {"perf": 1200.0, "soak": 3600.0, "asan-generate": 3600.0, "asan-build": 7200.0, "asan-unit": 3600.0,
            "asan-feature": 3600.0, "fuzz": 5400.0}
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


def run_json_script(name: str, script: Path, arguments: list[str], timeout: float, console: Console) -> JsonRun:
    """Runs a script with --json among `arguments`: its progress (on its stderr) is shown as it runs, its stdout parsed.
    Raises ToolNotFoundError when it cannot be started."""
    command = [PYTHON, str(script), *arguments]
    console.heading(f"{name}: {format_command(['python', paths.display_path(script), *arguments])}")
    started = time.monotonic()
    try:
        child = subprocess.Popen(command, cwd=paths.REPOSITORY_ROOT, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=None, env=child_environment())
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


SoakRun = tuple[list[hardening.SoakSample], list[str]]  # a subject's samples and the errors that ended it early


def soak_editor_scene(engine_client: Any, editor: Path, project_file: Path, scene: str, user_data: Path,
                      console: Console, deadline: SoakDeadline, frames: int = hardening.SOAK_FRAMES) -> SoakRun:
    """One scene's editor soak of `frames` ticks (frames / SOAK_STEP_TICKS play.step calls): its samples, each at
    position tick / frames, and the errors that ended it early (empty when it ran all its ticks). Each call waits at
    most SOAK_CALL_TIMEOUT_SECONDS and never past `deadline`; at the deadline the scene ends with an error naming it,
    and the editor is killed."""
    samples: list[hardening.SoakSample] = []
    errors: list[str] = []
    try:
        editor_process = engine_client.launch_editor(
            editor, ["--headless", "--renderer", "vulkan", "--audio-device", "none", "--automation", "--project",
                     str(project_file)], user_data, paths.workspace_name(), environment=child_environment())
    except (RuntimeError, TimeoutError, OSError) as error:
        errors.append(f"the editor did not start: {error}")
        console.print(f"  {scene}: {errors[0].splitlines()[0]}")
        return samples, errors
    tick = 0
    try:
        client = editor_process.connect("hardening-soak", call_log=engine_client.CallLog(None))
        with client:
            pid = editor_process.process.pid
            try:
                client.call("scene.open", {"path": scene}, timeout=deadline.limit(SOAK_CALL_TIMEOUT_SECONDS))
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
            except engine_client.EngineError as error:
                errors.append(f"{error}")
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
    """Soaks `subjects` in order and evaluates each (hardening.evaluate_soak, after the errors that ended it early).
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
        run = run_json_script(name, SCRIPTS / "Test.py", options, TIMEOUTS[f"asan-{suite}"] + 60.0, console)
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
    try:
        available = hardening.available_memory_bytes()
    except OSError as error:
        return Step(name, Status.FAILED, f"cannot read the available memory: {error}")
    logical_cpus = os.cpu_count() or 1
    count = arguments.fuzz_processes or hardening.fuzz_process_count(logical_cpus, available,
                                                                      hardening.FUZZ_PEAK_PROCESS_BYTES)
    # Refuse a shard count too small for the step's timeout before anything starts, rather than time out after an
    # hour on the machine's free memory alone; each case's limit grows with its shard's share of the work.
    minimum = hardening.fuzz_minimum_processes(TIMEOUTS["fuzz"], iterations)
    problem = hardening.fuzz_capacity_problem(count, minimum, logical_cpus, available,
                                              hardening.FUZZ_PEAK_PROCESS_BYTES, TIMEOUTS["fuzz"])
    if problem is not None:
        return Step(name, Status.FAILED, problem, exit_code=EXIT_INIT_FAILED,
                    data={"processes": count, "minimumProcesses": minimum, "availableBytes": available,
                          "logicalCpus": logical_cpus})
    timeout_scale = hardening.fuzz_timeout_scale(count, iterations)
    try:
        environment = hardening.asan_environment(child_environment())
    except FileNotFoundError as error:
        return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)
    console.heading(f"{name}: {len(targets)} target(s) x {iterations} iteration(s) in {count} process(es) "
                    f"({logical_cpus} logical CPUs, {available >> 20} MB available; each case's limit scaled by "
                    f"{timeout_scale}; about {hardening.fuzz_expected_seconds(count, iterations) / 60:.0f} min "
                    f"expected)")

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
            deadline = started + TIMEOUTS["fuzz"]
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
        failures.insert(0, f"fuzz: the shards did not finish within {TIMEOUTS['fuzz']:.0f} s")
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

    junit_steps = [Step(f"{target.reader} ({target.case})",
                        Status.FAILED if any(line.startswith(f"fuzz: {target.reader}:") for line in coverage_failures)
                        else Status.PASSED,
                        "; ".join(line for line in coverage_failures if line.startswith(f"fuzz: {target.reader}:"))
                        or f"{target.iterations} iterations; {costs[target.reader] * 1e6:.1f} us per iteration",
                        seconds.get(target.case, 0.0)) for target in targets]
    junit_steps += [Step(shard.label,
                         Status.FAILED if any(line.startswith(f"fuzz: {shard.label} ") for line in failures)
                         else Status.PASSED,
                         next((line for line in failures if line.startswith(f"fuzz: {shard.label} ")),
                              f"peak private memory {shard.peak_private_bytes >> 20} MB")) for shard in shards]
    write_junit(FUZZ_JUNIT, "Fuzz.Release.Asan", junit_steps)
    data = {"junit": paths.display_path(FUZZ_JUNIT), "processes": count, "iterations": iterations,
            "timeoutScale": timeout_scale, "targets": len(targets), "secondsPerIteration": costs,
            "peakProcessPrivateBytes": peak, "counts": counts, "failures": failures}
    if failures:
        return Step(name, Status.TIMEOUT if timed_out else Status.FAILED,
                    f"{len(failures)} failure(s): {failures[0].splitlines()[0]}", duration, data=data)
    detail = (f"{len(targets)} target(s) ran {iterations} iterations each in {count} process(es), clean; the peak "
              f"private memory of a process {peak >> 20} MB")
    if iterations != hardening.FUZZ_HARDENING_ITERATIONS or arguments.fuzz_processes:
        return Step(name, Status.WARNING, f"{detail}; development run (--fuzz-iterations or --fuzz-processes)",
                    duration, data=data)
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
    "fuzz": step_fuzz,
}


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the hardening stage: perf capture, soak, AddressSanitizer build and suites, long fuzz.",
        epilog="Exit codes: 0 every step passed (or warned), 1 a step failed, 2 usage error, 3 a build output, the "
               "AddressSanitizer runtime or a current export package is missing, or the machine has too little memory "
               "or too few CPUs for the fuzz to finish in time (it needs at least 14 logical CPUs and 14 GiB of "
               "available memory at 1,000,000 iterations), 5 a step timed out.",
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
    parser.add_argument("--fuzz-processes", type=int,
                        help="fuzz: processes instead of the computed count; a development aid that makes the step a "
                             "warning, and a count below the minimum the step's timeout needs (14 at 1,000,000 "
                             "iterations) is refused with exit code 3")
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    arguments = parser.parse_args(argv)
    try:
        arguments.steps = paths.parse_name_list(arguments.steps, STEPS, "step")
    except ValueError as error:
        parser.error(str(error))
    for option in ("jobs", "fuzz_iterations", "fuzz_processes"):
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
    console = Console(arguments.json)
    try:
        host = paths.host()
    except paths.UnsupportedHostError as error:
        console.print(f"Hardening.py: error: {error}")
        return EXIT_USAGE
    console.heading(f"Hardening ({host.system}): {', '.join(arguments.steps)}")
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
    exit_code = overall_exit_code(steps)
    console.summary("Hardening summary", steps)
    if arguments.json:
        emit_json({"success": exit_code == EXIT_SUCCESS, "steps": [step.to_json() for step in steps]})
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
