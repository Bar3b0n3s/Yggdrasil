"""Repository layout, build configurations and the host platform.

Output directories follow premake5.lua: bin/<OutputDir>/<Project>/ with OutputDir =
"%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}". premake canonicalizes the macOS workspace architecture "ARM64"
to "AARCH64", so a macOS Debug build lands in bin/Debug-macosx-AARCH64/.
"""

from __future__ import annotations

import dataclasses
import functools
import os
import platform
import re
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_ROOT = REPOSITORY_ROOT / "Scripts"
VENDOR_ROOT = REPOSITORY_ROOT / "Vendor"
BIN_ROOT = REPOSITORY_ROOT / "bin"
BIN_INT_ROOT = REPOSITORY_ROOT / "bin-int"
TEST_RESULTS_ROOT = BIN_ROOT / "TestResults"
TOOLCHAIN_PIN_PATH = SCRIPTS_ROOT / "Lib" / "Toolchain.json"
WORKSPACE_SCRIPT = REPOSITORY_ROOT / "premake5.lua"

CONFIGURATIONS = ("Debug", "Release", "Dist")

# Directories a directory walk never enters when the tree is not a git work tree (git ls-files honours .gitignore
# instead): tool caches, virtual environments, vendored code and build outputs.
WALK_SKIPPED_DIRECTORIES = frozenset(
    {
        ".git",
        ".vs",
        ".vscode",
        ".idea",
        ".cache",
        "__pycache__",
        ".venv",
        "node_modules",
        "Vendor",
        "bin",
        "bin-int",
        "Library",
    }
)

# premake5.lua names the workspace once: `WorkspaceName = "<name>"`, then `workspace (WorkspaceName)`.
_WORKSPACE_NAME_PATTERN = re.compile(r'^WorkspaceName\s*=\s*"([^"\n]+)"', re.MULTILINE)


def argument_path(value: str) -> Path:
    """The argparse type of a path given on the command line: made absolute against the caller's directory, which is
    still the working directory while the arguments are parsed (enter_repository_root comes after the parse)."""
    return Path(value).absolute()


def tool_argument(value: str) -> str:
    """The argparse type of a tool given on the command line as a path or as a name looked up on PATH: a value with a
    directory separator is a path, made absolute against the caller's directory like argument_path; a bare name is kept
    as it is, so it stays a PATH lookup."""
    return str(Path(value).absolute()) if any(separator in value for separator in {"/", os.sep}) else value


def enter_repository_root() -> None:
    """Make this checkout's repository root the process's working directory. Every entry script calls it in its main()
    right after parsing its arguments, so it works from any directory (Docs/Architecture.md §2.3): a path given on its
    command line has already been made absolute against the caller's directory (argument_path, tool_argument), and the
    children it starts and the relative paths its tools print resolve in its own checkout's root, never in the
    caller's directory or in another checkout it was started from."""
    os.chdir(REPOSITORY_ROOT)


class UnsupportedHostError(Exception):
    """The scripts do not support this operating system or CPU architecture."""


class WorkspaceScriptError(Exception):
    """premake5.lua cannot be read or does not name the workspace."""


@functools.cache
def workspace_name() -> str:
    """The premake workspace name, read from premake5.lua, which is its single definition (the generated .slnx and
    .xcworkspace are named after it). Code never spells it (Architecture: the display name appears only there)."""
    try:
        text = WORKSPACE_SCRIPT.read_text(encoding="utf-8")
    except OSError as error:
        raise WorkspaceScriptError(f"cannot read {WORKSPACE_SCRIPT}: {error}") from None
    match = _WORKSPACE_NAME_PATTERN.search(text)
    if match is None:
        raise WorkspaceScriptError(f"{WORKSPACE_SCRIPT} does not define the workspace name (WorkspaceName = \"...\")")
    return match.group(1)


@dataclasses.dataclass(frozen=True)
class Host:
    system: str  # premake system name: windows, linux, macosx
    architecture: str  # premake's canonical workspace architecture, as it appears in OutputDir
    machine: str  # normalized CPU of this machine: x86_64 or arm64

    @property
    def is_windows(self) -> bool:
        return self.system == "windows"

    @property
    def is_linux(self) -> bool:
        return self.system == "linux"

    @property
    def is_macos(self) -> bool:
        return self.system == "macosx"


def _normalized_machine() -> str:
    machine = platform.machine().lower()
    if machine in ("amd64", "x86_64", "x64"):
        return "x86_64"
    if machine in ("arm64", "aarch64"):
        return "arm64"
    return machine


def host() -> Host:
    """The host platform, mapped to the workspace's architecture for that system (premake5.lua)."""
    machine = _normalized_machine()
    if sys.platform == "win32":
        # Windows on ARM runs the x64 toolchain and binaries under emulation; the workspace targets x86_64 only.
        if machine not in ("x86_64", "arm64"):
            raise UnsupportedHostError(f"unsupported Windows CPU '{platform.machine()}' (x86_64 required)")
        return Host("windows", "x86_64", machine)
    if sys.platform.startswith("linux"):
        if machine != "x86_64":
            raise UnsupportedHostError(
                f"unsupported Linux CPU '{platform.machine()}': the workspace targets x86_64 on Linux "
                f"(Architecture §16)"
            )
        return Host("linux", "x86_64", machine)
    if sys.platform == "darwin":
        if machine not in ("arm64", "x86_64"):
            raise UnsupportedHostError(f"unsupported macOS CPU '{platform.machine()}'")
        return Host("macosx", "AARCH64", machine)
    raise UnsupportedHostError(f"unsupported operating system '{sys.platform}' (Windows, Linux or macOS required)")


def output_directory_name(config: str, target: Host | None = None) -> str:
    target = target or host()
    return f"{config}-{target.system}-{target.architecture}"


def output_directory(config: str, target: Host | None = None) -> Path:
    """bin/<OutputDir>, the parent of every project's target directory for this configuration."""
    return BIN_ROOT / output_directory_name(config, target)


def executable_name(name: str) -> str:
    return f"{name}.exe" if os.name == "nt" else name


def display_path(path: Path) -> str:
    """Repository-relative forward-slash path when inside the repository, otherwise the absolute path."""
    try:
        return path.resolve().relative_to(REPOSITORY_ROOT).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def tests_run_user_data(report: Path) -> Path:
    """The user-data root (--user-data-dir) that Scripts/Test.py gives the Tests process of the run whose JUnit report
    is `report` (bin/TestResults/<report stem>.xml): <report stem>/UserData beside it (Docs/Architecture.md §2.3). Every
    run of that suite and configuration in the checkout shares it; Test.py and Hardening.py's debug-heap step read the
    run's crash reports below it (crash_report_folder)."""
    return report.with_suffix("") / "UserData"


def crash_report_folder(user_data: Path) -> Path:
    """Where a process of the workspace whose user-data root is `user_data` writes its crash reports (Architecture §4.4
    and §4.13): <root>/<product>/Crashes/, each report crash-<epoch>-<pid>[-<attempt>].txt with a .dmp minidump beside
    it on Windows."""
    return user_data / workspace_name() / "Crashes"


def parse_configurations(text: str, allowed: tuple[str, ...] = CONFIGURATIONS) -> list[str]:
    """Parse a comma-separated configuration list ("Debug,Release"), case-insensitively, keeping the canonical order.

    Raises ValueError naming the allowed values for an unknown or empty entry.
    """
    by_lower = {config.lower(): config for config in allowed}
    selected: set[str] = set()
    for item in text.split(","):
        name = item.strip().lower()
        if name not in by_lower:
            raise ValueError(f"unknown configuration '{item.strip()}' (expected {', '.join(allowed)})")
        selected.add(by_lower[name])
    return [config for config in allowed if config in selected]


def parse_name_list(text: str, allowed: tuple[str, ...], kind: str) -> list[str]:
    """Parse a comma-separated list of names from `allowed` ("all" selects every one), keeping the canonical order."""
    items = [item.strip().lower() for item in text.split(",") if item.strip()]
    if not items:
        raise ValueError(f"empty {kind} list (expected some of {', '.join(allowed)})")
    if items == ["all"]:
        return list(allowed)
    unknown = sorted(set(items) - set(allowed))
    if unknown:
        raise ValueError(f"unknown {kind} '{', '.join(unknown)}' (expected {', '.join(allowed)} or all)")
    return [name for name in allowed if name in items]
