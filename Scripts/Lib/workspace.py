"""Reading the workspaces premake generated in place (no --to): which projects exist in which configuration, how to
address them, and which toolset the make/ninja files were generated for.

  vs2026  <Workspace>.slnx               MSBuild solution targets are "<Solution folder>\\<Project>"
  gmake   Makefile + <Project>.make      `make config=<lowercase config> <Project>`
  ninja   build.ninja + <Project>.ninja  phony targets "<Config>" and "<Project>_<Config>"
  xcode4  <Workspace>.xcworkspace + <Project>.xcodeproj  one target per project, named like the project

<Workspace> is the workspace name that premake5.lua defines (paths.workspace_name()).
"""

from __future__ import annotations

import dataclasses
import fnmatch
import re
import xml.etree.ElementTree as ElementTree
from pathlib import Path

from . import paths

# The workspace file each generator writes, relative to the workspace location; {workspace} is the workspace name.
_WORKSPACE_FILE_TEMPLATES = {
    "vs2026": "{workspace}.slnx",
    "gmake": "Makefile",
    "ninja": "build.ninja",
    "xcode4": "{workspace}.xcworkspace",
}
ACTIONS = tuple(_WORKSPACE_FILE_TEMPLATES)

# MSBuild replaces these characters with '_' in solution target names (ProjectInSolution.CleanseProjectName).
_MSBUILD_TARGET_CHARACTERS = re.compile(r"[%$@;.()']")


class WorkspaceError(Exception):
    """The generated workspace is missing or not in the expected format."""


class WorkspaceMissingError(WorkspaceError):
    """No workspace was generated for the requested action."""


class ProjectSelectionError(WorkspaceError):
    """A requested project is unknown or does not exist in the requested configuration."""


def workspace_file_name(action: str) -> str:
    """The name of the workspace file that `action` writes, relative to the workspace location."""
    try:
        name = paths.workspace_name()
    except paths.WorkspaceScriptError as error:
        raise WorkspaceError(str(error)) from None
    return _WORKSPACE_FILE_TEMPLATES[action].format(workspace=name)


def workspace_file(action: str, root: Path = paths.REPOSITORY_ROOT) -> Path:
    return root / workspace_file_name(action)


def generated_actions(root: Path = paths.REPOSITORY_ROOT) -> list[str]:
    """Generator actions whose workspace file exists under `root`."""
    return [action for action in ACTIONS if workspace_file(action, root).exists()]


@dataclasses.dataclass
class Workspace:
    action: str
    path: Path
    projects: list[str]  # every project, in generator order
    configurations: dict[str, list[str]]  # configuration -> projects that exist in it

    def projects_in(self, config: str) -> list[str]:
        return self.configurations.get(config, [])

    def require_project(self, project: str, config: str) -> str:
        """The canonical project name; raises WorkspaceError if it is unknown or absent from `config`."""
        by_lower = {name.lower(): name for name in self.projects}
        name = by_lower.get(project.lower())
        if name is None:
            raise ProjectSelectionError(f"unknown project '{project}' (projects: {', '.join(sorted(self.projects))})")
        if name not in self.projects_in(config):
            raise ProjectSelectionError(
                f"project {name} does not exist in the {config} configuration (Architecture §2.2)"
            )
        return name


# --------------------------------------------------------------------------------------------------------------------
# Visual Studio solution (.slnx)
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class Solution(Workspace):
    platform: str = "x64"
    target_names: dict[str, str] = dataclasses.field(default_factory=dict)  # project -> MSBuild solution target
    project_files: dict[str, Path] = dataclasses.field(default_factory=dict)
    utility_projects: set[str] = dataclasses.field(default_factory=set)


def _builds_in(project: ElementTree.Element, solution_configuration: str) -> bool:
    default = True
    for build in project.findall("Build"):
        pattern = build.get("Solution")
        value = build.get("Project", "true").lower() == "true"
        if pattern is None:
            default = value
        elif fnmatch.fnmatchcase(solution_configuration, pattern):
            return value
    return default


def read_solution(path: Path) -> Solution:
    try:
        root = ElementTree.parse(path).getroot()
    except (OSError, ElementTree.ParseError) as error:
        raise WorkspaceError(f"cannot read {paths.display_path(path)}: {error}") from None
    configurations = [element.get("Name", "") for element in root.findall("./Configurations/BuildType")]
    platforms = [element.get("Name", "") for element in root.findall("./Configurations/Platform")]
    if not configurations or len(platforms) != 1:
        raise WorkspaceError(f"{paths.display_path(path)}: expected configurations and exactly one platform")
    platform = platforms[0]

    entries: list[tuple[str, ElementTree.Element]] = []
    for project in root.findall("Project"):
        entries.append(("", project))
    for folder in root.iter("Folder"):
        folder_path = folder.get("Name", "").strip("/")
        for project in folder.findall("Project"):
            entries.append((folder_path, project))

    solution = Solution("vs2026", path, [], {config: [] for config in configurations}, platform)
    for folder_path, element in entries:
        relative = element.get("Path", "").replace("\\", "/")
        project_file = path.parent / relative
        name = Path(relative).stem
        solution.projects.append(name)
        solution.project_files[name] = project_file
        parts = [part for part in folder_path.split("/") if part] + [name]
        solution.target_names[name] = "\\".join(_MSBUILD_TARGET_CHARACTERS.sub("_", part) for part in parts)
        for config in configurations:
            if _builds_in(element, f"{config}|{platform}"):
                solution.configurations[config].append(name)
        try:
            text = project_file.read_text(encoding="utf-8-sig", errors="replace")
            if "<ConfigurationType>Utility</ConfigurationType>" in text:
                solution.utility_projects.add(name)
        except OSError:
            raise WorkspaceError(f"{paths.display_path(path)} lists {relative}, which does not exist") from None
    return solution


# --------------------------------------------------------------------------------------------------------------------
# GNU make (gmake)
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class Makefiles(Workspace):
    project_makefiles: dict[str, Path] = dataclasses.field(default_factory=dict)
    toolset: str = "gcc"


_PROJECTS_PATTERN = re.compile(r"^PROJECTS\s*:=\s*(.*)$", re.MULTILINE)
_CONFIG_BRANCH_PATTERN = re.compile(r"^(?:else\s+)?ifeq\s+\(\$\(config\),(\w+)\)\s*$")
_CONFIG_ASSIGNMENT_PATTERN = re.compile(r"^\s+(\S+)_config\s*=\s*(\w+)\s*$")
# "-C Engine -f Engine.make config=$(Engine_config)". A project alone in its directory gets a plain "Makefile".
_SUBMAKE_PATTERN = re.compile(r"-C (\S+) -f (\S+) config=\$\((\S+?)_config\)")
_MAKE_CXX_PATTERN = re.compile(r"ifeq \(\$\(origin CXX\), default\)\s*\n\s*CXX = (\S+)")


def _canonical_configuration(name: str) -> str:
    for config in paths.CONFIGURATIONS:
        if config.lower() == name.lower():
            return config
    raise WorkspaceError(f"unexpected configuration '{name}' in the generated workspace")


def read_makefiles(path: Path) -> Makefiles:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise WorkspaceError(f"cannot read {paths.display_path(path)}: {error}") from None
    projects_match = _PROJECTS_PATTERN.search(text)
    if not projects_match:
        raise WorkspaceError(f"{paths.display_path(path)} has no PROJECTS list; is it a premake workspace makefile?")
    projects = projects_match.group(1).split()

    configurations: dict[str, list[str]] = {}
    current: str | None = None
    for line in text.splitlines():
        branch = _CONFIG_BRANCH_PATTERN.match(line)
        if branch:
            current = _canonical_configuration(branch.group(1))
            configurations[current] = []
            continue
        if line.strip() in ("else", "endif"):
            current = None
            continue
        assignment = _CONFIG_ASSIGNMENT_PATTERN.match(line)
        if current is not None and assignment:
            configurations[current].append(assignment.group(1))

    makefiles = Makefiles("gmake", path, projects, configurations)
    for directory, makefile, project in _SUBMAKE_PATTERN.findall(text):
        makefiles.project_makefiles[project] = (path.parent / directory / makefile).resolve()
    for project in projects:
        project_makefile = makefiles.project_makefiles.get(project)
        if project_makefile is None or not project_makefile.is_file():
            raise WorkspaceError(f"{paths.display_path(path)}: makefile of project {project} not found")

    toolsets = set()
    for project_makefile in makefiles.project_makefiles.values():
        match = _MAKE_CXX_PATTERN.search(project_makefile.read_text(encoding="utf-8", errors="replace"))
        if match:
            toolsets.add("clang" if "clang" in match.group(1) else "gcc")
    if len(toolsets) != 1:
        raise WorkspaceError(f"{paths.display_path(path)}: cannot determine the toolset (found {sorted(toolsets)})")
    makefiles.toolset = toolsets.pop()
    return makefiles


# --------------------------------------------------------------------------------------------------------------------
# Ninja
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class NinjaFiles(Workspace):
    toolset: str = "gcc"


_NINJA_CONFIG_PATTERN = re.compile(r"^build (\w+): phony (.*)$", re.MULTILINE)
_NINJA_SUBNINJA_PATTERN = re.compile(r"^subninja (\S+)$", re.MULTILINE)
_NINJA_RULE_PATTERN = re.compile(r"^rule cxx_(\w+)$", re.MULTILINE)


def read_ninja(path: Path) -> NinjaFiles:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise WorkspaceError(f"cannot read {paths.display_path(path)}: {error}") from None
    configurations: dict[str, list[str]] = {}
    for name, targets in _NINJA_CONFIG_PATTERN.findall(text):
        if name.lower() not in (config.lower() for config in paths.CONFIGURATIONS):
            continue
        config = _canonical_configuration(name)
        suffix = f"_{name}"
        configurations[config] = [target[: -len(suffix)] for target in targets.split() if target.endswith(suffix)]
    if not configurations:
        raise WorkspaceError(f"{paths.display_path(path)} defines no configuration targets")
    projects: list[str] = []
    for config_projects in configurations.values():
        projects.extend(project for project in config_projects if project not in projects)

    toolsets = set()
    for subninja in _NINJA_SUBNINJA_PATTERN.findall(text):
        try:
            toolsets.update(_NINJA_RULE_PATTERN.findall((path.parent / subninja).read_text(encoding="utf-8")))
        except OSError as error:
            raise WorkspaceError(f"cannot read {subninja}: {error}") from None
    if len(toolsets) != 1:
        raise WorkspaceError(f"{paths.display_path(path)}: cannot determine the toolset (found {sorted(toolsets)})")
    return NinjaFiles("ninja", path, projects, configurations, "clang" if "clang" in toolsets.pop() else "gcc")


# --------------------------------------------------------------------------------------------------------------------
# Xcode
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class XcodeWorkspace(Workspace):
    project_files: dict[str, Path] = dataclasses.field(default_factory=dict)  # project -> .xcodeproj
    target_names: dict[str, str] = dataclasses.field(default_factory=dict)  # project -> its single target
    dependencies: dict[str, set[str]] = dataclasses.field(default_factory=dict)  # project -> referenced projects

    def top_level_projects(self, config: str) -> list[str]:
        """Projects in `config` that no other project in `config` depends on. Building them builds everything."""
        present = set(self.projects_in(config))
        referenced = set().union(*(self.dependencies.get(name, set()) for name in present)) if present else set()
        return [name for name in self.projects_in(config) if name not in referenced]


_XCODE_TARGET_PATTERN = re.compile(
    r"isa = PBX(?:Native|Aggregate|Legacy)Target;\s*buildConfigurationList = (\w+) [^;]*;(.*?)\n\t\t\};", re.DOTALL
)
_XCODE_TARGET_NAME_PATTERN = re.compile(r"^\s*name = (\"?)(.+?)\1;\s*$", re.MULTILINE)
_XCODE_CONFIG_LIST_TEMPLATE = (
    r"\t\t{identifier} /\* [^*]* \*/ = \{{\s*isa = XCConfigurationList;\s*buildConfigurations = \((.*?)\);"
)
_XCODE_CONFIG_NAME_PATTERN = re.compile(r"/\* (\w+) \*/")
_XCODE_CONTAINER_PATTERN = re.compile(r"containerPortal = \w+ /\* (.+?)\.xcodeproj \*/;")


def _workspace_file_references(element: ElementTree.Element, base: Path) -> list[Path]:
    found: list[Path] = []
    for child in element:
        location = child.get("location", "")
        kind, _, relative = location.partition(":")
        if kind == "container":
            child_base = base
        elif kind in ("group", "absolute"):
            child_base = Path(relative) if kind == "absolute" else base / relative
        else:
            child_base = base
        if child.tag == "FileRef":
            found.append(child_base)
        elif child.tag == "Group":
            found.extend(_workspace_file_references(child, child_base))
    return found


def read_xcode_workspace(path: Path) -> XcodeWorkspace:
    contents = path / "contents.xcworkspacedata"
    try:
        root = ElementTree.parse(contents).getroot()
    except (OSError, ElementTree.ParseError) as error:
        raise WorkspaceError(f"cannot read {paths.display_path(contents)}: {error}") from None
    workspace = XcodeWorkspace("xcode4", path, [], {config: [] for config in paths.CONFIGURATIONS})
    for project_path in _workspace_file_references(root, path.parent):
        if project_path.suffix != ".xcodeproj":
            continue
        pbxproj = project_path / "project.pbxproj"
        try:
            text = pbxproj.read_text(encoding="utf-8")
        except OSError:
            raise WorkspaceError(f"{paths.display_path(path)} references {project_path}, which is missing") from None
        targets = _XCODE_TARGET_PATTERN.findall(text)
        if len(targets) != 1:
            raise WorkspaceError(f"{paths.display_path(pbxproj)}: expected exactly one target, found {len(targets)}")
        configuration_list, target_body = targets[0]
        target_name = _XCODE_TARGET_NAME_PATTERN.search(target_body)
        configuration_block = re.search(
            _XCODE_CONFIG_LIST_TEMPLATE.format(identifier=re.escape(configuration_list)), text, re.DOTALL
        )
        if not target_name or not configuration_block:
            raise WorkspaceError(f"{paths.display_path(pbxproj)}: cannot read the target's name and configurations")
        name = project_path.stem
        workspace.projects.append(name)
        workspace.project_files[name] = project_path.resolve()
        workspace.target_names[name] = target_name.group(2)
        # In-place projects reference their dependencies by relative path ("../Vendor/GLFW/GLFW.xcodeproj").
        workspace.dependencies[name] = {Path(match).name for match in _XCODE_CONTAINER_PATTERN.findall(text)} - {name}
        for config in _XCODE_CONFIG_NAME_PATTERN.findall(configuration_block.group(1)):
            if config in workspace.configurations:
                workspace.configurations[config].append(name)
    if not workspace.projects:
        raise WorkspaceError(f"{paths.display_path(path)} references no projects")
    return workspace


def read_workspace(action: str, root: Path = paths.REPOSITORY_ROOT) -> Workspace:
    path = workspace_file(action, root)
    if not path.exists():
        raise WorkspaceMissingError(
            f"{paths.display_path(path)} not found: run python Scripts/Generate.py --action {action} first"
        )
    readers = {
        "vs2026": read_solution,
        "gmake": read_makefiles,
        "ninja": read_ninja,
        "xcode4": read_xcode_workspace,
    }
    return readers[action](path)
