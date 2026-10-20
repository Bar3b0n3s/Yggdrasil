"""The shared prebuilt vendor-library cache (Docs/Architecture.md §2.2, §2.3; Docs/Decisions/0028-backlog-program.md
decision 16, T-VCACHE).

Every checkout of the repository compiles the vendored static libraries (the projects whose compiled sources all lie
under Vendor/) from the same pinned sources with the same settings, so Scripts/Build.py links them from one cache shared
by every worktree instead of compiling them in each. Only MSBuild builds use it (Windows, the only verified platform);
the make, ninja and Xcode builds compile the vendored libraries as before.

Location: <git common directory>/vendor-cache (`git rev-parse --git-common-dir`: D:/Engine/.git for the main checkout
and every worktree of it). It is shared by exactly the checkouts that share the repository, lies outside every working
tree (so no checkout's bin/ removal or `git clean` touches it), and git never tracks it. Removing the directory is safe
only while no build runs in any checkout (Scripts/Locks.py lists no build holder): a running build links the entries it
verified where they are. The next build compiles what it needs and publishes it again. Entries are never evicted: the
sets of an older toolchain or vendor change stay until the directory is removed (Docs/Backlog.md).

Layout (FORMAT 1), below <root>/v1:
  entries/<Project>/<Configuration>-<key>/  a published entry: entry.json, the library and its compiler PDBs
  staging/<id>/                              a build compiling missing entries (MSBuild's OutDir and IntDir per project)
  trash/<id>/                                entries that failed verification, removed once nothing uses them
  populate.lock, populate.owner.json         held, and described, while a build compiles and publishes entries

The key is the first KEY_LENGTH hex digits of the SHA-256 of an entry's inputs as canonical JSON (project_inputs):
  - the project's generated build settings for the configuration and platform (its .vcxproj): every property, tool
    setting and compiled file with its per-file settings, so every define (the ABI-relevant ones of AGENTS.md among
    them), the floating-point model and options, the instruction set (Jolt's /arch:SSE4.2), the sanitizer (EnableASAN),
    the runtime library, optimization and debug-information settings and the platform toolset (v145, or ClangCL for
    clang-cl). Paths are relative to the repository root and the output and intermediate directories are left out, so
    every checkout and every workspace location (in place, --to bin-int/Asan or bin-int/Portability) computes the same
    key for the same build;
  - the toolchain: the Visual Studio version, the MSVC tools version, the Windows SDK version and the content of the
    per-user property sheet every generated project imports (user_properties_digest). MSBuild compiles with exactly
    that toolchain: Build.py removes from its environment the variables that would select another toolset or add
    compiler options (TOOLCHAIN_ENVIRONMENT_VARIABLES: VCToolsVersion, LLVMInstallDir, CL, ...);
  - the source pin of every Vendor/<Lib> directory the project compiles from, includes or force-includes (its own,
    Vendor/imgui for ImGuizmo, Vendor/stb for miniaudio): the version and commit its VENDOR.md records, and a digest of
    the content of every file git lists there (tracked, or untracked and not ignored: VENDOR.md, premake5.lua and the
    sources).
A project that names an input the key cannot digest is never cached (UnkeyedInputError; it compiles in the workspace,
fail-closed): an include, library, #using or forced-include path outside Vendor/ (a first-party configuration header in
Engine/Config, say) or built from an MSBuild property (an SDK's $(VULKAN_SDK)/Include), or an item outside Vendor/.
Equal inputs give equal keys, and a change to any input gives another key, so an entry built from other inputs is never
found under the current key. Entries are immutable. FORMAT is part of every key and names the directory: raise it
whenever the way entries are compiled, recorded or verified changes, so no entry of the old way is ever linked.

Publishing is atomic: a missing entry is compiled in staging/ (its own OutDir and IntDir per project), its library and
compiler PDBs move into a staging directory with its entry.json (the inputs, the SHA-256 and size of every file, the
warnings the compile reported), and that directory is renamed to the entry's name. A reader therefore sees either no
entry or a complete one. Builds that compile entries hold populate.lock, so one build compiles a missing entry while the
others wait and then link it.

Before linking, every entry is verified (verify_entry): its record must parse, have this format, hash to its key and
record exactly the current inputs, and every file must have its recorded size and SHA-256. An entry that fails is never
linked: it moves to trash/ and is compiled and published again. The linker finds each library's compiler PDB beside the
library (the path recorded in the objects names the staging directory, which no longer exists), so debug information
is complete.

Linking in place: Build.py passes MSBuild ForceImportAfterCppTargets (the documented hook that imports a .targets file
after Microsoft.Cpp.targets in every C++ project of the build) with the file write_link_override writes into the
workspace's bin-int/VendorCache/: a cached vendor project builds nothing, every project drops its ProjectReference to
it, and every linking project lists the cached library in Link.AdditionalDependencies. The generated workspace itself
is unchanged, so its project sets stay those of the other generators (CI.py's portability check), a build in the IDE
or with Build.py --no-vendor-cache compiles the vendored libraries as before, and no build output is ever copied
between checkouts (MSBuild's .tlog files hold absolute paths): consumers link the cache's files where they are.
"""

from __future__ import annotations

import contextlib
import dataclasses
import datetime
import hashlib
import json
import os
import re
import secrets
import shutil
import sys
import time
import xml.etree.ElementTree as ElementTree
from collections.abc import Callable, Iterable, Iterator, Mapping
from pathlib import Path
from typing import Any

from . import paths
from .process import ToolNotFoundError, run_captured

FORMAT = 1
CACHE_DIRECTORY_NAME = "vendor-cache"
KEY_LENGTH = 32
MANIFEST_NAME = "entry.json"
LOCK_NAME = "populate.lock"
OWNER_NAME = "populate.owner.json"
LOCK_POLL_SECONDS = 0.5
GIT_TIMEOUT_SECONDS = 60.0
# The text that stands for the repository root in recorded warnings, so another checkout reports them at its own paths.
REPOSITORY_PLACEHOLDER = "{repository}"
# The workspace-relative directory of the generated MSBuild files (write_link_override, write_populate_project).
WORKSPACE_DIRECTORY = Path("bin-int") / "VendorCache"
SKIP_TARGETS_NAME = "VendorCacheSkip.targets"
NO_REFERENCES_NAME = "NoProjectReferences.targets"

MSBUILD_NAMESPACE = "http://schemas.microsoft.com/developer/msbuild/2003"
_NAMESPACE = f"{{{MSBUILD_NAMESPACE}}}"
_CONDITION_PATTERN = re.compile(r"^\s*'\$\(Configuration\)\|\$\(Platform\)'\s*==\s*'([^|']*)\|([^']*)'\s*$")
# Properties that name where this checkout writes its output: never part of the key.
EXCLUDED_PROPERTIES = frozenset({"OutDir", "IntDir"})
# Item types that do not change what the project compiles (premake's links of one static library to another become
# ProjectReference items, and the librarian does not merge them).
EXCLUDED_ITEM_TYPES = frozenset({"ProjectReference", "ProjectConfiguration"})
# Settings that hold ;-separated lists of paths a compile reads, normalized to repository-relative paths.
_PATH_LIST_SETTING = re.compile(
    r"(IncludeDirectories|IncludePath|LibraryDirectories|LibraryPath|UsingDirectories|ForcedIncludeFiles|"
    r"ForcedUsingFiles)$")
# An entry of a path list that only inherits the list's earlier value: %(Name) or $(Name) of the setting itself.
_INHERITANCE_PATTERN = re.compile(r"^[%$]\((?P<name>[A-Za-z_][A-Za-z0-9_]*)\)$")
COMPILE_ITEM_TYPES = frozenset({"ClCompile"})
LINKING_CONFIGURATION_TYPES = frozenset({"Application", "DynamicLibrary"})
VENDOR_DIRECTORY = paths.VENDOR_ROOT.name  # repository-relative
# Characters MSBuild gives a meaning in property values, item specifications and conditions.
_MSBUILD_SPECIAL = re.compile(r"[%$@;'?*]")
# A VENDOR.md table row: "| Version | 5.6.0 (...) |", "| Version / tag | `0.741` |", "| Commit | `e77f...` (...) |".
_PIN_ROW_PATTERN = re.compile(r"^\|\s*(Version[^|]*|Commit)\s*\|\s*(.*?)\s*\|\s*$", re.MULTILINE)
_COMMIT_PATTERN = re.compile(r"\b[0-9a-f]{40}\b")
_MSBUILD_NODE_PREFIX = re.compile(r"^\s*\d+(?::\d+)?>")


class VendorCacheError(Exception):
    """The cache cannot be used for this build (no git repository, an unreadable project file, an unusable directory,
    a timeout while waiting for another build). The build then compiles the vendored libraries itself."""


class EntryExistsError(VendorCacheError):
    """An entry is already published under the key: entries are immutable and never overwritten."""


class UnkeyedInputError(VendorCacheError):
    """A project names an input the key cannot digest (unkeyed_inputs): it is never cached, so it compiles in the
    workspace."""


# --------------------------------------------------------------------------------------------------------------------
# Location
# --------------------------------------------------------------------------------------------------------------------


def git_common_directory(repository_root: Path = paths.REPOSITORY_ROOT) -> Path:
    """The repository's git common directory (shared by every worktree). Raises VendorCacheError without git or a
    repository."""
    command = ["git", "-C", str(repository_root), "rev-parse", "--path-format=absolute", "--git-common-dir"]
    try:
        result = run_captured(command, timeout=GIT_TIMEOUT_SECONDS)
    except ToolNotFoundError as error:
        raise VendorCacheError(f"git is not available ({error})") from None
    text = result.output.strip().splitlines()[-1].strip() if result.output.strip() else ""
    if not result.succeeded or not text:
        raise VendorCacheError(f"{repository_root} is not in a git repository ({result.describe_exit()})")
    return Path(text)


def default_root(repository_root: Path = paths.REPOSITORY_ROOT) -> Path:
    """<git common directory>/vendor-cache, shared by every worktree of the repository."""
    return git_common_directory(repository_root) / CACHE_DIRECTORY_NAME


# Environment variables MSBuild or the compiler would read to select another toolchain than the one the key records, or
# to add compiler options to every compile (environment variables become MSBuild properties, and VCToolsVersion,
# VCToolsInstallDir, LLVMInstallDir and LLVMToolsVersion are only defaulted when empty; cl.exe reads CL and _CL_).
# Build.py removes them from MSBuild's environment (msbuild_environment), so MSBuild compiles with exactly the recorded
# toolchain.
TOOLCHAIN_ENVIRONMENT_VARIABLES = ("VCToolsVersion", "VCToolsInstallDir", "LLVMInstallDir", "LLVMToolsVersion", "CL",
                                   "_CL_")
# The per-user property sheet every generated project imports ($(UserRootDir)\Microsoft.Cpp.<Platform>.user.props, the
# user root being %LOCALAPPDATA%\Microsoft\MSBuild\v4.0).
USER_PROPERTIES_DIRECTORY = Path("Microsoft") / "MSBuild" / "v4.0"


def msbuild_environment(environment: Mapping[str, str]) -> dict[str, str]:
    """`environment` without TOOLCHAIN_ENVIRONMENT_VARIABLES (compared without letter case, as Windows and MSBuild
    compare them), for every MSBuild run of Build.py."""
    removed = {name.upper() for name in TOOLCHAIN_ENVIRONMENT_VARIABLES}
    return {name: value for name, value in environment.items() if name.upper() not in removed}


def user_properties_digest(platform: str, environment: Mapping[str, str] | None = None) -> str:
    """The SHA-256 of the per-user property sheet (Microsoft.Cpp.<platform>.user.props below %LOCALAPPDATA%), which
    can set any compiler option of every project; "absent" without one."""
    local = (os.environ if environment is None else environment).get("LOCALAPPDATA", "")
    if not local:
        return "absent"
    path = Path(local) / USER_PROPERTIES_DIRECTORY / f"Microsoft.Cpp.{platform}.user.props"
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except FileNotFoundError:
        return "absent"
    except OSError as error:
        raise VendorCacheError(f"cannot read {path}: {error}") from None


def toolchain_identity(visual_studio_version: str, msvc_version: str, windows_sdk_version: str,
                       user_properties: str) -> dict[str, str]:
    """The toolchain part of every key: an update of Visual Studio (its compilers, clang-cl and MSBuild's C++ targets),
    of the MSVC tools or of the Windows SDK, or a change of the per-user property sheet (user_properties_digest), gives
    every entry a new key."""
    return {"visualStudio": visual_studio_version, "msvc": msvc_version, "windowsSdk": windows_sdk_version,
            "userProperties": user_properties}


# --------------------------------------------------------------------------------------------------------------------
# Project files and keys
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class ProjectFile:
    """A generated Visual Studio project file (.vcxproj)."""

    path: Path
    root: ElementTree.Element

    @property
    def name(self) -> str:
        return self.path.stem


def read_project_file(path: Path) -> ProjectFile:
    try:
        return ProjectFile(path.resolve(), ElementTree.parse(path).getroot())
    except (OSError, ElementTree.ParseError) as error:
        raise VendorCacheError(f"cannot read {path}: {error}") from None


def _tag(element: ElementTree.Element) -> str:
    return element.tag[len(_NAMESPACE):] if element.tag.startswith(_NAMESPACE) else element.tag


def _applies(element: ElementTree.Element, config: str, platform: str) -> bool:
    """Whether `element` applies to `config`|`platform`: no condition, or premake's configuration condition naming
    them. Any other condition (premake writes none on settings or items) counts as applying, so whatever it guards is
    part of the key."""
    condition = element.get("Condition")
    if condition is None:
        return True
    match = _CONDITION_PATTERN.match(condition)
    return match is None or (match.group(1) == config and match.group(2) == platform)


def _replace_root(text: str, root: Path, replacement: str) -> str:
    """`text` with every spelling of the absolute path `root` (either separator, any letter case) replaced."""
    for spelling in {str(root), root.as_posix()}:
        text = re.sub(re.escape(spelling), lambda _: replacement, text, flags=re.IGNORECASE)
    return text


def repository_path(base: Path, value: str, repository_root: Path) -> str:
    """A path from a project file (relative to the absolute directory `base`, either separator) as a repository-relative
    POSIX path, or an absolute POSIX path outside the repository. Pure path arithmetic: generated projects name tens of
    thousands of files, and the file system is never asked."""
    text = value.replace("\\", "/")
    absolute = os.path.normpath(text if os.path.isabs(text) else os.path.join(base, text))
    root = os.path.normpath(repository_root)
    if os.path.normcase(absolute) == os.path.normcase(root):
        return "."
    prefix = root if root.endswith(os.sep) else root + os.sep
    if os.path.normcase(absolute).startswith(os.path.normcase(prefix)):
        return Path(absolute[len(prefix):]).as_posix()
    return Path(absolute).as_posix()


def _normalize_value(name: str, text: str | None, base: Path, repository_root: Path) -> str:
    value = (text or "").strip()
    if _PATH_LIST_SETTING.search(name):
        entries = []
        for entry in value.split(";"):
            entry = entry.strip()
            if entry and not entry.startswith(("$(", "%(")):
                entry = repository_path(base, entry, repository_root)
            entries.append(entry)
        value = ";".join(entries)
    return _replace_root(value, repository_root, REPOSITORY_PLACEHOLDER)


def project_properties(project: ProjectFile, config: str, platform: str,
                       repository_root: Path = paths.REPOSITORY_ROOT) -> dict[str, str]:
    """Every property of the groups that apply to `config`|`platform`, later definitions winning, OutDir and IntDir
    left out, paths relative to the repository root."""
    properties: dict[str, str] = {}
    for group in project.root.iter(f"{_NAMESPACE}PropertyGroup"):
        if not _applies(group, config, platform):
            continue
        for child in group:
            name = _tag(child)
            if name not in EXCLUDED_PROPERTIES and _applies(child, config, platform):
                properties[name] = _normalize_value(name, child.text, project.path.parent, repository_root)
    return dict(sorted(properties.items()))


def project_settings(project: ProjectFile, config: str, platform: str,
                     repository_root: Path = paths.REPOSITORY_ROOT) -> dict[str, Any]:
    """The project's build settings for `config`|`platform`, as the key records them: "properties"
    (project_properties), "tools" (the item definitions: ClCompile, Lib, Link, ... each name -> text) and "items" (every
    item with its type, repository-relative path and the per-file settings that apply), with paths relative to the
    repository root."""
    base = project.path.parent
    properties = project_properties(project, config, platform, repository_root)
    tools: dict[str, dict[str, str]] = {}
    for group in project.root.iter(f"{_NAMESPACE}ItemDefinitionGroup"):
        if not _applies(group, config, platform):
            continue
        for tool in group:
            tool_name = _tag(tool)
            if tool_name in EXCLUDED_ITEM_TYPES:
                continue
            values = tools.setdefault(tool_name, {})
            for child in tool:
                if _applies(child, config, platform):
                    values[_tag(child)] = _normalize_value(_tag(child), child.text, base, repository_root)
    items: list[dict[str, Any]] = []
    for group in project.root.iter(f"{_NAMESPACE}ItemGroup"):
        if not _applies(group, config, platform):
            continue
        for item in group:
            item_type = _tag(item)
            include = item.get("Include")
            if item_type in EXCLUDED_ITEM_TYPES or include is None or not _applies(item, config, platform):
                continue
            metadata = {_tag(child): _normalize_value(_tag(child), child.text, base, repository_root)
                        for child in item if _applies(child, config, platform)}
            items.append({"type": item_type, "path": repository_path(base, include, repository_root),
                          "metadata": dict(sorted(metadata.items()))})
    items.sort(key=lambda item: (item["type"], item["path"]))
    return {
        "properties": dict(sorted(properties.items())),
        "tools": {name: dict(sorted(values.items())) for name, values in sorted(tools.items())},
        "items": items,
    }


def compiled_sources(settings: Mapping[str, Any]) -> list[str]:
    """The repository-relative files `settings` compiles (ClCompile items not excluded from the build)."""
    return [item["path"] for item in settings["items"]
            if item["type"] in COMPILE_ITEM_TYPES and item["metadata"].get("ExcludedFromBuild", "").lower() != "true"]


def is_vendor_library(settings: Mapping[str, Any]) -> bool:
    """A vendored static library: a StaticLibrary that compiles at least one file, every one of them under Vendor/."""
    sources = compiled_sources(settings)
    return (settings["properties"].get("ConfigurationType") == "StaticLibrary" and bool(sources)
            and all(source.startswith(f"{VENDOR_DIRECTORY}/") for source in sources))


def library_name(project: ProjectFile, settings: Mapping[str, Any]) -> str:
    """The library file the project builds: TargetName and TargetExt (premake writes both)."""
    properties = settings["properties"]
    return properties.get("TargetName", project.name) + properties.get("TargetExt", ".lib")


def unkeyed_inputs(settings: Mapping[str, Any]) -> list[str]:
    """The inputs of `settings` (project_settings) whose content the key cannot digest, each as "<setting>: <entry>":
    an entry of a path-list setting (include, library and #using directories, forced includes) that is neither under
    Vendor/ nor an inheritance of the setting's earlier value (%(Name), or $(Name) of the setting itself), so a path
    elsewhere in the repository, outside it, or built from an MSBuild property (an SDK's $(VULKAN_SDK)/Include); and an
    item outside Vendor/. Empty for a project the key covers."""
    found: list[str] = []
    for values in [settings["properties"], *settings["tools"].values()]:
        for name, value in values.items():
            if not _PATH_LIST_SETTING.search(name):
                continue
            for entry in (entry.strip() for entry in value.split(";")):
                inheritance = _INHERITANCE_PATTERN.match(entry)
                if not entry or (inheritance and (entry.startswith("%") or inheritance.group("name") == name)):
                    continue
                if "$(" in entry or "%(" in entry or not entry.startswith(f"{VENDOR_DIRECTORY}/"):
                    found.append(f"{name}: {entry}")
    found += [f"{item['type']}: {item['path']}" for item in settings["items"]
              if not item["path"].startswith(f"{VENDOR_DIRECTORY}/")]
    return found


def vendor_directories(settings: Mapping[str, Any]) -> list[str]:
    """The Vendor/<Lib> directories the project compiles from, includes or force-includes, sorted."""
    referenced = [item["path"] for item in settings["items"]]
    for values in [settings["properties"], *settings["tools"].values()]:
        for name, value in values.items():
            if _PATH_LIST_SETTING.search(name):
                referenced += [entry for entry in value.split(";") if entry]
    directories = set()
    for path in referenced:
        parts = path.split("/")
        if len(parts) >= 2 and parts[0] == VENDOR_DIRECTORY:
            directories.add(f"{VENDOR_DIRECTORY}/{parts[1]}")
    return sorted(directories)


def read_pin(directory: Path) -> dict[str, str]:
    """The version and commit a Vendor/<Lib>/VENDOR.md records in its table ("" when absent)."""
    try:
        text = (directory / "VENDOR.md").read_text(encoding="utf-8")
    except OSError:
        return {"version": "", "commit": ""}
    pin = {"version": "", "commit": ""}
    for label, value in _PIN_ROW_PATTERN.findall(text):
        if label.startswith("Version") and not pin["version"]:
            pin["version"] = value.replace("`", "")
        elif label == "Commit" and not pin["commit"]:
            commit = _COMMIT_PATTERN.search(value)
            pin["commit"] = commit.group(0) if commit else value
    return pin


class SourceDigester:
    """Digests of the files git lists in vendor directories (tracked, or untracked and not ignored), each file hashed
    at most once per instance."""

    def __init__(self, repository_root: Path = paths.REPOSITORY_ROOT) -> None:
        self.repository_root = repository_root
        self._files: dict[str, list[str]] = {}
        self._hashes: dict[str, str] = {}

    def files(self, directory: str) -> list[str]:
        """The files git lists below the repository-relative `directory` (one git call lists all of Vendor/)."""
        top = directory.split("/", 1)[0]
        if top not in self._files:
            command = ["git", "-C", str(self.repository_root), "ls-files", "--cached", "--others", "--exclude-standard",
                       "-z", "--", top]
            try:
                result = run_captured(command, timeout=GIT_TIMEOUT_SECONDS)
            except ToolNotFoundError as error:
                raise VendorCacheError(f"git is not available ({error})") from None
            if not result.succeeded:
                raise VendorCacheError(f"git ls-files {top} failed ({result.describe_exit()}): {result.tail(3)}")
            self._files[top] = sorted(set(name for name in result.output.split("\0") if name.strip()))
        prefix = os.path.normcase(directory.rstrip("/") + "/")
        listed = [name for name in self._files[top] if os.path.normcase(name).startswith(prefix)]
        if not listed:
            # Every vendored directory holds at least its VENDOR.md: an empty listing would leave its sources unkeyed.
            raise VendorCacheError(f"git lists no file in {directory}")
        return listed

    def file_hash(self, name: str) -> str:
        if name not in self._hashes:
            try:
                self._hashes[name] = hashlib.sha256((self.repository_root / name).read_bytes()).hexdigest()
            except FileNotFoundError:
                self._hashes[name] = "missing"  # listed by the index, deleted in the working tree
            except OSError as error:
                raise VendorCacheError(f"cannot read {name}: {error}") from None
        return self._hashes[name]

    def digest(self, directories: Iterable[str]) -> tuple[str, int]:
        """(SHA-256 over every listed file's path and content hash, number of files)."""
        digest = hashlib.sha256()
        count = 0
        for directory in sorted(set(directories)):
            for name in self.files(directory):
                digest.update(f"{name}\0{self.file_hash(name)}\n".encode("utf-8"))
                count += 1
        return digest.hexdigest(), count


def project_inputs(project: ProjectFile, config: str, platform: str, toolchain: Mapping[str, str],
                   digester: SourceDigester) -> dict[str, Any]:
    """Everything an entry's library depends on (see the module documentation); entry_key hashes it. Raises
    UnkeyedInputError, naming them, when the project names inputs the key cannot digest (unkeyed_inputs)."""
    settings = project_settings(project, config, platform, digester.repository_root)
    unkeyed = unkeyed_inputs(settings)
    if unkeyed:
        raise UnkeyedInputError(f"{project.name} ({config}) names inputs the key cannot digest, so it is never cached: "
                                f"{'; '.join(unkeyed)}")
    directories = vendor_directories(settings)
    digest, count = digester.digest(directories)
    return {
        "format": FORMAT,
        "project": project.name,
        "configuration": config,
        "platform": platform,
        "toolchain": dict(sorted(toolchain.items())),
        "settings": settings,
        "sources": {
            "directories": directories,
            "pins": {directory: read_pin(digester.repository_root / directory) for directory in directories},
            "digest": digest,
            "files": count,
        },
    }


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def entry_key(inputs: Any) -> str:
    return hashlib.sha256(canonical_json(inputs)).hexdigest()[:KEY_LENGTH]


def first_difference(recorded: Any, expected: Any, location: str = "") -> str | None:
    """The first place where two JSON values differ ("settings.tools.ClCompile.PreprocessorDefinitions: 'A' instead of
    'B'"), or None when they are equal."""
    if isinstance(recorded, dict) and isinstance(expected, dict):
        for name in sorted(set(recorded) | set(expected)):
            where = f"{location}.{name}" if location else name
            if name not in recorded:
                return f"{where}: missing"
            if name not in expected:
                return f"{where}: not expected"
            found = first_difference(recorded[name], expected[name], where)
            if found is not None:
                return found
        return None
    if isinstance(recorded, list) and isinstance(expected, list):
        for index, (left, right) in enumerate(zip(recorded, expected)):
            found = first_difference(left, right, f"{location}[{index}]")
            if found is not None:
                return found
        if len(recorded) != len(expected):
            return f"{location}: {len(recorded)} entries instead of {len(expected)}"
        return None
    if recorded != expected:
        return f"{location or 'value'}: {json.dumps(recorded)} instead of {json.dumps(expected)}"
    return None


# --------------------------------------------------------------------------------------------------------------------
# The cache directory
# --------------------------------------------------------------------------------------------------------------------


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _try_lock(descriptor: int) -> bool:
    """Takes the exclusive lock of an open lock file without waiting; False while another process holds it."""
    if sys.platform == "win32":
        import msvcrt

        os.lseek(descriptor, 0, os.SEEK_SET)
        try:
            msvcrt.locking(descriptor, msvcrt.LK_NBLCK, 1)
        except OSError:
            return False
        return True
    import fcntl

    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return False
    return True


def _unlock(descriptor: int) -> None:
    if sys.platform == "win32":
        import msvcrt

        os.lseek(descriptor, 0, os.SEEK_SET)
        msvcrt.locking(descriptor, msvcrt.LK_UNLCK, 1)
        return
    import fcntl

    fcntl.flock(descriptor, fcntl.LOCK_UN)


def _remove_tree(path: Path) -> bool:
    """Removes a directory tree; False when something in it is still in use (it stays for a later cleanup)."""
    shutil.rmtree(path, ignore_errors=True)
    return not path.exists()


class VendorCache:
    """The cache at `root` (default_root()); FORMAT selects the v<FORMAT> directory below it."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.directory = root / f"v{FORMAT}"

    @property
    def entries(self) -> Path:
        return self.directory / "entries"

    @property
    def staging(self) -> Path:
        return self.directory / "staging"

    @property
    def trash(self) -> Path:
        return self.directory / "trash"

    def entry_directory(self, project: str, config: str, key: str) -> Path:
        return self.entries / project / f"{config}-{key}"

    def read_manifest(self, entry: Path) -> tuple[dict[str, Any] | None, list[str]]:
        """(the entry's record, its problems): no record, unreadable JSON, another format, or a key that is not the
        directory's or not the hash of the recorded inputs."""
        path = entry / MANIFEST_NAME
        try:
            manifest = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return None, [f"{MANIFEST_NAME} is missing (not a published entry)"]
        except (OSError, ValueError) as error:
            return None, [f"{MANIFEST_NAME} is unreadable: {error}"]
        if not isinstance(manifest, dict) or not isinstance(manifest.get("inputs"), dict):
            return None, [f"{MANIFEST_NAME} is not an entry record"]
        problems = []
        if manifest.get("format") != FORMAT:
            problems.append(f"its format is {manifest.get('format')!r}, not {FORMAT}")
        key = manifest.get("key")
        if not entry.name.endswith(f"-{key}"):
            problems.append(f"its record names key {key!r}, not the directory's")
        if entry_key(manifest["inputs"]) != key:
            problems.append("its recorded inputs do not hash to its key (the record was changed)")
        return manifest, problems

    def verify(self, entry: Path, inputs: Mapping[str, Any]) -> list[str]:
        """Why the entry at `entry` must not be linked for `inputs` (empty when it is complete and intact): its record
        (read_manifest), recorded inputs other than `inputs`, and any file whose size or SHA-256 differs from the record
        or that is missing."""
        manifest, problems = self.read_manifest(entry)
        if manifest is None:
            return problems
        difference = first_difference(manifest["inputs"], dict(inputs))
        if difference is not None:
            problems.append(f"it records other inputs than this build's ({difference})")
        artifacts = manifest.get("artifacts")
        if not isinstance(artifacts, dict) or manifest.get("library") not in artifacts:
            return problems + ["its record lists no library"]
        for name, record in sorted(artifacts.items()):
            path = entry / name
            if not isinstance(record, dict) or not path.is_file():
                problems.append(f"{name} is missing")
                continue
            if path.stat().st_size != record.get("size"):
                problems.append(f"{name} is {path.stat().st_size} bytes, not the recorded {record.get('size')}")
            elif file_sha256(path) != record.get("sha256"):
                problems.append(f"{name} does not have its recorded SHA-256 (corrupt)")
        return problems

    def publish(self, staged: Path, inputs: Mapping[str, Any], library: str, warnings: Iterable[str] = (),
                seconds: float = 0.0, checkout: Path = paths.REPOSITORY_ROOT) -> Path:
        """Publishes the files in the directory `staged` (the library and its compiler PDBs) as the entry for `inputs`:
        entry.json is written into `staged` first, then `staged` is renamed to the entry's directory in one step, so a
        reader sees no entry or a complete one. Raises EntryExistsError when the entry exists (entries are immutable;
        callers hold populate_lock) and VendorCacheError when `library` is not in `staged`."""
        key = entry_key(dict(inputs))
        target = self.entry_directory(str(inputs["project"]), str(inputs["configuration"]), key)
        files = sorted(path for path in staged.iterdir() if path.is_file() and path.name != MANIFEST_NAME)
        if library not in {path.name for path in files}:
            raise VendorCacheError(f"{staged} holds no {library}")
        manifest = {
            "format": FORMAT,
            "key": key,
            "project": inputs["project"],
            "configuration": inputs["configuration"],
            "library": library,
            "artifacts": {path.name: {"size": path.stat().st_size, "sha256": file_sha256(path)} for path in files},
            "warnings": [_replace_root(warning, checkout, REPOSITORY_PLACEHOLDER) for warning in warnings],
            "inputs": inputs,
            "created": {"time": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
                        "checkout": checkout.as_posix(), "pid": os.getpid(), "seconds": round(seconds, 1)},
        }
        record = staged / MANIFEST_NAME
        with record.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps(manifest, indent="\t", sort_keys=True) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            raise EntryExistsError(f"{target} is already published")
        try:
            os.rename(staged, target)
        except OSError as error:
            if target.exists():
                raise EntryExistsError(f"{target} is already published") from None
            raise VendorCacheError(f"cannot publish {target}: {error}") from None
        return target

    def discard(self, entry: Path) -> bool:
        """Moves an entry that failed verification out of entries/ (into trash/, removed when nothing uses it); False
        when it is in use and cannot move (callers then compile the library in the workspace)."""
        self.trash.mkdir(parents=True, exist_ok=True)
        destination = self.trash / f"{entry.parent.name}-{entry.name}-{secrets.token_hex(4)}"
        try:
            os.rename(entry, destination)
        except FileNotFoundError:
            return True
        except OSError:
            return False
        _remove_tree(destination)
        return True

    def remove_leftovers(self) -> None:
        """Removes staging and trash directories of builds that ended (callers hold populate_lock, so no build of this
        cache still uses them); a directory still in use stays for a later build."""
        for directory in (self.staging, self.trash):
            if directory.is_dir():
                for child in directory.iterdir():
                    _remove_tree(child)

    def lock_owner(self) -> str:
        """Who holds populate_lock, as populate.owner.json describes it ("an unknown process" without one)."""
        try:
            owner = json.loads((self.directory / OWNER_NAME).read_text(encoding="utf-8"))
            return f"pid {owner['pid']} in {owner['checkout']} since {owner['since']}"
        except (OSError, ValueError, KeyError, TypeError):
            return "an unknown process"

    @contextlib.contextmanager
    def populate_lock(self, timeout: float, on_wait: Callable[[str], None]) -> Iterator[None]:
        """Holds the cache's exclusive lock (populate.lock; msvcrt on Windows, fcntl elsewhere), waiting up to `timeout`
        seconds for another build to release it; `on_wait` is told once who holds it. Raises VendorCacheError on a
        timeout or when the cache directory cannot be written."""
        try:
            self.directory.mkdir(parents=True, exist_ok=True)
            descriptor = os.open(self.directory / LOCK_NAME, os.O_RDWR | os.O_CREAT, 0o666)
        except OSError as error:
            raise VendorCacheError(f"cannot open {self.directory / LOCK_NAME}: {error}") from None
        try:
            deadline = time.monotonic() + timeout
            told = False
            while not _try_lock(descriptor):
                if time.monotonic() >= deadline:
                    raise VendorCacheError(f"timed out after {timeout:.0f} s waiting for {self.directory / LOCK_NAME}, "
                                           f"held by {self.lock_owner()}")
                if not told:
                    on_wait(f"waiting for the vendor cache: {self.lock_owner()} is compiling entries")
                    told = True
                time.sleep(LOCK_POLL_SECONDS)
            owner = self.directory / OWNER_NAME
            try:
                owner.write_text(json.dumps({"pid": os.getpid(), "checkout": paths.REPOSITORY_ROOT.as_posix(),
                                             "since": datetime.datetime.now().isoformat(timespec="seconds")}),
                                 encoding="utf-8")
                yield
            finally:
                owner.unlink(missing_ok=True)
                _unlock(descriptor)
        finally:
            os.close(descriptor)


# --------------------------------------------------------------------------------------------------------------------
# MSBuild files
# --------------------------------------------------------------------------------------------------------------------


def msbuild_literal(path: Path) -> str:
    """`path` as a Windows path for an MSBuild file. Raises VendorCacheError for a path holding a character MSBuild
    gives a meaning (% $ @ ; ' ? *), which the generated files could not express safely."""
    text = str(path)
    if _MSBUILD_SPECIAL.search(text):
        raise VendorCacheError(f"{text} holds a character MSBuild interprets ({_MSBUILD_SPECIAL.pattern})")
    return text


def _same_path(property_name: str, path: Path) -> str:
    return f"'$({property_name})' == '{msbuild_literal(path)}'"


def _write_project(path: Path, project: ElementTree.Element, comment: str) -> Path:
    """Writes an MSBuild file through a temporary file, leaving an identical one untouched (MSBuild's up-to-date checks
    see the files a build imports, so rewriting one would make the next build redo work)."""
    ElementTree.indent(project, space="  ")
    text = (f'<?xml version="1.0" encoding="utf-8"?>\n<!-- {comment} -->\n'
            + ElementTree.tostring(project, encoding="unicode") + "\n")
    try:
        if path.read_text(encoding="utf-8") == text:
            return path
    except OSError:
        pass  # absent or unreadable: written below
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text, encoding="utf-8", newline="\n")
    os.replace(temporary, path)
    return path


def consumers(project_files: Iterable[Path], config: str,
              platform: str) -> tuple[dict[Path, list[Path]], set[Path]]:
    """(each project file -> the project files its ProjectReference items name, the project files that link in
    `config`|`platform`: ConfigurationType Application or DynamicLibrary), every path resolved."""
    references: dict[Path, list[Path]] = {}
    linking: set[Path] = set()
    for path in project_files:
        project = read_project_file(path)
        named: list[Path] = []
        for group in project.root.iter(f"{_NAMESPACE}ItemGroup"):
            if not _applies(group, config, platform):
                continue
            for item in group:
                include = item.get("Include")
                if _tag(item) == "ProjectReference" and include and _applies(item, config, platform):
                    named.append((project.path.parent / include.replace("\\", "/")).resolve())
        references[project.path] = named
        if project_properties(project, config, platform).get("ConfigurationType") in LINKING_CONFIGURATION_TYPES:
            linking.add(project.path)
    return references, linking


@dataclasses.dataclass
class LinkedEntry:
    """A vendored project of this build whose library is linked from the cache."""

    project: str
    project_file: Path
    entry: Path
    library: Path
    published: bool  # compiled and published by this build
    warnings: list[str]  # the warnings its compile reported, at this checkout's paths


def write_link_override(path: Path, linked: list[LinkedEntry], references: Mapping[Path, list[Path]],
                        linking: set[Path]) -> Path:
    """Writes the ForceImportAfterCppTargets file of a build (and SKIP_TARGETS_NAME beside it): each project of
    `linked` builds nothing; no project keeps a ProjectReference to one; each linking project (`linking`: its project
    file, ConfigurationType Application or DynamicLibrary) lists the cached library of every linked project it
    references (`references`: project file -> the project files of its ProjectReference items) in
    Link.AdditionalDependencies. Returns `path`."""
    by_file = {entry.project_file: entry for entry in linked}
    project = ElementTree.Element("Project", xmlns=MSBUILD_NAMESPACE)
    for entry in linked:
        group = ElementTree.SubElement(project, "PropertyGroup",
                                       Condition=_same_path("MSBuildProjectFullPath", entry.project_file))
        ElementTree.SubElement(group, "EngineVendorCacheLibrary").text = msbuild_literal(entry.library)
    ElementTree.SubElement(project, "Import", Project=SKIP_TARGETS_NAME,
                           Condition="'$(EngineVendorCacheLibrary)' != ''")
    removals = ElementTree.SubElement(project, "ItemGroup")
    for entry in linked:
        ElementTree.SubElement(removals, "ProjectReference", Remove=msbuild_literal(entry.project_file))
    for consumer in sorted(linking):
        libraries = [by_file[reference].library for reference in references.get(consumer, []) if reference in by_file]
        if libraries:
            group = ElementTree.SubElement(project, "PropertyGroup",
                                           Condition=_same_path("MSBuildProjectFullPath", consumer))
            ElementTree.SubElement(group, "EngineVendorCacheLibraries").text = ";".join(
                msbuild_literal(library) for library in libraries)
    definitions = ElementTree.SubElement(project, "ItemDefinitionGroup",
                                         Condition="'$(EngineVendorCacheLibraries)' != ''")
    link = ElementTree.SubElement(definitions, "Link")
    ElementTree.SubElement(link, "AdditionalDependencies").text = (
        "%(AdditionalDependencies);$(EngineVendorCacheLibraries)")
    _write_project(path, project, "Generated by Scripts/Build.py: vendored libraries linked from the shared cache "
                                  "(Scripts/Lib/vendor_cache.py)")

    skip = ElementTree.Element("Project", xmlns=MSBUILD_NAMESPACE)
    build = ElementTree.SubElement(skip, "Target", Name="Build", Returns="$(EngineVendorCacheLibrary)")
    ElementTree.SubElement(build, "Message", Importance="high",
                           Text="  $(MSBuildProjectName) -> $(EngineVendorCacheLibrary) (vendor cache)")
    ElementTree.SubElement(skip, "Target", Name="Rebuild", DependsOnTargets="Build",
                           Returns="$(EngineVendorCacheLibrary)")
    _write_project(path.parent / SKIP_TARGETS_NAME, skip,
                   "Generated by Scripts/Build.py: a vendored project whose library is linked from the shared cache "
                   "builds nothing (Scripts/Lib/vendor_cache.py)")
    return path


@dataclasses.dataclass
class StagedBuild:
    """A missing entry compiled by this build, in its own staging directory."""

    project_file: Path
    inputs: dict[str, Any]
    library: str
    directory: Path

    @property
    def project(self) -> str:
        return str(self.inputs["project"])

    @property
    def output_directory(self) -> Path:
        return self.directory / "out"

    @property
    def intermediate_directory(self) -> Path:
        return self.directory / "obj"


def write_populate_project(path: Path, builds: list[StagedBuild], solution: Path) -> Path:
    """Writes the MSBuild project that compiles every staged build in parallel, each with the solution's
    configuration, platform and solution properties and its own OutDir and IntDir in its staging directory, and
    NO_REFERENCES_NAME beside it, which every staged build imports (ForceImportAfterCppTargets): it drops the project's
    ProjectReference items, so no referenced project is evaluated with the staged build's directories (a static library
    compiles without its references, and the librarian never merges another library into it). Returns `path`."""
    references = ElementTree.Element("Project", xmlns=MSBUILD_NAMESPACE)
    ElementTree.SubElement(ElementTree.SubElement(references, "ItemGroup"), "ProjectReference",
                           Remove="@(ProjectReference)")
    no_references = _write_project(path.parent / NO_REFERENCES_NAME, references,
                                   "Generated by Scripts/Build.py: a vendored library compiled for the shared cache "
                                   "builds without its project references (Scripts/Lib/vendor_cache.py)")
    project = ElementTree.Element("Project", xmlns=MSBUILD_NAMESPACE, DefaultTargets="Build")
    items = ElementTree.SubElement(project, "ItemGroup")
    solution_properties = [
        f"SolutionDir={msbuild_literal(solution.parent)}\\",
        f"SolutionPath={msbuild_literal(solution)}",
        f"SolutionName={solution.stem}",
        f"SolutionFileName={solution.name}",
        f"SolutionExt={solution.suffix}",
        f"ForceImportAfterCppTargets={msbuild_literal(no_references)}",
    ]
    for build in builds:
        item = ElementTree.SubElement(items, "VendorProject", Include=msbuild_literal(build.project_file))
        properties = [
            f"Configuration={build.inputs['configuration']}",
            f"Platform={build.inputs['platform']}",
            f"OutDir={msbuild_literal(build.output_directory)}\\",
            f"IntDir={msbuild_literal(build.intermediate_directory)}\\",
            "BuildProjectReferences=false",
            *solution_properties,
        ]
        ElementTree.SubElement(item, "AdditionalProperties").text = ";".join(properties)
    target = ElementTree.SubElement(project, "Target", Name="Build")
    ElementTree.SubElement(target, "MSBuild", Projects="@(VendorProject)", BuildInParallel="true",
                           StopOnFirstFailure="false")
    _write_project(path, project, "Generated by Scripts/Build.py: vendored libraries compiled for the shared cache "
                                  "(Scripts/Lib/vendor_cache.py)")
    return path


def staged_files(build: StagedBuild) -> list[Path]:
    """The library and every compiler PDB the staged build wrote (MSBuild puts a static library's PDB in OutDir, older
    toolsets in IntDir as vc<version>.pdb). Raises VendorCacheError when the library is missing or two PDBs share a
    name (the linker looks a PDB up by its name beside the library)."""
    library = build.output_directory / build.library
    if not library.is_file():
        raise VendorCacheError(f"{build.project}: the build wrote no {library}")
    files = [library]
    names = {build.library.lower()}
    for directory in (build.output_directory, build.intermediate_directory):
        for pdb in sorted(directory.glob("*.pdb")):
            if pdb.name.lower() in names:
                raise VendorCacheError(f"{build.project}: two compiler PDBs named {pdb.name}")
            names.add(pdb.name.lower())
            files.append(pdb)
    return files


def attributed_warnings(warnings: Iterable[str], project_file: Path) -> list[str]:
    """The diagnostics MSBuild attributed to `project_file` (each line ends with "[<project file>]"), without the
    "<node>:<project>>" prefix of a parallel build, which says nothing about the library."""
    suffix = f"[{project_file}]".lower()
    return [_MSBUILD_NODE_PREFIX.sub("", warning).strip() for warning in warnings
            if warning.rstrip().lower().endswith(suffix)]


def recorded_warnings(manifest: Mapping[str, Any], checkout: Path = paths.REPOSITORY_ROOT) -> list[str]:
    """An entry's recorded warnings at `checkout`'s paths (a cached library reports the warnings of its compile in every
    build that links it, so a cache hit passes or fails Build.py's warning rules as compiling it would)."""
    return [str(warning).replace(REPOSITORY_PLACEHOLDER, str(checkout)) for warning in manifest.get("warnings", [])]


# --------------------------------------------------------------------------------------------------------------------
# A build's use of the cache
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class PopulateResult:
    """What compiling the staged builds reported: success, and every warning and error line."""

    succeeded: bool
    warnings: list[str]
    errors: list[str]


class PopulateFailedError(VendorCacheError):
    """Compiling missing entries failed; the build fails with the compile's errors, as it would without the cache."""

    def __init__(self, result: PopulateResult) -> None:
        super().__init__("compiling the vendored libraries failed")
        self.result = result


@dataclasses.dataclass
class Plan:
    """How one build uses the cache."""

    root: Path
    linked: list[LinkedEntry] = dataclasses.field(default_factory=list)
    compiled_in_workspace: list[str] = dataclasses.field(default_factory=list)  # entries that could not be published
    notes: list[str] = dataclasses.field(default_factory=list)  # entries rejected and replaced, publishing problems
    # Warnings of this build's compile that MSBuild attributed to no compiled library (they are not recorded).
    other_warnings: list[str] = dataclasses.field(default_factory=list)
    verify_seconds: float = 0.0
    populate_seconds: float = 0.0
    wait_seconds: float = 0.0

    @property
    def warnings(self) -> list[str]:
        """Every warning the linked libraries' compiles reported, and this build's compile's other warnings."""
        return [warning for entry in self.linked for warning in entry.warnings] + self.other_warnings

    def to_json(self) -> dict[str, Any]:
        return {
            "root": self.root.as_posix(),
            "linked": [{"project": entry.project, "entry": entry.entry.as_posix(), "published": entry.published}
                       for entry in self.linked],
            "compiledInWorkspace": self.compiled_in_workspace,
            "notes": self.notes,
            "verifySeconds": round(self.verify_seconds, 2),
            "populateSeconds": round(self.populate_seconds, 1),
            "waitSeconds": round(self.wait_seconds, 1),
        }

    def summary(self) -> str:
        published = sum(1 for entry in self.linked if entry.published)
        text = f"vendor cache: {len(self.linked)} librar{'y' if len(self.linked) == 1 else 'ies'} linked"
        if published:
            text += f" ({published} compiled and published in {self.populate_seconds:.1f} s)"
        if self.compiled_in_workspace:
            text += f", {len(self.compiled_in_workspace)} compiled in the workspace"
        return text


def prepare(cache: VendorCache, project_files: Mapping[str, Path], config: str, platform: str,
            toolchain: Mapping[str, str], solution: Path,
            populate: Callable[[Path, list[StagedBuild]], PopulateResult], timeout: float,
            log: Callable[[str], None], repository_root: Path = paths.REPOSITORY_ROOT) -> Plan:
    """The cache entries for the vendored projects among `project_files` (name -> project file) in `config`|`platform`:
    every intact entry is linked; the missing ones, and the ones that fail verification (moved to trash/), are compiled
    under populate_lock by `populate` (given the generated project and the staged builds) and published, then linked.
    Raises PopulateFailedError when that compile fails, and VendorCacheError when the cache cannot be used."""
    started = time.monotonic()
    plan = Plan(cache.root)
    digester = SourceDigester(repository_root)
    wanted: list[tuple[Path, dict[str, Any], str]] = []
    for name in sorted(project_files):
        project = read_project_file(project_files[name])
        if project_properties(project, config, platform, repository_root).get("ConfigurationType") != "StaticLibrary":
            continue
        settings = project_settings(project, config, platform, repository_root)
        if not is_vendor_library(settings):
            continue
        try:
            inputs = project_inputs(project, config, platform, toolchain, digester)
        except UnkeyedInputError as error:
            # Fail-closed: a library whose inputs the key cannot see is never linked from the cache.
            plan.notes.append(f"{error}; compiled in the workspace")
            plan.compiled_in_workspace.append(project.name)
            continue
        wanted.append((project.path, inputs, library_name(project, settings)))

    def linked_entry(project_file: Path, inputs: Mapping[str, Any], entry: Path, published: bool) -> LinkedEntry:
        manifest, _ = cache.read_manifest(entry)
        assert manifest is not None  # verified, or published by this build
        return LinkedEntry(str(inputs["project"]), project_file, entry, entry / str(manifest["library"]), published,
                           recorded_warnings(manifest, repository_root))

    def look_up(project_file: Path, inputs: dict[str, Any]) -> tuple[LinkedEntry | None, list[str]]:
        entry = cache.entry_directory(str(inputs["project"]), config, entry_key(inputs))
        if not entry.exists():
            return None, []
        problems = cache.verify(entry, inputs)
        if problems:
            return None, problems
        return linked_entry(project_file, inputs, entry, False), []

    missing: list[tuple[Path, dict[str, Any], str]] = []
    for project_file, inputs, library in wanted:
        linked, _ = look_up(project_file, inputs)
        if linked is not None:
            plan.linked.append(linked)
        else:
            missing.append((project_file, inputs, library))
    plan.verify_seconds = time.monotonic() - started
    if not missing:
        return plan

    waiting = time.monotonic()
    with cache.populate_lock(timeout, log):
        plan.wait_seconds = time.monotonic() - waiting
        cache.remove_leftovers()
        staging = cache.staging / secrets.token_hex(8)
        builds: list[StagedBuild] = []
        for project_file, inputs, library in missing:
            linked, problems = look_up(project_file, inputs)  # another build may have published it meanwhile
            if linked is not None:
                plan.linked.append(linked)
                continue
            entry = cache.entry_directory(str(inputs["project"]), config, entry_key(inputs))
            if problems:
                plan.notes.append(f"{inputs['project']}: rejected {entry} ({'; '.join(problems)})")
                if not cache.discard(entry):
                    plan.notes.append(f"{inputs['project']}: {entry} is in use and was not replaced; compiled in the "
                                      f"workspace")
                    plan.compiled_in_workspace.append(str(inputs["project"]))
                    continue
            builds.append(StagedBuild(project_file, inputs, library, staging / str(inputs["project"])))
        if builds:
            log(f"vendor cache: compiling {', '.join(build.project for build in builds)} ({config})")
            _compile_and_publish(cache, builds, staging, solution, populate, plan, linked_entry, repository_root)
    plan.linked.sort(key=lambda entry: entry.project)
    return plan


def _compile_and_publish(cache: VendorCache, builds: list[StagedBuild], staging: Path, solution: Path,
                         populate: Callable[[Path, list[StagedBuild]], PopulateResult], plan: Plan,
                         linked_entry: Callable[[Path, Mapping[str, Any], Path, bool], LinkedEntry],
                         repository_root: Path) -> None:
    """prepare's compile of `builds` in `staging` (removed afterwards) and the publishing of each entry, under
    populate_lock. An entry that cannot be published is compiled in the workspace instead."""
    compile_started = time.monotonic()
    try:
        populate_project = write_populate_project(staging / "Populate.proj", builds, solution)
        result = populate(populate_project, builds)
        plan.populate_seconds = time.monotonic() - compile_started
        if not result.succeeded:
            raise PopulateFailedError(result)
        suffixes = tuple(f"[{build.project_file}]".lower() for build in builds)
        plan.other_warnings = [warning for warning in result.warnings
                               if not warning.rstrip().lower().endswith(suffixes)]
        for build in builds:
            try:
                staged = build.directory / "entry"
                staged.mkdir()
                for file in staged_files(build):
                    os.replace(file, staged / file.name)
                warnings = attributed_warnings(result.warnings, build.project_file)
                entry = cache.publish(staged, build.inputs, build.library, warnings, plan.populate_seconds,
                                      repository_root)
            except (VendorCacheError, OSError) as error:
                plan.notes.append(f"{build.project}: not published ({error}); compiled in the workspace")
                plan.compiled_in_workspace.append(build.project)
                continue
            plan.linked.append(linked_entry(build.project_file, build.inputs, entry, True))
    finally:
        _remove_tree(staging)
