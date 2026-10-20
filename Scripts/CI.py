#!/usr/bin/env python3
"""The single CI entry point (Docs/Architecture.md §15.8): runs the stages in order and stops at the first failing one.

The stages, with the configurations of the §15.8 matrix:
  setup        n/a                   Scripts/Setup.py (toolchain checks, pinned premake, Tools/MCP/.venv and .mcp.json)
  generate     n/a                   Scripts/Generate.py (host workspace, compile_commands.json)
  lint         n/a                   static checks, shared with PreCommit.py (Scripts/Lib/scripts.py):
                                     Scripts/CheckBuildConfig.py passes on the workspace and fails, with the fixture's
                                     own defect, on every fixture workspace under Tests/Data/BuildConfig/;
                                     Scripts/Lint.py (header self-containment and the contract stubs and skipped tests
                                     included) and Lint.py --self-test (every seeded fixture under Tests/Data/Lint/
                                     fails with exactly its expected findings); Scripts/CheckRuntimeMap.py --self-test
                                     (every seeded map under Tests/Data/MapFiles/ gives exactly its expected findings,
                                     in every map format); Scripts/AuditProvenance.py --self-test (every seeded case
                                     under Tests/Data/Provenance/ yields exactly the findings and warnings of its
                                     Expected.json, and every finding code has a case); Scripts/Format.py --check;
                                     every fixture generator under Tests/Data/Generate/ with --check (the committed
                                     fixtures are byte-identical to a fresh generation);
                                     Scripts/GenerateDetMathReference.py --check (its self-test,
                                     then the committed DetMath oracle is byte-identical to a fresh generation);
                                     Scripts/FetchAssets.py defaults --check
                                     (the committed engine resources match their SHA-256 pins, offline) and
                                     FetchAssets.py fixture --check (the pinned third-party test fixtures match their
                                     pins and Tests/Data/LICENSES.md, offline)
  build        Debug, Release, Dist  Scripts/Build.py per configuration, plus, after Debug, the Roadmap M0 acceptance
                                     check "Shaders: slang-only change is not skipped by the up-to-date check":
                                     touching only a .slang file re-runs the shader rule, the next build does not; and,
                                     right after Dist, the Roadmap M15 acceptance step "Dist: map file contains no
                                     automation, Analysis, ImGui or AssetPipeline symbols": Scripts/CheckRuntimeMap.py
                                     --config Dist on the Dist Runtime's linker map, which must also hold the
                                     FeatureTest harness's required symbols (ADR 0024 decision 11)
  bake         Release               Editor --headless --renderer vulkan --bake-engine-assets (ADR 0010 decision 13,
                                     ADR 0013 decision 9): fills the configuration-independent engine cooked cache
                                     bin/EngineCache from Resources/EngineAssets.json, incrementally (up-to-date entries
                                     are kept), the built-in environments with the GPU environment baker; entries this
                                     build has no importer or generator for are skipped with a warning, and a failed
                                     bake fails the stage. Without a usable Vulkan device the editor exits 3 and the
                                     stage fails, unless --gpu-optional is given: the bake then reruns with --renderer
                                     none, the environments are skipped with a warning and the stage ends as a warning
  unit         Debug, Release        Scripts/Test.py --suite unit --junit, which also fails on a skipped test case
                                     outside the child-process targets
  gpu          Debug, Release        Scripts/Test.py --suite gpu --junit --require-gpu: the GPU test cases with
                                     validation and synchronization validation, under API 1.4 and capped at 1.3
                                     (§8.1, §15.3); a machine without a usable Vulkan device fails here unless
                                     --gpu-optional is given (hosted CI runners without a GPU), which reports those
                                     test cases as passed without running, with the reason, and the step as a warning
  golden       Release               Scripts/Test.py --suite golden --junit --require-gpu: the golden images of this
                                     machine's device class (§15.4); smoke mode, a warning, where it has none (always
                                     so on Lavapipe: no software-rasterizer goldens); --gpu-optional as for gpu
  feature      Debug, Release        Scripts/Test.py --suite feature --junit --require-gpu: Editor --headless --project
                                     Projects/FeatureTest --run-tests --strict (FeatureTest in editor mode, §15.5, its
                                     unknown fields and components errors, §6) with the six
                                     per-mode coverage gates evaluated and green, no skipped case row and the project
                                     unchanged outside Library/ (Docs/Decisions/0022-m14-contract.md decision 7); an
                                     .hdr import needs the GPU baker, so --gpu-optional applies as for gpu
  automation   Release               Scripts/Test.py --suite automation --junit --require-gpu: the Python suites
                                     Tests/Automation and Tools/MCP/tests against the headless editor, in the MCP
                                     bridge's virtual environment (the setup stage creates it), and the method coverage
                                     gate (§15.6, §15.7); on Linux it needs a display with a window manager, like
                                     unit. Its screenshot tests start rendering editors, so --gpu-optional applies as
                                     for gpu
  export       Release, Dist         Scripts/Test.py --suite export --config Release,Dist --junit --require-gpu: the
                                     testing export of FeatureTest in each configuration (bin/Exports/<Config>/
                                     FeatureTest), run with --feature-test, its six per-mode gates evaluated against
                                     its own mode and one counted Debug.Break in its Api.Debug row, and the cross-mode
                                     merge over the Editor (the feature stage's Feature-Release.json), Release and Dist
                                     reports (§15.1 T3, §15.6; ADR 0024 decision 9); the exporting editor runs with
                                     --strict; --gpu-optional as for gpu
  determinism  Debug, Release, Dist  Scripts/Test.py --suite determinism --config Debug,Release,Dist --junit
                                     --require-gpu: every Testing.Replays file of FeatureTest, verified with
                                     input.replay {verify, strictHash} (its checkpoints compared) in the --renderer
                                     none --strict Debug and Release editors (CPU only) and read from the export
                                     stage's current Release and Dist reports (never rerun), against its committed
                                     FinalStateHash and across the four runs (§15.1 T3d; ADR 0024 decision 10); in the
                                     editor runs every replay with input events must fail input.replay {verify,
                                     strictHash, withoutInput} (the input check, §13.6); a missing or stale exported
                                     report fails it, or with --gpu-optional leaves the exported runs out with a
                                     warning
  games        Debug, Release, Dist  Scripts/Test.py --suite games --config Debug,Release,Dist --junit --require-gpu:
                                     every staged demo game below Projects/ and Tests/Projects/ (§15.1 T5; ADR 0026
                                     decisions 7 and 8): its provenance audit of the staged files, the editor halves
                                     (the build-settings check first: a set StartScene that a non-empty
                                     Export.BuildScenes lists, ADR 0027 decision 4, B1; then script.check,
                                     project.validate, the canonical re-save dry run and test.run with every replay at
                                     its committed hash, checkpoints compared, in strict Debug and Release editors on
                                     the staged files; at least one committed replay with input events, and every one
                                     must fail input.replay {verify, strictHash, withoutInput}, the input check of
                                     §13.6), and the exported half (a testing Dist export from the Release editor run
                                     with --feature-test, its hashes identical to the editor halves', and a plain Dist
                                     export's smoke test); it passes with a note while no game exists; --gpu-optional
                                     lets the editor and Dist halves pass without running, as warnings, on a machine
                                     without a usable Vulkan device
  portability  n/a                   premake --os=linux gmake, --os=linux ninja, --os=macosx xcode4 (and the vs2026
                                     reference) into bin-int/Portability/, each checked against the expected file list
                                     and project/configuration set; the xcode4 projects' precompiled headers
                                     (GCC_PREFIX_HEADER, resolved like Xcode does) and the gmake ones must exist, and
                                     the first-party xcode4 projects must enable LTO (LLVM_LTO) in Dist; the
                                     Runtime's link settings of every target and configuration
                                     (Scripts/Lib/portability.py: the Linux $ORIGIN RPATH, the macOS Frameworks rpath,
                                     deployment target and tool product, the map file everywhere, the Windows
                                     subsystems; ADR 0024 decision 18); then clang-cl
                                     builds of Tests in Release (Engine, EditorCore and the vendored libraries included)
                                     and of every Dist project (asserts compiled out), warnings tolerated only in
                                     vendored code, when Visual Studio's C++ Clang component
                                     is installed, otherwise reported as skipped (a failure with --require-clang-cl)
  hardening    Release               hardened milestone gates only (CI.py --milestone, or named in --stages):
                                     Scripts/Hardening.py --steps perf,soak,asan-generate,asan-build,asan-unit,
                                     asan-feature,fuzz, one step each, failing fast: the perf capture (warning-only),
                                     the soak of every scene of every project under Projects/ (FeatureTest and the
                                     demos there, not Tests/Projects/; a new script error during a scene's soak fails
                                     it) and of the exported Release Runtime, the
                                     AddressSanitizer workspace (bin-int/Asan) with its unit and feature suites, and the
                                     1,000,000-iteration fuzz of every reader of Tests/Data/Fuzz/Targets.json (§15.1
                                     T6; ADR 0024 decisions 12 to 16 and 19). A plain CI.py lists it as not run,
                                     naming ADR 0028 decision 12's rule (MILESTONE_ONLY_NOTE)

--milestone (Architecture §15.9; ADR 0024 decision 19) runs every available stage plus hardening, with
--require-clang-cl. Which milestone end runs it is ADR 0028 decision 12's rule (Roadmap rule 1): within a phase, only
the last lander's milestone-end CI.py adds --milestone, on the merged tree of all the phase's milestones, when any
milestone of the phase touches core systems, file readers (fuzzed formats) or the renderer; every other milestone end
runs CI.py --require-clang-cl. It is the whole matrix on a machine with a device in strict mode, so combining it with
--stages, --skip-stages, --configs, --toolset, --no-clang-cl, --gpu-optional or --contract is a usage error (exit code
2); --json and --summary-junit combine freely.

Modes (Roadmap rule 3, Docs/Decisions/0004-contract-stub-gate.md): strict by default, so a milestone cannot end with a
contract stub (ENGINE_CONTRACT_STUB) or a skipped test case outside the child-process targets. Contract mode is a
contract checkpoint's targeted runs on a milestone branch, Lint.py --allow-contract-stubs and Test.py --allow-skips
(ADR 0028 decision 11). --contract passes --allow-contract-stubs to Lint.py (lint) and --allow-skips to Test.py (unit,
gpu, golden, feature, automation, export, determinism, games), like PreCommit.py --contract (Scripts/Lib/scripts.py);
both stay available, but no gate run of the backlog program uses them. The run prints which mode it is in at the start
and in the summary.

Machine-wide locks (Scripts/Lib/locks.py; Docs/Architecture.md §2.3, ADR 0028 decision 16): before its first stage,
CI.py takes the exclusive gate lock, then the GPU lock, then the build lock uncapped, all shared by every checkout of
the repository, and holds them for its whole run. From the moment it holds the gate, no new Build.py, Test.py, GPU run
or MCP bridge editor launch starts in any checkout, unless it runs inside a run that already held a machine lock when
the gate began (the children of a Hardening.py run with a GPU step, further launches of a bridge that holds the GPU
lock): the gate waits for that run instead. It then waits for the runs that already hold the GPU or build lock to end.
Its child processes inherit its locks through ENGINE_HELD_LOCKS, so its own Build.py and Test.py runs neither wait for
them nor take another build slot. Every wait reports the holder (pid, worktree, command) and lasts at most
--lock-timeout seconds (exit code 5); --no-wait fails at once instead (exit code 3). Only one gate runs at a time.

Results: each stage's own output, a summary table, bin/TestResults/CI.xml (JUnit, one test case per step; another
path with --summary-junit, so separate runs of one CI job keep separate summaries), the unit, gpu, golden, feature
(with the editor's JSON report beside it), automation, export (with the Runtime's FeatureTest-<Config> reports),
determinism, games (Games-<Config>.xml, Games-<Name>-Editor-<Config>.json and Games-<Name>-Dist.{json,xml}) and
hardening (Soak, Perf, Unit/Feature/Fuzz-Release-Asan) files in bin/TestResults/, and the actual,
expected and diff images of failed golden comparisons in bin/TestResults/Golden/.

Exit codes (§4.1): 0 every selected stage passed, otherwise the code of the first failing step: 1 failed,
2 usage error, 3 a required tool or file is missing, or --no-wait found a machine lock held, 5 timeout (of a step, or
of the wait for a machine lock).
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"CI.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import contextlib
import dataclasses
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from collections.abc import Iterator
from typing import Any, Callable

from Lib import locks, paths, portability, premake, toolchain, workspace
from Lib.process import ToolNotFoundError, child_environment, format_command, run_streamed, terminate_tree
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
from Lib.scripts import CONTRACT_FLAG_HELP, mode_note, run_script, run_static_checks, test_mode_arguments

SHADER_MANIFEST = paths.REPOSITORY_ROOT / "Resources" / "Shaders" / "Shaders.json"
# The Roadmap M0 acceptance name of the shader up-to-date check, used verbatim as its step name (ReviewChecklist §8).
SHADER_ACCEPTANCE_NAME = "Shaders: slang-only change is not skipped by the up-to-date check"
# The Roadmap M15 acceptance name of the Dist map check, used verbatim as its step name (ADR 0024 decision 11).
RUNTIME_MAP_ACCEPTANCE_NAME = "Dist: map file contains no automation, Analysis, ImGui or AssetPipeline symbols"
# The hardening stage's steps, in Scripts/Hardening.py's order (ADR 0024 decision 19).
HARDENING_STEPS = ("perf", "soak", "asan-generate", "asan-build", "asan-unit", "asan-feature", "fuzz")
# Why a run without --milestone lists the hardening stage as not run: ADR 0028 decision 12's rule.
MILESTONE_ONLY_NOTE = ("hardened milestone gates only: CI.py --milestone at a phase's last lander when the phase "
                       "touches core systems, file readers or the renderer (ADR 0028 decision 12); every other "
                       "milestone end runs CI.py --require-clang-cl")
PORTABILITY_ROOT = paths.BIN_INT_ROOT / "Portability"
SUMMARY_JUNIT = paths.TEST_RESULTS_ROOT / "CI.xml"

# Generation targets of the portability stage. vs2026 is generated as well: its solution is the reference for the
# project set and the per-configuration projects every other generator must reproduce.
PORTABILITY_TARGETS = (("windows", "vs2026"), ("linux", "gmake"), ("linux", "ninja"), ("macosx", "xcode4"))

# xcode4 setting that holds the precompiled header, and the projects whose Dist configuration must enable LTO there
# (premake's xcode4 generator ignores linktimeoptimization; Dependencies.lua sets LLVM_LTO instead).
XCODE_PREFIX_HEADER_PATTERN = re.compile(r"^\s*GCC_PREFIX_HEADER = (\"(?:[^\"\\]|\\.)*\"|[^;]+);", re.MULTILINE)
XCODE_DIST_LTO_PROJECTS = ("Engine", "Runtime")
MAKE_PCH_PATTERN = re.compile(r"^\s*PCH = (\S+)\s*$", re.MULTILINE)
# The summary line of Editor --bake-engine-assets (Editor/EditorApp.cpp), the bake step's detail.
BAKE_SUMMARY_PATTERN = re.compile(r"Engine assets: \d+ baked, \d+ up to date, \d+ not baked")
ENGINE_CACHE = paths.REPOSITORY_ROOT / "bin" / "EngineCache"

TIMEOUTS = {
    "setup": 1800.0,
    "generate": 600.0,
    "build": 7200.0,
    "bake": 1800.0,
    "unit": 1800.0,
    "gpu": 3600.0,
    "golden": 1800.0,
    "feature": 3600.0,
    "automation": 3600.0,
    "export": 3600.0,
    "determinism": 3600.0,
    "games": 3600.0,
    "portability": 600.0,
    "clang-cl": 7200.0,
    "runtime-map": 600.0,
    # Scripts/Hardening.py bounds each of its steps itself (its TIMEOUTS: perf 1200 s, soak 3600 s, asan generate
    # 3600 s, asan build 7200 s, asan unit 3600 s, asan feature 3600 s, 22,800 s in all, plus 60 s of grace for each
    # sanitized suite, and the fuzz step's timeout, which follows its measured per-target costs within a budget of
    # 5,400 s per 35 targets, Lib/hardening.py fuzz_budget_seconds: 5,400 s for today's 35); this bounds the whole run
    # above their sum (Hardening.py longest_run_seconds), which holds up to 49 fuzz targets (ADR 0024 "Integration",
    # budgets and timeouts; Tests/Automation/test_hardening.py checks it for the committed fuzz targets and the 49).
    "hardening": 30600.0,
}


@dataclasses.dataclass(frozen=True)
class Stage:
    name: str
    configurations: tuple[str, ...] | None  # the §15.8 matrix; None for configuration-independent stages
    available: bool
    planned: str = ""
    # §15.8 runs it at the hardened milestone gates only (ADR 0028 decision 12): CI.py --milestone, or --stages naming
    # it; "all" lists it as not run.
    milestone_only: bool = False


# §15.8 order.
STAGES = (
    Stage("setup", None, True),
    Stage("generate", None, True),
    Stage("lint", None, True),
    Stage("build", ("Debug", "Release", "Dist"), True),
    Stage("bake", ("Release",), True),
    Stage("unit", ("Debug", "Release"), True),
    Stage("gpu", ("Debug", "Release"), True),
    Stage("golden", ("Release",), True),
    Stage("feature", ("Debug", "Release"), True),
    Stage("automation", ("Release",), True),
    Stage("export", ("Release", "Dist"), True),
    Stage("determinism", ("Debug", "Release", "Dist"), True),
    Stage("games", ("Debug", "Release", "Dist"), True),
    Stage("portability", None, True),
    Stage("hardening", ("Release",), True, milestone_only=True),
)
STAGE_BY_NAME = {stage.name: stage for stage in STAGES}


class Runner:
    def __init__(self, arguments: argparse.Namespace, console: Console) -> None:
        self.arguments = arguments
        self.console = console
        self.printed: list[Step] = []  # holds the steps, so identity checks stay valid

    def done(self, step: Step) -> Step:
        """Print a finished step's result line once, as soon as it is known."""
        if not any(printed is step for printed in self.printed):
            self.printed.append(step)
            self.console.result(step)
        return step

    def script(self, name: str, script: str, script_arguments: list[str], timeout: float,
               expected_exit: int = 0, expected_output: re.Pattern[str] | None = None) -> Step:
        return self.done(run_script(name, script, script_arguments, self.console, timeout, expected_exit,
                                    expected_output))

    # ----------------------------------------------------------------------------------------------------------------

    def setup(self) -> list[Step]:
        return [self.script("setup", "Setup.py", [], TIMEOUTS["setup"])]

    def generate(self) -> list[Step]:
        arguments = ["--toolset", self.arguments.toolset] if self.arguments.toolset else []
        return [self.script("generate", "Generate.py", arguments, TIMEOUTS["generate"])]

    def lint(self) -> list[Step]:
        """The static checks of §15.8, shared with PreCommit.py. Each runs even after an earlier one failed."""
        return run_static_checks(self.console, self.done, self.arguments.contract)

    def build(self, configs: list[str]) -> list[Step]:
        steps: list[Step] = []
        for config in configs:
            steps.append(self.script(f"build {config}", "Build.py", ["--config", config], TIMEOUTS["build"]))
            if steps[-1].failed:
                return steps
            if config != "Dist":
                steps.append(self.script(f"references {config}", "GenerateDocs.py", ["--check", "--config", config],
                                         180.0))
                if steps[-1].failed:
                    return steps
            if config == "Debug":
                steps.append(self.done(self.shader_up_to_date_check(config)))
                if steps[-1].failed:
                    return steps
            if config == "Dist":
                # Roadmap M15: the shipping Runtime links no automation, Luau Analysis, ImGui or AssetPipeline code and
                # keeps the FeatureTest harness (§14.4; PreCommit builds no Dist, so the check lives here).
                steps.append(self.script(RUNTIME_MAP_ACCEPTANCE_NAME, "CheckRuntimeMap.py", ["--config", "Dist"],
                                         TIMEOUTS["runtime-map"]))
                if steps[-1].failed:
                    return steps
        return steps

    def shader_up_to_date_check(self, config: str) -> Step:
        """Roadmap M0: "Shaders: slang-only change is not skipped by the up-to-date check", scripted with Build.py.

        The stamp is rewritten by every complete CompileShaders.py run, so its modification time shows whether the
        build ran the shader rule.
        """
        name = f"{SHADER_ACCEPTANCE_NAME} ({config})"
        started = time.monotonic()
        stamp = paths.output_directory(config) / "Shaders" / ".stamp"
        try:
            manifest = json.loads(SHADER_MANIFEST.read_text(encoding="utf-8"))
            shader = SHADER_MANIFEST.parent / manifest["Programs"][0]["File"]
        except (OSError, ValueError, KeyError, IndexError, TypeError) as error:
            return Step(name, Status.FAILED,
                        f"cannot pick a shader from {paths.display_path(SHADER_MANIFEST)}: {error}")
        if not stamp.is_file() or not shader.is_file():
            return Step(name, Status.FAILED, f"{paths.display_path(stamp)} or {paths.display_path(shader)} is missing "
                                             f"after the {config} build")

        def build_once(label: str) -> Step:
            return self.script(f"{name}: {label}", "Build.py", ["--config", config], TIMEOUTS["build"])

        before = stamp.stat().st_mtime_ns
        deadline = time.monotonic() + 5.0
        while time.time_ns() <= before and time.monotonic() < deadline:
            time.sleep(0.05)  # the new timestamp must be later than the stamp's
        os.utime(shader)  # content unchanged; only the modification time moves
        self.console.print(f"touched {paths.display_path(shader)}")
        rebuilt = build_once(f"after touching {shader.name}")
        if rebuilt.failed:
            return Step(name, Status.FAILED, rebuilt.detail, time.monotonic() - started)
        after_touch = stamp.stat().st_mtime_ns
        if after_touch == before:
            return Step(name, Status.FAILED, f"touching {shader.name} did not re-run the shader rule (stamp unchanged)",
                        time.monotonic() - started)
        action = next((a for a in premake.ACTIONS_BY_SYSTEM[paths.host().system]
                       if workspace.workspace_file(a).exists()), None)
        if action == "xcode4":
            return Step(name, Status.PASSED, f"touching {shader.name} re-ran the shader rule (xcode4 runs the script "
                                             f"phase on every build, so no up-to-date half)",
                        time.monotonic() - started)
        again = build_once("no change")
        if again.failed:
            return Step(name, Status.FAILED, again.detail, time.monotonic() - started)
        if stamp.stat().st_mtime_ns != after_touch:
            return Step(name, Status.FAILED, "an unchanged build re-ran the shader rule (stamp rewritten)",
                        time.monotonic() - started)
        return Step(name, Status.PASSED, f"touching {shader.name} re-ran the shader rule; the next build skipped it",
                    time.monotonic() - started)

    def bake(self, configs: list[str]) -> list[Step]:
        steps: list[Step] = []
        for config in configs:
            steps.append(self.done(self.bake_engine_assets(config)))
            if steps[-1].failed:
                return steps
        return steps

    def bake_engine_assets(self, config: str) -> Step:
        """§15.8 bake: the editor of `config` fills bin/EngineCache (shared by every configuration) and exits 0, or 1
        when an entry failed to bake. The built-in environments are GPU bakes (M8, Docs/Decisions/0013-m8-decisions.md
        decision 9), so it runs a rendering editor; a machine without a usable Vulkan device fails here (the editor
        exits 3) unless --gpu-optional is given, which reruns it with --renderer none: the environments are then
        skipped with a warning and the step ends as a warning."""
        name = f"bake {config}"
        editor = paths.output_directory(config) / "Editor" / paths.executable_name("Editor")
        if not editor.is_file():
            return Step(name, Status.FAILED, f"{paths.display_path(editor)} not found: run python Scripts/Build.py "
                                             f"--config {config} --project Editor", exit_code=EXIT_INIT_FAILED)
        renderer = "vulkan"
        while True:
            with tempfile.TemporaryDirectory(prefix="CI-Bake-") as user_data:
                command = [str(editor), "--headless", "--renderer", renderer, "--bake-engine-assets",
                           f"--user-data-dir={user_data}"]
                self.console.heading(f"{name}: {format_command(command)}")
                try:
                    result = run_streamed(command, cwd=paths.REPOSITORY_ROOT, env=child_environment(),
                                          timeout=TIMEOUTS["bake"], echo=self.console.stream,
                                          collect=BAKE_SUMMARY_PATTERN)
                except ToolNotFoundError as error:
                    return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)
            if renderer == "vulkan" and result.exit_code == EXIT_INIT_FAILED and self.arguments.gpu_optional:
                self.console.heading(f"{name}: no usable Vulkan device (--gpu-optional): baking without the "
                                     "environments")
                renderer = "none"
                continue
            break
        if result.timed_out:
            return Step(name, Status.TIMEOUT, f"Editor --bake-engine-assets {result.describe_exit()}", result.duration)
        if not result.succeeded:
            hint = " (no usable Vulkan device? --gpu-optional bakes without the environments)" \
                if result.exit_code == EXIT_INIT_FAILED else ""
            return Step(name, Status.FAILED,
                        f"Editor --bake-engine-assets {result.describe_exit()}{hint}: {result.tail(1)}",
                        result.duration, data={"outputTail": result.tail(40)})
        match = BAKE_SUMMARY_PATTERN.search(result.collected[-1]) if result.collected else None
        summary = match.group(0) if match else "no summary line"
        if renderer == "none":
            return Step(name, Status.WARNING,
                        f"{paths.display_path(ENGINE_CACHE)}: {summary}; no usable Vulkan device, so the built-in "
                        "environments were not baked", result.duration)
        return Step(name, Status.PASSED, f"{paths.display_path(ENGINE_CACHE)}: {summary}", result.duration)

    def unit(self, configs: list[str]) -> list[Step]:
        mode = test_mode_arguments(self.arguments.contract)
        return [self.script(f"unit {config}", "Test.py", ["--suite", "unit", "--config", config, "--junit", *mode],
                            TIMEOUTS["unit"]) for config in configs]

    def device_suite(self, suite: str, configs: list[str]) -> list[Step]:
        """The gpu, golden, feature and automation stages (§15.8): with --require-gpu, so a test case without a device
        (a GPU test case, the FeatureTest run, or an automation test that starts a rendering editor) fails instead of
        passing without running (§15.2), unless --gpu-optional says this machine may have no usable device. The gpu
        suite runs both API caps per configuration (Test.py)."""
        mode = test_mode_arguments(self.arguments.contract)
        device = [] if self.arguments.gpu_optional else ["--require-gpu"]
        steps: list[Step] = []
        for config in configs:
            steps.append(self.script(f"{suite} {config}", "Test.py",
                                     ["--suite", suite, "--config", config, "--junit", *device, *mode],
                                     TIMEOUTS[suite]))
            if steps[-1].failed:
                return steps
        return steps

    def export(self, configs: list[str]) -> list[Step]:
        """§15.8 export (ADR 0024 decision 9): one Test.py run over the selected configurations, which exports
        FeatureTest for testing, runs each package with --feature-test, evaluates its per-mode gates and, with both
        configurations, merges the Editor, Release and Dist reports. It needs a device (--require-gpu unless
        --gpu-optional), like the feature stage whose Feature-Release.json it merges."""
        mode = test_mode_arguments(self.arguments.contract)
        device = [] if self.arguments.gpu_optional else ["--require-gpu"]
        return [self.script(f"export {', '.join(configs)}", "Test.py",
                            ["--suite", "export", "--config", ",".join(configs), "--junit", *device, *mode],
                            TIMEOUTS["export"])]

    def determinism(self, configs: list[str]) -> list[Step]:
        """§15.8 determinism (ADR 0024 decision 10): one Test.py run over all the selected configurations, which
        compares the editor runs' hashes (CPU only) and the exported runs' hashes (read from the export stage's current
        reports) with the committed ones and with each other. --require-gpu unless --gpu-optional: a missing or stale
        exported report then fails the step instead of leaving those runs out with a warning."""
        mode = test_mode_arguments(self.arguments.contract)
        device = [] if self.arguments.gpu_optional else ["--require-gpu"]
        return [self.script(f"determinism {', '.join(configs)}", "Test.py",
                            ["--suite", "determinism", "--config", ",".join(configs), "--junit", *device, *mode],
                            TIMEOUTS["determinism"])]

    def games(self, configs: list[str]) -> list[Step]:
        """§15.8 games (ADR 0026 decision 8): one Test.py run over the selected configurations, which audits every
        staged demo game, runs its Debug and Release editor halves (so its replays verify in both editor
        configurations, Roadmap rule 1) and its exported Dist half, whose hashes it compares with the editor halves'.
        It needs a device (--require-gpu unless --gpu-optional) and, for the Dist half, the Dist build."""
        mode = test_mode_arguments(self.arguments.contract)
        device = [] if self.arguments.gpu_optional else ["--require-gpu"]
        return [self.script(f"games {', '.join(configs)}", "Test.py",
                            ["--suite", "games", "--config", ",".join(configs), "--junit", *device, *mode],
                            TIMEOUTS["games"])]

    def hardening(self) -> list[Step]:
        """§15.8 hardening (§15.1 T6; ADR 0024 decision 19): Scripts/Hardening.py with every step, run once with
        --json, so each of its steps becomes one step here under its own name. Hardening.py fails fast and reports the
        steps after a failed one as not run; it bounds each step itself, TIMEOUTS["hardening"] the whole run."""
        name = "hardening"
        script = paths.SCRIPTS_ROOT / "Hardening.py"
        arguments = ["--steps", ",".join(HARDENING_STEPS), "--json",
                     *(["--gpu-optional"] if self.arguments.gpu_optional else [])]
        if not script.is_file():
            return [Step(name, Status.FAILED, f"{paths.display_path(script)} does not exist (Architecture §2.3)",
                         exit_code=EXIT_INIT_FAILED)]
        shown = format_command([Path(sys.executable).name, paths.display_path(script), *arguments])
        self.console.heading(f"{name}: {shown}")
        started = time.monotonic()
        try:
            # Its progress goes to its standard error (inherited, so it streams); its standard output is the JSON
            # result.
            child = subprocess.Popen([sys.executable, str(script), *arguments], cwd=paths.REPOSITORY_ROOT,
                                     stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=None,
                                     env=child_environment())
        except OSError as error:
            return [Step(name, Status.FAILED, f"cannot start {paths.display_path(script)}: {error}",
                         exit_code=EXIT_INIT_FAILED)]
        try:
            output, _ = child.communicate(timeout=TIMEOUTS["hardening"])
        except subprocess.TimeoutExpired:
            terminate_tree(child)
            child.communicate()
            return [Step(name, Status.TIMEOUT, f"Hardening.py ran longer than {TIMEOUTS['hardening']:.0f} s",
                         time.monotonic() - started)]
        except BaseException:
            terminate_tree(child)
            raise
        document = parse_json_document(output.decode("utf-8", errors="replace"))
        reported = [entry for entry in document.get("steps", []) if isinstance(entry, dict)]
        if not reported:
            return [Step(name, Status.FAILED, f"Hardening.py exited with code {child.returncode} without a JSON result",
                         time.monotonic() - started, exit_code=child.returncode or EXIT_FAILED)]
        steps: list[Step] = []
        for entry in reported:
            try:
                status = Status(entry.get("status", "failed"))
            except ValueError:
                status = Status.FAILED
            exit_code = entry.get("exitCode")
            data = {key: value for key, value in entry.items()
                    if key not in ("name", "status", "detail", "durationSeconds", "exitCode")}
            steps.append(Step(str(entry.get("name", name)), status, str(entry.get("detail", "")),
                              float(entry.get("durationSeconds", 0.0) or 0.0),
                              exit_code=exit_code if isinstance(exit_code, int) else None, data=data))
        if child.returncode != 0 and not any(step.failed for step in steps):
            steps.append(Step(name, Status.FAILED, f"Hardening.py exited with code {child.returncode}",
                              time.monotonic() - started, exit_code=child.returncode))
        return steps

    # ----------------------------------------------------------------------------------------------------------------

    def portability(self) -> list[Step]:
        steps: list[Step] = []
        generated: dict[tuple[str, str], tuple[Path, list[str]]] = {}
        for system, action in PORTABILITY_TARGETS:
            step, location, files = self.generate_portability(system, action)
            steps.append(self.done(step))
            if step.failed:
                return steps
            generated[(system, action)] = (location, files)
        steps += self.check_portability(generated)
        if steps[-1].failed:
            return steps
        steps.append(self.done(self.check_generated_settings(generated)))
        if steps[-1].failed:
            return steps
        steps.append(self.done(self.check_link_settings(generated)))
        if steps[-1].failed:
            return steps
        steps.append(self.done(self.clang_cl_build()))
        return steps

    def generate_portability(self, system: str, action: str) -> tuple[Step, Path, list[str]]:
        name = f"portability generate {system} {action}"
        location = PORTABILITY_ROOT / f"{system}-{action}"
        if location.exists():
            shutil.rmtree(location)
        location.mkdir(parents=True)
        arguments = ["--fatal", f"--os={system}", f"--to={location}", action]
        self.console.heading(f"{name}: premake5 {' '.join(arguments)}")
        result = premake.run(arguments, timeout=TIMEOUTS["portability"])
        files = sorted(Path(file).resolve().relative_to(location.resolve()).as_posix()
                       for file in premake.generated_files(result.output))
        if result.timed_out:
            return Step(name, Status.TIMEOUT, f"premake {result.describe_exit()}", result.duration), location, files
        if not result.succeeded:
            self.console.print(result.output.rstrip())
            return (Step(name, Status.FAILED, f"premake {result.describe_exit()}: {result.tail(1)}", result.duration,
                         data={"outputTail": result.tail(40)}), location, files)
        return (Step(name, Status.PASSED, f"{len(files)} files in {paths.display_path(location)}", result.duration,
                     data={"files": files}), location, files)

    def check_portability(self, generated: dict[tuple[str, str], tuple[Path, list[str]]]) -> list[Step]:
        """Compare every generated workspace with the vs2026 reference: file list, projects and configurations."""
        steps: list[Step] = []
        reference_location, _ = generated[("windows", "vs2026")]
        solution_name = workspace.workspace_file_name("vs2026")
        reference = workspace.read_solution(reference_location / solution_name)
        utility = reference.utility_projects
        expected_files = {
            ("windows", "vs2026"): {solution_name} | {f"{p}.vcxproj" for p in reference.projects},
            ("linux", "gmake"): {"Makefile"} | {f"{p}.make" for p in reference.projects},
            # premake's ninja generator emits no Utility projects (Engine/premake5.lua attaches the shader rule to
            # Engine instead).
            ("linux", "ninja"): {"build.ninja"} | {f"{p}.ninja" for p in reference.projects if p not in utility},
            ("macosx", "xcode4"): {f"{workspace.workspace_file_name('xcode4')}/contents.xcworkspacedata"}
            | {f"{p}.xcodeproj/project.pbxproj" for p in reference.projects},
        }
        readers: dict[str, Callable[[Path], workspace.Workspace]] = {
            "vs2026": workspace.read_solution,
            "gmake": workspace.read_makefiles,
            "ninja": workspace.read_ninja,
            "xcode4": workspace.read_xcode_workspace,
        }
        for (system, action), (location, files) in generated.items():
            name = f"portability check {system} {action}"
            problems: list[str] = []
            present = set(files)
            expected = expected_files[(system, action)]
            if action == "vs2026":
                present = {file for file in present if not file.endswith(".vcxproj.filters")}
            if present - expected:
                problems.append(f"unexpected files: {', '.join(sorted(present - expected))}")
            if expected - present:
                problems.append(f"missing files: {', '.join(sorted(expected - present))}")
            try:
                parsed = readers[action](location / workspace.workspace_file_name(action))
                for config in paths.CONFIGURATIONS:
                    wanted = {p for p in reference.projects_in(config) if action != "ninja" or p not in utility}
                    actual = set(parsed.projects_in(config))
                    if actual != wanted:
                        difference = sorted(actual.symmetric_difference(wanted))
                        problems.append(f"{config} projects differ from the vs2026 reference: {', '.join(difference)}")
            except workspace.WorkspaceError as error:
                problems.append(str(error))
            if problems:
                steps.append(self.done(Step(name, Status.FAILED, "; ".join(problems))))
                return steps
            projects = len(reference.projects) - (len(utility) if action == "ninja" else 0)
            steps.append(self.done(Step(name, Status.PASSED, f"{len(expected)} expected files, {projects} projects; "
                                                             f"Debug/Release/Dist project sets match the reference")))
        return steps

    @staticmethod
    def check_generated_settings(generated: dict[tuple[str, str], tuple[Path, list[str]]]) -> Step:
        """Settings the file-list comparison cannot see. Xcode resolves a relative GCC_PREFIX_HEADER against the
        directory that holds the .xcodeproj (SRCROOT), never through the include path; gmake's PCH path is relative to
        the makefile. Both must name an existing header. The first-party xcode4 projects must turn on LTO in Dist."""
        name = "portability check precompiled headers and Dist LTO"
        problems: list[str] = []
        checked = 0
        xcode_location, xcode_files = generated[("macosx", "xcode4")]
        for file in xcode_files:
            if not file.endswith(".xcodeproj/project.pbxproj"):
                continue
            pbxproj = xcode_location / file
            project = Path(file).parent.stem
            text = pbxproj.read_text(encoding="utf-8")
            for match in XCODE_PREFIX_HEADER_PATTERN.finditer(text):
                value = match.group(1).strip().strip('"')
                header = Path(value) if Path(value).is_absolute() else pbxproj.parent.parent / value
                checked += 1
                if not header.is_file():
                    problems.append(f"{project}.xcodeproj: GCC_PREFIX_HEADER {value} does not exist (resolved as "
                                    f"{header.as_posix()})")
            if project in XCODE_DIST_LTO_PROJECTS and not re.search(
                    r"LLVM_LTO = (?:\(\s*)?\"?YES\"?", text):
                problems.append(f"{project}.xcodeproj: Dist does not set LLVM_LTO = YES (link-time optimization)")
        make_location, make_files = generated[("linux", "gmake")]
        for file in make_files:
            if not file.endswith(".make"):
                continue
            makefile = make_location / file
            for match in MAKE_PCH_PATTERN.finditer(makefile.read_text(encoding="utf-8")):
                header = Path(match.group(1))
                header = header if header.is_absolute() else makefile.parent / header
                checked += 1
                if not header.is_file():
                    problems.append(f"{file}: PCH {match.group(1)} does not exist")
        if problems:
            return Step(name, Status.FAILED, "; ".join(problems))
        return Step(name, Status.PASSED, f"{checked} precompiled-header settings resolve to existing headers; "
                                         f"{', '.join(XCODE_DIST_LTO_PROJECTS)} enable LTO in Dist (xcode4)")

    @staticmethod
    def check_link_settings(generated: dict[tuple[str, str], tuple[Path, list[str]]]) -> Step:
        """The Runtime's generated link settings of every target and configuration (ADR 0024 decision 18,
        portability.check_runtime_link_settings): the Linux $ORIGIN RPATH and the map file (gmake, ninja), the macOS
        Frameworks rpath, map, deployment target and tool product type (xcode4), /MAP and the subsystems (vs2026)."""
        name = "portability check Runtime link settings"
        problems = portability.check_runtime_link_settings(
            {target: location for target, (location, _) in generated.items()})
        if problems:
            return Step(name, Status.FAILED, "; ".join(problems))
        targets = ", ".join(f"{system} {action}" for system, action in sorted(portability.TARGETS))
        return Step(name, Status.PASSED, f"{targets}: every configuration's Runtime link settings as required")

    def clang_cl_build(self) -> Step:
        name = "portability clang-cl build (Release, Dist)"
        if self.arguments.no_clang_cl:
            return Step(name, Status.SKIPPED, "--no-clang-cl given")
        host = paths.host()
        if not host.is_windows:
            return Step(name, Status.SKIPPED, "clang-cl builds run on Windows only")
        try:
            visual_studio = toolchain.find_visual_studio()
        except toolchain.ToolchainError as error:
            return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)
        # The C++ Clang component provides both the compiler and MSBuild's ClangCL platform toolset, which lives under
        # the VCTargets directory of the MSBuild version (MSBuild/Microsoft/VC/v<NNN>/).
        root = visual_studio.installation_path
        clang_cl = root / "VC" / "Tools" / "Llvm" / "x64" / "bin" / "clang-cl.exe"
        vc_targets = root / "MSBuild" / "Microsoft" / "VC"
        platform_toolsets = sorted(vc_targets.glob("*/Platforms/x64/PlatformToolsets/ClangCL"))
        if not clang_cl.is_file() or not any(path.is_dir() for path in platform_toolsets):
            reason = ("the Visual Studio C++ Clang component (clang-cl and the ClangCL platform toolset) is not "
                      "installed")
            if self.arguments.require_clang_cl:
                return Step(name, Status.FAILED, f"{reason} (--require-clang-cl)", exit_code=EXIT_INIT_FAILED)
            return Step(name, Status.SKIPPED, reason)
        location = PORTABILITY_ROOT / "windows-vs2026-clang"
        generated = self.script(f"{name}: generate", "Generate.py",
                                ["--toolset", "clang", "--to", str(location), "--no-compile-commands"],
                                TIMEOUTS["generate"])
        if generated.failed:
            return Step(name, Status.FAILED, generated.detail, generated.duration)
        # Vendor sources keep their own warning settings, and clang-cl reports more for them than MSVC; first-party
        # code is -Werror under clang-cl too. Any other warning (linker, MSBuild) fails the build.
        built = self.script(name, "Build.py", ["--workspace-dir", str(location), "--config", "Release",
                                               "--project", "Tests", "--allow-vendor-warnings"], TIMEOUTS["clang-cl"])
        built.duration += generated.duration
        if built.failed:
            return built
        # Dist as well: asserts compile out there, so Clang's -Wunneeded-internal-declaration and -Wunused-variable
        # catch helpers and values only asserts use, which MSVC never reports (the Linux and macOS jobs would).
        dist = self.script(f"{name}: Dist", "Build.py", ["--workspace-dir", str(location), "--config", "Dist",
                                                          "--allow-vendor-warnings"], TIMEOUTS["clang-cl"])
        if dist.failed:
            return Step(name, Status.FAILED, dist.detail, built.duration + dist.duration, exit_code=dist.exit_code)
        return Step(name, Status.PASSED, f"{built.detail}; Dist: {dist.detail}", built.duration + dist.duration)


def parse_json_document(text: str) -> dict[str, Any]:
    """The first JSON object a child script printed with --json on its standard output (other lines before or after it,
    such as workflow annotations, are ignored), or {} when there is none."""
    decoder = json.JSONDecoder()
    start = text.find("{")
    while start != -1:
        try:
            document, _ = decoder.raw_decode(text, start)
            if isinstance(document, dict):
                return document
        except json.JSONDecodeError:
            pass
        start = text.find("{", start + 1)
    return {}


# The options --milestone refuses (ADR 0024 decision 19): a milestone gate is the whole matrix, with a device, in strict
# mode.
MILESTONE_REFUSED_OPTIONS = ("--stages", "--skip-stages", "--configs", "--toolset", "--no-clang-cl", "--gpu-optional",
                             "--contract")


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the CI stages (Architecture §15.8) in order, stopping at the first failing stage.",
        epilog="Exit codes: 0 success, else the first failing step's code: 1 failed, 2 usage, 3 missing tool or file, "
               "or --no-wait found a machine lock held, 5 timeout.",
    )
    available = [stage.name for stage in STAGES if stage.available and not stage.milestone_only]
    milestone_only = [stage.name for stage in STAGES if stage.available and stage.milestone_only]
    unavailable = [stage.name for stage in STAGES if not stage.available]
    later = f"; later stages ({', '.join(unavailable)}) are reported as not available" if unavailable else ""
    parser.add_argument("--stages", default=None,
                        help=f"comma-separated stages (default: all stages that exist: {', '.join(available)}, with "
                             f"{', '.join(milestone_only)} listed as not run: {MILESTONE_ONLY_NOTE}); naming "
                             f"{', '.join(milestone_only)} runs it{later}")
    parser.add_argument("--skip-stages", default="", help="comma-separated stages to leave out")
    parser.add_argument("--configs", default=None,
                        help="restrict the configuration matrix (default: Debug,Release,Dist)")
    parser.add_argument("--milestone", action="store_true",
                        help="the hardened milestone-end gate (Roadmap rule 1, §15.9): every available stage plus "
                             "hardening, with --require-clang-cl. Within a phase only the last lander's milestone end "
                             "runs it, on the merged tree of the phase, when any milestone of the phase touches core "
                             "systems, file readers (fuzzed formats) or the renderer; every other milestone end runs "
                             "CI.py --require-clang-cl (ADR 0028 decision 12). Refused together with --stages, "
                             "--skip-stages, --configs, --toolset, --no-clang-cl, --gpu-optional and --contract")
    parser.add_argument("--toolset", choices=("msc", "gcc", "clang"),
                        help="compiler toolset passed to Generate.py (Linux: gcc or clang)")
    parser.add_argument("--no-clang-cl", action="store_true",
                        help="portability: leave out the clang-cl builds (Release Tests and Dist; Windows; by default "
                             "they run when Visual Studio's C++ Clang component is installed)")
    parser.add_argument("--require-clang-cl", action="store_true",
                        help="portability: fail (exit code 3) instead of skipping the clang-cl build when Visual "
                             "Studio's C++ Clang component is missing (CI runners that must have it)")
    parser.add_argument("--gpu-optional", action="store_true",
                        help="gpu, golden, feature, automation, export and games: run without --require-gpu, so on a "
                             "machine without a usable Vulkan device the GPU test cases, the FeatureTest runs, the "
                             "rendering automation tests and the games' editor and Dist halves pass without running, "
                             "naming the reason, and the steps end as warnings (the GitHub-hosted Windows and macOS "
                             "runners, which have no GPU), and a game's Dist smoke test that did not run only warns; "
                             "determinism: a missing or stale exported report leaves the exported runs out with a "
                             "warning; bake: "
                             "without a usable device, bake without the environments (--renderer none) and end as a "
                             "warning; by default a missing device fails them")
    parser.add_argument("--contract", action="store_true", help=CONTRACT_FLAG_HELP)
    parser.add_argument("--summary-junit", type=Path, default=SUMMARY_JUNIT,
                        help=f"JUnit summary of this run (default: {paths.display_path(SUMMARY_JUNIT)})")
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    locks.add_wait_arguments(parser)
    arguments = parser.parse_args(argv)
    locks.check_wait_arguments(parser, arguments)
    names = tuple(stage.name for stage in STAGES)
    if arguments.milestone:
        given = {
            "--stages": arguments.stages is not None,
            "--skip-stages": bool(arguments.skip_stages.strip()),
            "--configs": arguments.configs is not None,
            "--toolset": arguments.toolset is not None,
            "--no-clang-cl": arguments.no_clang_cl,
            "--gpu-optional": arguments.gpu_optional,
            "--contract": arguments.contract,
        }
        refused = [option for option in MILESTONE_REFUSED_OPTIONS if given[option]]
        if refused:
            parser.error(f"--milestone is the whole matrix on a machine with a device in strict mode, so it cannot be "
                         f"combined with {', '.join(refused)}")
        arguments.require_clang_cl = True
    stages_text = "all" if arguments.stages is None or arguments.milestone else arguments.stages
    try:
        if stages_text.strip().lower() == "all":
            # "all" lists the milestone-only stages too, which run_stage reports as not run without --milestone.
            selected = available + milestone_only
            arguments.explicit_stages = set(milestone_only) if arguments.milestone else set()
        else:
            selected = paths.parse_name_list(stages_text, names, "stage")
            arguments.explicit_stages = set(selected)
        skipped = paths.parse_name_list(arguments.skip_stages, names, "stage") if arguments.skip_stages.strip() else []
        arguments.configs = paths.parse_configurations(arguments.configs or ",".join(paths.CONFIGURATIONS))
    except ValueError as error:
        parser.error(str(error))
    arguments.selected = [name for name in names if name in selected and name not in skipped]
    if not arguments.selected:
        parser.error("no stage selected")
    if arguments.no_clang_cl and arguments.require_clang_cl:
        parser.error("--no-clang-cl and --require-clang-cl are mutually exclusive")
    arguments.summary_junit = arguments.summary_junit.resolve()
    return arguments


def run_stage(runner: Runner, stage: Stage, configs: list[str]) -> list[Step]:
    if not stage.available:
        return [Step(stage.name, Status.NOT_AVAILABLE, f"the {stage.name} stage does not exist yet; planned for "
                                                       f"{stage.planned}")]
    if stage.milestone_only and stage.name not in runner.arguments.explicit_stages:
        return [Step(stage.name, Status.NOT_RUN, MILESTONE_ONLY_NOTE)]
    stage_configs = [config for config in (stage.configurations or ()) if config in configs]
    if stage.configurations is not None and not stage_configs:
        return [Step(stage.name, Status.SKIPPED, f"none of its configurations ({', '.join(stage.configurations)}) "
                                                 f"is selected by --configs")]
    handlers: dict[str, Callable[[], list[Step]]] = {
        "setup": runner.setup,
        "generate": runner.generate,
        "lint": runner.lint,
        "build": lambda: runner.build(stage_configs),
        "bake": lambda: runner.bake(stage_configs),
        "unit": lambda: runner.unit(stage_configs),
        "gpu": lambda: runner.device_suite("gpu", stage_configs),
        "golden": lambda: runner.device_suite("golden", stage_configs),
        "feature": lambda: runner.device_suite("feature", stage_configs),
        "automation": lambda: runner.device_suite("automation", stage_configs),
        "export": lambda: runner.export(stage_configs),
        "determinism": lambda: runner.determinism(stage_configs),
        "games": lambda: runner.games(stage_configs),
        "portability": runner.portability,
        "hardening": runner.hardening,
    }
    return handlers[stage.name]()


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    console = Console(arguments.json)
    try:
        host = paths.host()
    except paths.UnsupportedHostError as error:
        console.print(f"CI.py: error: {error}")
        return EXIT_USAGE
    gate = "milestone gate (--milestone); " if arguments.milestone else ""
    console.heading(f"CI on {host.system} ({host.machine}), Python {platform.python_version()}: {gate}stages "
                    f"{', '.join(arguments.selected)}; configurations {', '.join(arguments.configs)}; "
                    f"{mode_note(arguments.contract)}")

    with contextlib.ExitStack() as held:
        try:
            for hold in take_gate_locks(arguments, console):
                held.enter_context(hold)
        except locks.LockUnavailable as error:
            # Nothing ran, so the last gate's CI.xml stays as it was.
            console.result(locks.unavailable_step(error))
            console.print(f"\nCI did not start (exit code {error.exit_code})")
            if arguments.json:
                emit_json({"success": False, "exitCode": error.exit_code, "lock": error.to_json(), "stages": []})
            return error.exit_code
        return run_stages(arguments, console, host)


def take_gate_locks(arguments: argparse.Namespace, console: Console) -> Iterator[locks.Hold]:
    """The gate lock, then the GPU lock, then the build lock uncapped (the locks' order), each yielded as soon as it is
    held so the caller releases what it got when a later one stays unavailable. Raises locks.LockUnavailable."""
    timeout = locks.wait_seconds(arguments)
    yield locks.acquire_gate(timeout, console.print)
    yield locks.acquire_gpu(timeout, console.print)
    yield locks.acquire_build(locks.UNCAPPED, timeout, console.print)


def run_stages(arguments: argparse.Namespace, console: Console, host: paths.Host) -> int:
    """Every selected stage in order, failing fast; the summary, CI.xml and the --json document. Returns the exit
    code."""
    runner = Runner(arguments, console)
    results: list[tuple[str, list[Step]]] = []
    failed_stage: str | None = None
    started = time.monotonic()
    for name in arguments.selected:
        stage = STAGE_BY_NAME[name]
        if failed_stage is not None:
            reason = MILESTONE_ONLY_NOTE if stage.milestone_only and name not in arguments.explicit_stages \
                else f"stage {failed_stage} failed"
            results.append((name, [Step(name, Status.NOT_RUN, reason)]))
            continue
        stage_steps = run_stage(runner, stage, arguments.configs)
        for step in stage_steps:
            runner.done(step)
        results.append((name, stage_steps))
        if any(step.failed for step in stage_steps):
            failed_stage = name

    steps = [step for _, stage_steps in results for step in stage_steps]
    ran = [step for step in steps if step.status not in (Status.NOT_AVAILABLE, Status.NOT_RUN, Status.SKIPPED)]
    exit_code = overall_exit_code(steps)
    if exit_code == EXIT_SUCCESS and not ran:
        console.print("CI.py: nothing ran (every selected stage is unavailable or skipped)")
        exit_code = EXIT_FAILED
    write_junit(arguments.summary_junit, "CI", steps)
    console.summary(f"CI summary ({time.monotonic() - started:.0f} s; JUnit: "
                    f"{paths.display_path(arguments.summary_junit)})", steps)
    console.print(f"  {mode_note(arguments.contract)}")
    verdict = "passed" if exit_code == EXIT_SUCCESS else f"FAILED in stage {failed_stage or 'selection'}"
    console.print(f"\nCI {verdict} (exit code {exit_code})")
    if arguments.json:
        emit_json({
            "success": exit_code == EXIT_SUCCESS,
            "exitCode": exit_code,
            "host": {"system": host.system, "machine": host.machine},
            "configurations": arguments.configs,
            "contract": arguments.contract,
            "milestone": arguments.milestone,
            "stages": [{"stage": name, "steps": [step.to_json() for step in stage_steps]}
                       for name, stage_steps in results],
            "junit": paths.display_path(arguments.summary_junit),
        })
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
