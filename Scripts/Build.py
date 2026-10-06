#!/usr/bin/env python3
"""Build the generated workspace (Docs/Architecture.md §2.2, §2.3).

The generator is detected from the workspace files Scripts/Generate.py wrote in place (or chosen with --action):
  vs2026  MSBuild from Visual Studio 2026 (located with vswhere) on the workspace's .slnx. MSBuild warnings fail the
          build (the milestone rule is zero warnings at /W4 /WX). --allow-vendor-warnings tolerates warnings located
          in files under Vendor/ only (vendored code keeps its own warning settings, which clang-cl reports more of);
          --allow-warnings tolerates every warning. After a build that includes Runtime, the app-local CRT
          (vcruntime140.dll, vcruntime140_1.dll, msvcp140.dll from VC/Redist/MSVC/<newest>/x64/Microsoft.VC145.CRT) is
          copied to bin/<OutputDir>/Runtime/Redist/.
  gmake   make -j<jobs> config=<config> with CC/CXX/AR set to the selected compiler: CC/CXX/AR from the environment,
          else the newest installed g++-N/clang++-N (GCC >= 14, Clang >= 18) matching the toolset the makefiles
          were generated for, with the matching gcc-ar/llvm-ar for the LTO archives of Dist. Clang on Linux links with
          lld (-fuse-ld=lld in the generated projects), which Scripts/Lib/toolchain.py requires.
  ninja   ninja -j<jobs> <Config> or <Project>_<Config>, with the same compiler selection (premake's ninja rules
          call gcc/g++/ar or clang/clang++/ar by name, so a directory of links to the selected tools goes first on
          PATH when those names would resolve to other versions).
  xcode4  xcodebuild -project <P>.xcodeproj -target <P> -configuration <Config> for every top-level project (the
          ones no other project depends on; their dependencies build through the project references).

Projects that do not exist in a configuration (EditorCore, Editor and Tests in Dist) are never built in it.

Exit codes: 0 success, 1 build failed (or produced warnings under MSBuild), 2 usage error (unknown project, a project
absent from the configuration), 3 the toolchain or generated workspace is missing, 5 timeout.
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"Build.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import os
import re
import shutil
import time
from pathlib import Path

from Lib import paths, premake, toolchain, workspace
from Lib.process import ProcessResult, ToolNotFoundError, child_environment, run_streamed
from Lib.report import (
    EXIT_INIT_FAILED,
    EXIT_SUCCESS,
    EXIT_USAGE,
    Console,
    Status,
    Step,
    configure_stdio,
    emit_json,
    overall_exit_code,
)

BUILD_TIMEOUT_SECONDS = 7200.0
TOOL_LINK_ROOT = paths.BIN_INT_ROOT / "Toolchain"
RUNTIME_PROJECT = "Runtime"
REPORTED_DIAGNOSTICS = 30

# Diagnostics in GCC/Clang/ld/xcodebuild output ("file:line:col: warning: ...", "ld: warning: ...").
POSIX_WARNING_PATTERN = re.compile(r"(^|: )warning: ", re.IGNORECASE)
POSIX_ERROR_PATTERN = re.compile(r"(^|: )(fatal )?error: |^make(\[\d+\])?: \*\*\*|^ninja: error|\*\* BUILD FAILED \*\*")
MSBUILD_NODE_PREFIX = re.compile(r"^\s*\d+>")
# The file a diagnostic is located in: "path(line[,column]): warning ..." (MSBuild, cl, clang-cl) or
# "path:line:column: warning: ..." (GCC, Clang). Diagnostics without a file (linker, MSBuild) have no match.
DIAGNOSTIC_FILE_PATTERNS = (
    re.compile(r"^(?P<file>.+?)\(\d+(?:,\d+)*\)\s*:"),
    re.compile(r"^(?P<file>(?:[A-Za-z]:)?[^:]+?):\d+:(?:\d+:)?\s"),
)


class BuildSetupError(Exception):
    """The toolchain or the generated workspace is missing or unusable (exit code 3)."""


class BuildUsageError(Exception):
    """The command line asks for something the workspace does not have (exit code 2)."""


def unique(lines: list[str]) -> list[str]:
    """Distinct non-empty diagnostics in first-seen order, without MSBuild's "<node>>" prefix of parallel builds."""
    seen: set[str] = set()
    result = []
    for line in lines:
        text = MSBUILD_NODE_PREFIX.sub("", line).strip()
        if text and text not in seen:
            seen.add(text)
            result.append(text)
    return result


def is_vendor_diagnostic(diagnostic: str, root: Path) -> bool:
    """True when the diagnostic is located in a file under Vendor/ (relative paths resolve against `root`)."""
    for pattern in DIAGNOSTIC_FILE_PATTERNS:
        match = pattern.match(diagnostic)
        if match:
            path = Path(match.group("file").strip())
            path = path if path.is_absolute() else root / path
            try:
                path.resolve().relative_to(paths.VENDOR_ROOT.resolve())
            except (ValueError, OSError):
                return False
            return True
    return False


def split_vendor_warnings(warnings: list[str], arguments: argparse.Namespace,
                          root: Path) -> tuple[list[str], list[str]]:
    """(enforced, tolerated) warnings: with --allow-vendor-warnings the ones located under Vendor/ are tolerated."""
    if not arguments.allow_vendor_warnings:
        return warnings, []
    tolerated = [warning for warning in warnings if is_vendor_diagnostic(warning, root)]
    return [warning for warning in warnings if warning not in tolerated], tolerated


def finish_step(name: str, result: ProcessResult, warnings: list[str], errors: list[str], enforce_warnings: bool,
                summary: str, tolerated: list[str] | None = None) -> Step:
    tolerated = tolerated or []
    data = {"warnings": warnings[:REPORTED_DIAGNOSTICS], "warningCount": len(warnings),
            "errors": errors[:REPORTED_DIAGNOSTICS], "errorCount": len(errors),
            "vendorWarnings": tolerated[:REPORTED_DIAGNOSTICS], "vendorWarningCount": len(tolerated)}
    if tolerated:
        summary += f", {len(tolerated)} tolerated warning(s) in vendored code"
    if result.timed_out:
        return Step(name, Status.TIMEOUT, f"build {result.describe_exit()}", result.duration, data=data)
    if not result.succeeded:
        first = f": {errors[0]}" if errors else ""
        return Step(name, Status.FAILED, f"build failed ({result.describe_exit()}, {len(errors)} error(s)){first}",
                    result.duration, data=data)
    if warnings and enforce_warnings:
        return Step(name, Status.FAILED, f"{len(warnings)} warning(s) (zero allowed; --allow-warnings to relax): "
                                         f"{warnings[0]}", result.duration, data=data)
    if warnings:
        return Step(name, Status.WARNING, f"{summary}, {len(warnings)} warning(s): {warnings[0]}", result.duration,
                    data=data)
    return Step(name, Status.PASSED, f"{summary}, 0 warnings", result.duration, data=data)


# --------------------------------------------------------------------------------------------------------------------
# Windows: MSBuild
# --------------------------------------------------------------------------------------------------------------------


def logger_argument(index: int, path: Path, verbosity: str) -> str:
    location = f'"{path}"' if " " in str(path) else str(path)
    return f"-flp{index}:logfile={location};{verbosity};encoding=utf-8"


def copy_crt_redist(visual_studio: toolchain.VisualStudio, config: str, root: Path) -> dict[str, object]:
    """Copy the app-local CRT next to the Runtime build output (<workspace>/bin/<OutputDir>/Runtime/Redist).
    Unchanged files are not rewritten."""
    version, source = toolchain.find_crt_redist(visual_studio)
    target = root / "bin" / paths.output_directory_name(config) / RUNTIME_PROJECT / "Redist"
    target.mkdir(parents=True, exist_ok=True)
    copied: list[str] = []
    for name in toolchain.CRT_REDIST_FILES:
        source_file, target_file = source / name, target / name
        source_status = source_file.stat()
        if target_file.is_file():
            target_status = target_file.stat()
            if (target_status.st_size, target_status.st_mtime_ns) == (source_status.st_size, source_status.st_mtime_ns):
                continue
        shutil.copy2(source_file, target_file)
        copied.append(name)
    return {"version": version, "source": source.as_posix(), "destination": paths.display_path(target),
            "copied": copied}


def build_msbuild(solution: workspace.Solution, config: str, project: str | None, arguments: argparse.Namespace,
                  console: Console) -> Step:
    visual_studio = toolchain.find_visual_studio()
    name = f"build {config}" + (f" {project}" if project else "")
    log_root = solution.path.parent / "bin-int" / "BuildLogs"
    log_root.mkdir(parents=True, exist_ok=True)
    stem = f"{config}-{project or 'All'}"
    errors_log, warnings_log = log_root / f"{stem}.errors.log", log_root / f"{stem}.warnings.log"
    for log in (errors_log, warnings_log):
        log.unlink(missing_ok=True)

    targets: list[str] = []
    if project:
        targets.append(f"-t:{solution.target_names[project]}" + (":Rebuild" if arguments.rebuild else ""))
    elif arguments.rebuild:
        targets.append("-t:Rebuild")
    command = [
        str(visual_studio.msbuild),
        str(solution.path),
        f"-m:{arguments.jobs}",
        "-nologo",
        "-nodeReuse:false",  # lingering worker nodes would keep the output pipe open
        f"-v:{'normal' if arguments.verbose else 'minimal'}",
        f"-p:Configuration={config}",
        f"-p:Platform={solution.platform}",
        logger_argument(1, errors_log, "errorsonly"),
        logger_argument(2, warnings_log, "warningsonly"),
        *targets,
    ]
    console.print(f"MSBuild ({visual_studio.display_name} {visual_studio.version}, "
                  f"MSVC {visual_studio.vc_tools_version})")
    # English diagnostics, so logs and parsers see the same text on every machine.
    environment = child_environment({"VSLANG": "1033", "MSBUILDDISABLENODEREUSE": "1"})
    result = run_streamed(command, cwd=solution.path.parent, env=environment, timeout=arguments.timeout,
                          echo=console.stream)

    def read_log(path: Path) -> list[str]:
        try:
            return unique(path.read_text(encoding="utf-8-sig", errors="replace").splitlines())
        except OSError:
            return []

    warnings, errors = read_log(warnings_log), read_log(errors_log)
    warnings, tolerated = split_vendor_warnings(warnings, arguments, solution.path.parent)
    built = [project] if project else solution.projects_in(config)
    step = finish_step(name, result, warnings, errors, not arguments.allow_warnings,
                       f"{len(built)} project(s) built" if not project else f"{project} built", tolerated)
    if not step.failed and (project is None or project == RUNTIME_PROJECT):
        redist = copy_crt_redist(visual_studio, config, solution.path.parent)
        step.data["crtRedist"] = redist
        copied = len(redist["copied"])  # type: ignore[arg-type]
        step.detail += (f"; CRT {redist['version']} -> {redist['destination']} "
                        f"({copied} copied, {len(toolchain.CRT_REDIST_FILES) - copied} up to date)")
    return step


# --------------------------------------------------------------------------------------------------------------------
# Linux/macOS: make and ninja
# --------------------------------------------------------------------------------------------------------------------


def compiler_environment(toolset: str, console: Console) -> tuple[toolchain.Compiler, dict[str, str]]:
    compiler = toolchain.select_compiler(toolset, os.environ)
    console.print(f"Compiler: {compiler.describe()}")
    return compiler, child_environment({"CC": compiler.cc, "CXX": compiler.cxx, "AR": compiler.ar})


def tool_links_for_ninja(toolset: str, compiler: toolchain.Compiler, environment: dict[str, str],
                         console: Console) -> None:
    """premake's ninja rules invoke gcc/g++/ar (or clang/clang++/ar) by name. When those names resolve to other
    tools than the selected ones, put a directory of links named like that first on PATH."""
    names = {"gcc": compiler.cc, "g++": compiler.cxx, "ar": compiler.ar} if toolset == "gcc" else \
        {"clang": compiler.cc, "clang++": compiler.cxx, "ar": compiler.ar}
    wanted: dict[str, str] = {}
    for name, tool in names.items():
        located = shutil.which(tool)
        if located is None:
            raise BuildSetupError(f"{tool} not found on PATH")
        wanted[name] = os.path.realpath(located)

    def resolves_to_selected(name: str) -> bool:
        located = shutil.which(name)
        return located is not None and os.path.realpath(located) == wanted[name]

    if all(resolves_to_selected(name) for name in wanted):
        return
    directory = TOOL_LINK_ROOT / re.sub(r"[^A-Za-z0-9_.-]", "_", Path(compiler.cxx).name)
    directory.mkdir(parents=True, exist_ok=True)
    for name, target in wanted.items():
        link = directory / name
        if link.is_symlink() or link.exists():
            if os.path.realpath(link) == target:
                continue
            link.unlink()
        link.symlink_to(target)
    environment["PATH"] = str(directory) + os.pathsep + environment.get("PATH", "")
    links = ", ".join(f"{name} -> {target}" for name, target in wanted.items())
    console.print(f"ninja tool links: {paths.display_path(directory)} ({links})")


def build_make(makefiles: workspace.Makefiles, config: str, project: str | None, arguments: argparse.Namespace,
               console: Console) -> Step:
    _, environment = compiler_environment(makefiles.toolset, console)
    command = ["make", "-C", str(makefiles.path.parent), f"-j{arguments.jobs}", f"config={config.lower()}"]
    if arguments.rebuild:
        command.append("-B")
    if arguments.verbose:
        command.append("verbose=1")
    if project:
        command.append(project)
    return build_posix(command, environment, config, project, makefiles, arguments, console)


def build_ninja(ninja: workspace.NinjaFiles, config: str, project: str | None, arguments: argparse.Namespace,
                console: Console) -> Step:
    compiler, environment = compiler_environment(ninja.toolset, console)
    tool_links_for_ninja(ninja.toolset, compiler, environment, console)
    targets = [f"{project}_{config}"] if project else [config]
    root = str(ninja.path.parent)
    if arguments.rebuild:
        cleaned = run_streamed(["ninja", "-C", root, "-t", "clean", *targets], env=environment, timeout=600,
                               echo=console.stream)
        if not cleaned.succeeded:
            return Step(f"build {config}", Status.FAILED, f"ninja -t clean failed ({cleaned.describe_exit()})",
                        cleaned.duration)
    command = ["ninja", "-C", root, f"-j{arguments.jobs}", *targets] + (["-v"] if arguments.verbose else [])
    return build_posix(command, environment, config, project, ninja, arguments, console)


def build_posix(command: list[str], environment: dict[str, str], config: str, project: str | None,
                generated: workspace.Workspace, arguments: argparse.Namespace, console: Console) -> Step:
    name = f"build {config}" + (f" {project}" if project else "")
    try:
        result = run_streamed(command, cwd=generated.path.parent, env=environment, timeout=arguments.timeout,
                              echo=console.stream, collect=re.compile(f"{POSIX_WARNING_PATTERN.pattern}|"
                                                                      f"{POSIX_ERROR_PATTERN.pattern}", re.IGNORECASE))
    except ToolNotFoundError as error:
        raise BuildSetupError(f"{error} (install it, see Scripts/Setup.py)") from None
    warnings = unique([line for line in result.collected if POSIX_WARNING_PATTERN.search(line)])
    errors = unique([line for line in result.collected if POSIX_ERROR_PATTERN.search(line)])
    built = [project] if project else generated.projects_in(config)
    # Only MSBuild enforces zero warnings: first-party code already builds with -Werror everywhere (§2.2).
    return finish_step(name, result, warnings, errors, False,
                       f"{len(built)} project(s) built" if not project else f"{project} built")


# --------------------------------------------------------------------------------------------------------------------
# macOS: xcodebuild
# --------------------------------------------------------------------------------------------------------------------


def build_xcode(xcode: workspace.XcodeWorkspace, config: str, project: str | None, arguments: argparse.Namespace,
                console: Console) -> Step:
    name = f"build {config}" + (f" {project}" if project else "")
    roots = [project] if project else xcode.top_level_projects(config)
    pattern = re.compile(f"{POSIX_WARNING_PATTERN.pattern}|{POSIX_ERROR_PATTERN.pattern}", re.IGNORECASE)
    started = time.monotonic()
    collected: list[str] = []
    result: ProcessResult | None = None
    for root in roots:
        command = [
            "xcodebuild",
            "-project", str(xcode.project_files[root]),
            "-target", xcode.target_names[root],
            "-configuration", config,
            "-sdk", "macosx",
            "-jobs", str(arguments.jobs),
            "-parallelizeTargets",
            "-hideShellScriptEnvironment",
            *(["clean"] if arguments.rebuild else []),
            "build",
            # Local and CI builds are not signed (Architecture §1.2); the arm64 linker still signs ad hoc.
            "CODE_SIGNING_ALLOWED=NO",
        ]
        console.print(f"xcodebuild {root} ({config})")
        remaining = None if arguments.timeout is None else max(1.0, arguments.timeout - (time.monotonic() - started))
        try:
            result = run_streamed(command, cwd=xcode.project_files[root].parent, env=child_environment(),
                                  timeout=remaining, echo=console.stream, collect=pattern)
        except ToolNotFoundError as error:
            raise BuildSetupError(f"{error} (install Xcode {toolchain.MINIMUM_XCODE}+)") from None
        collected += result.collected
        if not result.succeeded:
            break
    assert result is not None
    result.duration = time.monotonic() - started
    warnings = unique([line for line in collected if POSIX_WARNING_PATTERN.search(line)])
    errors = unique([line for line in collected if POSIX_ERROR_PATTERN.search(line)])
    built = [project] if project else xcode.projects_in(config)
    return finish_step(name, result, warnings, errors, False,
                       f"{len(built)} project(s) built" if not project else f"{project} built")


# --------------------------------------------------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------------------------------------------------


def detect_action(host: paths.Host, requested: str | None, root: Path) -> str:
    candidates = premake.ACTIONS_BY_SYSTEM[host.system]
    if requested:
        if requested not in candidates:
            raise BuildUsageError(f"--action {requested} cannot build on {host.system} (available: "
                                  f"{', '.join(candidates)})")
        return requested
    present = [action for action in candidates if workspace.workspace_file(action, root).exists()]
    if not present:
        raise BuildSetupError(f"no generated workspace found: run python Scripts/Generate.py "
                              f"(expected {paths.display_path(workspace.workspace_file(candidates[0], root))})")
    return present[0]  # the host default wins when several generators were run


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build the generated workspace (MSBuild, make, ninja or xcodebuild).",
        epilog="Exit codes: 0 success, 1 build failed, 2 usage error, 3 toolchain or workspace missing, 5 timeout.",
    )
    parser.add_argument("--config", default="Debug",
                        help="configuration(s), comma-separated: Debug, Release, Dist (default: Debug)")
    parser.add_argument("--project", help="build only this project and its dependencies (default: every project "
                                          "that exists in the configuration)")
    parser.add_argument("--action", choices=sorted({a for s in premake.ACTIONS_BY_SYSTEM.values() for a in s}),
                        help="generator whose output to build (default: detected; the host default wins)")
    parser.add_argument("--workspace-dir", type=Path, default=paths.REPOSITORY_ROOT,
                        help="directory of a workspace generated with Generate.py --to (default: the repository, "
                             "where Generate.py writes it in place)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="parallel jobs (default: CPU count)")
    parser.add_argument("--rebuild", action="store_true", help="rebuild instead of building incrementally")
    parser.add_argument("--allow-warnings", action="store_true",
                        help="do not fail an MSBuild build that reports warnings")
    parser.add_argument("--allow-vendor-warnings", action="store_true",
                        help="do not fail an MSBuild build for warnings located in files under Vendor/ (first-party "
                             "code builds with warnings as errors anyway); every other warning still fails it")
    parser.add_argument("--timeout", type=float, default=BUILD_TIMEOUT_SECONDS,
                        help=f"seconds per configuration (default: {BUILD_TIMEOUT_SECONDS:.0f})")
    parser.add_argument("--verbose", action="store_true", help="show full command lines and normal verbosity")
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    arguments = parser.parse_args(argv)
    try:
        arguments.configs = paths.parse_configurations(arguments.config)
    except ValueError as error:
        parser.error(str(error))
    if arguments.jobs < 1:
        parser.error("--jobs must be at least 1")
    arguments.workspace_dir = arguments.workspace_dir.resolve()
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    return arguments


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    console = Console(arguments.json)
    steps: list[Step] = []
    action = None
    exit_code = EXIT_SUCCESS
    try:
        host = paths.host()
        action = detect_action(host, arguments.action, arguments.workspace_dir)
        generated = workspace.read_workspace(action, arguments.workspace_dir)
        projects = {config: (generated.require_project(arguments.project, config) if arguments.project else None)
                    for config in arguments.configs}
        for config in arguments.configs:
            console.heading(f"Build {config} ({action})" + (f": {projects[config]}" if projects[config] else ""))
            if isinstance(generated, workspace.Solution):
                step = build_msbuild(generated, config, projects[config], arguments, console)
            elif isinstance(generated, workspace.Makefiles):
                step = build_make(generated, config, projects[config], arguments, console)
            elif isinstance(generated, workspace.NinjaFiles):
                step = build_ninja(generated, config, projects[config], arguments, console)
            else:
                assert isinstance(generated, workspace.XcodeWorkspace)
                step = build_xcode(generated, config, projects[config], arguments, console)
            steps.append(step)
            console.result(step)
            if step.failed:
                break
        exit_code = overall_exit_code(steps)
    except (BuildUsageError, workspace.ProjectSelectionError) as error:
        exit_code = EXIT_USAGE
        steps.append(Step("build", Status.FAILED, str(error), exit_code=exit_code))
        console.result(steps[-1])
    except (BuildSetupError, workspace.WorkspaceError, toolchain.ToolchainError, paths.UnsupportedHostError,
            ToolNotFoundError) as error:
        exit_code = EXIT_INIT_FAILED
        steps.append(Step("build", Status.FAILED, str(error), exit_code=exit_code))
        console.result(steps[-1])

    console.summary("Build summary", steps)
    if arguments.json:
        emit_json({"success": exit_code == EXIT_SUCCESS, "action": action, "steps": [s.to_json() for s in steps]})
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
