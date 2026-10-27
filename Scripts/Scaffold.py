#!/usr/bin/env python3
"""Regenerate Projects/FeatureTest from its committed scaffolds, or check that they reproduce it (Docs/Architecture.md
§2.3, §15.5; Docs/Decisions/0022-m14-contract.md decision 8).

The FeatureTest content is written by automation batch scaffolds, Projects/FeatureTest/Scaffold/<Name>.jsonl, which
Scaffold/Scaffolds.json lists in run order with the id seed and the renderer of each:

  {"Format": "FeatureTestScaffolds", "Version": 1, "Scaffolds": [{"File": "<Name>.jsonl", "IdSeed": <n>,
   "Renderer": "none" | "vulkan"[, "GoldenDirectories": ["Assets/<dir>", ...]]}, ...]}

A manifest that lists a file twice, or gives two scaffolds the same IdSeed (the second would draw the first one's ids
for its entities and assets), is refused naming both entries, before anything is removed (exit code 1).

Golden scaffolds (Docs/Architecture.md §15.4; ADR 0028's phase-1 ledger, "Golden scenes"): a scaffold with
GoldenDirectories writes golden-image content (scenes, materials, textures), and only below those directories, which it
owns: each is "Assets/" followed by '/'-separated plain names (letters, digits, '_', '-', '.', not starting with '.'),
and none may equal, contain or lie inside a kept directory (below) or another golden directory, its own or another
scaffold's (the manifest is refused naming both). Its scenes are golden scenes, which the golden tests list from this
manifest (Tests/Source/Support/GoldenScene.h). It is regenerated and checked like every other scaffold.

Kept content: Golden.jsonl's outputs below Assets/Scenes/Golden, Assets/Materials/Golden, Assets/Models/Golden and
Assets/Textures/Golden are not regenerated: they are exactly the files Scaffold/Baseline/Provenance.json records below
Assets/, each with the XXH64 of the bytes its recorded run wrote. They change only through a recorded regeneration (ADR
0028's ledger): their requests move into a golden scaffold, whose run writes them again with provenance, and their
baseline entries go (T-FTPROV moved the M9 part this way into GoldenRendererII.jsonl). A baseline entry below Assets/
outside the kept directories is refused (exit code 1).

Regeneration, in place (the default) or in a temporary copy (--check):
  1. remove every output of the scaffolds: every file below Assets/ but the kept files (a file in a kept directory
     that the baseline does not record is removed too), every directory left empty, and the project's Library/ import
     cache, so the in-place result equals the one the check computes from a copy without Library/;
  2. copy Scaffold/Baseline/FeatureTest.eproj over FeatureTest.eproj and Scaffold/Baseline/Provenance.json over
     Automation/Provenance.json;
  3. run each scaffold in order: `Editor --headless --renderer <Renderer> --audio-device none --project <project>
     --id-seed <IdSeed> --batch <project>/Scaffold/<File>` with a temporary --user-data-dir. Each scaffold ends with
     project.refreshAssets, so every .meta, and with it every asset handle, comes from the seeded run. Every file the
     run adds, changes or removes outside Library/ (the *.bak backups and Automation/Provenance.json, which every run
     writes, aside) is checked: a golden scaffold's must lie below its GoldenDirectories, no scaffold may touch another
     golden scaffold's directories, and none may touch anything below a kept directory; a violation fails the
     scaffold's step, naming the files.

--check copies the project without Library/ into a temporary directory, regenerates the copy and fails, naming each file
that differs, is missing or is extra, unless every file outside Library/ is byte-identical to the repository's (the
editor's *.bak backups, which the project's .gitignore excludes, are not compared), and each kept file of the
repository's project holds the bytes its baseline entry records ("kept-differs", "kept-missing"). A
scaffold with Renderer "vulkan" (an .hdr import needs the GPU environment baker) fails without a usable Vulkan device
and names the reason. In place, an editor that has the project open makes the run fail before anything is removed: the
project's Library/Editor.lock is probed on every OS like the engine's ProjectLock::IsHeld (a non-blocking lock of the
lock's byte on Windows, a shared non-blocking flock on Linux and macOS), released at once if it succeeds.

Every run, --check included, holds the machine-wide GPU lock (Scripts/Lib/locks.py; Docs/Architecture.md §2.3, ADR 0028
decision 16), shared by every checkout of the repository, from its start to its end: it waits while another GPU run
or a gate (CI.py) holds it, unless it runs inside that run, reporting the holder (pid, worktree, command), at most
--lock-timeout seconds (exit code 5); --no-wait fails at once instead (exit code 3).

It works from any directory (Docs/Architecture.md §2.3): once it has parsed its arguments, main() makes the repository
root of its own checkout the working directory.

Exit codes: 0 regenerated (or reproduced byte for byte), 1 a scaffold failed or the check found differences, 2 usage
error, 3 the editor or the scaffold manifest is missing, or --no-wait found the GPU lock held, 5 timeout (of a
scaffold, or of the wait for the GPU lock).
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"Scaffold.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import dataclasses
import hashlib
import json
import os
import re
import shutil
import tempfile
import time
from collections.abc import Iterable
from pathlib import Path

if sys.platform == "win32":
    import msvcrt
else:
    import fcntl

from Lib import locks, paths, xxh64
from Lib.process import ToolNotFoundError, child_environment, format_command, run_captured, run_streamed
from Lib.report import (
    EXIT_INIT_FAILED,
    EXIT_SUCCESS,
    Console,
    Status,
    Step,
    configure_stdio,
    emit_json,
    overall_exit_code,
)

PROJECT = paths.REPOSITORY_ROOT / "Projects" / "FeatureTest"
PROJECT_FILE_NAME = "FeatureTest.eproj"
MANIFEST = Path("Scaffold") / "Scaffolds.json"
MANIFEST_FORMAT = "FeatureTestScaffolds"
MANIFEST_VERSION = 1
BASELINE_PROVENANCE = Path("Scaffold") / "Baseline" / "Provenance.json"
# The project's provenance, which every scaffold run rewrites (project-relative, '/' separators).
PROVENANCE = "Automation/Provenance.json"
# The baseline files and where regeneration copies them.
BASELINE = {Path("Scaffold") / "Baseline" / "FeatureTest.eproj": Path(PROJECT_FILE_NAME),
            BASELINE_PROVENANCE: Path(PROVENANCE)}
# The directories of Golden.jsonl's kept outputs, which regeneration keeps (ADR 0022 decision 8): the files the baseline
# provenance records there (read_kept_files).
KEPT_ASSET_DIRECTORIES = ("Assets/Scenes/Golden", "Assets/Materials/Golden", "Assets/Models/Golden",
                          "Assets/Textures/Golden")
GOLDEN_DIRECTORIES = "GoldenDirectories"
ASSETS_DIRECTORY = "Assets"
# One name of a golden directory below Assets/.
DIRECTORY_NAME = re.compile(r"[A-Za-z0-9_-][A-Za-z0-9_.-]*")
CACHE_DIRECTORY = "Library"
# The editor's single-writer lock (Engine/Platform/ProjectLock.h): on Windows an exclusive LockFileEx on one byte at
# WINDOWS_LOCK_OFFSET (far past the holder's pid text), on Linux and macOS an exclusive flock on the whole file.
LOCK_FILE = Path(CACHE_DIRECTORY) / "Editor.lock"
WINDOWS_LOCK_OFFSET = 0x7FFF_FFFF_FFFF_FFFE
BACKUP_SUFFIX = ".bak"
RENDERERS = ("none", "vulkan")
MAX_ID_SEED = 2**64 - 1
SCAFFOLD_TIMEOUT_SECONDS = 1800.0
GPU_PROBE_TIMEOUT_SECONDS = 120.0
DIFFERENCES_IN_DETAIL = 3


@dataclasses.dataclass(frozen=True)
class Scaffold:
    """One entry of Scaffolds.json."""

    file: str
    id_seed: int
    renderer: str
    # A golden scaffold's directories (project-relative, below Assets/); empty for every other scaffold.
    golden_directories: tuple[str, ...] = ()


class ManifestError(Exception):
    """Scaffolds.json or the baseline provenance is missing or malformed."""


def is_below(path: str, directory: str) -> bool:
    """Whether the project-relative `path` is `directory` or lies inside it ('/'-separated)."""
    return path == directory or path.startswith(directory + "/")


def overlaps(first: str, second: str) -> bool:
    """Whether one of two directories equals or contains the other, ignoring letter case (on a case-insensitive file
    system "Assets/scenes/Golden" is the kept "Assets/Scenes/Golden")."""
    return is_below(first.casefold(), second.casefold()) or is_below(second.casefold(), first.casefold())


def read_golden_directories(where: str, value: object) -> tuple[str, ...]:
    """A golden scaffold's GoldenDirectories: a non-empty list of distinct directories, each "Assets/" and plain names.
    Raises ManifestError naming `where` otherwise."""
    if not isinstance(value, list) or not value:
        raise ManifestError(f"{where}: {GOLDEN_DIRECTORIES} must be a non-empty list of directories below "
                            f"{ASSETS_DIRECTORY}/")
    directories: list[str] = []
    for directory in value:
        names = directory.split("/") if isinstance(directory, str) else []
        if len(names) < 2 or names[0] != ASSETS_DIRECTORY \
                or not all(DIRECTORY_NAME.fullmatch(name) for name in names[1:]):
            raise ManifestError(f"{where}: {GOLDEN_DIRECTORIES} entry {json.dumps(directory)} must be "
                                f"'{ASSETS_DIRECTORY}/' followed by '/'-separated names of letters, digits, '_', '-' "
                                "and '.', none starting with '.'")
        kept = next((kept for kept in KEPT_ASSET_DIRECTORIES if overlaps(directory, kept)), None)
        if kept is not None:
            raise ManifestError(f"{where}: {GOLDEN_DIRECTORIES} entry '{directory}' overlaps the kept directory "
                                f"'{kept}' (Golden.jsonl's outputs, which regeneration keeps)")
        earlier = next((earlier for earlier in directories if overlaps(directory, earlier)), None)
        if earlier is not None:
            raise ManifestError(f"{where}: {GOLDEN_DIRECTORIES} entries '{earlier}' and '{directory}' overlap")
        directories.append(directory)
    return tuple(directories)


def read_manifest(project: Path) -> list[Scaffold]:
    """The scaffolds of `project`'s Scaffold/Scaffolds.json in run order. Raises FileNotFoundError when the manifest
    does not exist and ManifestError naming the problem when it is malformed or names a scaffold that does not exist."""
    path = project / MANIFEST
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ManifestError(f"{path}: not readable UTF-8 JSON: {error}") from error
    if not isinstance(document, dict) or document.get("Format") != MANIFEST_FORMAT \
            or document.get("Version") != MANIFEST_VERSION:
        raise ManifestError(f"{path}: expected {{\"Format\": \"{MANIFEST_FORMAT}\", "
                            f"\"Version\": {MANIFEST_VERSION}, ...}}")
    entries = document.get("Scaffolds")
    if not isinstance(entries, list):
        raise ManifestError(f"{path}: \"Scaffolds\" must be a list")
    unknown = sorted(set(document) - {"Format", "Version", "Scaffolds"})
    if unknown:
        raise ManifestError(f"{path}: unknown member '{unknown[0]}'")
    scaffolds: list[Scaffold] = []
    for index, entry in enumerate(entries):
        where = f"{path}: Scaffolds[{index}]"
        if not isinstance(entry, dict) or set(entry) - {GOLDEN_DIRECTORIES} != {"File", "IdSeed", "Renderer"}:
            raise ManifestError(f"{where} must hold exactly File, IdSeed and Renderer, and {GOLDEN_DIRECTORIES} for "
                                "a golden scaffold")
        file, seed, renderer = entry["File"], entry["IdSeed"], entry["Renderer"]
        if not isinstance(file, str) or not file.endswith(".jsonl") or "/" in file or "\\" in file \
                or file.startswith("."):
            raise ManifestError(f"{where}: File must name a .jsonl file in Scaffold/")
        if not (project / "Scaffold" / file).is_file():
            raise ManifestError(f"{where}: Scaffold/{file} does not exist")
        if isinstance(seed, bool) or not isinstance(seed, int) or not 0 <= seed <= MAX_ID_SEED:
            raise ManifestError(f"{where}: IdSeed must be an integer from 0 to 2^64-1")
        if renderer not in RENDERERS:
            raise ManifestError(f"{where}: Renderer must be one of {', '.join(RENDERERS)}")
        first = next((earlier for earlier, scaffold in enumerate(scaffolds) if scaffold.file == file), None)
        if first is not None:
            raise ManifestError(f"{path}: Scaffolds[{first}] and Scaffolds[{index}] both list Scaffold/{file}: each "
                                f"scaffold runs once")
        first = next((earlier for earlier, scaffold in enumerate(scaffolds) if scaffold.id_seed == seed), None)
        if first is not None:
            raise ManifestError(f"{path}: Scaffolds[{first}] ({scaffolds[first].file}) and Scaffolds[{index}] ({file}) "
                                f"have the same IdSeed {seed}: each scaffold needs its own, or the second would create "
                                f"its entities and assets with the first one's ids")
        golden: tuple[str, ...] = ()
        if GOLDEN_DIRECTORIES in entry:
            golden = read_golden_directories(where, entry[GOLDEN_DIRECTORIES])
            for earlier_index, earlier in enumerate(scaffolds):
                shared = next(((mine, theirs) for mine in golden for theirs in earlier.golden_directories
                               if overlaps(mine, theirs)), None)
                if shared is not None:
                    raise ManifestError(f"{path}: Scaffolds[{earlier_index}] ({earlier.file}) and Scaffolds[{index}] "
                                        f"({file}) both own golden directory '{shared[1]}' ('{shared[0]}'): each "
                                        "golden scaffold writes only below its own directories")
        scaffolds.append(Scaffold(file, seed, renderer, golden))
    return scaffolds


def read_kept_files(project: Path) -> dict[str, str]:
    """Golden.jsonl's kept outputs: every path below Assets/ that `project`'s Scaffold/Baseline/Provenance.json
    records, with the XXH64 (16 lower-case hex digits) its entry records. Raises FileNotFoundError when the baseline
    does not exist and ManifestError when it is malformed or records a path below Assets/ outside the kept
    directories."""
    path = project / BASELINE_PROVENANCE
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ManifestError(f"{path}: not readable UTF-8 JSON: {error}") from error
    entries = document.get("Entries") if isinstance(document, dict) else None
    if not isinstance(entries, list):
        raise ManifestError(f"{path}: expected a provenance document with an \"Entries\" list")
    kept: dict[str, str] = {}
    for index, entry in enumerate(entries):
        relative = entry.get("Path") if isinstance(entry, dict) else None
        recorded = entry.get("XXH64") if isinstance(entry, dict) else None
        if not isinstance(relative, str) or not isinstance(recorded, str):
            raise ManifestError(f"{path}: Entries[{index}] needs a Path and an XXH64 string")
        if not is_below(relative, ASSETS_DIRECTORY):
            continue
        if not any(is_below(relative, directory) for directory in KEPT_ASSET_DIRECTORIES):
            raise ManifestError(f"{path}: Entries[{index}] records '{relative}', outside the kept directories "
                                f"({', '.join(KEPT_ASSET_DIRECTORIES)}): the baseline records only Golden.jsonl's "
                                "kept outputs")
        kept[relative] = recorded
    return kept


def check_kept_files(project: Path, kept: dict[str, str]) -> list[str]:
    """One "kept-differs: <path>" or "kept-missing: <path>" line, sorted by path, per kept file of `project` that does
    not hold the bytes its baseline entry records or does not exist: kept outputs change only through a recorded
    regeneration (Docs/Decisions/0028-backlog-program.md, the phase-1 ledger's "Golden scenes")."""
    problems: list[str] = []
    for relative in sorted(kept):
        file = project / relative
        if not file.is_file():
            problems.append(f"kept-missing: {relative}")
        elif xxh64.xxh64_hex(file.read_bytes()) != kept[relative]:
            problems.append(f"kept-differs: {relative}")
    return problems


def project_lock_held(project: Path) -> bool:
    """Whether a process holds `project`'s editor lock (Library/Editor.lock), probed like ProjectLock::IsHeld: a lock
    that this probe can take for a moment (and releases at once) is free, and so is a missing lock file. Raises OSError
    when the lock file exists but cannot be opened or probed for another reason."""
    try:
        handle = (project / LOCK_FILE).open("rb")
    except FileNotFoundError:
        return False
    with handle:
        descriptor = handle.fileno()
        if sys.platform == "win32":
            # _locking locks from the current position; a lock conflicting with the holder's exclusive byte fails at
            # once.
            os.lseek(descriptor, WINDOWS_LOCK_OFFSET, os.SEEK_SET)
            try:
                msvcrt.locking(descriptor, msvcrt.LK_NBLCK, 1)
            except OSError:
                return True
            os.lseek(descriptor, WINDOWS_LOCK_OFFSET, os.SEEK_SET)
            msvcrt.locking(descriptor, msvcrt.LK_UNLCK, 1)
            return False
        try:
            fcntl.flock(descriptor, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except BlockingIOError:
            return True
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        return False


def remove_scaffold_outputs(project: Path, kept: Iterable[str]) -> list[str]:
    """Step 1: removes every file below `project`/Assets except the `kept` ones (project-relative), every directory
    there that is left empty, and the project's Library/. Returns the project-relative paths of the removed files,
    sorted."""
    keep = set(kept)
    removed: list[str] = []
    assets = project / ASSETS_DIRECTORY
    if assets.is_dir():
        # Deepest first, so a directory's files are gone when its turn comes.
        for path in sorted(assets.rglob("*"), key=lambda item: len(item.parts), reverse=True):
            relative = path.relative_to(project).as_posix()
            if path.is_dir() and not path.is_symlink():
                if not any(path.iterdir()):
                    path.rmdir()
            elif relative not in keep:
                path.unlink()
                removed.append(relative)
    cache = project / CACHE_DIRECTORY
    if cache.exists():
        shutil.rmtree(cache)
    return sorted(removed)


def restore_baseline(project: Path) -> None:
    """Step 2: the committed baseline project file and provenance, byte for byte."""
    for source, destination in BASELINE.items():
        target = project / destination
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((project / source).read_bytes())


def editor_executable(config: str) -> Path:
    return paths.output_directory(config) / "Editor" / paths.executable_name("Editor")


def probe_device(editor: Path) -> str | None:
    """None when a headless editor can create its Vulkan device here; otherwise why not."""
    with tempfile.TemporaryDirectory(prefix="Scaffold-GpuProbe-") as directory:
        command = [str(editor), "--headless", "--frames", "1", "--audio-device", "none", f"--user-data-dir={directory}"]
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


def run_scaffold(editor: Path, project: Path, scaffold: Scaffold, console: Console, timeout: float) -> Step:
    """Step 3 for one scaffold: a seeded batch run of `editor` on `project`."""
    name = f"scaffold {scaffold.file}"
    with tempfile.TemporaryDirectory(prefix="Scaffold-UserData-") as user_data:
        command = [str(editor), "--headless", "--renderer", scaffold.renderer, "--audio-device", "none", "--project",
                   str(project), "--id-seed", str(scaffold.id_seed),
                   "--batch", str(project / "Scaffold" / scaffold.file), f"--user-data-dir={user_data}"]
        console.heading(f"{name}: {format_command([paths.display_path(editor), *command[1:]])}")
        try:
            result = run_streamed(command, cwd=paths.REPOSITORY_ROOT, env=child_environment(), timeout=timeout,
                                  echo=console.stream)
        except ToolNotFoundError as error:
            return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)
    data = {"file": scaffold.file, "idSeed": scaffold.id_seed, "renderer": scaffold.renderer}
    if result.timed_out:
        return Step(name, Status.TIMEOUT, f"the batch run {result.describe_exit()}", result.duration, data=data)
    if not result.succeeded:
        data["outputTail"] = result.tail(40)
        errors = [line for line in result.output.splitlines() if "[error]" in line or "[critical]" in line]
        cause = errors[-1] if errors else result.tail(1)
        return Step(name, Status.FAILED, f"the batch run {result.describe_exit()}: {cause}", result.duration,
                    data=data)
    return Step(name, Status.PASSED, f"seed {scaffold.id_seed}, renderer {scaffold.renderer}", result.duration,
                data=data)


def snapshot_files(project: Path) -> dict[str, bytes]:
    """The SHA-256 of every file of `project` outside Library/ (the *.bak backups excluded), by relative path."""
    return {relative: hashlib.sha256(path.read_bytes()).digest() for relative, path in project_files(project).items()}


def find_isolation_violations(scaffold: Scaffold, before: dict[str, bytes], after: dict[str, bytes],
                              scaffolds: list[Scaffold]) -> list[str]:
    """What `scaffold`'s run broke of step 3's rules, one line per file it added, changed or removed (Automation/
    Provenance.json aside), sorted by path: a file below a kept directory; for a golden scaffold, a file outside its
    GoldenDirectories; a file below another golden scaffold's GoldenDirectories. `before` and `after` are
    snapshot_files of the run's project; `scaffolds` the manifest."""
    violations: list[str] = []
    for relative in sorted(set(before) | set(after)):
        if before.get(relative) == after.get(relative) or relative == PROVENANCE:
            continue
        kept = next((directory for directory in KEPT_ASSET_DIRECTORIES if is_below(relative, directory)), None)
        if kept is not None:
            violations.append(f"{relative}: below the kept directory {kept} (Golden.jsonl's outputs change only "
                              "through a recorded regeneration)")
            continue
        if scaffold.golden_directories and not any(is_below(relative, directory)
                                                   for directory in scaffold.golden_directories):
            directories = ", ".join(scaffold.golden_directories)
            violations.append(f"{relative}: outside its {GOLDEN_DIRECTORIES} ({directories})")
            continue
        owner = next((other for other in scaffolds if other.file != scaffold.file
                      and any(is_below(relative, directory) for directory in other.golden_directories)), None)
        if owner is not None:
            violations.append(f"{relative}: below the {GOLDEN_DIRECTORIES} of the golden scaffold {owner.file}")
    return violations


def regenerate(editor: Path, project: Path, scaffolds: list[Scaffold], console: Console,
               timeout: float = SCAFFOLD_TIMEOUT_SECONDS) -> list[Step]:
    """Steps 1 to 3 on `project`; stops at the first failing step."""
    started = time.monotonic()
    if any(scaffold.renderer == "vulkan" for scaffold in scaffolds):
        reason = probe_device(editor)
        if reason is not None:
            needing = ", ".join(scaffold.file for scaffold in scaffolds if scaffold.renderer == "vulkan")
            return [Step("device", Status.FAILED, f"{needing} need(s) a Vulkan device (Renderer vulkan), and there is "
                                                  f"none usable: {reason}")]
    try:
        if project_lock_held(project):
            return [Step("clean", Status.FAILED, f"an editor has {paths.display_path(project)} open (it holds "
                                                 f"{LOCK_FILE.as_posix()}): close it and run again; nothing was "
                                                 "removed")]
        kept = read_kept_files(project)
    except FileNotFoundError:
        return [Step("clean", Status.FAILED, f"{paths.display_path(project / BASELINE_PROVENANCE)} does not exist; "
                                             "nothing was removed")]
    except (ManifestError, OSError) as error:
        return [Step("clean", Status.FAILED, f"{error}; nothing was removed")]
    try:
        removed = remove_scaffold_outputs(project, kept)
        restore_baseline(project)
    except OSError as error:
        return [Step("clean", Status.FAILED, f"cannot remove the scaffold outputs of {paths.display_path(project)} "
                                             f"(is an editor open on it?): {error}")]
    steps = [Step("clean", Status.PASSED, f"removed {len(removed)} scaffold output file(s), kept {len(kept)} file(s) "
                                          "the baseline records and restored the baseline", time.monotonic() - started,
                  data={"removed": len(removed), "kept": len(kept)})]
    console.result(steps[0])
    before = snapshot_files(project)
    for scaffold in scaffolds:
        step = run_scaffold(editor, project, scaffold, console, timeout)
        if not step.failed:
            after = snapshot_files(project)
            violations = find_isolation_violations(scaffold, before, after, scaffolds)
            before = after
            if violations:
                shown = "; ".join(violations[:DIFFERENCES_IN_DETAIL])
                more = len(violations) - DIFFERENCES_IN_DETAIL
                step = Step(step.name, Status.FAILED, f"the run wrote {len(violations)} file(s) it may not touch: "
                                                      f"{shown}" + (f" and {more} more" if more > 0 else ""),
                            step.duration, data={**step.data, "violations": violations})
        console.result(step)
        steps.append(step)
        if step.failed:
            break
    return steps


def project_files(root: Path) -> dict[str, Path]:
    """Every file below `root` outside its Library/, by '/'-separated relative path, except the editor's *.bak backups
    (which every save writes and the project's .gitignore excludes, so they are never part of the committed project)."""
    files: dict[str, Path] = {}
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        if relative.parts[0] == CACHE_DIRECTORY or path.suffix == BACKUP_SUFFIX or not path.is_file():
            continue
        files[relative.as_posix()] = path
    return files


def compare_projects(expected: Path, actual: Path) -> list[str]:
    """One line per file that differs between the two projects outside Library/: "differs", "missing" (in `actual`)
    or "extra" (only in `actual`), sorted by path."""
    left, right = project_files(expected), project_files(actual)
    differences: list[str] = []
    for relative in sorted(set(left) | set(right)):
        if relative not in right:
            differences.append(f"missing: {relative}")
        elif relative not in left:
            differences.append(f"extra: {relative}")
        elif left[relative].read_bytes() != right[relative].read_bytes():
            differences.append(f"differs: {relative}")
    return differences


def copy_without_cache(source: Path, destination: Path) -> None:
    shutil.copytree(source, destination, ignore=lambda directory, names: [CACHE_DIRECTORY]
                    if Path(directory) == source and CACHE_DIRECTORY in names else [])


def check(editor: Path, scaffolds: list[Scaffold], console: Console) -> list[Step]:
    """--check: regenerates a copy of the project without Library/ and compares it with the repository's."""
    with tempfile.TemporaryDirectory(prefix="Scaffold-Check-") as directory:
        copy = Path(directory) / PROJECT.name
        copy_without_cache(PROJECT, copy)
        steps = regenerate(editor, copy, scaffolds, console)
        if any(step.failed for step in steps):
            return steps
        started = time.monotonic()
        # The copy's baseline is the repository's, which the clean step has read and validated.
        differences = compare_projects(PROJECT, copy) + check_kept_files(PROJECT, read_kept_files(copy))
    for difference in differences:
        console.print(f"DIFFERENCE: Projects/FeatureTest/{difference.split(': ', 1)[1]} "
                      f"({difference.split(':', 1)[0]})")
    if differences:
        shown = "; ".join(differences[:DIFFERENCES_IN_DETAIL])
        more = len(differences) - DIFFERENCES_IN_DETAIL
        step = Step("check", Status.FAILED, f"{len(differences)} file(s) of Projects/FeatureTest are not reproduced by "
                                            f"the scaffolds: {shown}" + (f" and {more} more" if more > 0 else ""),
                    time.monotonic() - started, data={"differences": differences})
    else:
        step = Step("check", Status.PASSED, "the scaffolds reproduce Projects/FeatureTest byte for byte outside "
                                            "Library/", time.monotonic() - started)
    console.result(step)
    return [*steps, step]


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    parser = argparse.ArgumentParser(
        description="Regenerate Projects/FeatureTest from Scaffold/Scaffolds.json, or check that the scaffolds "
                    "reproduce it byte for byte (Architecture §2.3, §15.5).",
        epilog="Exit codes: 0 success, 1 a scaffold failed or the check found differences, 2 usage error, 3 the "
               "editor or the scaffold manifest is missing, or --no-wait found the GPU lock held, 5 timeout.",
    )
    parser.add_argument("--config", choices=("Debug", "Release"), default="Release",
                        help="the editor configuration that runs the scaffolds (default: Release)")
    parser.add_argument("--check", action="store_true",
                        help="regenerate a temporary copy and fail unless it is byte-identical outside Library/; the "
                             "repository's project is never written")
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    locks.add_wait_arguments(parser)
    arguments = parser.parse_args(sys.argv[1:] if argv is None else argv)
    paths.enter_repository_root()
    locks.check_wait_arguments(parser, arguments)
    console = Console(arguments.json)

    steps: list[Step]
    try:
        hold = locks.acquire_gpu(locks.wait_seconds(arguments), console.print)
    except locks.LockUnavailable as error:
        steps = [locks.unavailable_step(error)]
        console.result(steps[0])
    else:
        with hold:
            steps = scaffold_steps(arguments, console)
    exit_code = overall_exit_code(steps)
    console.summary("Scaffold summary", steps)
    if arguments.json:
        emit_json({"success": exit_code == EXIT_SUCCESS, "exitCode": exit_code, "check": arguments.check,
                   "steps": [step.to_json() for step in steps]})
    return exit_code


def scaffold_steps(arguments: argparse.Namespace, console: Console) -> list[Step]:
    """The regeneration or the check, under the GPU lock."""
    editor = editor_executable(arguments.config)
    steps: list[Step]
    if not editor.is_file():
        steps = [Step("editor", Status.FAILED, f"{paths.display_path(editor)} not found: run python Scripts/Build.py "
                                               f"--config {arguments.config} --project Editor",
                      exit_code=EXIT_INIT_FAILED)]
    else:
        try:
            scaffolds = read_manifest(PROJECT)
        except FileNotFoundError:
            steps = [Step("manifest", Status.FAILED, f"{paths.display_path(PROJECT / MANIFEST)} does not exist",
                          exit_code=EXIT_INIT_FAILED)]
        except ManifestError as error:
            steps = [Step("manifest", Status.FAILED, str(error))]
        else:
            console.heading(f"{'Checking' if arguments.check else 'Regenerating'} Projects/FeatureTest with "
                            f"{len(scaffolds)} scaffold(s) and the {arguments.config} editor")
            steps = check(editor, scaffolds, console) if arguments.check else regenerate(editor, PROJECT, scaffolds,
                                                                                         console)
    for step in steps:
        if step.failed and step.name in ("editor", "manifest", "device", "clean"):
            console.result(step)
    return steps


if __name__ == "__main__":
    sys.exit(main())
