"""The Dist map-file check (Docs/Architecture.md §14.4; Roadmap M15 acceptance "Dist: map file contains no automation,
Analysis, ImGui or AssetPipeline symbols"; Docs/Decisions/0024-m15-contract.md decision 11). Stream D implements this
module and Scripts/CheckRuntimeMap.py; CI.py runs the check right after its Dist build, as a step named with the
acceptance name verbatim, and the static checks run CheckRuntimeMap.py --self-test over Tests/Data/MapFiles/.

The Runtime links a map file in every configuration (Runtime/premake5.lua). The parser reads the four formats the
supported linkers write:
  - "msvc": link.exe /MAP ("Publics by Value" and "Static symbols": address, decorated name, Rva+Base, Lib:Object);
  - "lld": LLVM lld's map, from ld.lld -Map (the Linux Clang job: "VMA LMA Size Align Out In Symbol") and lld-link /MAP
    when a toolset links with it ("Address Size Align Out In Symbol"); the input lines name archive(member);
  - "gnu": GNU ld -Map (the Linux GCC job: input sections with their archive member, then the symbol lines);
  - "ld64": Apple ld -map ("# Object files:" with [index] archive(member), "# Symbols:" address size [index] name).
LTO partitions lose the archive in every format, so families are matched on symbol names alone there.

Symbol names are matched only through namespace or type components of decorated, mangled or demangled names (MSVC
"@ImGui@@", "@Frontend@Luau@@"; Itanium "N5ImGui", "N4Luau8Frontend"; demangled "ImGui::", "Luau::Frontend::"), never raw
substrings, so string literals ("??_C@_05...ImGui...") never count. A symbol's origin counts only through its library
component (the "<library>" of "<library>:<object>" or "<archive>(<member>)": imgui.lib, libLuauAnalysis.a), never through
its object-file stem, so MSVC's "Watchdog1.obj" never counts. The families, derived from the headers so a new type or
function is covered without editing this module:
  - automation: every type and free function declared in Engine/Source/Engine/Automation/**.h (AutomationServer and the
    RPC parsers among them);
  - Analysis: Luau::Frontend and any symbol whose origin is the LuauAnalysis library;
  - ImGui: the ImGui namespace, any symbol whose origin is the ImGui library, and every type and free function declared in
    Engine/Source/Engine/ImGui/**.h (ImGuiLayer, ImGuiRenderer, ImGuiScreenshot, StatsOverlay and their peers);
  - AssetPipeline: every type and free function declared in Engine/Source/Engine/AssetPipeline/**.h.
The acceptance step names exactly these four. If stream D removes the Luau compiler from Dist (§14.4 "no Luau compiler"),
the same change appends "LuauCompiler" (Luau::Compiler, Luau::Parser, luau_compile and any symbol whose origin is the
Luau.Compiler or Luau.Ast library) to FORBIDDEN_FAMILIES with its fixtures, so the compiler cannot return unnoticed; that
extension of the frozen API is approved by ADR 0024 decision 11.
REQUIRED_SYMBOLS must be present, so the FeatureTest harness provably stays in Dist (§14.4).
"""

from __future__ import annotations

import dataclasses
import re
from pathlib import Path

FORBIDDEN_FAMILIES = ("automation", "Analysis", "ImGui", "AssetPipeline")
# Types the Dist Runtime must still contain (§14.4: "the FeatureTest harness stays in Dist").
REQUIRED_SYMBOLS = ("FeatureTestRunner", "ReplayPlayer")
MAP_FORMATS = ("msvc", "lld", "gnu", "ld64")


@dataclasses.dataclass(frozen=True)
class MapSymbol:
    name: str  # the decorated (MSVC), mangled (Itanium) or demangled name as the map lists it
    origin: str  # "<library>:<object>", "<archive>(<member>)" or the object file; "" when the map does not say
    library: str  # the origin's library or archive component ("imgui.lib", "libLuauAnalysis.a"); "" for a bare object


@dataclasses.dataclass(frozen=True)
class FamilyPatterns:
    """How one family is recognized: a symbol belongs to it when a `names` pattern matches its name or a `libraries`
    pattern matches its library component."""

    names: tuple[re.Pattern[str], ...]
    libraries: tuple[re.Pattern[str], ...]


@dataclasses.dataclass(frozen=True)
class MapFile:
    format: str  # one of MAP_FORMATS
    symbols: tuple[MapSymbol, ...]


def parse_map(text: str) -> MapFile:
    """Parses a map file's text, detecting its format. Raises ValueError for text that matches none of MAP_FORMATS or
    lists no symbol."""
    raise NotImplementedError("contract stub: runtime map (ADR 0024 decision 11, stream D)")


def family_patterns(repository_root: Path) -> dict[str, FamilyPatterns]:
    """The patterns of each FORBIDDEN_FAMILIES entry, derived from the headers the module docstring names (so a new
    Automation, ImGui or AssetPipeline type or function is covered without editing this module)."""
    raise NotImplementedError("contract stub: runtime map (ADR 0024 decision 11, stream D)")


def check_map(map_file: MapFile, patterns: dict[str, FamilyPatterns]) -> list[str]:
    """The findings of a Dist map: "forbidden <family>: <symbol> (<origin>)" per matching symbol, sorted, then "missing
    required symbol: <name>" for each REQUIRED_SYMBOLS entry no symbol names."""
    raise NotImplementedError("contract stub: runtime map (ADR 0024 decision 11, stream D)")
