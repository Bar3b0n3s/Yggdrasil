#!/usr/bin/env python3
"""Format first-party C++ with clang-format 22.x (Docs/CodeStyle.md §1, Docs/Architecture.md §2.3).

Every C and C++ file (.h, .cpp and the extensions CodeStyle §4.1 rules out, so that none escapes formatting) under
Engine/, Editor/, Runtime/, Tests/, Tools/ and Projects/, plus the C++ headers that Resources/Shaders/ shares with the
shaders, is formatted in place with the repository's .clang-format. Vendor/ and build outputs are never touched, and
neither are Slang shaders: CodeStyle §15 keeps them out of clang-format.

--check changes nothing: it lists the files that need formatting, with the first line that differs, and exits 1 when
there are any. It is equivalent to `clang-format --dry-run -Werror --style=file`.

clang-format must be major version 22, because other versions format some constructs differently. It is looked up
as --clang-format, then $CLANG_FORMAT, then Visual Studio's bundled LLVM (VC/Tools/Llvm/x64/bin, found with vswhere),
then clang-format-22 and clang-format on PATH (Scripts/Lib/toolchain.py). On Linux and macOS,
`pip install "clang-format==22.1.*"` provides it.

Exit codes: 0 nothing to change (or everything formatted), 1 --check found files that need formatting, or
clang-format failed on a file, 2 usage error, 3 clang-format is missing or has the wrong version, 5 clang-format
timed out.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from Lib import paths, toolchain
from Lib.report import EXIT_FAILED, EXIT_INIT_FAILED, EXIT_SUCCESS, EXIT_TIMEOUT, EXIT_USAGE

REPOSITORY_ROOT = paths.REPOSITORY_ROOT

# Directories whose C and C++ files are first-party (CodeStyle: "The rules apply to everything under Engine/, Editor/,
# Runtime/, Tests/, Tools/ and Projects/"), and Resources/Shaders/ for the C++ headers shared with Slang.
FORMATTED_ROOTS = ("Engine", "Editor", "Runtime", "Tests", "Tools", "Projects", "Resources/Shaders")
CXX_EXTENSIONS = (".h", ".hpp", ".hh", ".hxx", ".inl", ".c", ".cc", ".cpp", ".cxx")
EXCLUDED_DIRECTORIES = ("Vendor", "bin", "bin-int")
TOOL_TIMEOUT_SECONDS = 120.0
EXIT_NEEDS_FORMATTING = 1


class UsageError(Exception):
    """Bad arguments (exit code 2)."""


class ToolFailure(Exception):
    """clang-format failed on a file (exit code 1)."""


class ToolTimeout(Exception):
    """clang-format did not finish within TOOL_TIMEOUT_SECONDS (exit code 5)."""


@dataclasses.dataclass(frozen=True)
class FileResult:
    path: str  # relative to the repository root, with forward slashes
    changed: bool
    first_difference: int  # 1-based line of the first difference, 0 when unchanged


# --------------------------------------------------------------------------------------------------------------------
# clang-format
# --------------------------------------------------------------------------------------------------------------------


def find_clang_format(override: str | None) -> tuple[Path, str]:
    """The clang-format to use and its version text (Scripts/Lib/toolchain.py; raises ToolchainError)."""
    executable, version = toolchain.find_clang_format(override)
    return executable, toolchain.version_text(version)


# --------------------------------------------------------------------------------------------------------------------
# Files
# --------------------------------------------------------------------------------------------------------------------


def list_repository_files(root: Path) -> list[str]:
    """Tracked and untracked files git does not ignore, relative to root; a directory walk outside a work tree."""
    try:
        result = subprocess.run(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
            cwd=root,
            capture_output=True,
            timeout=TOOL_TIMEOUT_SECONDS,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        result = None
    if result is not None and result.returncode == 0:
        names = {name for name in result.stdout.decode("utf-8", errors="replace").split("\0") if name}
        return sorted(name for name in names if (root / name).is_file())

    files: list[str] = []
    for directory, subdirectories, names in os.walk(root):
        subdirectories[:] = sorted(name for name in subdirectories if name not in paths.WALK_SKIPPED_DIRECTORIES)
        files += [(Path(directory) / name).relative_to(root).as_posix() for name in names]
    return sorted(files)


def is_under(relative: str, directory: str) -> bool:
    return relative == directory or relative.startswith(directory.rstrip("/") + "/")


def first_party_files(root: Path, selected: list[str] | None) -> list[str]:
    return [
        relative
        for relative in list_repository_files(root)
        if relative.lower().endswith(CXX_EXTENSIONS)
        and any(is_under(relative, directory) for directory in FORMATTED_ROOTS)
        and not any(is_under(relative, directory) for directory in EXCLUDED_DIRECTORIES)
        and (selected is None or any(is_under(relative, path) for path in selected))
    ]


def first_difference(original: bytes, formatted: bytes) -> int:
    original_lines = original.split(b"\n")
    formatted_lines = formatted.split(b"\n")
    # The texts may differ in length; past the shorter one, the first extra line is the difference.
    for index, (left, right) in enumerate(zip(original_lines, formatted_lines, strict=False)):
        if left != right:
            return index + 1
    return min(len(original_lines), len(formatted_lines))


def format_file(clang_format: Path, root: Path, relative: str, write: bool) -> FileResult:
    """Run clang-format on one file (its output goes to stdout) and compare; in format mode, replace the file."""
    path = root / relative
    original = path.read_bytes()
    try:
        result = subprocess.run(
            [str(clang_format), "--style=file", "--fallback-style=none", str(path)],
            capture_output=True,
            timeout=TOOL_TIMEOUT_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise ToolTimeout(f"clang-format timed out after {TOOL_TIMEOUT_SECONDS:.0f} s on {relative}") from error
    except OSError as error:
        raise ToolFailure(f"clang-format failed on {relative}: {error}") from error
    if result.returncode != 0:
        raise ToolFailure(
            f"clang-format failed on {relative}: {result.stderr.decode('utf-8', errors='replace').strip()}"
        )
    formatted = result.stdout
    if formatted == original:
        return FileResult(relative, False, 0)
    if write:
        # Write next to the file and replace it, so an interrupted run never leaves a truncated source file.
        handle, temporary = tempfile.mkstemp(prefix=".format-", dir=path.parent)
        try:
            with os.fdopen(handle, "wb") as stream:
                stream.write(formatted)
            os.replace(temporary, path)
        except OSError:
            Path(temporary).unlink(missing_ok=True)
            raise
    return FileResult(relative, True, first_difference(original, formatted))


# --------------------------------------------------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------------------------------------------------


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Format first-party C++ (Engine, Editor, Runtime, Tests, Tools, Projects and the headers shared "
        "with shaders) with clang-format 22.x and the repository's .clang-format; Vendor/ is never touched "
        "(Docs/CodeStyle.md section 1).",
        epilog="Exit codes: 0 nothing to change or everything formatted, 1 --check found files that need formatting "
        "or clang-format failed, 2 usage error, 3 clang-format missing or of the wrong version, 5 timeout.",
    )
    parser.add_argument("--check", action="store_true", help="report files that need formatting without modifying them")
    parser.add_argument(
        "--paths",
        nargs="+",
        metavar="PATH",
        help="only these files or directories (relative to the repository root, or absolute)",
    )
    parser.add_argument(
        "--clang-format", help="path to clang-format 22.x (default: $CLANG_FORMAT, Visual Studio's LLVM, then PATH)"
    )
    parser.add_argument(
        "--jobs", type=int, default=os.cpu_count() or 1, help="parallel clang-format processes (default: CPU count)"
    )
    parser.add_argument("--json", action="store_true", help="print a machine-readable report on stdout")
    return parser.parse_args(argv)


def resolve_paths(root: Path, values: list[str] | None) -> list[str] | None:
    if not values:
        return None
    selected: list[str] = []
    for value in values:
        candidate = Path(value)
        absolute = (candidate if candidate.is_absolute() else root / candidate).resolve()
        if not absolute.exists():
            raise UsageError(f"--paths: {value} does not exist")
        try:
            relative = absolute.relative_to(root).as_posix()
        except ValueError as error:
            raise UsageError(f"--paths: {value} is outside the repository") from error
        if relative == ".":
            return None
        selected.append(relative)
    return selected


def run(arguments: argparse.Namespace) -> dict:
    selected = resolve_paths(REPOSITORY_ROOT, arguments.paths)
    clang_format, version = find_clang_format(arguments.clang_format)
    files = first_party_files(REPOSITORY_ROOT, selected)

    def process(relative: str) -> FileResult:
        return format_file(clang_format, REPOSITORY_ROOT, relative, not arguments.check)

    jobs = max(1, arguments.jobs)
    if jobs == 1 or len(files) <= 1:
        results = [process(relative) for relative in files]
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
            results = list(executor.map(process, files))
    changed = [result for result in results if result.changed]
    return {
        "success": not (arguments.check and changed),
        "mode": "check" if arguments.check else "format",
        "clangFormat": clang_format.as_posix(),
        "version": version,
        "files": len(files),
        "changed": [{"file": result.path, "line": result.first_difference} for result in changed],
    }


def main(argv: list[str] | None = None) -> int:
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    try:
        report = run(arguments)
    except (UsageError, toolchain.ToolchainError, ToolFailure, ToolTimeout, OSError) as error:
        if arguments.json:
            print(json.dumps({"success": False, "error": str(error)}, indent=2))
        print(f"Format.py: error: {error}", file=sys.stderr)
        if not arguments.json:
            print(f"[FAILED] format: {error}")
        if isinstance(error, toolchain.ToolchainError):
            return EXIT_INIT_FAILED
        if isinstance(error, ToolTimeout):
            return EXIT_TIMEOUT
        return EXIT_USAGE if isinstance(error, UsageError) else EXIT_FAILED

    if arguments.json:
        print(json.dumps(report, indent=2))
    else:
        verb = "needs formatting" if arguments.check else "formatted"
        for entry in report["changed"]:
            print(f"{entry['file']}:{entry['line']}: format: {verb} (clang-format {report['version']})")
        if arguments.check:
            state = (
                "passed"
                if report["success"]
                else f"FAILED ({len(report['changed'])} file(s) need formatting; run Scripts/Format.py)"
            )
        else:
            state = f"formatted {len(report['changed'])} file(s)"
        print(
            f"Format: {state} [{report['files']} file(s); clang-format {report['version']} at {report['clangFormat']}]"
        )
        if not report["success"]:
            # The summary line CI.py and PreCommit.py quote for a failing step.
            first = report["changed"][0]
            print(f"[FAILED] format: {len(report['changed'])} file(s) need formatting, first: {first['file']}:"
                  f"{first['line']}")
    if arguments.check and report["changed"]:
        return EXIT_NEEDS_FORMATTING
    return EXIT_SUCCESS


if __name__ == "__main__":
    sys.exit(main())
