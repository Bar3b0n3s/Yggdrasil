#!/usr/bin/env python3
"""The single CI entry point (Docs/Architecture.md §15.8): runs the stages in order and stops at the first failing one.

Stages that exist (milestone M5), with the configurations of the §15.8 matrix:
  setup        n/a                   Scripts/Setup.py (toolchain checks, pinned premake)
  generate     n/a                   Scripts/Generate.py (host workspace, compile_commands.json)
  lint         n/a                   static checks, shared with PreCommit.py (Scripts/Lib/scripts.py):
                                     Scripts/CheckBuildConfig.py passes on the workspace and fails, with the fixture's
                                     own defect, on every fixture workspace under Tests/Data/BuildConfig/;
                                     Scripts/Lint.py (header self-containment and the contract stubs and skipped tests
                                     included) and Lint.py --self-test (every seeded fixture under Tests/Data/Lint/
                                     fails with exactly its expected findings); Scripts/Format.py --check
  build        Debug, Release, Dist  Scripts/Build.py per configuration, plus, after Debug, the Roadmap M0 acceptance
                                     check "Shaders: slang-only change is not skipped by the up-to-date check":
                                     touching only a .slang file re-runs the shader rule, the next build does not
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
  portability  n/a                   premake --os=linux gmake, --os=linux ninja, --os=macosx xcode4 (and the vs2026
                                     reference) into bin-int/Portability/, each checked against the expected file list
                                     and project/configuration set; the xcode4 projects' precompiled headers
                                     (GCC_PREFIX_HEADER, resolved like Xcode does) and the gmake ones must exist, and
                                     the first-party xcode4 projects must enable LTO (LLVM_LTO) in Dist; then clang-cl
                                     builds of Tests in Release (Engine, EditorCore and the vendored libraries included)
                                     and of every Dist project (asserts compiled out), warnings tolerated only in
                                     vendored code, when Visual Studio's C++ Clang component
                                     is installed, otherwise reported as skipped (a failure with --require-clang-cl)
Later stages of §15.8 (bake, feature, automation, export, determinism, games, hardening) are accepted by --stages and
reported as "not-available" until their milestone.

Modes (Roadmap rule 3, Docs/Decisions/0004-contract-stub-gate.md): strict by default, so a milestone cannot end with a
contract stub (ENGINE_CONTRACT_STUB) or a skipped test case outside the child-process targets. --contract passes
--allow-contract-stubs to Lint.py (lint) and --allow-skips to Test.py (unit, gpu, golden), like PreCommit.py --contract
(Scripts/Lib/scripts.py); the run prints which mode it is in at the start and in the summary.

Results: each stage's own output, a summary table, bin/TestResults/CI.xml (JUnit, one test case per step; another
path with --summary-junit, so separate runs of one CI job keep separate summaries), the unit, gpu and golden JUnit
files in bin/TestResults/, and the actual, expected and diff images of failed golden comparisons in
bin/TestResults/Golden/.

Exit codes (§4.1): 0 every selected stage passed, otherwise the code of the first failing step: 1 failed,
2 usage error, 3 a required tool or file is missing, 5 timeout.
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"CI.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import dataclasses
import json
import os
import re
import shutil
import time
from pathlib import Path
from typing import Callable

from Lib import paths, premake, toolchain, workspace
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

TIMEOUTS = {
    "setup": 1800.0,
    "generate": 600.0,
    "build": 7200.0,
    "unit": 1800.0,
    "gpu": 3600.0,
    "golden": 1800.0,
    "portability": 600.0,
    "clang-cl": 7200.0,
}


@dataclasses.dataclass(frozen=True)
class Stage:
    name: str
    configurations: tuple[str, ...] | None  # the §15.8 matrix; None for configuration-independent stages
    available: bool
    planned: str = ""


# §15.8 order.
STAGES = (
    Stage("setup", None, True),
    Stage("generate", None, True),
    Stage("lint", None, True),
    Stage("build", ("Debug", "Release", "Dist"), True),
    Stage("bake", ("Release",), False, "M6 (engine asset bake)"),
    Stage("unit", ("Debug", "Release"), True),
    Stage("gpu", ("Debug", "Release"), True),
    Stage("golden", ("Release",), True),
    Stage("feature", ("Debug", "Release"), False, "M14 (FeatureTest)"),
    Stage("automation", ("Release",), False, "M4 (automation core)"),
    Stage("export", ("Release", "Dist"), False, "M7 (Exporter v0) / M15 (testing exports)"),
    Stage("determinism", ("Debug", "Release"), False, "M14 (determinism stage)"),
    Stage("games", ("Release",), False, "M16-M18 (demo games)"),
    Stage("portability", None, True),
    Stage("hardening", ("Release",), False, "M15 (ASan, soak, fuzz; milestone gates)"),
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
            if config == "Debug":
                steps.append(self.done(self.shader_up_to_date_check(config)))
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

    def unit(self, configs: list[str]) -> list[Step]:
        mode = test_mode_arguments(self.arguments.contract)
        return [self.script(f"unit {config}", "Test.py", ["--suite", "unit", "--config", config, "--junit", *mode],
                            TIMEOUTS["unit"]) for config in configs]

    def device_suite(self, suite: str, configs: list[str]) -> list[Step]:
        """The gpu and golden stages (§15.8): with --require-gpu, so a test case without a device fails instead of
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


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the CI stages (Architecture §15.8) in order, stopping at the first failing stage.",
        epilog="Exit codes: 0 success, else the first failing step's code: 1 failed, 2 usage, 3 missing tool or file, "
               "5 timeout.",
    )
    available = [stage.name for stage in STAGES if stage.available]
    parser.add_argument("--stages", default="all",
                        help=f"comma-separated stages (default: all stages that exist: {', '.join(available)}); "
                             f"later stages ({', '.join(s.name for s in STAGES if not s.available)}) are reported "
                             f"as not available")
    parser.add_argument("--skip-stages", default="", help="comma-separated stages to leave out")
    parser.add_argument("--configs", default=",".join(paths.CONFIGURATIONS),
                        help="restrict the configuration matrix (default: Debug,Release,Dist)")
    parser.add_argument("--toolset", choices=("msc", "gcc", "clang"),
                        help="compiler toolset passed to Generate.py (Linux: gcc or clang)")
    parser.add_argument("--no-clang-cl", action="store_true",
                        help="portability: leave out the clang-cl builds (Release Tests and Dist; Windows; by default "
                             "they run when Visual Studio's C++ Clang component is installed)")
    parser.add_argument("--require-clang-cl", action="store_true",
                        help="portability: fail (exit code 3) instead of skipping the clang-cl build when Visual "
                             "Studio's C++ Clang component is missing (CI runners that must have it)")
    parser.add_argument("--gpu-optional", action="store_true",
                        help="gpu and golden: run without --require-gpu, so on a machine without a usable Vulkan "
                             "device the GPU test cases pass without running, naming the reason, and the steps end as "
                             "warnings (the GitHub-hosted Windows and macOS runners, which have no GPU); by default a "
                             "missing device fails them")
    parser.add_argument("--contract", action="store_true", help=CONTRACT_FLAG_HELP)
    parser.add_argument("--summary-junit", type=Path, default=SUMMARY_JUNIT,
                        help=f"JUnit summary of this run (default: {paths.display_path(SUMMARY_JUNIT)})")
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    arguments = parser.parse_args(argv)
    names = tuple(stage.name for stage in STAGES)
    try:
        selected = available if arguments.stages.strip().lower() == "all" else \
            paths.parse_name_list(arguments.stages, names, "stage")
        skipped = paths.parse_name_list(arguments.skip_stages, names, "stage") if arguments.skip_stages.strip() else []
        arguments.configs = paths.parse_configurations(arguments.configs)
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
    stage_configs = [config for config in (stage.configurations or ()) if config in configs]
    if stage.configurations is not None and not stage_configs:
        return [Step(stage.name, Status.SKIPPED, f"none of its configurations ({', '.join(stage.configurations)}) "
                                                 f"is selected by --configs")]
    handlers: dict[str, Callable[[], list[Step]]] = {
        "setup": runner.setup,
        "generate": runner.generate,
        "lint": runner.lint,
        "build": lambda: runner.build(stage_configs),
        "unit": lambda: runner.unit(stage_configs),
        "gpu": lambda: runner.device_suite("gpu", stage_configs),
        "golden": lambda: runner.device_suite("golden", stage_configs),
        "portability": runner.portability,
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
    console.heading(f"CI on {host.system} ({host.machine}), Python {platform.python_version()}: stages "
                    f"{', '.join(arguments.selected)}; configurations {', '.join(arguments.configs)}; "
                    f"{mode_note(arguments.contract)}")

    runner = Runner(arguments, console)
    results: list[tuple[str, list[Step]]] = []
    failed_stage: str | None = None
    started = time.monotonic()
    for name in arguments.selected:
        stage = STAGE_BY_NAME[name]
        if failed_stage is not None:
            results.append((name, [Step(name, Status.NOT_RUN, f"stage {failed_stage} failed")]))
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
            "stages": [{"stage": name, "steps": [step.to_json() for step in stage_steps]}
                       for name, stage_steps in results],
            "junit": paths.display_path(arguments.summary_junit),
        })
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
