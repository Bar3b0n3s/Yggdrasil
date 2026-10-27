#!/usr/bin/env python3
"""Build a game's native module (Docs/Architecture.md §2.3; Docs/Decisions/0030-m19-contract.md decisions 30, 33 to 35).

  python Scripts/BuildNative.py --project <root> --kind module|runtime --config Debug|Release|Dist --out <dir>
                                [--json] [--jobs N] [--timeout S] [--lock-timeout S | --no-wait]

The module is every .cpp and .h file below <root>/Native/ (Lib/native_build.py list_sources). Windows only
(Docs/Decisions/0028-backlog-program.md decision 10): elsewhere the script exits 3 naming the platform. A build:
  1. checks the sources with Scripts/Lint.py's native-module rules (lint_native_module, decision 35: the banned APIs,
     the simulation path's included, the include rule, the file rule, no mutable state and naming); each finding is a
     NATIVE_LINT_ERROR diagnostic, and nothing compiles; then, whatever the lint found, checks that each source is
     clang-format clean, as Format.py --check does (decision 35), with the clang-format 22 Format.py finds and the
     repository's .clang-format named explicitly (a project outside the repository has none above it): each source
     that is not is a NATIVE_FORMAT_WARNING diagnostic at its first differing line, which names the line clang-format
     writes there, and a clang-format that is missing or fails is one too. These warnings fail nothing, so a build still
     succeeds, but Format.py --check fails the gate on such a source, and only native.write can rewrite it
     (provenance);
  2. takes the machine-wide build lock in a capped slot, as Build.py --jobs N does (Lib/locks.py), at most half the
     logical CPUs as jobs;
  3. for --kind runtime, finds what the Runtime links: the configuration's RuntimeCore and Engine libraries in
     bin/<OutputDir>/ (Build.py builds them with the Runtime) and the vendored libraries the prebuilt Runtime links,
     from the shared vendor cache as Build.py links them (Lib/vendor_cache.py, compiling a missing entry), or from
     bin/<OutputDir>/<Library>/ where Build.py compiled them in the workspace;
  4. generates a workspace below <out>/Workspace/ with the pinned premake from Scripts/Premake/NativeModule.lua, which
     compiles the sources with the engine's own ApplyFirstPartySettings() and Use<Lib>() functions (precise floating
     point, warnings as errors, the first-party defines, the engine's instruction set), and builds it with MSBuild
     (vswhere), its objects below <out>/Intermediate/:
       module   <out>/NativeModule.dll, the editor's shared library (Debug or Release; Dist has no editor), with
                ENGINE_NATIVE_SHARED_LIBRARY defined and no library linked;
       runtime  <out>/Runtime.exe, the game's Runtime of the configuration with the module linked in;
     a compiler or linker warning fails the build like an error (warnings as errors), while an MSBuild warning about
     the build itself (MSB8029: an output directory below %TEMP%) is recorded with severity Warning and fails nothing;
     then hashes the sources again: MSBuild read them from disk after step 1 linted and hashed them, so a build whose
     sources changed while it ran (a native.write, or an edit.undo of one) fails with a NATIVE_BUILD_ERROR, since it
     may have compiled code the lint never saw under a source hash it was not built from;
  5. writes the record <out>/NativeBuild.json (Lib/native_build.py; NativeBuildRecord in
     Editor/Source/EditorCore/Native/NativeBuild.h): the kind, configuration, source hash, sources, success, the
     output's and the log's file names and every diagnostic; the whole output goes to <out>/Build.log. With --json the
     record is also printed on standard output.
<out> must be missing or empty. Once the arguments are valid, the record is written for every outcome, a failure to
start the build included (a NATIVE_BUILD_ERROR diagnostic), so the editor reads why a build failed from it; its
SourceHash is empty when the build failed before it could hash the sources (not Windows, or a source it could not
read), which both readers accept in the record of a failed build. The editor runs this script by fixed paths with
fixed arguments (EditorCore/Native/NativeBuild.h CreateNativeBuildBackend, §13.2).

It works from any directory (Docs/Architecture.md §2.3): a relative path given on its command line is resolved against
the caller's directory, then main() makes the repository root of its own checkout the working directory.

Exit codes: 0 built, 1 a lint, compile or link failure (the record says which), 2 usage error, 3 no Native/ or no
source, not Windows, the toolchain or premake missing, the build outputs it links missing (bin/<OutputDir>/...), or
--no-wait found the build lock held, 5 a timeout (of the build, or of the wait for the build lock).
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"BuildNative.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import json
import os
import time
from pathlib import Path
from typing import TextIO

import Build  # the MSBuild runner Build.py builds the workspace with (Scripts/ is this script's directory)
import Format  # the format check of decision 35
import Lint  # decision 35's native-module rules
from Lib import locks, native_build, paths, premake, toolchain, vendor_cache, workspace
from Lib.process import ToolNotFoundError
from Lib.report import EXIT_FAILED, EXIT_INIT_FAILED, EXIT_SUCCESS, EXIT_TIMEOUT, Console, configure_stdio

BUILD_TIMEOUT_SECONDS = 1800.0
PREMAKE_TIMEOUT_SECONDS = 300.0
NATIVE_WORKSPACE_SCRIPT = paths.REPOSITORY_ROOT / "Scripts" / "Premake" / "NativeModule.lua"
NATIVE_WORKSPACE_FILE = "NativeModule.slnx"
RUNTIME_PROJECT = "Runtime"
# The configuration's first-party libraries a native Runtime links (Runtime/premake5.lua), below bin/<OutputDir>/.
RUNTIME_LIBRARIES = ("RuntimeCore", "Engine")
# The style the format check formats the sources with: the repository's, named explicitly (format_diagnostics).
FORMAT_STYLE_FILE = paths.REPOSITORY_ROOT / ".clang-format"
# The most characters of clang-format's line a NATIVE_FORMAT_WARNING quotes.
FORMAT_QUOTED_CHARACTERS = 160


class BuildFailure(Exception):
    """The build stops with `exit_code` and one NATIVE_BUILD_ERROR diagnostic (`str(self)`)."""

    def __init__(self, message: str, exit_code: int) -> None:
        super().__init__(message)
        self.exit_code = exit_code


class Tee:
    """The console's stream and the build's log at once: everything the build prints goes to Build.log too."""

    def __init__(self, console: TextIO, log: TextIO) -> None:
        self.console = console
        self.log = log

    def write(self, text: str) -> int:
        self.console.write(text)
        self.log.write(text)
        return len(text)

    def flush(self) -> None:
        self.console.flush()
        self.log.flush()


class NativeBuild:
    """One run of the script: its arguments, what it found and the diagnostics of the record."""

    def __init__(self, arguments: argparse.Namespace, console: Console) -> None:
        self.arguments = arguments
        self.console = console
        self.project = arguments.project
        self.out = arguments.out
        self.kind = native_build.KINDS[0] if arguments.kind == "module" else native_build.KINDS[1]
        self.config: str = arguments.config
        self.sources: list[str] = []
        self.source_hash = ""
        self.diagnostics: list[native_build.Diagnostic] = []
        self.output = ""

    # --- The record ----------------------------------------------------------------------------------------------

    def record(self, succeeded: bool) -> native_build.BuildRecord:
        return native_build.BuildRecord(self.kind, self.config, self.source_hash, self.sources, succeeded,
                                        self.output if succeeded else "", native_build.LOG_FILE, self.diagnostics)

    # --- Steps ---------------------------------------------------------------------------------------------------

    def find_sources(self) -> None:
        if not paths.host().is_windows:
            raise BuildFailure(f"native modules are built on Windows only, not on {platform.system()} (Docs/Decisions/"
                               "0030-m19-contract.md decision 34)", EXIT_INIT_FAILED)
        try:
            self.sources = native_build.list_sources(self.project)
            self.source_hash = native_build.source_hash(self.project, self.config,
                                                        paths.REPOSITORY_ROOT / native_build.API_HEADER)
        except OSError as error:
            raise BuildFailure(f"cannot read the native module's sources: {error}", EXIT_INIT_FAILED) from None
        if not (self.project / native_build.NATIVE_DIRECTORY).is_dir():
            raise BuildFailure(f"{self.project.as_posix()} has no {native_build.NATIVE_DIRECTORY}/ directory: a game's "
                               f"native module is its .cpp and .h files below it", EXIT_INIT_FAILED)
        if not self.sources:
            native = (self.project / native_build.NATIVE_DIRECTORY).as_posix()
            raise BuildFailure(f"{native} holds no .cpp or .h source (write the module's sources there, one naming "
                               f"ENGINE_NATIVE_MODULE)", EXIT_INIT_FAILED)
        self.console.print(f"{len(self.sources)} source(s), source hash {self.source_hash} ({self.config})")

    def lint(self) -> bool:
        """Decision 35's checks; False when they found anything (each finding a NATIVE_LINT_ERROR)."""
        try:
            findings = Lint.lint_native_module(self.project)
        except (Lint.UsageError, OSError) as error:
            raise BuildFailure(f"cannot check the native module's sources: {error}", EXIT_INIT_FAILED) from None
        for finding in findings:
            self.diagnostics.append(native_build.Diagnostic("Error", native_build.LINT_ERROR, finding.file,
                                                            finding.line, 0, f"{finding.code}: {finding.message}"))
            self.console.print(f"{finding.file}:{finding.line}: {finding.code}: {finding.message}")
        self.console.print(f"lint: {len(findings)} finding(s)")
        return not findings

    def check_format(self) -> None:
        """Step 1's format check (format_diagnostics); its NATIVE_FORMAT_WARNING diagnostics fail nothing."""
        diagnostics = format_diagnostics(self.project, self.sources)
        for diagnostic in diagnostics:
            self.diagnostics.append(diagnostic)
            self.console.print(diagnostic.describe())
        self.console.print(f"format: {len(diagnostics)} warning(s)")

    def runtime_library_directories(self, visual_studio: toolchain.VisualStudio, deadline: float) -> list[str]:
        """The directories of every library the native Runtime links (step 3); raises BuildFailure (exit code 3) for
        a missing one."""
        root = paths.REPOSITORY_ROOT
        output = root / "bin" / paths.output_directory_name(self.config)
        hint = f"build them with python Scripts/Build.py --config {self.config} --project {RUNTIME_PROJECT}"
        missing = [output / name / f"{name}.lib" for name in RUNTIME_LIBRARIES
                   if not (output / name / f"{name}.lib").is_file()]
        if missing:
            raise BuildFailure(f"the {self.config} Runtime's libraries are missing ("
                               f"{', '.join(paths.display_path(path) for path in missing)}): {hint}", EXIT_INIT_FAILED)
        try:
            solution = workspace.read_workspace("vs2026", root)
        except workspace.WorkspaceError as error:
            raise BuildFailure(f"{error} (run python Scripts/Generate.py)", EXIT_INIT_FAILED) from None
        assert isinstance(solution, workspace.Solution)
        if RUNTIME_PROJECT not in solution.projects_in(self.config):
            raise BuildFailure(f"the generated workspace has no {self.config} {RUNTIME_PROJECT}", EXIT_INIT_FAILED)

        # The vendored static libraries the prebuilt Runtime links: those it builds with.
        vendored: dict[str, Path] = {}
        for name in sorted(solution.builds_with(RUNTIME_PROJECT)):
            project_file = solution.project_files.get(name)
            if project_file is None or name not in solution.projects_in(self.config):
                continue
            project = vendor_cache.read_project_file(project_file)
            properties = vendor_cache.project_properties(project, self.config, solution.platform, root)
            if properties.get("ConfigurationType") != "StaticLibrary":
                continue
            settings = vendor_cache.project_settings(project, self.config, solution.platform, root)
            if vendor_cache.is_vendor_library(settings):
                vendored[name] = output / name / vendor_cache.library_name(project, settings)

        cached = self.vendor_cache_libraries(solution, visual_studio, {name: solution.project_files[name]
                                                                      for name in vendored}, deadline)
        directories = [(output / name).as_posix() for name in RUNTIME_LIBRARIES]
        absent: list[str] = []
        for name, library in sorted(vendored.items()):
            located = cached.get(name, library)
            if not located.is_file():
                absent.append(paths.display_path(located))
            directories.append(located.parent.as_posix())
        if absent:
            raise BuildFailure(f"the vendored libraries the {self.config} Runtime links are missing "
                               f"({', '.join(absent)}): {hint}", EXIT_INIT_FAILED)
        return directories

    def vendor_cache_libraries(self, solution: workspace.Solution, visual_studio: toolchain.VisualStudio,
                               project_files: dict[str, Path], deadline: float) -> dict[str, Path]:
        """The vendored libraries the shared cache holds for this configuration, as Build.py links them (compiling a
        missing entry); {} when the cache cannot be used, so the workspace's own libraries are linked."""
        log_stem = self.out / native_build.INTERMEDIATE_DIRECTORY / "VendorCache"

        def populate(populate_project: Path, builds: list[vendor_cache.StagedBuild]) -> vendor_cache.PopulateResult:
            result, warnings, errors = Build.run_msbuild(visual_studio, populate_project, [], log_stem, self.arguments,
                                                         self.console, max(1.0, deadline - time.monotonic()))
            return vendor_cache.PopulateResult(result.succeeded, warnings, errors)

        try:
            cache = vendor_cache.VendorCache(vendor_cache.default_root())
            identity = vendor_cache.toolchain_identity(visual_studio.version, visual_studio.vc_tools_version,
                                                       toolchain.find_windows_sdk()[0],
                                                       vendor_cache.user_properties_digest(solution.platform))
            plan = vendor_cache.prepare(cache, project_files, self.config, solution.platform, identity, solution.path,
                                        populate, max(1.0, deadline - time.monotonic()), self.console.print)
        except vendor_cache.PopulateFailedError as error:
            reported = "; ".join(error.result.errors[:3]) or "MSBuild reported no error"
            raise BuildFailure(f"compiling the vendored libraries for the vendor cache failed: {reported}",
                               EXIT_FAILED) from None
        except (vendor_cache.VendorCacheError, toolchain.ToolchainError, OSError) as error:
            self.console.print(f"vendor cache not used: {error}")
            return {}
        for note in plan.notes:
            self.console.print(f"vendor cache: {note}")
        return {entry.project: entry.library for entry in plan.linked}

    def generate(self, library_directories: list[str]) -> Path:
        """Step 4's workspace; returns its .slnx."""
        workspace_directory = self.out / native_build.WORKSPACE_DIRECTORY
        inputs = native_build.write_workspace_inputs(workspace_directory / native_build.WORKSPACE_INPUTS_FILE, {
            "Kind": self.kind,
            "Configuration": self.config,
            "ProjectRoot": self.project.as_posix(),
            "Sources": [(self.project / source).as_posix() for source in self.sources],
            "WorkspaceDirectory": workspace_directory.as_posix(),
            "OutputDirectory": self.out.as_posix(),
            "IntermediateDirectory": (self.out / native_build.INTERMEDIATE_DIRECTORY).as_posix(),
            "LibraryDirectories": library_directories,
        })
        arguments = [f"--file={NATIVE_WORKSPACE_SCRIPT.as_posix()}", f"--native-inputs={inputs.as_posix()}", "--fatal",
                     "--os=windows", "vs2026"]
        try:
            result = premake.run(arguments, timeout=PREMAKE_TIMEOUT_SECONDS)
        except ToolNotFoundError as error:
            raise BuildFailure(f"{error} (run python Scripts/Setup.py)", EXIT_INIT_FAILED) from None
        self.console.print(result.output.rstrip())
        if result.timed_out:
            raise BuildFailure(f"premake {result.describe_exit()}", EXIT_TIMEOUT)
        if not result.succeeded:
            raise BuildFailure(f"premake could not generate the native workspace ({result.describe_exit()}): "
                               f"{result.tail(5)}", EXIT_FAILED)
        solution = workspace_directory / NATIVE_WORKSPACE_FILE
        if not solution.is_file():
            raise BuildFailure(f"premake wrote no {paths.display_path(solution)}", EXIT_FAILED)
        return solution

    def compile(self, visual_studio: toolchain.VisualStudio, solution: Path, deadline: float) -> int:
        """MSBuild on the native workspace; the exit code of the build (0 or 1, 5 for a timeout)."""
        options = [f"-p:Configuration={self.config}", "-p:Platform=x64"]
        log_stem = self.out / native_build.INTERMEDIATE_DIRECTORY / "MSBuild"
        result, warnings, errors = Build.run_msbuild(visual_studio, solution, options, log_stem, self.arguments,
                                                     self.console, max(1.0, deadline - time.monotonic()))
        for line in errors + warnings:
            diagnostic = native_build.parse_tool_diagnostic(line, self.project, paths.REPOSITORY_ROOT)
            if diagnostic is not None and diagnostic not in self.diagnostics:
                self.diagnostics.append(diagnostic)
        if result.timed_out:
            self.diagnostics.append(native_build.Diagnostic("Error", native_build.BUILD_ERROR,
                                                            message=f"MSBuild {result.describe_exit()}"))
            return EXIT_TIMEOUT
        output = self.out / (native_build.MODULE_LIBRARY if self.kind == native_build.KINDS[0]
                             else native_build.RUNTIME_EXECUTABLE)
        failed = [diagnostic for diagnostic in self.diagnostics if diagnostic.severity == "Error"]
        if result.succeeded and not failed and output.is_file():
            self.output = output.name
            return EXIT_SUCCESS
        if not failed:
            # MSBuild failed without a diagnostic of its own (or wrote no output): the record still says why.
            reason = "wrote no " + output.name if result.succeeded else result.describe_exit()
            log = (output.parent / native_build.LOG_FILE).as_posix()
            self.diagnostics.append(native_build.Diagnostic("Error", native_build.BUILD_ERROR,
                                                            message=f"MSBuild failed ({reason}); see {log}"))
        return EXIT_FAILED

    def check_sources_unchanged(self) -> bool:
        """Step 4's last check: the sources hashed again, as find_sources hashed them before the lint. False, with a
        NATIVE_BUILD_ERROR, when the hash differs or the sources cannot be read again."""
        try:
            current = native_build.source_hash(self.project, self.config,
                                               paths.REPOSITORY_ROOT / native_build.API_HEADER)
        except OSError as error:
            reason = f"they cannot be read again: {error}"
        else:
            if current == self.source_hash:
                return True
            reason = f"their source hash is {current} after the build, {self.source_hash} when they were linted"
        self.diagnostics.append(native_build.Diagnostic(
            "Error", native_build.BUILD_ERROR,
            message=f"the sources changed during the build ({reason}), so it may have compiled code the lint did not "
                    f"check: build again once nothing writes them"))
        return False

    def run(self) -> int:
        """Steps 1 to 4; the exit code. Raises BuildFailure."""
        started = time.monotonic()
        deadline = started + self.arguments.timeout
        self.find_sources()
        linted = self.lint()
        self.check_format()
        if not linted:
            return EXIT_FAILED
        try:
            visual_studio = toolchain.find_visual_studio()
        except toolchain.ToolchainError as error:
            raise BuildFailure(str(error), EXIT_INIT_FAILED) from None
        try:
            hold = take_build_lock(self.arguments, self.console)
        except locks.LockUnavailable as error:
            raise BuildFailure(str(error), error.exit_code) from None
        try:
            self.console.print(f"MSBuild ({visual_studio.display_name} {visual_studio.version}, "
                               f"MSVC {visual_studio.vc_tools_version})")
            libraries = (self.runtime_library_directories(visual_studio, deadline)
                         if self.kind == native_build.KINDS[1] else [])
            solution = self.generate(libraries)
            exit_code = self.compile(visual_studio, solution, deadline)
        finally:
            hold.release()
        if exit_code == EXIT_SUCCESS and not self.check_sources_unchanged():
            self.output = ""
            exit_code = EXIT_FAILED
        self.console.print(f"native build: {'built ' + self.output if exit_code == EXIT_SUCCESS else 'failed'} in "
                           f"{time.monotonic() - started:.1f} s")
        return exit_code


def format_diagnostics(project: Path, sources: list[str]) -> list[native_build.Diagnostic]:
    """Step 1's format check of `sources` (project-relative, as list_sources lists them), each formatted by the
    clang-format 22 that Format.py finds (Format.find_clang_format without --clang-format: $CLANG_FORMAT first) with the
    repository's .clang-format named explicitly, since `project` may lie outside the repository, and compared with its
    bytes as Format.py --check compares them. One NATIVE_FORMAT_WARNING for each source that differs, at its first
    differing line, whose message quotes the line clang-format writes there, or says that the line differs only by its
    CRLF ending (the repository's style writes LF), and counts the source's lines that end in CRLF; one for a source
    clang-format fails on; one without a location when no clang-format 22 is found, so no source is checked.
    Every diagnostic is a Warning."""

    def warning(file: str, line: int, message: str) -> native_build.Diagnostic:
        return native_build.Diagnostic("Warning", native_build.FORMAT_WARNING, file, line, 0, message)

    try:
        clang_format, version = Format.find_clang_format(None)
    except toolchain.ToolchainError as error:
        return [warning("", 0, f"the format check did not run: {error}")]
    diagnostics: list[native_build.Diagnostic] = []
    for source in sources:
        path = project / source
        try:
            original = path.read_bytes()
            formatted = Format.formatted_text(clang_format, path, source, FORMAT_STYLE_FILE)
        except (Format.ToolFailure, Format.ToolTimeout, OSError) as error:
            diagnostics.append(warning(source, 0, f"the format check did not run on it: {error}"))
            continue
        if formatted == original:
            continue
        line = Format.first_difference(original, formatted)
        original_lines = original.split(b"\n")
        formatted_lines = formatted.split(b"\n")
        shared = min(len(original_lines), len(formatted_lines))
        # The repository's .clang-format writes LF line endings: a CRLF line differs by its invisible carriage return.
        crlf_lines = original.count(b"\r\n") if b"\r\n" not in formatted else 0
        if line <= shared and original_lines[line - 1] != formatted_lines[line - 1]:
            if crlf_lines > 0 and original_lines[line - 1].removesuffix(b"\r") == formatted_lines[line - 1]:
                others = f" and {crlf_lines - 1} other line(s)" if crlf_lines > 1 else ""
                detail = f"line {line}{others} end in CRLF (\\r\\n), which clang-format writes as LF (\\n)"
                crlf_lines = 0
            else:
                text = formatted_lines[line - 1].decode("utf-8", errors="replace")
                if len(text) > FORMAT_QUOTED_CHARACTERS:
                    text = text[:FORMAT_QUOTED_CHARACTERS] + "..."
                detail = f"clang-format writes line {line} as {json.dumps(text, ensure_ascii=False)}"
        else:
            detail = f"clang-format ends the file differently after line {line} (its final newline or blank lines)"
        if crlf_lines > 0:
            detail += f"; {crlf_lines} line(s) also end in CRLF (\\r\\n), which clang-format writes as LF (\\n)"
        diagnostics.append(warning(source, line, f"not clang-format clean: {detail} (clang-format {version} with the "
                                                 f"repository's .clang-format), so Format.py --check fails on it; "
                                                 f"write it formatted with native.write"))
    return diagnostics


def take_build_lock(arguments: argparse.Namespace, console: Console) -> locks.Hold:
    """A capped slot of the machine-wide build lock (one of the two, beside one other capped build), with at most half
    the logical CPUs as jobs; sets arguments.jobs and arguments.capped for Build.run_msbuild."""
    cpu_count = os.cpu_count() or 1
    hold = locks.acquire_build(locks.CAPPED, locks.wait_seconds(arguments), console.print)
    arguments.jobs = Build.build_jobs(hold.mode, arguments.requested_jobs, cpu_count)
    arguments.capped = hold.capped
    under = " under the hold of a parent process" if hold.inherited else ""
    console.print(f"Build lock: {hold.mode}, {arguments.jobs} job(s){under}")
    return hold


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a game's native module (the .cpp and .h files below <project>/Native/) into the editor's "
                    "shared library or the game's Runtime (Windows only).",
        epilog="Exit codes: 0 built, 1 lint, compile or link failure, 2 usage error, 3 no Native/ or no source, not "
               "Windows, the toolchain or build outputs missing, or --no-wait found the build lock held, 5 timeout.",
    )
    parser.add_argument("--project", type=paths.argument_path, required=True,
                        help="the game project's directory (holding Native/)")
    parser.add_argument("--kind", choices=("module", "runtime"), required=True,
                        help="module: the editor's shared library (Debug or Release); runtime: the game's Runtime "
                             "executable with the module linked in")
    parser.add_argument("--config", choices=native_build.CONFIGURATIONS, required=True, help="the configuration")
    parser.add_argument("--out", type=paths.argument_path, required=True,
                        help="the build's output directory, missing or empty: the output, NativeBuild.json, Build.log")
    parser.add_argument("--jobs", type=int,
                        help="parallel compilers, at most half the logical CPUs (default: half): a capped build")
    parser.add_argument("--timeout", type=float, default=BUILD_TIMEOUT_SECONDS,
                        help=f"seconds for the whole build (default: {BUILD_TIMEOUT_SECONDS:.0f})")
    parser.add_argument("--json", action="store_true", help="print the build record on stdout")
    locks.add_wait_arguments(parser)
    arguments = parser.parse_args(argv)
    if arguments.kind == "module" and arguments.config == "Dist":
        parser.error("--kind module builds the editor's library: --config Debug or Release (Dist has no editor)")
    if arguments.jobs is not None and arguments.jobs < 1:
        parser.error("--jobs must be at least 1")
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    locks.check_wait_arguments(parser, arguments)
    arguments.project = arguments.project.resolve()
    arguments.out = arguments.out.resolve()
    try:
        if arguments.out.exists() and (not arguments.out.is_dir() or any(arguments.out.iterdir())):
            parser.error(f"--out {arguments.out.as_posix()} must be a missing or empty directory")
        arguments.out.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        parser.error(f"--out {arguments.out.as_posix()}: {error}")
    arguments.requested_jobs = arguments.jobs
    arguments.capped = True
    arguments.verbose = False
    return arguments


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    paths.enter_repository_root()
    standard = Console(arguments.json)
    with (arguments.out / native_build.LOG_FILE).open("w", encoding="utf-8", newline="\n") as log:
        console = Console(arguments.json)
        console.stream = Tee(standard.stream, log)  # type: ignore[assignment]
        build = NativeBuild(arguments, console)
        try:
            exit_code = build.run()
        except BuildFailure as failure:
            exit_code = failure.exit_code
            build.diagnostics.append(native_build.Diagnostic("Error", native_build.BUILD_ERROR, message=str(failure)))
            console.print(f"native build: {failure}")
        record = build.record(exit_code == EXIT_SUCCESS)
    native_build.write_record(arguments.out, record)
    if arguments.json:
        sys.stdout.write(native_build.record_text(record))
        sys.stdout.flush()
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
