"""The portability stage's checks of generated build settings for the code paths only other platforms run (Docs/
Architecture.md §14.1, §15.8, §16; Docs/Decisions/0024-m15-contract.md decision 18). The lead's CI.py portability stage
calls check_runtime_link_settings after its existing generated-settings check, on the directories it generated into
bin-int/Portability/.
"""

from __future__ import annotations

import re
import xml.etree.ElementTree as ElementTree
from pathlib import Path

CONFIGURATIONS = ("Debug", "Release", "Dist")
# The targets the check reads, each with the file premake writes for the Runtime project.
TARGETS = {
    ("linux", "gmake"): "Runtime.make",
    ("linux", "ninja"): "Runtime.ninja",
    ("macosx", "xcode4"): "Runtime.xcodeproj/project.pbxproj",
    ("windows", "vs2026"): "Runtime.vcxproj",
}
LINUX_RPATH = "-Wl,-rpath,'$$ORIGIN'"
MACOS_RPATH = "-Wl,-rpath,@executable_path/../Frameworks"
MACOS_DEPLOYMENT_TARGET = "14.0"
MACOS_PRODUCT_TYPE = "com.apple.product-type.tool"
WINDOWS_SUBSYSTEMS = {"Debug": "Console", "Release": "Console", "Dist": "Windows"}

_MSBUILD_NAMESPACE = {"msbuild": "http://schemas.microsoft.com/developer/msbuild/2003"}
_GMAKE_CONFIG = re.compile(r"^(?:else\s+)?ifeq\s+\(\$\(config\),(\w+)\)\s*$")
_NINJA_LDFLAGS = re.compile(r"^ldflags_Runtime_(\w+)\s*=(.*)$", re.MULTILINE)
_PBX_BUILD_CONFIGURATION = re.compile(r"([0-9A-F]{24}) /\* [^*]*? \*/ = \{\s*isa = XCBuildConfiguration;(.*?)\n\t\t\};",
                                      re.DOTALL)
_PBX_CONFIGURATION_LIST = re.compile(r"/\* Build configuration list for (PBXProject|PBXNativeTarget) \"Runtime\" "
                                     r"\*/ = \{\s*isa = XCConfigurationList;(.*?)\};", re.DOTALL)
_PBX_LIST_ENTRY = re.compile(r"([0-9A-F]{24}) /\* (\w+) \*/")
_PBX_NAME = re.compile(r"^\s*name = (\w+);\s*$", re.MULTILINE)
_PBX_DEPLOYMENT_TARGET = re.compile(r"^\s*MACOSX_DEPLOYMENT_TARGET = \"?([^\";]+)\"?;", re.MULTILINE)
_PBX_LDFLAGS = re.compile(r"^\s*OTHER_LDFLAGS = \((.*?)\);", re.MULTILINE | re.DOTALL)
_PBX_PRODUCT_TYPE = re.compile(r"isa = PBXNativeTarget;.*?productType = \"([^\"]+)\";", re.DOTALL)


def _map_problem(flags: list[str], option: str, config: str, system: str) -> str | None:
    """Why `flags` lack the map option of `config` (`option` followed by a path ending in bin/<Config>-<system>-<arch>/
    Runtime/Runtime.map), or None."""
    paths = [flag[len(option):].strip("\"'") for flag in flags if flag.startswith(option)]
    expected = re.compile(rf"(?:^|/)bin/{config}-{system}-[A-Za-z0-9_]+/Runtime/Runtime\.map$")
    if not paths:
        return f"the link line lacks {option}<map> (the map file the Dist map check reads, §14.4)"
    if not any(expected.search(path.replace("\\", "/")) for path in paths):
        return f"{option} names {paths[0]}, not bin/{config}-{system}-<arch>/Runtime/Runtime.map"
    return None


def _check_linux(label: str, link_lines: dict[str, str]) -> list[str]:
    problems: list[str] = []
    for config in CONFIGURATIONS:
        line = link_lines.get(config)
        if line is None:
            problems.append(f"{label} {config}: no link flags for the Runtime")
            continue
        flags = line.split()
        if LINUX_RPATH not in flags:
            problems.append(f"{label} {config}: the link line lacks {LINUX_RPATH} (exported games load the libraries "
                            f"beside the executable, §14.1, §16)")
        problem = _map_problem(flags, "-Wl,-Map=", config, "linux")
        if problem is not None:
            problems.append(f"{label} {config}: {problem}")
    return problems


def _gmake_link_lines(text: str) -> dict[str, str]:
    lines: dict[str, str] = {}
    config: str | None = None
    for line in text.splitlines():
        match = _GMAKE_CONFIG.match(line)
        if match is not None:
            config = match.group(1)
            continue
        if line.startswith("endif"):
            config = None
            continue
        if config is not None and line.startswith("ALL_LDFLAGS +="):
            for name in CONFIGURATIONS:
                if name.lower() == config:
                    lines[name] = lines.get(name, "") + " " + line[len("ALL_LDFLAGS +="):]
    return lines


def _ninja_link_lines(text: str) -> dict[str, str]:
    return {match.group(1): match.group(2) for match in _NINJA_LDFLAGS.finditer(text)}


def _check_xcode(label: str, text: str) -> list[str]:
    configurations = {identifier: body for identifier, body in _PBX_BUILD_CONFIGURATION.findall(text)}
    lists: dict[str, dict[str, str]] = {}
    for owner, body in _PBX_CONFIGURATION_LIST.findall(text):
        lists[owner] = {name: identifier for identifier, name in _PBX_LIST_ENTRY.findall(body)}
    problems: list[str] = []
    if "PBXNativeTarget" not in lists:
        return [f"{label}: no build configuration list for the Runtime target"]
    product = _PBX_PRODUCT_TYPE.search(text)
    if product is None or product.group(1) != MACOS_PRODUCT_TYPE:
        found = product.group(1) if product is not None else "none"
        problems.append(f"{label}: the Runtime's product type is {found}, not {MACOS_PRODUCT_TYPE} (the exporter "
                        f"copies a plain executable and assembles the bundle itself, §14.1)")
    for config in CONFIGURATIONS:
        # The target's settings override the project's.
        bodies = [configurations.get(lists.get(owner, {}).get(config, ""), "")
                  for owner in ("PBXNativeTarget", "PBXProject")]
        if not bodies[0]:
            problems.append(f"{label} {config}: the Runtime target has no {config} configuration")
            continue

        def setting(pattern: re.Pattern[str]) -> str | None:
            for body in bodies:
                match = pattern.search(body)
                if match is not None:
                    return match.group(1)
            return None

        deployment = setting(_PBX_DEPLOYMENT_TARGET)
        if deployment != MACOS_DEPLOYMENT_TARGET:
            problems.append(f"{label} {config}: MACOSX_DEPLOYMENT_TARGET is {deployment or 'unset'}, not "
                            f"{MACOS_DEPLOYMENT_TARGET} (§16)")
        ldflags = setting(_PBX_LDFLAGS)
        flags = [item.strip().strip(",").strip().strip("\"") for item in (ldflags or "").split("\n")]
        flags = [flag for flag in flags if flag]
        if MACOS_RPATH not in flags:
            problems.append(f"{label} {config}: OTHER_LDFLAGS lacks {MACOS_RPATH} (exported bundles load the loader "
                            f"and MoltenVK from Contents/Frameworks, §16)")
        problem = _map_problem(flags, "-Wl,-map,", config, "macosx")
        if problem is not None:
            problems.append(f"{label} {config}: {problem}")
    return problems


def _check_visual_studio(label: str, text: str) -> list[str]:
    try:
        root = ElementTree.fromstring(text)
    except ElementTree.ParseError as error:
        return [f"{label}: Runtime.vcxproj is not XML: {error}"]
    problems: list[str] = []
    groups = {}
    for group in root.findall("msbuild:ItemDefinitionGroup", _MSBUILD_NAMESPACE):
        condition = group.get("Condition", "")
        match = re.search(r"=='(\w+)\|", condition)
        if match is not None:
            groups[match.group(1)] = group
    for config in CONFIGURATIONS:
        group = groups.get(config)
        if group is None:
            problems.append(f"{label} {config}: no ItemDefinitionGroup for the configuration")
            continue
        options = group.findtext("msbuild:Link/msbuild:AdditionalOptions", default="", namespaces=_MSBUILD_NAMESPACE)
        if "/MAP" not in options.split():
            problems.append(f"{label} {config}: the Link AdditionalOptions lack /MAP (the map file the Dist map check "
                            f"reads, §14.4)")
        subsystem = group.findtext("msbuild:Link/msbuild:SubSystem", default="", namespaces=_MSBUILD_NAMESPACE)
        if subsystem != WINDOWS_SUBSYSTEMS[config]:
            problems.append(f"{label} {config}: SubSystem is {subsystem or 'unset'}, not {WINDOWS_SUBSYSTEMS[config]} "
                            f"(Dist ships without a console window, §14.3)")
    return problems


def check_runtime_link_settings(generated: dict[tuple[str, str], Path]) -> list[str]:
    """The problems of the Runtime's generated link settings, one line each ("<system>-<action> <config>: <problem>"),
    over `generated` ({(system, action): the directory premake wrote}), for every configuration (Debug, Release, Dist):
      - linux gmake (Runtime.make ALL_LDFLAGS) and linux ninja (Runtime.ninja ldflags_Runtime_<Config>): the link line
        holds -Wl,-rpath,'$$ORIGIN' (make and ninja turn $$ into $, the quotes keep the shell from expanding it) and
        -Wl,-Map= naming bin/<Config>-linux-x86_64/Runtime/Runtime.map;
      - macosx xcode4 (Runtime.xcodeproj/project.pbxproj): OTHER_LDFLAGS holds -Wl,-rpath,@executable_path/../Frameworks
        and -Wl,-map,; MACOSX_DEPLOYMENT_TARGET is 14.0; the product type is com.apple.product-type.tool (the exporter
        copies a plain executable and assembles the bundle itself);
      - windows vs2026 (Runtime.vcxproj): /MAP in the Link AdditionalOptions of every configuration (premake writes the
        linkoptions of Runtime/premake5.lua there; there is no GenerateMapFile element), SubSystem Windows in Dist and
        Console otherwise.
    A target missing from `generated` is a problem."""
    problems: list[str] = []
    for (system, action), file_name in TARGETS.items():
        label = f"{system}-{action}"
        directory = generated.get((system, action))
        if directory is None:
            problems.append(f"{label}: not generated (the portability stage generates every target)")
            continue
        path = directory / file_name
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            problems.append(f"{label}: cannot read {path}: {error}")
            continue
        if action == "gmake":
            problems += _check_linux(label, _gmake_link_lines(text))
        elif action == "ninja":
            problems += _check_linux(label, _ninja_link_lines(text))
        elif action == "xcode4":
            problems += _check_xcode(label, text)
        else:
            problems += _check_visual_studio(label, text)
    return problems
