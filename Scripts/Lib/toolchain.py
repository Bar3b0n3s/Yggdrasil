"""Pinned tool versions and toolchain discovery.

Where the pins live:
- Scripts/Lib/Toolchain.json: the Vulkan SDK and slangc versions (CompileShaders.py requires slangc to match exactly).
- This module: the minimum MSVC, GCC, Clang, Apple Clang and Xcode versions, the clang-format major version and the
  clang-tidy minimum (Architecture §16, CodeStyle §1).
- Scripts/Lib/premake.py: the premake release and its SHA-256 digests.
- .github/workflows/ci.yml: the SHA-256 digests of the Vulkan SDK downloads.

Discovery:
- Windows: Visual Studio 2026 (toolset v145) is located with vswhere, never through PATH or a hard-coded path.
- Linux/macOS: GCC >= 14, Clang >= 18 or Apple Clang >= 17 (Xcode 26, Architecture §16) are selected for the
  make/ninja generators; CC/CXX/AR from the environment take precedence. The archiver must understand the compiler's
  LTO objects (Dist links with LTO), and Clang on Linux links with lld (Dependencies.lua): both are required, never
  replaced by a fallback.
"""

from __future__ import annotations

import dataclasses
import json
import os
import re
import shutil
import sys
from pathlib import Path
from typing import Mapping

from . import paths
from .process import ToolNotFoundError, run_captured

VISUAL_STUDIO_VERSION_RANGE = "[18.0,19.0)"  # Visual Studio 2026, platform toolset v145
VISUAL_STUDIO_NAME = "Visual Studio 2026"
VC_TOOLS_COMPONENT = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
MINIMUM_VC_TOOLS = (14, 51)  # MSVC 14.51 (Architecture §16, CodeStyle §13)
CRT_REDIST_FOLDER = "Microsoft.VC145.CRT"
CRT_REDIST_FILES = ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll")

MINIMUM_GCC = 14
MINIMUM_CLANG = 19  # with libstdc++: Clang 18 reports __cpp_concepts 201907, so libstdc++ hides std::expected
MINIMUM_APPLE_CLANG = 17  # Apple Clang 17 ships with Xcode 26, the minimum Xcode (Architecture §16)
MINIMUM_XCODE = 26
CLANG_FORMAT_MAJOR = 22  # Docs/CodeStyle.md §1
MINIMUM_CLANG_TIDY = 19  # it parses the code with libstdc++, so it needs Clang 19 too (std::expected); Ubuntu 24.04 ships it

# Newest first: the highest installed version that meets the minimum is selected.
_VERSIONED_CANDIDATES = range(30, 0, -1)


class ToolchainError(Exception):
    """A required tool is missing, too old or misconfigured. The message says how to fix it."""


def parse_version(text: str) -> tuple[int, ...]:
    """'14.51.36231' -> (14, 51, 36231). Raises ValueError for anything that is not dot-separated integers."""
    return tuple(int(part) for part in text.strip().split("."))


def version_text(version: tuple[int, ...]) -> str:
    return ".".join(str(part) for part in version)


# --------------------------------------------------------------------------------------------------------------------
# Pins
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Pins:
    vulkan_sdk_version: str
    slangc_version: str


def load_pins() -> Pins:
    try:
        document = json.loads(paths.TOOLCHAIN_PIN_PATH.read_text(encoding="utf-8"))
        vulkan_sdk_version = document["VulkanSdk"]["Version"]
        slangc_version = document["Slangc"]["Version"]
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, KeyError, TypeError) as error:
        raise ToolchainError(
            f"{paths.display_path(paths.TOOLCHAIN_PIN_PATH)} must be valid JSON with VulkanSdk.Version and "
            f"Slangc.Version ({error})"
        ) from None
    if not isinstance(vulkan_sdk_version, str) or not isinstance(slangc_version, str):
        raise ToolchainError(f"{paths.display_path(paths.TOOLCHAIN_PIN_PATH)}: versions must be strings")
    return Pins(vulkan_sdk_version, slangc_version)


# --------------------------------------------------------------------------------------------------------------------
# Vulkan SDK
# --------------------------------------------------------------------------------------------------------------------

_HEADER_VERSION_PATTERN = re.compile(r"^#define\s+VK_HEADER_VERSION\s+(\d+)\s*$", re.MULTILINE)
_HEADER_COMPLETE_PATTERN = re.compile(
    r"^#define\s+VK_HEADER_VERSION_COMPLETE\s+VK_MAKE_API_VERSION\(\s*\d+\s*,\s*(\d+)\s*,\s*(\d+)\s*,", re.MULTILINE
)


def vulkan_sdk_header_version(sdk: Path) -> tuple[int, int, int]:
    """The SDK's Vulkan header version (major, minor, patch) from include/vulkan/vulkan_core.h."""
    candidates = [sdk / "Include" / "vulkan" / "vulkan_core.h", sdk / "include" / "vulkan" / "vulkan_core.h"]
    header = next((candidate for candidate in candidates if candidate.is_file()), None)
    if header is None:
        raise ToolchainError(f"{sdk} has no include/vulkan/vulkan_core.h; VULKAN_SDK does not point to a Vulkan SDK")
    text = header.read_text(encoding="utf-8", errors="replace")
    patch = _HEADER_VERSION_PATTERN.search(text)
    complete = _HEADER_COMPLETE_PATTERN.search(text)
    if not patch or not complete:
        raise ToolchainError(f"could not read VK_HEADER_VERSION from {header}")
    return int(complete.group(1)), int(complete.group(2)), int(patch.group(1))


# --------------------------------------------------------------------------------------------------------------------
# Windows: Visual Studio, MSVC, CRT redistributables, Windows SDK
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class VisualStudio:
    installation_path: Path
    version: str
    display_name: str
    msbuild: Path
    vc_tools_version: str


def find_vswhere() -> Path:
    roots = [os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")]
    for root in filter(None, roots):
        candidate = Path(root) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if candidate.is_file():
            return candidate
    found = shutil.which("vswhere")
    if found:
        return Path(found)
    raise ToolchainError(
        "vswhere.exe not found: install Visual Studio 2026 with the 'Desktop development with C++' workload"
    )


def _query_vswhere(vswhere: Path, prerelease: bool) -> list[dict]:
    command = [
        str(vswhere),
        "-version", VISUAL_STUDIO_VERSION_RANGE,
        "-products", "*",
        "-requires", VC_TOOLS_COMPONENT,
        "-format", "json",
        "-utf8",
        "-nologo",
    ]
    if prerelease:
        command.append("-prerelease")
    result = run_captured(command, timeout=60)
    if not result.succeeded:
        raise ToolchainError(f"vswhere failed ({result.describe_exit()}): {result.tail(5)}")
    try:
        instances = json.loads(result.output or "[]")
    except json.JSONDecodeError as error:
        raise ToolchainError(f"vswhere returned invalid JSON: {error}") from None
    return [instance for instance in instances if isinstance(instance, dict)]


def find_visual_studio() -> VisualStudio:
    """The newest Visual Studio 2026 instance with the x64 C++ tools (release channel preferred over previews)."""
    vswhere = find_vswhere()
    instances = _query_vswhere(vswhere, prerelease=False) or _query_vswhere(vswhere, prerelease=True)
    if not instances:
        raise ToolchainError(
            f"no {VISUAL_STUDIO_NAME} installation with the x64 C++ build tools ({VC_TOOLS_COMPONENT}) found by "
            f"vswhere; install the 'Desktop development with C++' workload"
        )

    def instance_version(instance: dict) -> tuple[int, ...]:
        try:
            return parse_version(instance.get("installationVersion", "0"))
        except ValueError:
            return (0,)

    instance = max(instances, key=instance_version)
    installation = Path(instance["installationPath"])
    msbuild = next(
        (
            candidate
            for candidate in (
                installation / "MSBuild" / "Current" / "Bin" / "amd64" / "MSBuild.exe",
                installation / "MSBuild" / "Current" / "Bin" / "MSBuild.exe",
            )
            if candidate.is_file()
        ),
        None,
    )
    if msbuild is None:
        raise ToolchainError(f"MSBuild.exe not found in {installation}")

    auxiliary = installation / "VC" / "Auxiliary" / "Build"
    version_file = next(
        (
            candidate
            for candidate in (
                auxiliary / "Microsoft.VCToolsVersion.v145.default.txt",
                auxiliary / "Microsoft.VCToolsVersion.default.txt",
            )
            if candidate.is_file()
        ),
        None,
    )
    if version_file is None:
        raise ToolchainError(f"no MSVC toolset in {installation} (no Microsoft.VCToolsVersion file in {auxiliary})")
    vc_tools_version = version_file.read_text(encoding="utf-8").strip()
    compiler = installation / "VC" / "Tools" / "MSVC" / vc_tools_version / "bin" / "Hostx64" / "x64" / "cl.exe"
    if not compiler.is_file():
        raise ToolchainError(f"MSVC {vc_tools_version} is registered but {compiler} is missing; repair Visual Studio")
    try:
        if parse_version(vc_tools_version)[:2] < MINIMUM_VC_TOOLS:
            raise ToolchainError(
                f"MSVC {vc_tools_version} is older than {version_text(MINIMUM_VC_TOOLS)} (toolset v145); update "
                f"{VISUAL_STUDIO_NAME}"
            )
    except ValueError:
        raise ToolchainError(f"unrecognized MSVC version '{vc_tools_version}' in {version_file}") from None

    display_name = instance.get("displayName") or VISUAL_STUDIO_NAME
    version = instance.get("catalog", {}).get("productDisplayVersion") or instance.get("installationVersion", "")
    return VisualStudio(installation, version, display_name, msbuild, vc_tools_version)


def find_crt_redist(visual_studio: VisualStudio) -> tuple[str, Path]:
    """(version, directory) of the newest app-local CRT: VC/Redist/MSVC/<newest>/x64/Microsoft.VC145.CRT."""
    root = visual_studio.installation_path / "VC" / "Redist" / "MSVC"
    candidates: list[tuple[tuple[int, ...], str, Path]] = []
    if root.is_dir():
        for directory in root.iterdir():
            try:
                version = parse_version(directory.name)
            except ValueError:
                continue  # e.g. the "v145" alias folder
            crt = directory / "x64" / CRT_REDIST_FOLDER
            if all((crt / name).is_file() for name in CRT_REDIST_FILES):
                candidates.append((version, directory.name, crt))
    if not candidates:
        raise ToolchainError(
            f"no {CRT_REDIST_FOLDER} with {', '.join(CRT_REDIST_FILES)} under {root}; install the MSVC v145 "
            f"redistributables (part of the C++ workload)"
        )
    _, name, crt = max(candidates)
    return name, crt


def find_windows_sdk() -> tuple[str, Path]:
    """(version, include directory) of the newest installed Windows 10/11 SDK."""
    import winreg

    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SOFTWARE\Microsoft\Windows Kits\Installed Roots",
            0,
            winreg.KEY_READ | winreg.KEY_WOW64_32KEY,
        ) as key:
            root = Path(winreg.QueryValueEx(key, "KitsRoot10")[0])
    except OSError:
        raise ToolchainError("no Windows SDK registered (Windows Kits\\Installed Roots\\KitsRoot10)") from None
    candidates: list[tuple[tuple[int, ...], str, Path]] = []
    include = root / "Include"
    if include.is_dir():
        for directory in include.iterdir():
            try:
                version = parse_version(directory.name)
            except ValueError:
                continue
            if (directory / "um" / "Windows.h").is_file():
                candidates.append((version, directory.name, directory))
    if not candidates:
        raise ToolchainError(f"no Windows SDK headers under {include}; install a Windows 11 SDK with Visual Studio")
    _, name, directory = max(candidates)
    return name, directory


# --------------------------------------------------------------------------------------------------------------------
# clang-format
# --------------------------------------------------------------------------------------------------------------------

_CLANG_FORMAT_VERSION_PATTERN = re.compile(r"clang-format version (\d+)\.(\d+)\.(\d+)")


def clang_format_version(executable: Path) -> tuple[int, int, int]:
    try:
        result = run_captured([str(executable), "--version"], timeout=30)
    except ToolNotFoundError as error:
        raise ToolchainError(str(error)) from None
    match = _CLANG_FORMAT_VERSION_PATTERN.search(result.output)
    if not result.succeeded or not match:
        raise ToolchainError(f"'{executable} --version' failed: {result.tail(3)}")
    return int(match.group(1)), int(match.group(2)), int(match.group(3))


def visual_studio_llvm_directories() -> list[Path]:
    """VC/Tools/Llvm/x64/bin of every Visual Studio installation vswhere reports (previews included), where Visual
    Studio's C++ Clang tools put clang-format, clang-tidy, clang-query and clang-cl. Empty outside Windows."""
    if os.name != "nt":
        return []
    try:
        vswhere = find_vswhere()
        result = run_captured(
            [str(vswhere), "-all", "-prerelease", "-products", "*", "-property", "installationPath", "-utf8"],
            timeout=60,
        )
    except (ToolchainError, ToolNotFoundError):
        return []
    if not result.succeeded:
        return []
    directories = []
    for line in result.output.splitlines():
        candidate = Path(line.strip()) / "VC" / "Tools" / "Llvm" / "x64" / "bin"
        if line.strip() and candidate.is_dir() and candidate not in directories:
            directories.append(candidate)
    return directories


def find_clang_format(explicit: str | None = None) -> tuple[Path, tuple[int, int, int]]:
    """clang-format 22.x and its version: `explicit` (a path or a name on PATH), else $CLANG_FORMAT, else the copy
    bundled with Visual Studio, else clang-format-22 or clang-format on PATH. A candidate of another major version is
    skipped, unless it was named explicitly, which is then an error."""
    named = explicit or os.environ.get("CLANG_FORMAT")
    if named:
        path = Path(named)
        if not path.is_file():
            found = shutil.which(named)
            if not found:
                raise ToolchainError(f"clang-format not found at {named}")
            path = Path(found)
        version = clang_format_version(path)
        if version[0] != CLANG_FORMAT_MAJOR:
            raise ToolchainError(f"{path} is clang-format {version_text(version)}; version {CLANG_FORMAT_MAJOR}.x is "
                                 f"required (Docs/CodeStyle.md §1)")
        return path, version

    executable = "clang-format.exe" if os.name == "nt" else "clang-format"
    candidates = [directory / executable for directory in visual_studio_llvm_directories()]
    for name in (f"clang-format-{CLANG_FORMAT_MAJOR}", "clang-format"):
        found = shutil.which(name)
        if found:
            candidates.append(Path(found))
    seen_versions: list[str] = []
    for candidate in candidates:
        if not candidate.is_file():
            continue
        version = clang_format_version(candidate)
        if version[0] == CLANG_FORMAT_MAJOR:
            return candidate, version
        seen_versions.append(f"{candidate} is {version_text(version)}")
    detail = f" ({'; '.join(seen_versions)})" if seen_versions else ""
    raise ToolchainError(
        f"clang-format {CLANG_FORMAT_MAJOR}.x not found{detail}; it ships with Visual Studio 2026's C++ Clang tools on "
        f"Windows, elsewhere install it (for example pip install \"clang-format=={CLANG_FORMAT_MAJOR}.1.*\")"
    )


# --------------------------------------------------------------------------------------------------------------------
# GCC / Clang (make and ninja generators)
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Compiler:
    family: str  # "gcc", "clang" or "apple-clang"
    cc: str
    cxx: str
    ar: str
    version: tuple[int, ...]
    linker: str = ""  # the linker Clang uses on Linux (ld.lld, Dependencies.lua passes -fuse-ld=lld); empty otherwise

    @property
    def toolset(self) -> str:
        """The premake toolset (--cc) whose flags this compiler understands."""
        return "gcc" if self.family == "gcc" else "clang"

    def describe(self) -> str:
        linker = f" LD={self.linker}" if self.linker else ""
        return f"{self.family} {version_text(self.version)} (CC={self.cc} CXX={self.cxx} AR={self.ar}{linker})"


_CLANG_VERSION_PATTERN = re.compile(r"(Apple )?clang version (\d+)\.(\d+)(?:\.(\d+))?")
_GCC_VERSION_PATTERN = re.compile(r"^(\d+)(?:\.(\d+))?(?:\.(\d+))?$")


def identify_compiler(executable: str) -> tuple[str, tuple[int, ...]]:
    """(family, version) of a C or C++ compiler driver."""
    try:
        result = run_captured([executable, "--version"], timeout=30)
    except ToolNotFoundError as error:
        raise ToolchainError(str(error)) from None
    if not result.succeeded:
        raise ToolchainError(f"'{executable} --version' failed ({result.describe_exit()}): {result.tail(3)}")
    match = _CLANG_VERSION_PATTERN.search(result.output)
    if match:
        family = "apple-clang" if match.group(1) else "clang"
        return family, tuple(int(part) for part in match.groups()[1:] if part is not None)
    dumped = run_captured([executable, "-dumpfullversion"], timeout=30)
    version = _GCC_VERSION_PATTERN.match(dumped.output.strip()) if dumped.succeeded else None
    if not version:
        raise ToolchainError(f"could not identify the compiler '{executable}': {result.tail(1)}")
    return "gcc", tuple(int(part) for part in version.groups() if part is not None)


def minimum_version(family: str) -> int:
    return {"gcc": MINIMUM_GCC, "clang": MINIMUM_CLANG, "apple-clang": MINIMUM_APPLE_CLANG}[family]


def _sibling(executable: str, old: str, new: str) -> str:
    """The companion tool next to a compiler driver: g++-14 -> gcc-14, /usr/bin/clang++-19 -> /usr/bin/clang-19."""
    path = Path(executable)
    name = path.name.replace(old, new, 1)
    return str(path.with_name(name)) if path.parent != Path(".") else name


def _version_suffix(executable: str) -> str:
    """'-19' for clang++-19, '' for an unversioned driver."""
    match = re.search(r"(-\d+)$", Path(executable).name)
    return match.group(1) if match else ""


def _driver_program(cxx: str, name: str) -> str | None:
    """The absolute path of a tool the compiler driver itself would run (its -print-prog-name lookup, which searches
    the driver's installation directory before PATH), or None when the driver does not find it."""
    try:
        result = run_captured([cxx, f"-print-prog-name={name}"], timeout=30)
    except ToolNotFoundError:
        return None
    located = result.output.strip().splitlines()[-1:] if result.succeeded else []
    if located and Path(located[0]).is_absolute() and Path(located[0]).is_file():
        return located[0]
    return None


def _find_tool(cxx: str, names: list[str]) -> str | None:
    """The first of `names` next to the compiler or on PATH, else in the compiler driver's own program path."""
    path = Path(cxx)
    for name in names:
        if path.parent != Path("."):
            located = path.with_name(name)
            if located.is_file():
                return str(located)
        if shutil.which(name):
            return name
    for name in names:
        located_by_driver = _driver_program(cxx, name)
        if located_by_driver is not None:
            return located_by_driver
    return None


def _archiver_for(family: str, cxx: str) -> str:
    """An archiver that understands this compiler's LTO objects (the Dist configuration links with LTO): gcc-ar passes
    GCC's LTO plugin, llvm-ar writes the symbol index of LLVM bitcode, and Apple's ar reads bitcode through libLTO.
    Plain GNU ar would write static libraries without a usable symbol index for LTO objects, so it is never used as a
    fallback."""
    if family == "apple-clang":
        return "ar"
    suffix = _version_suffix(cxx)
    tool = "gcc-ar" if family == "gcc" else "llvm-ar"
    found = _find_tool(cxx, [f"{tool}{suffix}", tool])
    if found is None:
        package = f"gcc{suffix or '-14'}" if family == "gcc" else f"llvm{suffix or '-19'}"
        raise ToolchainError(f"{tool}{suffix} not found for {cxx}: the Dist configuration links with LTO, which needs "
                             f"{tool} (Ubuntu: sudo apt install {package}), or set AR")
    return found


def _linker_for(family: str, cxx: str) -> str:
    """Clang on Linux links with lld (Dependencies.lua adds -fuse-ld=lld): lld links the Dist LTO bitcode natively,
    where the system linker would need the LLVM gold plugin. The driver must find ld.lld in its own program path."""
    if family != "clang" or not sys.platform.startswith("linux"):
        return ""
    located = _driver_program(cxx, "ld.lld")
    if located is None:
        suffix = _version_suffix(cxx)
        raise ToolchainError(f"{cxx} does not find ld.lld: Clang builds on Linux link with lld (-fuse-ld=lld, "
                             f"Dependencies.lua); install it (Ubuntu: sudo apt install lld{suffix or '-19'})")
    return located


def select_compiler(toolset: str, environment: Mapping[str, str]) -> Compiler:
    """The C/C++ compiler for a generated make/ninja workspace whose premake toolset is `toolset` (gcc or clang).

    CC/CXX/AR from the environment win; otherwise the newest installed g++-N/clang++-N (or g++/clang++) that meets
    the minimum version is used, with the matching LTO-capable archiver and, for Clang on Linux, lld. A missing
    archiver or linker raises ToolchainError naming the package to install.
    """
    if toolset not in ("gcc", "clang"):
        raise ToolchainError(f"unsupported toolset '{toolset}' for make/ninja (expected gcc or clang)")
    driver, c_driver = ("g++", "gcc") if toolset == "gcc" else ("clang++", "clang")

    explicit = environment.get("CXX")
    if explicit:
        if not environment.get("CC") and driver not in Path(explicit).name:
            raise ToolchainError(f"CXX={explicit} is set without CC, and the C compiler cannot be derived from its "
                                 f"name; set CC too")
        family, version = identify_compiler(explicit)
        if (family == "gcc") != (toolset == "gcc"):
            raise ToolchainError(
                f"CXX={explicit} is {family}, but the workspace was generated for the {toolset} toolset; regenerate "
                f"with python Scripts/Generate.py --toolset {'gcc' if family == 'gcc' else 'clang'} or change CXX"
            )
        if version[0] < minimum_version(family):
            raise ToolchainError(f"CXX={explicit} is {family} {version_text(version)}; {minimum_version(family)} or "
                                 f"newer is required (Architecture §16)")
        cc = environment.get("CC") or _sibling(explicit, driver, c_driver)
        ar = environment.get("AR") or _archiver_for(family, explicit)
        return Compiler(family, cc, explicit, ar, version, _linker_for(family, explicit))

    candidates = [f"{driver}-{major}" for major in _VERSIONED_CANDIDATES] + [driver]
    rejected: list[str] = []
    for candidate in candidates:
        if not shutil.which(candidate):
            continue
        family, version = identify_compiler(candidate)
        if version[0] >= minimum_version(family):
            cc = environment.get("CC") or _sibling(candidate, driver, c_driver)
            ar = environment.get("AR") or _archiver_for(family, candidate)
            return Compiler(family, cc, candidate, ar, version, _linker_for(family, candidate))
        rejected.append(f"{candidate} is {version_text(version)}")
    found = f" (found: {'; '.join(rejected)})" if rejected else ""
    minimum = MINIMUM_GCC if toolset == "gcc" else MINIMUM_CLANG
    packages = "g++-14" if toolset == "gcc" else "clang-19 llvm-19 lld-19"
    raise ToolchainError(f"no {driver} {minimum} or newer found{found}; install it (Ubuntu: sudo apt install "
                         f"{packages}) or set CXX")


# --------------------------------------------------------------------------------------------------------------------
# Xcode
# --------------------------------------------------------------------------------------------------------------------

_XCODE_VERSION_PATTERN = re.compile(r"Xcode (\d+)(?:\.(\d+))?(?:\.(\d+))?")


def xcode_version() -> tuple[int, ...]:
    try:
        result = run_captured(["xcodebuild", "-version"], timeout=60)
    except ToolNotFoundError:
        raise ToolchainError(f"xcodebuild not found: install Xcode {MINIMUM_XCODE} or newer") from None
    match = _XCODE_VERSION_PATTERN.search(result.output)
    if not result.succeeded or not match:
        raise ToolchainError(
            f"'xcodebuild -version' failed: {result.tail(3)} (install Xcode {MINIMUM_XCODE}+ and select it with "
            f"xcode-select, not only the Command Line Tools)"
        )
    return tuple(int(part) for part in match.groups() if part is not None)
