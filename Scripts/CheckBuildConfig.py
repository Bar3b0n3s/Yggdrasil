#!/usr/bin/env python3
"""Check the generated build configuration for ABI and determinism errors (Docs/Architecture.md §2.2).

The workspace is generated into a temporary directory for every target the build or CI uses, with the pinned
premake (--fatal), and every generated project is parsed:

  windows        premake5 vs2026                .vcxproj files and the .slnx solution (MSVC)
  windows-clang  premake5 --cc=clang vs2026     .vcxproj files (clang-cl, the portability build of §15.8)
  linux          premake5 --os=linux gmake      per-project makefiles (GCC)
  linux-clang    premake5 --os=linux --cc=clang gmake  per-project makefiles (the Clang 18 CI job)
  macosx         premake5 --os=macosx xcode4    project.pbxproj files (Apple Clang)

For every compile unit (a project configuration, or a file that overrides it) the effective settings are computed
the way the compiler sees its command line, in order: the define list, then undefines, then every -D/-U (or /D, /U)
in the compiler options and per-file options, the last one for a name winning; the floating-point options; and the
instruction-set options (/arch:, -m<feature>, -mno-<feature>, -march=, -mcpu=). The check fails when, for any target
and configuration:

1. an ABI-relevant define differs between a vendored library project and a project that includes its headers (a
   consumer: its include path contains one of the library's include directories). The families come from each
   library's VENDOR.md (VENDOR_ABI_FAMILIES below); NDEBUG is compared for every library;
2. JPH_CROSS_PLATFORM_DETERMINISTIC is missing from JoltPhysics or from any Jolt consumer;
3. a Jolt consumer is compiled for another instruction set than JoltPhysics. Jolt/Core/Core.h derives JPH_USE_AVX2,
   JPH_USE_AVX, JPH_USE_F16C, JPH_USE_LZCNT, JPH_USE_TZCNT and more from the compiler's predefined __AVX2__,
   __AVX__, ... macros, and those are not part of JPH_VERSION_ID, so VerifyJoltVersionID() cannot catch a mismatch;
4. a first-party project or JoltPhysics uses a non-precise floating-point model: MSVC FloatingPointModel=Fast,
   /fp:fast, /fp:contract or /Qfast_transcendentals; GCC/Clang (and clang-cl's /clang: options) -ffast-math, -Ofast,
   -ffp-model=fast or any component of -ffast-math in effect (unsafe, associative or reciprocal math, finite math
   only, no NaNs or infinities, no signed zeros, no trapping math, limited complex range, approximate functions,
   fast excess precision), or an effective -ffp-contract other than "off" (it must be explicit: GCC's C++ default is
   "fast", clang-cl's is "on");
5. the solution builds EditorCore, Editor or Tests in Dist, or does not build Engine or Runtime in Dist.

--workspace runs the check on another workspace, such as the fixtures in Tests/Data/BuildConfig/, each of which is
the real workspace with one defect and must fail.

Exit codes: 0 no findings, 1 findings or project generation failed, 2 usage error, 3 premake is missing or not the
pinned version, 5 premake timed out.
"""

from __future__ import annotations

import argparse
import dataclasses
import fnmatch
import json
import os
import re
import shlex
import shutil
import sys
import tempfile
import xml.etree.ElementTree as ElementTree
from collections.abc import Callable, Iterable
from pathlib import Path
from typing import Any

from Lib import paths, premake
from Lib.process import ToolNotFoundError
from Lib.report import EXIT_FAILED, EXIT_INIT_FAILED, EXIT_SUCCESS, EXIT_TIMEOUT, EXIT_USAGE

REPOSITORY_ROOT = paths.REPOSITORY_ROOT
VENDOR_ROOT = paths.VENDOR_ROOT
CONFIGURATIONS = paths.CONFIGURATIONS
GENERATE_TIMEOUT_SECONDS = 600.0


@dataclasses.dataclass(frozen=True)
class Target:
    name: str
    action: str
    system: str
    toolset: str | None  # premake --cc; None is the generator's default (msc for vs2026, gcc for gmake)
    dialect: str  # how compiler options are read: "msvc", "clang-cl" or "gnu"


TARGETS = (
    Target("windows", "vs2026", "windows", None, "msvc"),
    Target("windows-clang", "vs2026", "windows", "clang", "clang-cl"),
    Target("linux", "gmake", "linux", None, "gnu"),
    Target("linux-clang", "gmake", "linux", "clang", "gnu"),
    Target("macosx", "xcode4", "macosx", None, "gnu"),
)
TARGET_BY_NAME = {target.name: target for target in TARGETS}


@dataclasses.dataclass(frozen=True)
class DefineFamily:
    """ABI-relevant define names of a library: exact names and name prefixes."""

    prefixes: tuple[str, ...] = ()
    names: tuple[str, ...] = ()

    def contains(self, name: str) -> bool:
        return name in self.names or any(name.startswith(prefix) for prefix in self.prefixes)


# ABI-relevant define families owned by each vendored library, from its VENDOR.md: consumers must define exactly what
# the library defines within these families. NDEBUG is compared for every library (GLOBAL_ABI_DEFINES).
_IMGUI_FAMILY = DefineFamily(prefixes=("IMGUI_",), names=("ImTextureID", "ImDrawIdx"))
_LUAU_FAMILY = DefineFamily(prefixes=("LUA_VECTOR_",), names=("LUA_USE_LONGJMP", "LUA_API", "LUACODE_API"))
VENDOR_ABI_FAMILIES: dict[str, DefineFamily] = {
    "JoltPhysics": DefineFamily(prefixes=("JPH_",)),
    "spdlog": DefineFamily(prefixes=("SPDLOG_",)),
    "miniaudio": DefineFamily(prefixes=("MA_NO_",), names=("MINIAUDIO_IMPLEMENTATION",)),
    "NVRHI": DefineFamily(
        prefixes=("VULKAN_HPP_", "VK_USE_PLATFORM_"),
        names=("NOMINMAX", "VK_ENABLE_BETA_EXTENSIONS", "NVRHI_SHARED_LIBRARY_INCLUDE", "NVRHI_SHARED_LIBRARY_BUILD"),
    ),
    "Luau": _LUAU_FAMILY,
    "LuauAnalysis": _LUAU_FAMILY,
    "ImGui": _IMGUI_FAMILY,
    "ImGuizmo": DefineFamily(_IMGUI_FAMILY.prefixes, _IMGUI_FAMILY.names + ("USE_IMGUI_API", "IMGUIZMO_NAMESPACE")),
    "GLFW": DefineFamily(prefixes=("_GLFW_",), names=("GLFW_DLL",)),
}
GLOBAL_ABI_DEFINES = ("NDEBUG",)

# Defines a library sets for its own translation units only, documented as ABI-neutral:
# - spdlog: SPDLOG_FWRITE_UNLOCKED is only read by spdlog's compiled sources (Vendor/spdlog/VENDOR.md).
# - Luau: NDEBUG in Release compiles out LUAU_ASSERT; no Luau layout depends on it and no Luau translation unit
#   includes vulkan.hpp (Architecture §2.2, Vendor/Luau/VENDOR.md).
# - GLFW: the backend selection _GLFW_WIN32/_GLFW_X11/_GLFW_COCOA is private (Vendor/GLFW/VENDOR.md).
VENDOR_PRIVATE_DEFINES: dict[str, frozenset[str]] = {
    "spdlog": frozenset({"SPDLOG_FWRITE_UNLOCKED"}),
    "Luau": frozenset({"NDEBUG"}),
    "LuauAnalysis": frozenset({"NDEBUG"}),
    "GLFW": frozenset({"_GLFW_WIN32", "_GLFW_X11", "_GLFW_COCOA"}),
}

JOLT_PROJECT = "JoltPhysics"
DETERMINISM_DEFINE = "JPH_CROSS_PLATFORM_DETERMINISTIC"
NOT_IN_DIST = ("EditorCore", "Editor", "Tests")
BUILT_IN_DIST = ("Engine", "Runtime")

MSBUILD_NAMESPACE = {"msb": "http://schemas.microsoft.com/developer/msbuild/2003"}
CONDITION_PATTERN = re.compile(r"'\$\(Configuration\)\|\$\(Platform\)'\s*==\s*'([^|']+)\|[^']*'")

# MSBuild EnableEnhancedInstructionSet values and the /arch: option each one stands for.
MSBUILD_INSTRUCTION_SETS = {
    "NoExtensions": "IA32",
    "StreamingSIMDExtensions": "SSE",
    "StreamingSIMDExtensions2": "SSE2",
    "AdvancedVectorExtensions": "AVX",
    "AdvancedVectorExtensions2": "AVX2",
    "AdvancedVectorExtensions512": "AVX512",
}
# GCC/Clang instruction-set feature options (-m<feature> and -mno-<feature>); -march= and -mcpu= are handled apart.
GNU_ISA_FEATURE_PATTERN = re.compile(
    r"^-m(no-)?(sse[0-9.]*|ssse3|sse4a|avx[0-9a-z_.-]*|fma4?|f16c|bmi2?|lzcnt|popcnt|abm|tbm|xop|aes|pclmul|sha|vaes|"
    r"vpclmulqdq|gfni|movbe|adx|rdrnd|rdseed|amx[0-9a-z_-]*|evex512|crc32|neon|sve[0-9]*)$"
)

# -ffast-math and the options it implies (GCC and Clang documentation), each mapped to the component it switches on
# (True) or off (False). -ffast-math and -Ofast switch every component on, -fno-fast-math switches every one off.
FAST_MATH_COMPONENT_OPTIONS: dict[str, tuple[str, bool]] = {
    "-funsafe-math-optimizations": ("unsafe math optimizations", True),
    "-fno-unsafe-math-optimizations": ("unsafe math optimizations", False),
    "-fassociative-math": ("associative math", True),
    "-fno-associative-math": ("associative math", False),
    "-freciprocal-math": ("reciprocal math", True),
    "-fno-reciprocal-math": ("reciprocal math", False),
    "-ffinite-math-only": ("finite math only", True),
    "-fno-finite-math-only": ("finite math only", False),
    "-fno-honor-nans": ("no NaNs", True),
    "-fhonor-nans": ("no NaNs", False),
    "-fno-honor-infinities": ("no infinities", True),
    "-fhonor-infinities": ("no infinities", False),
    "-fno-signed-zeros": ("no signed zeros", True),
    "-fsigned-zeros": ("no signed zeros", False),
    "-fno-trapping-math": ("no trapping math", True),
    "-ftrapping-math": ("no trapping math", False),
    "-fcx-limited-range": ("limited complex range", True),
    "-fno-cx-limited-range": ("limited complex range", False),
    "-fapprox-func": ("approximate functions", True),
    "-fno-approx-func": ("approximate functions", False),
    "-fexcess-precision=fast": ("fast excess precision", True),
    "-fexcess-precision=standard": ("fast excess precision", False),
    "-fexcess-precision=16": ("fast excess precision", False),
}
FAST_MATH_COMPONENTS = tuple(dict.fromkeys(component for component, _ in FAST_MATH_COMPONENT_OPTIONS.values()))
MSVC_UNSAFE_FP_OPTIONS = ("/fp:fast", "/fp:contract", "/qfast_transcendentals")


class UsageError(Exception):
    """Bad arguments (exit code 2)."""


class GenerationError(Exception):
    """premake failed to generate a target (exit code 1)."""


class GenerationTimeout(Exception):
    """premake did not finish within GENERATE_TIMEOUT_SECONDS (exit code 5)."""


@dataclasses.dataclass
class CompileUnit:
    """Effective compile settings of one project configuration, or of one file that overrides them."""

    defines: list[str]  # effective defines (NAME or NAME=VALUE) after every -D/-U in command-line order
    flags: list[str]  # compiler options in command-line order (clang-cl's /clang: prefix removed)
    msvc_fp_model: str | None = None  # MSVC FloatingPointModel element (None: compiler default /fp:precise)
    msvc_instruction_set: str | None = None  # MSVC EnableEnhancedInstructionSet element, as an /arch: value
    file: str | None = None  # set for a per-file override


@dataclasses.dataclass
class Project:
    name: str
    sources: list[Path] = dataclasses.field(default_factory=list)
    include_dirs: dict[str, set[str]] = dataclasses.field(default_factory=dict)  # config -> normalized paths
    units: dict[str, list[CompileUnit]] = dataclasses.field(default_factory=dict)  # config -> units (first: project)


@dataclasses.dataclass
class GeneratedWorkspace:
    target: Target
    projects: dict[str, Project]
    dist_builds: dict[str, bool] | None = None  # vs2026 only: project -> built in the Dist solution configuration


def normalize_path(path: Path) -> str:
    return os.path.normcase(os.path.normpath(str(path.resolve())))


def define_name(define: str) -> str:
    return define.split("=", 1)[0].strip()


def strip_clang_cl_prefix(flags: Iterable[str]) -> list[str]:
    """clang-cl passes GCC-style options through /clang:<option>; read them like any other option."""
    return [flag[len("/clang:") :] if flag.startswith("/clang:") else flag for flag in flags]


def define_operations(flags: Iterable[str]) -> list[tuple[str, str]]:
    """The -D/-U (and /D, /U) options in `flags`, in order, as ("D", "NAME[=VALUE]") or ("U", "NAME")."""
    operations: list[tuple[str, str]] = []
    pending: str | None = None
    for flag in flags:
        if pending is not None:
            operations.append((pending, flag))
            pending = None
            continue
        if len(flag) >= 2 and flag[0] in "-/" and flag[1] in "DU":
            value = flag[2:]
            if value:
                operations.append((flag[1], value))
            else:
                pending = flag[1]
    return operations


def apply_define_operations(operations: Iterable[tuple[str, str]]) -> list[str]:
    """Effective defines after `operations` in command-line order: the last -D or -U of a name wins."""
    state: dict[str, str | None] = {}
    for operation, value in operations:
        name = define_name(value)
        state.pop(name, None)
        state[name] = value if operation == "D" else None
    return [value for value in state.values() if value is not None]


# --------------------------------------------------------------------------------------------------------------------
# Generation
# --------------------------------------------------------------------------------------------------------------------


def generate(premake_executable: Path, workspace_script: Path, target: Target, output: Path) -> None:
    arguments = [f"--file={workspace_script}", "--fatal", f"--os={target.system}"]
    if target.toolset:
        arguments.append(f"--cc={target.toolset}")
    arguments += [f"--to={output}", target.action]
    result = premake.run(arguments, timeout=GENERATE_TIMEOUT_SECONDS, premake=premake_executable)
    if result.timed_out:
        raise GenerationTimeout(f"premake {' '.join(arguments)} {result.describe_exit()}")
    if not result.succeeded:
        raise GenerationError(f"premake {' '.join(arguments)} failed ({result.describe_exit()}):\n{result.tail(20)}")


# --------------------------------------------------------------------------------------------------------------------
# Visual Studio (.vcxproj, .slnx)
# --------------------------------------------------------------------------------------------------------------------


def condition_configuration(element: ElementTree.Element) -> str | None:
    match = CONDITION_PATTERN.search(element.get("Condition", ""))
    return match.group(1) if match else None


def split_msbuild_list(text: str | None) -> list[str]:
    if not text:
        return []
    return [item.strip() for item in text.split(";") if item.strip() and not item.strip().startswith(("%(", "$("))]


def msbuild_path(base: Path, value: str) -> Path:
    """A path from a .vcxproj (relative to `base`, backslash-separated) as a host path. On Linux and macOS a backslash
    is an ordinary file-name character, so it must be converted before joining, or no vcxproj path would ever match a
    directory such as Vendor/."""
    return base / value.replace("\\", "/")


def split_msvc_options(text: str | None) -> list[str]:
    if not text:
        return []
    return [option for option in shlex.split(text, posix=False) if not option.startswith("%(")]


@dataclasses.dataclass
class MsbuildCompileSettings:
    """The ClCompile settings of a project configuration, or of one file with the project's settings applied."""

    defines: list[str]
    undefines: list[str]
    options: list[str]
    fp_model: str | None
    instruction_set: str | None  # an /arch: value


def msbuild_inherits(text: str | None, tag: str) -> bool:
    """A file-level element replaces the project's value unless it is absent or lists %(<tag>)."""
    return text is None or f"%({tag})" in text


def msbuild_settings(values: dict[str, str | None], parent: MsbuildCompileSettings | None) -> MsbuildCompileSettings:
    defines_text = values.get("PreprocessorDefinitions")
    undefines_text = values.get("UndefinePreprocessorDefinitions")
    options_text = values.get("AdditionalOptions")
    instruction_set_text = values.get("EnableEnhancedInstructionSet")
    instruction_set = MSBUILD_INSTRUCTION_SETS.get(instruction_set_text) if instruction_set_text else None
    settings = MsbuildCompileSettings(
        split_msbuild_list(defines_text),
        split_msbuild_list(undefines_text),
        split_msvc_options(options_text),
        values.get("FloatingPointModel") or None,
        instruction_set,
    )
    if parent is None:
        return settings
    if msbuild_inherits(defines_text, "PreprocessorDefinitions"):
        settings.defines = parent.defines + settings.defines
    if msbuild_inherits(undefines_text, "UndefinePreprocessorDefinitions"):
        settings.undefines = parent.undefines + settings.undefines
    if msbuild_inherits(options_text, "AdditionalOptions"):
        settings.options = parent.options + settings.options
    settings.fp_model = settings.fp_model or parent.fp_model
    settings.instruction_set = settings.instruction_set or parent.instruction_set
    return settings


def msbuild_unit(settings: MsbuildCompileSettings, file: str | None) -> CompileUnit:
    """MSBuild's CL task passes /D (PreprocessorDefinitions), then /U (UndefinePreprocessorDefinitions), then
    AdditionalOptions, which may hold more /D and /U options."""
    flags = strip_clang_cl_prefix(settings.options)
    operations = [("D", define) for define in settings.defines] + [("U", name) for name in settings.undefines]
    operations += define_operations(flags)
    return CompileUnit(apply_define_operations(operations), flags, settings.fp_model, settings.instruction_set, file)


def parse_vcxproj(path: Path) -> Project:
    root = ElementTree.parse(path).getroot()
    base = path.parent
    project = Project(path.stem)
    tags = (
        "PreprocessorDefinitions",
        "UndefinePreprocessorDefinitions",
        "AdditionalOptions",
        "FloatingPointModel",
        "EnableEnhancedInstructionSet",
    )
    settings: dict[str, MsbuildCompileSettings] = {}

    for group in root.findall("msb:PropertyGroup", MSBUILD_NAMESPACE):
        config = condition_configuration(group)
        if config is None:
            continue
        directories = project.include_dirs.setdefault(config, set())
        for tag in ("ExternalIncludePath", "IncludePath"):
            for element in group.findall(f"msb:{tag}", MSBUILD_NAMESPACE):
                directories.update(normalize_path(msbuild_path(base, item)) for item in split_msbuild_list(element.text))

    for group in root.findall("msb:ItemDefinitionGroup", MSBUILD_NAMESPACE):
        config = condition_configuration(group)
        compile_settings = group.find("msb:ClCompile", MSBUILD_NAMESPACE)
        if config is None or compile_settings is None:
            continue
        values: dict[str, str | None] = {}
        for tag in tags + ("AdditionalIncludeDirectories",):
            element = compile_settings.find(f"msb:{tag}", MSBUILD_NAMESPACE)
            values[tag] = element.text or "" if element is not None else None
        project.include_dirs.setdefault(config, set()).update(
            normalize_path(msbuild_path(base, item)) for item in split_msbuild_list(values["AdditionalIncludeDirectories"])
        )
        settings[config] = msbuild_settings(values, None)
        project.units.setdefault(config, []).insert(0, msbuild_unit(settings[config], None))

    for item in root.iter(f"{{{MSBUILD_NAMESPACE['msb']}}}ClCompile"):
        include = item.get("Include")
        if include is None:
            continue
        project.sources.append(msbuild_path(base, include).resolve())
        overrides: dict[str, dict[str, str | None]] = {}
        for child in item:
            config = condition_configuration(child)
            tag = child.tag.split("}", 1)[-1]
            if config is not None:
                overrides.setdefault(config, {})[tag] = child.text or ""
        for config, values in overrides.items():
            if (values.get("ExcludedFromBuild") or "").lower() == "true":
                continue
            if not set(tags) & values.keys() or config not in settings:
                continue
            project.units[config].append(msbuild_unit(msbuild_settings(values, settings[config]), include))
    return project


def parse_slnx_dist_builds(path: Path) -> dict[str, bool]:
    """Return, for every project in the solution, whether it is built in the Dist solution configuration."""
    root = ElementTree.parse(path).getroot()
    platforms = [element.get("Name", "") for element in root.iter("Platform") if element.get("Name")]
    solution_configs = [f"Dist|{platform}" for platform in platforms] or ["Dist|x64"]
    builds: dict[str, bool] = {}
    for element in root.iter("Project"):
        name = Path(element.get("Path", "").replace("\\", "/")).stem
        default = True
        specific: list[tuple[str, bool]] = []
        for build in element.findall("Build"):
            value = build.get("Project", "true").lower() != "false"
            pattern = build.get("Solution")
            if pattern is None:
                default = value
            else:
                specific.append((pattern, value))
        result = True
        for solution_config in solution_configs:
            matched = [value for pattern, value in specific if fnmatch.fnmatchcase(solution_config, pattern)]
            result = result and (matched[-1] if matched else default)
        builds[name] = result
    return builds


def load_visual_studio(directory: Path, target: Target) -> GeneratedWorkspace:
    projects = {project.name: project for project in map(parse_vcxproj, sorted(directory.glob("*.vcxproj")))}
    solutions = sorted(directory.glob("*.slnx"))
    if len(solutions) != 1:
        raise GenerationError(f"expected exactly one .slnx in {directory}, found {len(solutions)}")
    return GeneratedWorkspace(target, projects, parse_slnx_dist_builds(solutions[0]))


# --------------------------------------------------------------------------------------------------------------------
# GNU make (premake gmake)
# --------------------------------------------------------------------------------------------------------------------

MAKE_CONFIG_PATTERN = re.compile(r"^(?:else\s+)?ifeq\s+\(\$\(config\),(\w+)\)\s*$")
MAKE_ASSIGNMENT_PATTERN = re.compile(r"^\s*(DEFINES|INCLUDES|ALL_CXXFLAGS|ALL_CFLAGS|PERFILE_FLAGS_\d+)\s*\+?=\s*(.*)$")
MAKE_CONDITIONAL_PATTERN = re.compile(r"^(?:ifeq|ifneq|ifdef|ifndef)\b")
MAKE_SOURCE_RULE_PATTERN = re.compile(r"^\$\(OBJDIR\)/\S+\.o:\s+(\S+)")
MAKE_VARIABLE_PATTERN = re.compile(r"^\$\((\w+)\)$")


def parse_makefile(path: Path) -> Project:
    """Parse a premake gmake project makefile: per-configuration DEFINES, INCLUDES and compiler flags.

    premake's compile command is $(CXX) $(ALL_CXXFLAGS) ..., where ALL_CXXFLAGS starts with $(ALL_CPPFLAGS) (which
    holds $(DEFINES) and $(INCLUDES)) and continues with the compiler options and buildoptions. So the -D/-U of
    DEFINES come first, then those of the options, then those of a file's PERFILE_FLAGS."""
    project = Project(path.stem)
    base = path.parent
    config_names = {name.lower(): name for name in CONFIGURATIONS}
    # Assignments outside any "ifeq ($(config),...)" block apply to every configuration; premake writes a variable
    # there when it is identical in all of them.
    shared = "*"
    current: str = shared
    values: dict[str, dict[str, list[str]]] = {}
    # Conditionals nest: the Clang makefiles have "ifeq ($(origin CC), default) ... endif" inside each configuration
    # block. depth counts the open conditionals; the configuration block is the outermost one.
    depth = 0

    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        match = MAKE_CONFIG_PATTERN.match(line)
        if match and (depth == 0 or (depth == 1 and stripped.startswith("else"))):
            current = config_names.get(match.group(1).lower(), match.group(1))
            depth = 1
            continue
        if MAKE_CONDITIONAL_PATTERN.match(stripped):
            depth += 1
            continue
        if stripped == "endif":
            depth = max(0, depth - 1)
            if depth == 0:
                current = shared
            continue
        source = MAKE_SOURCE_RULE_PATTERN.match(line)
        if source:
            project.sources.append((base / source.group(1)).resolve())
            continue
        assignment = MAKE_ASSIGNMENT_PATTERN.match(line)
        if assignment:
            tokens = shlex.split(assignment.group(2), posix=True)
            values.setdefault(current, {}).setdefault(assignment.group(1), []).extend(tokens)

    common = values.pop(shared, {})
    for config in [name for name in CONFIGURATIONS if name in values] or list(CONFIGURATIONS):
        variables: dict[str, list[str]] = {}
        for source_values in (common, values.get(config, {})):
            for name, tokens in source_values.items():
                variables.setdefault(name, []).extend(tokens)
        includes: set[str] = set()
        tokens = variables.get("INCLUDES", [])
        for index, token in enumerate(tokens):
            if token.startswith("-isystem") and len(token) > len("-isystem"):
                includes.add(normalize_path(base / token[len("-isystem") :]))
            elif token.startswith("-I") and len(token) > 2:
                includes.add(normalize_path(base / token[2:]))
            elif token in ("-isystem", "-I") and index + 1 < len(tokens):
                includes.add(normalize_path(base / tokens[index + 1]))
        project.include_dirs[config] = includes

        def expand(name: str, variables: dict[str, list[str]] = variables) -> list[str]:
            result: list[str] = []
            for token in variables.get(name, []):
                reference = MAKE_VARIABLE_PATTERN.match(token)
                if reference is None:
                    result.append(token)
                elif reference.group(1) in variables and reference.group(1) != name:
                    result.extend(expand(reference.group(1)))
            return result

        define_flags = variables.get("DEFINES", [])
        # First-party projects and JoltPhysics are C++: their translation units compile with ALL_CXXFLAGS.
        language = "ALL_CXXFLAGS" if "ALL_CXXFLAGS" in variables else "ALL_CFLAGS"
        flags = expand(language)
        units = [CompileUnit(apply_define_operations(define_operations(define_flags + flags)), flags)]
        for name in sorted(variable for variable in variables if variable.startswith("PERFILE_FLAGS_")):
            file_flags = expand(name)
            operations = define_operations(define_flags + file_flags)
            units.append(CompileUnit(apply_define_operations(operations), file_flags, file=name))
        project.units[config] = units
    return project


def load_gmake(directory: Path, target: Target) -> GeneratedWorkspace:
    projects = {project.name: project for project in map(parse_makefile, sorted(directory.glob("*.make")))}
    return GeneratedWorkspace(target, projects)


# --------------------------------------------------------------------------------------------------------------------
# Xcode (premake xcode4, project.pbxproj in the old-style property list format)
# --------------------------------------------------------------------------------------------------------------------

PLIST_TOKEN_PATTERN = re.compile(
    r'\s+|/\*.*?\*/|//[^\n]*|"(?:[^"\\]|\\.)*"|[{}();=,]|[^\s{}();=,"]+',
    re.DOTALL,
)


def parse_old_style_plist(text: str) -> Any:
    tokens: list[str] = []
    for match in PLIST_TOKEN_PATTERN.finditer(text):
        token = match.group(0)
        if token.isspace() or token.startswith(("/*", "//")):
            continue
        tokens.append(token)
    position = 0

    def value() -> Any:
        nonlocal position
        token = tokens[position]
        position += 1
        if token == "{":
            result: dict[str, Any] = {}
            while tokens[position] != "}":
                key = scalar(tokens[position])
                position += 1
                if tokens[position] != "=":
                    raise ValueError(f"expected '=' after key {key!r}")
                position += 1
                result[key] = value()
                if tokens[position] == ";":
                    position += 1
            position += 1
            return result
        if token == "(":
            items: list[Any] = []
            while tokens[position] != ")":
                items.append(value())
                if tokens[position] == ",":
                    position += 1
            position += 1
            return items
        return scalar(token)

    def scalar(token: str) -> str:
        if token.startswith('"'):
            body = token[1:-1]
            return re.sub(r"\\(.)", lambda escape: {"n": "\n", "t": "\t"}.get(escape.group(1), escape.group(1)), body)
        return token

    return value()


def xcode_list(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, list):
        return [str(item) for item in value]
    return shlex.split(str(value), posix=True)


def merge_xcode_setting(inherited: list[str], own: list[str] | None) -> list[str]:
    if own is None:
        return list(inherited)
    result: list[str] = []
    for item in own:
        result.extend(inherited if item == "$(inherited)" else [item])
    return result


def parse_pbxproj(path: Path) -> Project:
    root = parse_old_style_plist(path.read_text(encoding="utf-8"))
    objects: dict[str, dict[str, Any]] = root["objects"]
    source_root = path.parent.parent  # SRCROOT: the directory that contains the .xcodeproj
    project = Project(path.parent.stem)

    def configurations(list_id: str) -> dict[str, dict[str, Any]]:
        configuration_list = objects[list_id]
        return {
            objects[item]["name"]: objects[item].get("buildSettings", {})
            for item in configuration_list.get("buildConfigurations", [])
        }

    pbx_project = next(entry for entry in objects.values() if entry.get("isa") == "PBXProject")
    project_level = configurations(pbx_project["buildConfigurationList"])
    targets = [entry for entry in objects.values() if entry.get("isa") in ("PBXNativeTarget", "PBXAggregateTarget")]
    target_level = configurations(targets[0]["buildConfigurationList"]) if targets else {}

    # Source files: resolve PBXFileReference paths through their PBXGroup ancestors.
    parents: dict[str, str] = {}
    for identifier, entry in objects.items():
        if entry.get("isa") in ("PBXGroup", "PBXVariantGroup"):
            for child in entry.get("children", []):
                parents[child] = identifier

    def file_path(identifier: str) -> Path:
        entry = objects[identifier]
        parts = [entry.get("path", "")]
        tree = entry.get("sourceTree", "<group>")
        current = identifier
        while tree == "<group>" and current in parents:
            current = parents[current]
            group = objects[current]
            if "path" in group:
                parts.append(group["path"])
            tree = group.get("sourceTree", "<group>")
        relative = Path(*reversed([part for part in parts if part])) if any(parts) else Path()
        return relative if tree == "<absolute>" else source_root / relative

    per_file_flags: list[tuple[str, list[str]]] = []
    for entry in objects.values():
        if entry.get("isa") == "PBXSourcesBuildPhase":
            for build_file in entry.get("files", []):
                reference = objects[build_file].get("fileRef")
                if reference in objects:
                    source = file_path(reference).resolve()
                    project.sources.append(source)
                    compiler_flags = objects[build_file].get("settings", {}).get("COMPILER_FLAGS")
                    if compiler_flags:
                        per_file_flags.append((source.name, xcode_list(compiler_flags)))

    for config in CONFIGURATIONS:
        if config not in project_level and config not in target_level:
            continue
        merged: dict[str, list[str]] = {}
        for key in (
            "GCC_PREPROCESSOR_DEFINITIONS",
            "HEADER_SEARCH_PATHS",
            "USER_HEADER_SEARCH_PATHS",
            "SYSTEM_HEADER_SEARCH_PATHS",
            "OTHER_CFLAGS",
            "OTHER_CPLUSPLUSFLAGS",
        ):
            inherited = xcode_list(project_level.get(config, {}).get(key))
            own = target_level.get(config, {}).get(key)
            merged[key] = merge_xcode_setting(inherited, None if own is None else xcode_list(own))
        fast_math = "YES" in (
            target_level.get(config, {}).get("GCC_FAST_MATH"),
            project_level.get(config, {}).get("GCC_FAST_MATH"),
        )

        # C++ files compile with OTHER_CPLUSPLUSFLAGS, whose default is $(OTHER_CFLAGS). GCC_FAST_MATH adds
        # -ffast-math ahead of them. Xcode passes GCC_PREPROCESSOR_DEFINITIONS as -D options before them.
        other_c = merged["OTHER_CFLAGS"]
        other_cpp: list[str] = []
        for flag in merged["OTHER_CPLUSPLUSFLAGS"] or ["$(OTHER_CFLAGS)"]:
            other_cpp.extend(other_c if flag == "$(OTHER_CFLAGS)" else [flag])
        flags = (["-ffast-math"] if fast_math else []) + other_cpp
        listed = [("D", define) for define in merged["GCC_PREPROCESSOR_DEFINITIONS"] if define != "$(inherited)"]
        project.units[config] = [CompileUnit(apply_define_operations(listed + define_operations(flags)), flags)]
        for file_name, file_flags in per_file_flags:
            combined = flags + file_flags
            defines = apply_define_operations(listed + define_operations(combined))
            project.units[config].append(CompileUnit(defines, combined, file=file_name))
        project.include_dirs[config] = {
            normalize_path(source_root / directory)
            for key in ("HEADER_SEARCH_PATHS", "USER_HEADER_SEARCH_PATHS", "SYSTEM_HEADER_SEARCH_PATHS")
            for directory in merged[key]
            if not directory.startswith("$(")
        }
    return project


def load_xcode(directory: Path, target: Target) -> GeneratedWorkspace:
    projects = {
        project.name: project for project in map(parse_pbxproj, sorted(directory.glob("*.xcodeproj/project.pbxproj")))
    }
    return GeneratedWorkspace(target, projects)


LOADERS: dict[str, Callable[[Path, Target], GeneratedWorkspace]] = {
    "vs2026": load_visual_studio,
    "gmake": load_gmake,
    "xcode4": load_xcode,
}


# --------------------------------------------------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------------------------------------------------


def classify(projects: dict[str, Project]) -> tuple[set[str], set[str]]:
    """Split projects into vendored libraries (every compiled source under Vendor/) and first-party projects."""
    vendor_root = normalize_path(VENDOR_ROOT) + os.sep
    vendored: set[str] = set()
    first_party: set[str] = set()
    for name, project in projects.items():
        compiled = [source for source in project.sources if source.suffix.lower() in (".c", ".cc", ".cpp", ".m", ".mm")]
        if not compiled:
            continue
        if all(normalize_path(source).startswith(vendor_root) for source in compiled):
            vendored.add(name)
        else:
            first_party.add(name)
    return vendored, first_party


def abi_filter(vendor: str) -> Callable[[str], bool]:
    family = VENDOR_ABI_FAMILIES.get(vendor, DefineFamily())
    private = VENDOR_PRIVATE_DEFINES.get(vendor, frozenset())

    def relevant(define: str) -> bool:
        name = define_name(define)
        if name in private:
            return False
        return name in GLOBAL_ABI_DEFINES or family.contains(name)

    return relevant


def consumers_of(vendor: Project, projects: dict[str, Project], config: str) -> list[Project]:
    own = vendor.include_dirs.get(config, set())
    return [
        project
        for project in projects.values()
        if project.name != vendor.name and own & project.include_dirs.get(config, set())
    ]


def unit_location(project: str, unit: CompileUnit) -> str:
    return project + (f" (file {unit.file})" if unit.file else "")


def check_abi_defines(workspace: GeneratedWorkspace, vendored: set[str], findings: list[str]) -> None:
    target = workspace.target.name
    for vendor_name in sorted(vendored & workspace.projects.keys()):
        vendor = workspace.projects[vendor_name]
        relevant = abi_filter(vendor_name)
        for config in CONFIGURATIONS:
            if config not in vendor.units:
                continue
            expected = {define for define in vendor.units[config][0].defines if relevant(define)}
            for unit in vendor.units[config][1:]:
                own = {define for define in unit.defines if relevant(define)}
                if own != expected:
                    findings.append(
                        f"[{target}] {config}: {vendor_name} file {unit.file} overrides ABI defines "
                        f"{sorted(own ^ expected)}"
                    )
            for consumer in consumers_of(vendor, workspace.projects, config):
                for unit in consumer.units.get(config, []):
                    actual = {define for define in unit.defines if relevant(define)}
                    where = unit_location(consumer.name, unit)
                    # A missing or extra JPH_CROSS_PLATFORM_DETERMINISTIC has its own finding (determinism check).
                    missing = sorted(d for d in expected - actual if define_name(d) != DETERMINISM_DEFINE)
                    extra = sorted(d for d in actual - expected if define_name(d) != DETERMINISM_DEFINE)
                    if missing:
                        findings.append(
                            f"[{target}] {config}: {where} includes {vendor_name} headers but lacks "
                            f"{', '.join(missing)} that {vendor_name} defines (ABI mismatch)"
                        )
                    if extra:
                        findings.append(
                            f"[{target}] {config}: {where} includes {vendor_name} headers but defines "
                            f"{', '.join(extra)} that {vendor_name} does not (ABI mismatch)"
                        )


def check_determinism_define(workspace: GeneratedWorkspace, findings: list[str]) -> None:
    target = workspace.target.name
    jolt = workspace.projects.get(JOLT_PROJECT)
    if jolt is None:
        findings.append(f"[{target}] project {JOLT_PROJECT} not found in the generated workspace")
        return
    for config in CONFIGURATIONS:
        units = jolt.units.get(config)
        if not units:
            findings.append(f"[{target}] {JOLT_PROJECT} has no {config} configuration")
            continue
        for unit in units:
            if DETERMINISM_DEFINE not in map(define_name, unit.defines):
                findings.append(
                    f"[{target}] {config}: {unit_location(JOLT_PROJECT, unit)} does not define {DETERMINISM_DEFINE}"
                )
        for consumer in consumers_of(jolt, workspace.projects, config):
            for unit in consumer.units.get(config, []):
                if DETERMINISM_DEFINE not in map(define_name, unit.defines):
                    findings.append(
                        f"[{target}] {config}: {unit_location(consumer.name, unit)} includes Jolt headers but does "
                        f"not define {DETERMINISM_DEFINE}"
                    )


def instruction_set(unit: CompileUnit) -> dict[str, str]:
    """The effective instruction-set options of a unit: /arch: (or EnableEnhancedInstructionSet), -march=, -mcpu=
    and every -m<feature>/-mno-<feature>, the last option of each kind winning."""
    state: dict[str, str] = {}
    if unit.msvc_instruction_set:
        state["/arch:"] = unit.msvc_instruction_set
    for flag in unit.flags:
        lower = flag.lower()
        if lower.startswith(("/arch:", "-arch:")):
            state["/arch:"] = flag.split(":", 1)[1].upper()
        elif flag.startswith(("-march=", "-mcpu=")):
            option, value = flag.split("=", 1)
            state[option + "="] = value
        else:
            match = GNU_ISA_FEATURE_PATTERN.match(flag)
            if match:
                state["-m" + match.group(2)] = "off" if match.group(1) else "on"
    return state


def describe_instruction_set(state: dict[str, str]) -> str:
    if not state:
        return "compiler default"
    words = []
    for option, value in sorted(state.items()):
        if option.startswith("-m") and not option.endswith("="):
            words.append(option if value == "on" else "-mno-" + option[2:])
        else:
            words.append(option + value)
    return " ".join(words)


def check_jolt_instruction_set(workspace: GeneratedWorkspace, findings: list[str]) -> None:
    target = workspace.target.name
    jolt = workspace.projects.get(JOLT_PROJECT)
    if jolt is None:
        return
    for config in CONFIGURATIONS:
        units = jolt.units.get(config)
        if not units:
            continue
        expected = instruction_set(units[0])
        checked = [(JOLT_PROJECT, unit) for unit in units[1:]]
        checked += [
            (consumer.name, unit)
            for consumer in consumers_of(jolt, workspace.projects, config)
            for unit in consumer.units.get(config, [])
        ]
        for name, unit in checked:
            actual = instruction_set(unit)
            if actual != expected:
                findings.append(
                    f"[{target}] {config}: {unit_location(name, unit)} is compiled for a different instruction set "
                    f"({describe_instruction_set(actual)}) than JoltPhysics ({describe_instruction_set(expected)}); "
                    "Jolt's inline vector code and its JPH_USE_* feature macros must match the library (ODR, "
                    "determinism; Docs/Architecture.md section 2.2)"
                )


def gnu_floating_point_problems(flags: list[str]) -> list[str]:
    """Evaluate GCC/Clang floating-point options in command-line order (the last one wins)."""
    components = dict.fromkeys(FAST_MATH_COMPONENTS, False)
    fast_math = False
    contract: str | None = None
    for flag in flags:
        if flag in ("-ffast-math", "-Ofast"):
            fast_math = True
            components = dict.fromkeys(FAST_MATH_COMPONENTS, True)
        elif flag == "-fno-fast-math":
            fast_math = False
            components = dict.fromkeys(FAST_MATH_COMPONENTS, False)
        elif flag in FAST_MATH_COMPONENT_OPTIONS:
            component, enabled = FAST_MATH_COMPONENT_OPTIONS[flag]
            components[component] = enabled
        elif flag.startswith("-ffp-model="):
            model = flag.split("=", 1)[1]
            fast_math = model in ("fast", "aggressive")
            components = dict.fromkeys(FAST_MATH_COMPONENTS, fast_math)
            contract = {"fast": "fast", "aggressive": "fast", "precise": "on", "strict": "off"}.get(model, contract)
        elif flag.startswith("-ffp-contract="):
            contract = flag.split("=", 1)[1]
    problems = []
    if fast_math:
        problems.append("fast math is in effect (-ffast-math, -Ofast or -ffp-model=fast)")
    enabled = [component for component, on in components.items() if on]
    if enabled and not fast_math:
        problems.append(f"fast-math components are in effect ({', '.join(enabled)})")
    if contract != "off":
        if contract is None:
            problems.append("-ffp-contract=off is missing (GCC's C++ default is 'fast', clang-cl's is 'on')")
        else:
            problems.append(f"-ffp-contract is {contract!r} instead of 'off'")
    return problems


def msvc_floating_point_problems(unit: CompileUnit) -> list[str]:
    problems = []
    if unit.msvc_fp_model and unit.msvc_fp_model.lower() == "fast":
        problems.append("FloatingPointModel=Fast")
    for flag in unit.flags:
        normalized = "/" + flag[1:].lower() if flag[:1] in "-/" else flag.lower()
        if normalized in MSVC_UNSAFE_FP_OPTIONS:
            problems.append(flag)
    return problems


def check_floating_point(workspace: GeneratedWorkspace, first_party: set[str], findings: list[str]) -> None:
    target = workspace.target
    for name in sorted((first_party | {JOLT_PROJECT}) & workspace.projects.keys()):
        for config, units in workspace.projects[name].units.items():
            for unit in units:
                problems: list[str] = []
                if target.dialect in ("msvc", "clang-cl"):
                    problems += msvc_floating_point_problems(unit)
                if target.dialect in ("gnu", "clang-cl"):
                    problems += gnu_floating_point_problems(unit.flags)
                for problem in problems:
                    findings.append(
                        f"[{target.name}] {config}: {unit_location(name, unit)} uses a non-precise floating-point "
                        f"model: {problem} (first-party projects and JoltPhysics must use the precise model, "
                        "Docs/Architecture.md section 2.2)"
                    )


def check_solution(workspace: GeneratedWorkspace, findings: list[str]) -> None:
    target = workspace.target.name
    builds = workspace.dist_builds or {}
    for name in NOT_IN_DIST:
        if name not in builds:
            findings.append(f"[{target}] project {name} is missing from the solution")
        elif builds[name]:
            findings.append(f"[{target}] the solution builds {name} in Dist (it must have Build=false there)")
    for name in BUILT_IN_DIST:
        if name not in builds:
            findings.append(f"[{target}] project {name} is missing from the solution")
        elif not builds[name]:
            findings.append(f"[{target}] the solution does not build {name} in Dist")


def check_workspace(workspace: GeneratedWorkspace, vendored: set[str], first_party: set[str]) -> list[str]:
    findings: list[str] = []
    check_abi_defines(workspace, vendored, findings)
    check_determinism_define(workspace, findings)
    check_jolt_instruction_set(workspace, findings)
    check_floating_point(workspace, first_party, findings)
    if workspace.dist_builds is not None:
        check_solution(workspace, findings)
    return findings


# --------------------------------------------------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------------------------------------------------


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    names = ", ".join(target.name for target in TARGETS)
    parser = argparse.ArgumentParser(
        description="Generate the workspace for every build target into a temporary directory and check vendor/"
        "consumer ABI defines, JPH_CROSS_PLATFORM_DETERMINISTIC, the Jolt instruction set, the precise floating-point "
        "model and the Dist solution configuration (Docs/Architecture.md section 2.2).",
        epilog="Exit codes: 0 no findings, 1 findings or generation failure, 2 usage error, 3 premake missing or not "
        "the pinned version, 5 premake timed out.",
    )
    parser.add_argument(
        "--workspace",
        type=Path,
        default=REPOSITORY_ROOT,
        help="directory containing the premake5.lua to check (default: the repository root)",
    )
    parser.add_argument(
        "--targets",
        default=",".join(target.name for target in TARGETS),
        help=f"comma-separated targets (default: all of {names}); windows is always included",
    )
    parser.add_argument(
        "--premake", type=Path, help="premake5 to use; it must be the pinned version (default: Vendor/premake/bin)"
    )
    parser.add_argument("--keep", action="store_true", help="keep the generated projects and print their location")
    parser.add_argument("--json", action="store_true", help="print a machine-readable report on stdout")
    return parser.parse_args(argv)


def run(arguments: argparse.Namespace) -> dict[str, Any]:
    workspace_script = (arguments.workspace / "premake5.lua").resolve()
    if not workspace_script.is_file():
        raise UsageError(f"no premake5.lua in {arguments.workspace}")
    requested = [name.strip() for name in arguments.targets.split(",") if name.strip()]
    unknown = sorted(set(requested) - TARGET_BY_NAME.keys())
    if unknown:
        raise UsageError(f"unknown target(s) {', '.join(unknown)}; choose from {', '.join(TARGET_BY_NAME)}")
    # Vendored and first-party projects are classified from the Windows projects, so windows is always generated.
    targets = [target for target in TARGETS if target.name == "windows" or target.name in requested]
    premake_executable = premake.require_pinned(arguments.premake)

    temporary = Path(tempfile.mkdtemp(prefix="CheckBuildConfig-"))
    try:
        loaded: list[GeneratedWorkspace] = []
        for target in targets:
            output = temporary / target.name
            generate(premake_executable, workspace_script, target, output)
            loaded.append(LOADERS[target.action](output, target))

        vendored, first_party = classify(loaded[0].projects)
        findings: list[str] = []
        for workspace in loaded:
            findings += check_workspace(workspace, vendored, first_party)
        return {
            "workspace": workspace_script.as_posix(),
            "targets": [target.name for target in targets],
            "vendorProjects": sorted(vendored),
            "firstPartyProjects": sorted(first_party),
            "findings": findings,
            "generatedProjects": temporary.as_posix() if arguments.keep else None,
        }
    finally:
        if not arguments.keep:
            shutil.rmtree(temporary, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    try:
        report = run(arguments)
    except (
        UsageError,
        GenerationError,
        GenerationTimeout,
        premake.PremakeError,
        ToolNotFoundError,
        OSError,
        ElementTree.ParseError,
        ValueError,
        KeyError,
        IndexError,
        StopIteration,
    ) as error:
        if arguments.json:
            print(json.dumps({"success": False, "error": str(error)}, indent=2))
        print(f"CheckBuildConfig.py: error: {error}", file=sys.stderr)
        if not arguments.json:
            print(f"[FAILED] checkbuildconfig: {error}")
        if isinstance(error, (premake.PremakeError, ToolNotFoundError)):
            return EXIT_INIT_FAILED
        if isinstance(error, GenerationTimeout):
            return EXIT_TIMEOUT
        return EXIT_USAGE if isinstance(error, UsageError) else EXIT_FAILED

    report["success"] = not report["findings"]
    if arguments.json:
        print(json.dumps(report, indent=2))
    else:
        for finding in report["findings"]:
            print(f"CheckBuildConfig.py: error: {finding}", file=sys.stderr)
        status = "passed" if report["success"] else f"FAILED with {len(report['findings'])} finding(s)"
        print(
            f"CheckBuildConfig: {status} ({', '.join(report['targets'])}; {len(report['vendorProjects'])} vendored, "
            f"{len(report['firstPartyProjects'])} first-party projects; workspace {report['workspace']})"
        )
        if report["generatedProjects"]:
            print(f"Generated projects kept in {report['generatedProjects']}")
        if report["findings"]:
            # The summary line CI.py and PreCommit.py quote for a failing step.
            print(f"[FAILED] checkbuildconfig: {len(report['findings'])} finding(s), first: {report['findings'][0]}")
    return EXIT_SUCCESS if report["success"] else EXIT_FAILED


if __name__ == "__main__":
    sys.exit(main())
