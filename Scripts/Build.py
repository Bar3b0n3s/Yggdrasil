#!/usr/bin/env python3
"""Build the generated workspace (Docs/Architecture.md §2.2, §2.3).

The generator is detected from the workspace files Scripts/Generate.py wrote in place (or chosen with --action):
  vs2026  MSBuild from Visual Studio 2026 (located with vswhere) on the workspace's .slnx. MSBuild warnings fail the
          build (the milestone rule is zero warnings at /W4 /WX). --allow-vendor-warnings tolerates warnings located
          in files under Vendor/ only (vendored code keeps its own warning settings, which clang-cl reports more of);
          --allow-warnings tolerates every warning. After a build that includes Runtime (every project, Runtime
          itself, or a project that depends on it, such as Tests), the app-local CRT (vcruntime140.dll,
          vcruntime140_1.dll, msvcp140.dll from VC/Redist/MSVC/<newest>/x64/Microsoft.VC145.CRT) is copied to
          bin/<OutputDir>/Runtime/Redist/, where exports take it from (§14.2 step 4).
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

MSBuild builds link the vendored static libraries (the projects whose compiled sources all lie under Vendor/) from the
shared vendor cache, <git common directory>/vendor-cache, which every worktree of the repository shares
(Lib/vendor_cache.py; Docs/Decisions/0028-backlog-program.md decision 16): each library's entry is keyed by its
generated build settings (defines, floating-point and instruction-set options, sanitizer, toolset), the toolchain
(versions and the per-user property sheet), the configuration and its pinned sources, and verified (record, key, every
file's size and SHA-256) before it is linked. A library whose settings name an input the key cannot digest (a path
outside Vendor/ or built from an MSBuild property) is never cached and compiles in the workspace. Every MSBuild run gets
an environment without the variables that select another toolset or add compiler options (VCToolsVersion,
VCToolsInstallDir, LLVMInstallDir, LLVMToolsVersion, CL, _CL_; vendor_cache.TOOLCHAIN_ENVIRONMENT_VARIABLES), so it
compiles with the toolchain the build reports and the keys record. A missing entry, or one that fails verification, is
compiled once under the cache's lock (taken inside the build's machine-wide build lock, below, with its compiler cap),
published atomically and then linked; the workspace's own vendored projects build nothing (MSBuild
ForceImportAfterCppTargets, bin-int/VendorCache/). The warnings a library's compile reported count in every build that
links it. --no-vendor-cache compiles the vendored libraries in the workspace; --vendor-cache-dir uses another cache
directory. make, ninja and xcodebuild builds do not use the cache.

After a build that builds the Runtime (every project, Runtime itself, or a project that depends on it, such as Tests),
on every generator (Docs/Decisions/0024-m15-contract.md decisions 2, 5 and 8):
  - the notices file bin/<OutputDir>/Runtime/THIRD_PARTY_NOTICES.txt is generated from Vendor/*/VENDOR.md and
    Resources/LICENSES.md (Lib/notices.py; stage_runtime_notices), which every export packages; a missing or malformed
    "## Notices" section fails the build, naming the file;
  - on macOS, the Vulkan loader, MoltenVK and its ICD manifest of $VULKAN_SDK are staged into Runtime/Frameworks and
    Runtime/Vulkan/icd.d (stage_macos_frameworks), only from the SDK version Lib/Toolchain.json pins, with each file's
    SHA-256 in the report;
  - a Dist build archives the Runtime's symbols (.pdb and .map on Windows, .map and any .dSYM on macOS, .map on Linux)
    in bin/Symbols/<OutputDir>/Runtime/ (archive_dist_symbols), which exports never package.
Each step's --json result carries what it did ("notices", "macosFrameworks", "distSymbols", "vendorCache",
"buildStamp"), like "crtRedist".

Before each configuration's build, on every generator, the build stamp (write_build_stamp; Docs/Decisions/
0030-m19-contract.md decision 13): git (rev-parse, status and ls-files, without optional locks) gives HEAD's commit
and a dirty flag, set when a tracked file differs from the commit, staged or not, or when a file git neither tracks nor
ignores lies below Engine/, Editor/, Runtime/ or Resources/ (premake compiles every source there and the build copies
the resources); without git, or outside a repository, the stamp has no commit. It is written into
bin-int/<OutputDir>/Engine/BuildStamp.h, which Engine/Source/Engine/Core/BuildId.cpp includes, only when its content
changes, so a commit recompiles that one translation unit; the first stamp of a configuration also removes the BuildId
objects a build without one compiled. The step's detail ends with "; build stamp <commit>[.dirty]" and its --json result
holds "buildStamp" (path, written, commit, dirty, description, removedObjects).

The machine-wide build lock (Scripts/Lib/locks.py; Docs/Architecture.md §2.3, ADR 0028 decision 16) is taken after the
workspace is read and held across every requested configuration: one uncapped build or two capped builds at a time,
in every checkout of the repository, and none while a gate (CI.py) runs, unless this build runs inside it or inside
another run that holds a machine lock (a Hardening.py run with a GPU step). The mode follows --jobs, on a machine with C
logical CPUs:
  --jobs N, N <= C/2  capped: one of the two slots, at most N compilers for the whole build. MSBuild builds up to -m
                      projects at once, each compiling with /MP at CL_MPCount (MSBuild's per-project compiler count,
                      which Microsoft.Cl.Common.props makes the /MP count of every project, since the generated
                      projects enable MultiProcessorCompilation without a ProcessorNumber), so up to -m x CL_MPCount
                      cl.exe run at once: a capped build gets -m:2 and -p:CL_MPCount=N/2 (-m:1 and N below 4 jobs; the
                      vendor cache's compile of missing entries too); make and ninja -jN, xcodebuild -jobs N.
  --jobs N, N > C/2   uncapped: both slots, -m:N, and every project compiles with /MP at the logical CPU count.
  no --jobs           automatic: uncapped with C jobs when no build holds or claims the lock, capped with C/2 jobs
                      beside one capped build, and otherwise waiting until one of the two is free.
A build inside a hold it inherits (CI.py's) takes no slot of its own: its mode is the one --jobs asks for, but capped
under a capped hold. While the lock is held the build waits, reporting the holder (pid, worktree, command), at most
--lock-timeout seconds (exit code 5), or fails at once with --no-wait (exit code 3). The --json document's "buildLock"
says which mode and job count it ran with.

It works from any directory (Docs/Architecture.md §2.3): a relative path given on its command line is resolved against
the caller's directory, then main() makes the repository root of its own checkout the working directory.

Exit codes: 0 success, 1 build failed (or produced warnings under MSBuild), 2 usage error (unknown project, a project
absent from the configuration), 3 the toolchain or generated workspace is missing, or --no-wait found the build lock
held, 5 timeout (of the build, or of the wait for the build lock).
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"Build.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import dataclasses
import hashlib
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

from Lib import locks, notices, paths, premake, toolchain, vendor_cache, workspace
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
# The projects a capped MSBuild build builds at once (msbuild_parallel_arguments): the workspace's heaviest projects
# pair up (Engine beside EditorCore, Editor beside Runtime), so two keep the build's compilers busy.
CAPPED_PROJECTS = 2
TOOL_LINK_ROOT = paths.BIN_INT_ROOT / "Toolchain"
RUNTIME_PROJECT = "Runtime"
REPORTED_DIAGNOSTICS = 30
# What a macOS bundle carries from the Vulkan SDK (EditorCore/Export/AppBundle.h).
MACOS_FRAMEWORKS = ("libvulkan.1.dylib", "libMoltenVK.dylib")
MOLTENVK_ICD = "MoltenVK_icd.json"
# The build stamp (write_build_stamp; Docs/Decisions/0030-m19-contract.md decision 13): the header below the Engine
# project's intermediate directory that Core/BuildId.cpp includes, the commit's length in a build id, the directories
# whose untracked files make a build dirty (premake compiles every source there, added or not, and the build copies the
# resources), and the bound of one git query, a failure bound only.
BUILD_STAMP_PROJECT = "Engine"
BUILD_STAMP_FILE_NAME = "BuildStamp.h"
BUILD_STAMP_COMMIT_DIGITS = 12
BUILD_STAMP_SOURCE_DIRECTORIES = ("Engine", "Editor", "Runtime", "Resources")
BUILD_STAMP_GIT_TIMEOUT_SECONDS = 120.0
# The objects of Engine/Source/Engine/Core/BuildId.cpp in the Engine project's intermediate directory: MSVC's and the
# GCC/Clang generators' names.
BUILD_STAMP_OBJECTS = ("BuildId.obj", "BuildId.o")

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
        # Every error line, not only the first: under GitHub Actions the step's annotation shows them (Console.result),
        # since the end of a parallel build's output is usually other files compiling.
        data["outputTail"] = "\n".join(errors[:REPORTED_DIAGNOSTICS])
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


def builds_runtime(solution: workspace.Solution, config: str, project: str | None) -> bool:
    """Whether building `project` (None: every project of the configuration) builds the Runtime, directly or as a
    dependency (Tests depends on it), so its app-local CRT must be next to it (Docs/Decisions/0012-m7-decisions.md
    decision 14)."""
    if RUNTIME_PROJECT not in solution.projects_in(config):
        return False
    return project is None or RUNTIME_PROJECT in solution.builds_with(project)


def capped_projects(jobs: int) -> int:
    """How many projects a capped MSBuild build of `jobs` jobs builds at once (-m): CAPPED_PROJECTS, or 1 below
    2 x CAPPED_PROJECTS jobs, so each project still compiles with at least two compilers."""
    return CAPPED_PROJECTS if jobs >= 2 * CAPPED_PROJECTS else 1


def msbuild_parallel_arguments(arguments: argparse.Namespace) -> list[str]:
    """MSBuild's parallelism for a build of arguments.jobs jobs. MSBuild builds up to -m projects at once and each
    compiles with /MP at CL_MPCount, so up to -m x CL_MPCount cl.exe run at once (the "P x C" of MSBuild's
    documentation on building with multiple processors). An uncapped build: -m:<jobs>, /MP at the logical CPU count. A
    capped build bounds the product by its jobs, for the whole build: -m:<capped_projects> and CL_MPCount=<jobs> //
    that."""
    if not arguments.capped:
        return [f"-m:{arguments.jobs}"]
    projects = capped_projects(arguments.jobs)
    return [f"-m:{projects}", f"-p:CL_MPCount={arguments.jobs // projects}"]


def read_msbuild_log(path: Path) -> list[str]:
    try:
        return unique(path.read_text(encoding="utf-8-sig", errors="replace").splitlines())
    except OSError:
        return []


def run_msbuild(visual_studio: toolchain.VisualStudio, target: Path, options: list[str], log_stem: Path,
                arguments: argparse.Namespace, console: Console, timeout: float) -> tuple[ProcessResult, list[str],
                                                                                          list[str]]:
    """MSBuild on `target` (a solution or project file) with `options`, its errors and warnings logged to
    <log_stem>.errors.log and <log_stem>.warnings.log. Returns (result, warnings, errors)."""
    errors_log = log_stem.with_name(log_stem.name + ".errors.log")
    warnings_log = log_stem.with_name(log_stem.name + ".warnings.log")
    for log in (errors_log, warnings_log):
        log.unlink(missing_ok=True)
    command = [
        str(visual_studio.msbuild),
        str(target),
        *msbuild_parallel_arguments(arguments),
        "-nologo",
        "-nodeReuse:false",  # lingering worker nodes would keep the output pipe open
        f"-v:{'normal' if arguments.verbose else 'minimal'}",
        *options,
        logger_argument(1, errors_log, "errorsonly"),
        logger_argument(2, warnings_log, "warningsonly"),
    ]
    # English diagnostics, so logs and parsers see the same text on every machine; no variable that selects another
    # toolset or adds compiler options, so MSBuild compiles with the toolchain the build reports and the vendor cache's
    # keys record.
    environment = vendor_cache.msbuild_environment(child_environment({"VSLANG": "1033",
                                                                      "MSBUILDDISABLENODEREUSE": "1"}))
    result = run_streamed(command, cwd=target.parent, env=environment, timeout=timeout, echo=console.stream)
    return result, read_msbuild_log(warnings_log), read_msbuild_log(errors_log)


@dataclasses.dataclass
class VendorCacheUse:
    """How a build used the shared vendor cache: the plan, the ForceImportAfterCppTargets file (None when nothing is
    linked from the cache), why the cache was not used, or the failed compile of missing entries."""

    plan: vendor_cache.Plan | None = None
    override: Path | None = None
    reason: str = ""
    failed: Step | None = None

    def to_json(self) -> dict[str, object]:
        if self.plan is None:
            return {"used": False, "reason": self.reason}
        return {"used": True, **self.plan.to_json()}


def link_vendor_cache(solution: workspace.Solution, config: str, project: str | None,
                      visual_studio: toolchain.VisualStudio, arguments: argparse.Namespace, console: Console,
                      log_stem: Path, deadline: float) -> VendorCacheUse:
    """Links this build's vendored libraries from the shared cache (Lib/vendor_cache.py), compiling and publishing the
    missing ones first. A cache that cannot be used leaves the build to compile them in the workspace."""
    if not arguments.vendor_cache:
        return VendorCacheUse(reason="--no-vendor-cache given")
    name = f"build {config}" + (f" {project}" if project else "")
    built = solution.builds_with(project) if project else set(solution.projects_in(config))
    project_files = {candidate: solution.project_files[candidate] for candidate in sorted(built)
                     if candidate in solution.project_files and candidate in solution.projects_in(config)}
    compiled: dict[str, tuple[ProcessResult, list[str], list[str]]] = {}

    def populate(populate_project: Path, builds: list[vendor_cache.StagedBuild]) -> vendor_cache.PopulateResult:
        result, warnings, errors = run_msbuild(visual_studio, populate_project, [],
                                               log_stem.with_name(log_stem.name + ".vendor-cache"), arguments, console,
                                               max(1.0, deadline - time.monotonic()))
        compiled["result"] = (result, warnings, errors)
        return vendor_cache.PopulateResult(result.succeeded, warnings, errors)

    try:
        cache = vendor_cache.VendorCache(arguments.vendor_cache_dir or vendor_cache.default_root())
        identity = vendor_cache.toolchain_identity(visual_studio.version, visual_studio.vc_tools_version,
                                                   toolchain.find_windows_sdk()[0],
                                                   vendor_cache.user_properties_digest(solution.platform))
        plan = vendor_cache.prepare(cache, project_files, config, solution.platform, identity, solution.path,
                                    populate, max(1.0, deadline - time.monotonic()), console.print)
        override = None
        if plan.linked:
            references, linking = vendor_cache.consumers(project_files.values(), config, solution.platform)
            override = vendor_cache.write_link_override(
                solution.path.parent / vendor_cache.WORKSPACE_DIRECTORY / f"{config}.targets", plan.linked, references,
                linking)
    except vendor_cache.PopulateFailedError:
        result, warnings, errors = compiled["result"]
        warnings, tolerated = split_vendor_warnings(warnings, arguments, solution.path.parent)
        step = finish_step(name, result, warnings, errors, not arguments.allow_warnings,
                           "vendored libraries compiled for the vendor cache", tolerated)
        if not step.failed:  # MSBuild failed without reporting an error
            step.status = Status.FAILED
        step.detail = f"vendor cache: {step.detail}"
        return VendorCacheUse(reason="compiling the missing entries failed", failed=step)
    except (vendor_cache.VendorCacheError, toolchain.ToolchainError, OSError) as error:
        console.print(f"vendor cache not used: {error}")
        return VendorCacheUse(reason=str(error))
    if plan.linked or plan.compiled_in_workspace:
        console.print(f"{plan.summary()} from {plan.root.as_posix()} (verified in {plan.verify_seconds:.1f} s)")
    for note in plan.notes:
        console.print(f"vendor cache: {note}")
    return VendorCacheUse(plan, override)


def build_msbuild(solution: workspace.Solution, config: str, project: str | None, arguments: argparse.Namespace,
                  console: Console) -> Step:
    started = time.monotonic()
    deadline = started + arguments.timeout
    visual_studio = toolchain.find_visual_studio()
    name = f"build {config}" + (f" {project}" if project else "")
    log_root = solution.path.parent / "bin-int" / "BuildLogs"
    log_root.mkdir(parents=True, exist_ok=True)
    log_stem = log_root / f"{config}-{project or 'All'}"
    console.print(f"MSBuild ({visual_studio.display_name} {visual_studio.version}, "
                  f"MSVC {visual_studio.vc_tools_version})")
    cache = link_vendor_cache(solution, config, project, visual_studio, arguments, console, log_stem, deadline)
    if cache.failed is not None:
        cache.failed.data["vendorCache"] = cache.to_json()
        cache.failed.duration = time.monotonic() - started
        return cache.failed

    options = [f"-p:Configuration={config}", f"-p:Platform={solution.platform}"]
    if cache.override is not None:
        options.append(f"-p:ForceImportAfterCppTargets={cache.override}")
    if project:
        options.append(f"-t:{solution.target_names[project]}" + (":Rebuild" if arguments.rebuild else ""))
    elif arguments.rebuild:
        options.append("-t:Rebuild")
    result, warnings, errors = run_msbuild(visual_studio, solution.path, options, log_stem, arguments, console,
                                           max(1.0, deadline - time.monotonic()))
    if cache.plan is not None:
        # A library linked from the cache reports the warnings its compile reported, as compiling it here would.
        warnings = unique(warnings + cache.plan.warnings)
    warnings, tolerated = split_vendor_warnings(warnings, arguments, solution.path.parent)
    built = [project] if project else solution.projects_in(config)
    step = finish_step(name, result, warnings, errors, not arguments.allow_warnings,
                       f"{len(built)} project(s) built" if not project else f"{project} built", tolerated)
    step.duration = time.monotonic() - started  # the vendor cache's verification and compiles included
    step.data["vendorCache"] = cache.to_json()
    if cache.plan is not None and (cache.plan.linked or cache.plan.compiled_in_workspace):
        step.detail += f"; {cache.plan.summary()}"
    if not step.failed and builds_runtime(solution, config, project):
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
            # Only warnings, errors and the failure summary: without it, multi-kilobyte compile command lines bury the
            # diagnostics in logs and push them out of the output tail that CI annotations report.
            "-quiet",
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
# The build stamp (Docs/Decisions/0030-m19-contract.md decision 13)
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class BuildStamp:
    """The working tree a build compiles: the commit it has checked out and whether it differs from it."""

    commit: str  # the first BUILD_STAMP_COMMIT_DIGITS of HEAD's object name, lowercase; empty without git
    dirty: bool  # always False without a commit

    def describe(self) -> str:
        """The stamp as the build id's build metadata writes it: "<commit>", "<commit>.dirty" or "no commit"."""
        if not self.commit:
            return "no commit"
        return self.commit + (".dirty" if self.dirty else "")


def git_output(repository: Path, arguments: list[str]) -> str | None:
    """The standard output of `git -C <repository> <arguments>`, or None when git is missing or the command fails. Git
    takes no optional lock (the index refresh of git status), so a stamp never blocks or fails another git process."""
    command = ["git", "--no-optional-locks", "-C", str(repository), *arguments]
    try:
        completed = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                   timeout=BUILD_STAMP_GIT_TIMEOUT_SECONDS, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.decode("utf-8", errors="replace")


def read_build_stamp(repository: Path) -> BuildStamp:
    """The stamp of the working tree at `repository` (decision 13): HEAD's commit, and dirty when tracked files differ
    from it (staged or not) or when a file git neither tracks nor ignores lies below a directory the build compiles or
    copies (BUILD_STAMP_SOURCE_DIRECTORIES), since premake compiles every source there, added or not. Without git, or
    outside a repository, the stamp has no commit and is clean."""
    head = git_output(repository, ["rev-parse", "--verify", "--quiet", "HEAD^{commit}"])
    commit = (head or "").strip().lower()
    if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", commit):
        return BuildStamp("", False)
    modified = git_output(repository, ["status", "--porcelain=v1", "-z", "--untracked-files=no"])
    untracked = git_output(repository, ["ls-files", "-z", "--others", "--exclude-standard", "--",
                                        *BUILD_STAMP_SOURCE_DIRECTORIES])
    # A query that fails cannot prove the tree clean.
    dirty = modified is None or untracked is None or bool(modified) or bool(untracked)
    return BuildStamp(commit[:BUILD_STAMP_COMMIT_DIGITS], dirty)


def build_stamp_path(root: Path, config: str) -> Path:
    """The stamp header of `config` in the workspace at `root`: the Engine project's intermediate directory
    (<workspace>/bin-int/<OutputDir>/Engine, premake's objdir), which the build names with ENGINE_BUILD_STAMP_HEADER."""
    return root / "bin-int" / paths.output_directory_name(config) / BUILD_STAMP_PROJECT / BUILD_STAMP_FILE_NAME


def build_stamp_text(stamp: BuildStamp) -> str:
    """The stamp header Engine/Source/Engine/Core/BuildId.cpp includes."""
    return ("// Generated by Scripts/Build.py (Docs/Decisions/0030-m19-contract.md decision 13): the commit and the\n"
            "// dirty flag of the working tree this configuration is built from, which\n"
            "// Engine/Source/Engine/Core/BuildId.cpp reads. Do not edit.\n"
            "#pragma once\n"
            f'#define ENGINE_BUILD_COMMIT "{stamp.commit}"\n'
            f"#define ENGINE_BUILD_DIRTY {1 if stamp.dirty else 0}\n")


def write_build_stamp(root: Path, config: str, repository: Path) -> dict[str, object]:
    """Writes the `config` stamp of the working tree at `repository` into the workspace at `root` before its build,
    rewriting it only when its content changes, so a new commit recompiles one translation unit and an unchanged tree
    none. A first stamp also removes the BuildId objects a build without one compiled (the IDE's, before any Build.py
    run): the compiler read no header there, so incremental builds, which track only the headers a source read, would
    never recompile it. Raises OSError when a file cannot be written or removed."""
    stamp = read_build_stamp(repository)
    target = build_stamp_path(root, config)
    first = not target.is_file()
    written = write_if_changed(target, build_stamp_text(stamp).encode("utf-8"))
    removed: list[str] = []
    if first:
        for stale in BUILD_STAMP_OBJECTS:
            if (target.parent / stale).is_file():
                (target.parent / stale).unlink()
                removed.append(str(target.parent / stale))
    return {"path": str(target), "written": written, "commit": stamp.commit, "dirty": stamp.dirty,
            "description": stamp.describe(), "removedObjects": removed}


# --------------------------------------------------------------------------------------------------------------------
# What every build of the Runtime stages beside it (Docs/Decisions/0024-m15-contract.md decisions 2, 5 and 8)
# --------------------------------------------------------------------------------------------------------------------


def runtime_output(root: Path, config: str) -> Path:
    """<workspace>/bin/<OutputDir>/Runtime of `config` on this host."""
    return root / "bin" / paths.output_directory_name(config) / RUNTIME_PROJECT


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_if_changed(target: Path, data: bytes) -> bool:
    """Writes `data` to `target` through a temporary file unless it already holds exactly that; True when written."""
    if target.is_file() and target.read_bytes() == data:
        return False
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + ".tmp")
    temporary.write_bytes(data)
    os.replace(temporary, target)
    return True


def stage_runtime_notices(root: Path, config: str, system: str) -> dict[str, object]:
    """Writes THIRD_PARTY_NOTICES.txt for exports on `system` ("windows", "linux" or "macosx") next to the `config`
    Runtime of the workspace at `root` (bin/<OutputDir>/Runtime/), where every export takes it from (decision 8). An
    unchanged file is not rewritten. Raises ValueError, naming the file, for a missing or malformed "## Notices"
    section."""
    data = notices.generate_notices(paths.REPOSITORY_ROOT, system).encode("utf-8")
    target = runtime_output(root, config) / notices.NOTICES_FILE_NAME
    written = write_if_changed(target, data)
    return {"path": str(target), "written": written, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def archive_dist_symbols(root: Path) -> dict[str, object]:
    """Copies the Dist Runtime's symbols of the workspace at `root` into bin/Symbols/<OutputDir>/Runtime/ (§2.2
    "archived in bin/Symbols, never shipped"; decision 2): Runtime.map on every host, Runtime.pdb on Windows (both
    required), and on macOS Runtime.dSYM when the generator produced one (xcodebuild does; the make-style generators
    do not). Raises FileNotFoundError naming a required file that is missing."""
    host = paths.host()
    source = runtime_output(root, "Dist")
    destination = root / "bin" / "Symbols" / paths.output_directory_name("Dist") / RUNTIME_PROJECT
    required = [source / f"{RUNTIME_PROJECT}.map"]
    if host.is_windows:
        required.append(source / f"{RUNTIME_PROJECT}.pdb")
    missing = [paths.display_path(file) for file in required if not file.is_file()]
    if missing:
        raise FileNotFoundError(f"the Dist Runtime's symbols are missing: {', '.join(missing)}")
    destination.mkdir(parents=True, exist_ok=True)
    archived: list[str] = []
    for file in required:
        shutil.copy2(file, destination / file.name)
        archived.append(file.name)
    dsym = source / f"{RUNTIME_PROJECT}.dSYM"
    if host.is_macos and dsym.is_dir():
        shutil.rmtree(destination / dsym.name, ignore_errors=True)
        shutil.copytree(dsym, destination / dsym.name, symlinks=True)
        archived.append(dsym.name)
    return {"source": str(source), "destination": str(destination), "files": archived}


def stage_macos_frameworks(root: Path, config: str, sdk: Path) -> dict[str, object]:
    """Stages the macOS bundle's Vulkan loader, MoltenVK and MoltenVK's ICD manifest from the Vulkan SDK at `sdk`
    (lib/libvulkan.1.dylib, lib/libMoltenVK.dylib, share/vulkan/icd.d/MoltenVK_icd.json, symbolic links resolved) into
    Runtime/Frameworks/ and Runtime/Vulkan/icd.d/ next to the `config` Runtime of the workspace at `root`, where the
    exporter's bundle assembly takes them from (decision 5). Only the pinned SDK is used: raises
    toolchain.ToolchainError when the SDK's header version is not the one Lib/Toolchain.json pins (the check Setup.py
    makes), or when a file is missing. The result lists each staged file with its SHA-256."""
    pinned = toolchain.parse_version(toolchain.load_pins().vulkan_sdk_version)[:3]
    found = toolchain.vulkan_sdk_header_version(sdk)
    if found != pinned:
        raise toolchain.ToolchainError(
            f"the Vulkan SDK at {sdk} is {toolchain.version_text(found)}, but "
            f"{paths.display_path(paths.TOOLCHAIN_PIN_PATH)} pins {toolchain.version_text(pinned)}: exports "
            f"redistribute only the pinned SDK's loader and MoltenVK (install it and point VULKAN_SDK at it)")
    target = runtime_output(root, config)
    copies = [(sdk / "lib" / name, target / "Frameworks" / name) for name in MACOS_FRAMEWORKS]
    copies.append((sdk / "share" / "vulkan" / "icd.d" / MOLTENVK_ICD, target / "Vulkan" / "icd.d" / MOLTENVK_ICD))
    for source, _ in copies:
        if not source.is_file():
            raise toolchain.ToolchainError(f"the Vulkan SDK at {sdk} has no {source.relative_to(sdk).as_posix()}")
    files: list[dict[str, object]] = []
    for source, destination in copies:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)  # follows symbolic links: the bundle carries the library itself
        shutil.copymode(source, destination)
        files.append({"path": str(destination), "source": str(source.resolve()), "sha256": file_sha256(destination)})
    return {"sdk": str(sdk), "version": toolchain.version_text(found), "destination": str(target), "files": files}


def builds_runtime_in(generated: workspace.Workspace, config: str, project: str | None, started: float,
                      root: Path) -> bool:
    """Whether this build of `project` (None: every project) built the `config` Runtime: directly or as a dependency
    where the workspace records dependencies (MSBuild, Xcode); for make and ninja, which record none, when the Runtime
    executable was written during the build."""
    if RUNTIME_PROJECT not in generated.projects_in(config):
        return False
    if project is None or project == RUNTIME_PROJECT:
        return True
    if isinstance(generated, workspace.Solution):
        return builds_runtime(generated, config, project)
    if isinstance(generated, workspace.XcodeWorkspace):
        built: set[str] = set()
        pending = [project]
        while pending:
            name = pending.pop()
            if name not in built:
                built.add(name)
                pending.extend(generated.dependencies.get(name, set()))
        return RUNTIME_PROJECT in built
    executable = runtime_output(root, config) / paths.executable_name(RUNTIME_PROJECT)
    try:
        return executable.stat().st_mtime >= started
    except OSError:
        return False


def stage_runtime_outputs(step: Step, root: Path, config: str, host: paths.Host) -> None:
    """After `step` built the `config` Runtime: the notices file, on macOS the bundle's SDK files, and for Dist the
    archived symbols. A failure fails the step, naming what is wrong."""
    try:
        staged = stage_runtime_notices(root, config, host.system)
        step.data["notices"] = staged
        step.detail += f"; notices -> {paths.display_path(Path(str(staged['path'])))}"
        if host.is_macos:
            sdk = os.environ.get("VULKAN_SDK", "")
            if not sdk:
                raise toolchain.ToolchainError("VULKAN_SDK is not set: macOS builds stage the pinned SDK's loader and "
                                               "MoltenVK beside the Runtime (see Scripts/Setup.py)")
            frameworks = stage_macos_frameworks(root, config, Path(sdk))
            step.data["macosFrameworks"] = frameworks
            step.detail += f"; Vulkan SDK {frameworks['version']} frameworks staged"
        if config == "Dist":
            symbols = archive_dist_symbols(root)
            step.data["distSymbols"] = symbols
            step.detail += f"; symbols -> {paths.display_path(Path(str(symbols['destination'])))}"
    except (ValueError, FileNotFoundError, OSError) as error:
        step.status = Status.FAILED
        step.detail += f"; staging beside the Runtime failed: {error}"


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


def capped_jobs(cpu_count: int) -> int:
    """The most jobs of a capped build: half the logical CPUs (at least 1)."""
    return max(1, cpu_count // 2)


def requested_build_mode(jobs: int | None, cpu_count: int) -> str:
    """The build lock mode --jobs asks for: automatic without it, capped for at most half the logical CPUs, else
    uncapped."""
    if jobs is None:
        return locks.AUTO
    return locks.CAPPED if jobs <= capped_jobs(cpu_count) else locks.UNCAPPED


def build_jobs(mode: str, jobs: int | None, cpu_count: int) -> int:
    """The jobs a build runs with once its hold's mode is known: a capped one at most half the logical CPUs (half when
    --jobs is not given), an uncapped one --jobs or the CPU count."""
    if mode == locks.CAPPED:
        return min(jobs or capped_jobs(cpu_count), capped_jobs(cpu_count))
    return jobs or cpu_count


def take_build_lock(arguments: argparse.Namespace, console: Console) -> locks.Hold:
    """The machine-wide build lock in the mode --jobs asks for; sets arguments.jobs and arguments.capped from the hold.
    Raises locks.LockUnavailable."""
    cpu_count = os.cpu_count() or 1
    hold = locks.acquire_build(requested_build_mode(arguments.requested_jobs, cpu_count),
                               locks.wait_seconds(arguments), console.print)
    arguments.jobs = build_jobs(hold.mode, arguments.requested_jobs, cpu_count)
    arguments.capped = hold.capped
    cap = f"; MSBuild {' '.join(msbuild_parallel_arguments(arguments))}" if hold.capped else ""
    under = " under the hold of a parent process" if hold.inherited else ""
    console.print(f"Build lock: {hold.mode}, {arguments.jobs} job(s){cap}{under}")
    return hold


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build the generated workspace (MSBuild, make, ninja or xcodebuild).",
        epilog="Exit codes: 0 success, 1 build failed, 2 usage error, 3 toolchain or workspace missing, or --no-wait "
               "found the build lock held, 5 timeout (of the build, or of the wait for the build lock).",
    )
    parser.add_argument("--config", default="Debug",
                        help="configuration(s), comma-separated: Debug, Release, Dist (default: Debug)")
    parser.add_argument("--project", help="build only this project and its dependencies (default: every project "
                                          "that exists in the configuration)")
    parser.add_argument("--action", choices=sorted({a for s in premake.ACTIONS_BY_SYSTEM.values() for a in s}),
                        help="generator whose output to build (default: detected; the host default wins)")
    parser.add_argument("--workspace-dir", type=paths.argument_path, default=paths.REPOSITORY_ROOT,
                        help="directory of a workspace generated with Generate.py --to (default: the repository, "
                             "where Generate.py writes it in place)")
    parser.add_argument("--jobs", type=int,
                        help="parallel jobs; at most half the logical CPUs is a capped build (one of the build lock's "
                             "two slots, at most that many compilers for the whole build: MSBuild -m:2 with "
                             "CL_MPCount at half of it), more is uncapped "
                             "(the whole lock); default: uncapped with the CPU count when no build holds the lock, "
                             "capped with half of it beside one capped build")
    parser.add_argument("--rebuild", action="store_true", help="rebuild instead of building incrementally")
    parser.add_argument("--allow-warnings", action="store_true",
                        help="do not fail an MSBuild build that reports warnings")
    parser.add_argument("--allow-vendor-warnings", action="store_true",
                        help="do not fail an MSBuild build for warnings located in files under Vendor/ (first-party "
                             "code builds with warnings as errors anyway); every other warning still fails it")
    parser.add_argument("--no-vendor-cache", dest="vendor_cache", action="store_false",
                        help="MSBuild: compile the vendored libraries in the workspace instead of linking them from "
                             "the shared vendor cache")
    parser.add_argument("--vendor-cache-dir", type=paths.argument_path,
                        help="MSBuild: the shared vendor cache's directory (default: <git common directory>/"
                             f"{vendor_cache.CACHE_DIRECTORY_NAME}, which every worktree of the repository shares)")
    parser.add_argument("--timeout", type=float, default=BUILD_TIMEOUT_SECONDS,
                        help=f"seconds per configuration (default: {BUILD_TIMEOUT_SECONDS:.0f})")
    parser.add_argument("--verbose", action="store_true", help="show full command lines and normal verbosity")
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    locks.add_wait_arguments(parser)
    arguments = parser.parse_args(argv)
    try:
        arguments.configs = paths.parse_configurations(arguments.config)
    except ValueError as error:
        parser.error(str(error))
    if arguments.jobs is not None and arguments.jobs < 1:
        parser.error("--jobs must be at least 1")
    locks.check_wait_arguments(parser, arguments)
    arguments.requested_jobs = arguments.jobs
    arguments.capped = False
    arguments.workspace_dir = arguments.workspace_dir.resolve()
    if arguments.vendor_cache_dir is not None:
        arguments.vendor_cache_dir = arguments.vendor_cache_dir.resolve()
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    return arguments


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    paths.enter_repository_root()
    console = Console(arguments.json)
    steps: list[Step] = []
    action = None
    hold: locks.Hold | None = None
    exit_code = EXIT_SUCCESS
    try:
        host = paths.host()
        action = detect_action(host, arguments.action, arguments.workspace_dir)
        generated = workspace.read_workspace(action, arguments.workspace_dir)
        projects = {config: (generated.require_project(arguments.project, config) if arguments.project else None)
                    for config in arguments.configs}
        hold = take_build_lock(arguments, console)
        for config in arguments.configs:
            console.heading(f"Build {config} ({action})" + (f": {projects[config]}" if projects[config] else ""))
            try:
                stamp = write_build_stamp(generated.path.parent, config, paths.REPOSITORY_ROOT)
            except OSError as error:
                raise BuildSetupError(f"cannot write the build stamp of {config}: {error}") from error
            started = time.time()
            if isinstance(generated, workspace.Solution):
                step = build_msbuild(generated, config, projects[config], arguments, console)
            elif isinstance(generated, workspace.Makefiles):
                step = build_make(generated, config, projects[config], arguments, console)
            elif isinstance(generated, workspace.NinjaFiles):
                step = build_ninja(generated, config, projects[config], arguments, console)
            else:
                assert isinstance(generated, workspace.XcodeWorkspace)
                step = build_xcode(generated, config, projects[config], arguments, console)
            step.data["buildStamp"] = stamp
            step.detail += f"; build stamp {stamp['description']}"
            if not step.failed and builds_runtime_in(generated, config, projects[config], started,
                                                     generated.path.parent):
                stage_runtime_outputs(step, generated.path.parent, config, host)
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
    except locks.LockUnavailable as error:
        steps.append(locks.unavailable_step(error))
        exit_code = error.exit_code
        console.result(steps[-1])
    finally:
        if hold is not None:
            hold.release()

    console.summary("Build summary", steps)
    if arguments.json:
        build_lock = None if hold is None else {"mode": hold.mode, "jobs": arguments.jobs, "inherited": hold.inherited}
        emit_json({"success": exit_code == EXIT_SUCCESS, "action": action, "buildLock": build_lock,
                   "steps": [s.to_json() for s in steps]})
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
