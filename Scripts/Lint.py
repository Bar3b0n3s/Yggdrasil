#!/usr/bin/env python3
"""Static checks for first-party code (Docs/Architecture.md sections 2.3, 3, 4.6, 4.12 and Appendix A; CodeStyle.md).

Steps, selected with --steps (default: all, in this order):

  includes  CheckIncludes, against Scripts/ModuleRules.json: module layers and their may-include lists, the explicit
            layer-5 DAG, the project rules (EditorCore, Editor, Runtime, Tests), third-party and OS headers per
            module and file kind (public header or private file), Private/ headers, the NVRHI-free snapshot headers
            (RenderSnapshot.h and DebugDrawList.h, direct and transitive includes) and include spelling (full paths
            from an include root, quotes only for repository headers, no .cpp except the vendored implementation
            sources ModuleRules.json's SourceIncludes grants one file each, no precompiled header in a header,
            never inside a namespace).
  banned    banned APIs and constructs: throw and the std::rethrow/throw_with_nested family; try/catch outside the
            boundary files of section 4.6; std::filesystem calls without an std::error_code; nlohmann json
            at()/get<>() outside Core/Json; CRT transcendental functions (also through namespace aliases) and glm
            functions that call them on the simulation path (section 4.12); #define/#undef of the ABI-relevant
            configuration macros of vendored libraries and first-party settings (section 2.2); rand()/srand();
            dynamic_cast/typeid; output other than the logger; assert(); naked new/delete; the product name in C++,
            Slang, Python and Lua code; and what CodeStyle section 13 forbids. json at()/get<>() receivers are found
            by type with clang-query on the flags from compile_commands.json when it is available (aliases, values
            returned by functions and call chains included); otherwise a regex checker recognises receivers declared
            with a json type (or an alias of one) in the file or the header it implements.
  contract  milestone completeness (Roadmap rule 3, Docs/Decisions/0004-contract-stub-gate.md): ENGINE_CONTRACT_STUB
            anywhere in first-party C++ but its definition (contract-stub), and doctest::skip in Tests/Source unless
            the same TEST_CASE/TEST_CASE_FIXTURE/SUBCASE decorator expression also carries
            doctest::test_suite(Test::ChildTargetSuite), the child-process targets that are the only permanent skips
            (test-skip). Comments and string literals are ignored, decorators may span lines, and doctest may be named
            through a namespace alias or a using-directive. The Python equivalents (Docs/Decisions/0008-m4-decisions.md
            decision 16): a stub that raises NotImplementedError("contract stub ...") in Contract.PythonStubFiles
            (contract-stub), and unittest/pytest skip markers in Contract.PythonTestFiles (test-skip): the skip,
            skipIf, skipUnless and expectedFailure decorators, pytest.mark.skip, skipif and xfail, self.skipTest(...)
            and raising SkipTest. --allow-contract-stubs (contract mode, the commit of a milestone's contract task)
            reports what it found as allowed instead of as findings.
  naming    identifier naming. clang-tidy runs with .clang-tidy on the flags from compile_commands.json when it is
            available; otherwise a regex checker applies the same prefixes (m_, s_, g_) and PascalCase rules to types,
            functions, data members, statics, globals and macros. The mode that ran is reported. C++ file and
            directory names are checked in both modes.
  headers   header self-containment: every first-party header is compiled on its own, syntax-only, with the include
            paths and defines of its project from compile_commands.json, in a temporary directory. It is included
            twice, which also proves its include guard. Headers also start with #pragma once.
  python    syntax (also under the Python 3.10 grammar, f-strings included) and AST style checks (CodeStyle section
            15) for Scripts/, Tools/ and Tests/.
  json      strict JSON validity (UTF-8 without BOM, no NaN or Infinity, no duplicate keys) of project and data files.

compile_commands.json is generated into a temporary directory with `premake5 compile-commands`
(Scripts/Premake/CompileCommands.lua) unless --compile-commands names an existing one. Nothing is built and nothing
is written into the tree.

Findings are printed as <file>:<line>: <code>: <message>, with the file relative to --root. --root lints another tree
with the repository layout, such as a fixture under Tests/Data/Lint/; the rules, .clang-tidy and the vendored headers
still come from this repository. --self-test runs every fixture listed in Tests/Data/Lint/Fixtures.json and passes
only when each one fails with exactly its expected findings.

--mode selects the checkers that need clang tools (clang-tidy for naming, clang-query for json access): "auto" uses
each tool when it is installed and falls back to the regex checker otherwise, "clang" requires them, "regex" forces the
fallbacks. premake is the pinned release (Scripts/Lib/premake.py).

Exit codes: 0 no findings, 1 findings, a failed self-test or a tool that failed, 2 usage error or invalid rules,
3 a required tool (premake, clang-tidy or clang-query in --mode clang, a C++ compiler) is missing or has the wrong
version, 5 a tool timed out.
"""

from __future__ import annotations

import argparse
import ast
import bisect
import concurrent.futures
import dataclasses
import io
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import tokenize
from pathlib import Path
from typing import Any, Callable, Iterable

from Lib import paths as repository_paths
from Lib import premake, toolchain
from Lib.process import ToolNotFoundError
from Lib.report import EXIT_FAILED, EXIT_INIT_FAILED, EXIT_SUCCESS, EXIT_TIMEOUT, EXIT_USAGE

REPOSITORY_ROOT = repository_paths.REPOSITORY_ROOT
DEFAULT_RULES_PATH = REPOSITORY_ROOT / "Scripts" / "ModuleRules.json"
CLANG_TIDY_CONFIG_PATH = REPOSITORY_ROOT / ".clang-tidy"
FIXTURE_MANIFEST_PATH = REPOSITORY_ROOT / "Tests" / "Data" / "Lint" / "Fixtures.json"

STEPS = ("includes", "banned", "contract", "naming", "headers", "python", "json")
STEP_TITLES = {
    "includes": "CheckIncludes",
    "banned": "Banned APIs",
    "contract": "Contract stubs and skipped tests",
    "naming": "Naming",
    "headers": "Header self-containment",
    "python": "Python",
    "json": "JSON",
}
MODES = ("auto", "clang", "regex")
MINIMUM_CLANG_TIDY_MAJOR = toolchain.MINIMUM_CLANG_TIDY
LLVM_MAJOR_VERSIONS = tuple(range(30, MINIMUM_CLANG_TIDY_MAJOR - 1, -1))
TOOL_TIMEOUT_SECONDS = 600.0
PREMAKE_TIMEOUT_SECONDS = 600.0

EXIT_FINDINGS = EXIT_FAILED

# Directories never walked when the tree is not a git work tree (git ls-files honours .gitignore instead).
WALK_SKIPPED_DIRECTORIES = repository_paths.WALK_SKIPPED_DIRECTORIES

CXX_HEADER_EXTENSION = ".h"
CXX_SOURCE_EXTENSION = ".cpp"
# CodeStyle section 4.1: .h and .cpp only.
FORBIDDEN_CXX_EXTENSIONS = (".hpp", ".hh", ".hxx", ".cc", ".cxx", ".c++", ".inl", ".ipp", ".tpp")

STANDARD_HEADERS = frozenset(
    """
    algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv cfloat charconv chrono cinttypes climits
    clocale cmath codecvt compare complex concepts condition_variable coroutine csetjmp csignal cstdarg cstddef cstdint
    cstdio cstdlib cstring ctime cuchar cwchar cwctype debugging deque exception execution expected filesystem flat_map
    flat_set format forward_list fstream functional future generator hazard_pointer initializer_list inplace_vector
    iomanip ios iosfwd iostream istream iterator latch limits linalg list locale map mdspan memory memory_resource mutex
    new numbers numeric optional ostream print queue random ranges ratio rcu regex scoped_allocator semaphore set
    shared_mutex source_location span spanstream sstream stack stacktrace stdexcept stdfloat stop_token streambuf
    string string_view syncstream system_error text_encoding thread tuple type_traits typeindex typeinfo unordered_map
    unordered_set utility valarray variant vector version
    assert.h ctype.h errno.h fenv.h float.h inttypes.h limits.h locale.h math.h setjmp.h signal.h stdalign.h stdarg.h
    stdbool.h stddef.h stdint.h stdio.h stdlib.h string.h time.h uchar.h wchar.h wctype.h
    """.split()
)

CPP_KEYWORDS = frozenset(
    """
    alignas alignof and asm auto bool break case catch char char8_t char16_t char32_t class co_await co_return co_yield
    concept const consteval constexpr constinit const_cast continue decltype default delete do double dynamic_cast else
    enum explicit export extern false float for friend goto if inline int long mutable namespace new noexcept not
    nullptr operator or private protected public register reinterpret_cast requires return short signed sizeof static
    static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union
    unsigned using virtual void volatile wchar_t while __declspec __attribute__
    """.split()
)


class UsageError(Exception):
    """Bad arguments or invalid rules (exit code 2)."""


class ToolMissing(Exception):
    """A required tool is missing or has the wrong version (exit code 3)."""


class ToolFailure(Exception):
    """A tool ran and failed (exit code 1)."""


@dataclasses.dataclass(frozen=True, order=True)
class Finding:
    file: str
    line: int
    code: str
    message: str

    def format(self) -> str:
        return f"{self.file}:{self.line}: {self.code}: {self.message}"

    def as_dict(self) -> dict[str, Any]:
        return {"file": self.file, "line": self.line, "code": self.code, "message": self.message}


@dataclasses.dataclass
class StepReport:
    name: str
    status: str = "passed"  # passed, failed, skipped
    files: int = 0
    findings: list[Finding] = dataclasses.field(default_factory=list)
    detail: str = ""

    def as_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "title": STEP_TITLES[self.name],
            "status": self.status,
            "files": self.files,
            "detail": self.detail,
            "findings": [finding.as_dict() for finding in self.findings],
        }


# --------------------------------------------------------------------------------------------------------------------
# Paths and globs
# --------------------------------------------------------------------------------------------------------------------


def glob_to_regex(pattern: str) -> re.Pattern[str]:
    """Translate a path glob to a regex: ** spans directories, * and ? stay within one path component."""
    parts: list[str] = []
    index = 0
    while index < len(pattern):
        if pattern.startswith("**/", index):
            parts.append("(?:.*/)?")
            index += 3
        elif pattern.startswith("**", index):
            parts.append(".*")
            index += 2
        elif pattern[index] == "*":
            parts.append("[^/]*")
            index += 1
        elif pattern[index] == "?":
            parts.append("[^/]")
            index += 1
        else:
            parts.append(re.escape(pattern[index]))
            index += 1
    return re.compile("^" + "".join(parts) + "$")


class GlobSet:
    def __init__(self, patterns: Iterable[str]) -> None:
        self.patterns = tuple(patterns)
        self.compiled = tuple(glob_to_regex(pattern) for pattern in self.patterns)

    def matches(self, relative: str) -> bool:
        return any(regex.match(relative) for regex in self.compiled)


def is_under(relative: str, directory: str) -> bool:
    return relative == directory or relative.startswith(directory.rstrip("/") + "/")


def host_system() -> str:
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform == "darwin":
        return "macosx"
    return "linux"


def list_tree_files(root: Path) -> list[str]:
    """Files of the tree as sorted POSIX paths relative to root: tracked and untracked files git does not ignore."""
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
        subdirectories[:] = sorted(name for name in subdirectories if name not in WALK_SKIPPED_DIRECTORIES)
        for name in names:
            files.append((Path(directory) / name).relative_to(root).as_posix())
    return sorted(files)


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")


# --------------------------------------------------------------------------------------------------------------------
# Rules (Scripts/ModuleRules.json)
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class ModuleRule:
    name: str
    layer: int
    includes: tuple[str, ...]
    max_layer: int | None
    headers: tuple[str, ...]
    third_party_public: tuple[str, ...]
    third_party_private: tuple[str, ...]
    system_headers: str | tuple[str, ...]  # "None", "Private" or module subdirectories
    private_directories: tuple[str, ...]
    restricted_third_party: dict[str, GlobSet]  # library -> the only files of the module that may include it


@dataclasses.dataclass
class ProjectRule:
    name: str
    kind: str  # "Engine" or "Project"
    paths: GlobSet
    flags_from: str
    files_outside_modules: dict[str, str]
    modules_allow: tuple[str, ...]
    modules_deny: tuple[str, ...]
    projects: tuple[str, ...]
    third_party_allow: tuple[str, ...]
    third_party_deny: tuple[str, ...]
    restricted_third_party: dict[str, tuple[str, ...]]
    guard_condition: str
    guarded_modules: tuple[str, ...]
    guarded_third_party: tuple[str, ...]
    system_headers: bool


@dataclasses.dataclass
class Rules:
    path: Path
    include_roots: tuple[str, ...]
    cxx_roots: tuple[str, ...]
    engine_module_root: str
    platform_directories: dict[str, tuple[str, ...]]
    third_party: dict[str, tuple[Path, ...]]
    global_public: tuple[str, ...]
    global_private: tuple[str, ...]
    modules: dict[str, ModuleRule]
    layer5_edges: frozenset[tuple[str, str]]
    snapshot_files: tuple[str, ...]
    snapshot_modules: tuple[str, ...]
    snapshot_third_party: tuple[str, ...]
    snapshot_transitive_third_party: tuple[str, ...]
    projects: list[ProjectRule]
    precompiled_headers: dict[str, tuple[str, ...]]
    simulation_modules: tuple[str, ...]
    simulation_exceptions: GlobSet
    try_catch_files: GlobSet
    json_access_files: GlobSet
    abi_macro_files: GlobSet
    new_delete_files: GlobSet
    output_files: GlobSet
    contract_stub_definition_files: GlobSet
    contract_test_files: GlobSet
    contract_python_stub_files: GlobSet
    contract_python_test_files: GlobSet
    source_includes: dict[str, tuple[str, ...]]  # file -> the vendored source files it may include
    python_roots: tuple[str, ...]
    python_exclude: GlobSet
    python_entry_points: GlobSet
    python_libraries: GlobSet
    python_third_party: GlobSet
    json_extensions: tuple[str, ...]
    json_lines_extensions: tuple[str, ...]
    json_exclude: GlobSet

    def project_names(self) -> set[str]:
        return {project.name for project in self.projects}


def _strings(value: Any, where: str) -> tuple[str, ...]:
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise UsageError(f"{where} must be a list of strings")
    return tuple(value)


def _object(value: Any, where: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise UsageError(f"{where} must be an object")
    return value


def load_rules(path: Path) -> Rules:
    try:
        document = _object(json.loads(path.read_text(encoding="utf-8")), str(path))
    except (OSError, json.JSONDecodeError) as error:
        raise UsageError(f"cannot read the rules file {path}: {error}") from error
    if document.get("Format") != "ModuleRules" or document.get("Version") != 1:
        raise UsageError(f"{path}: expected Format 'ModuleRules', Version 1")
    try:
        rules = parse_rules(document, path)
    except KeyError as error:
        raise UsageError(f"invalid rules in {path}: missing key {error}") from error
    except (TypeError, ValueError) as error:
        raise UsageError(f"invalid rules in {path}: {error}") from error
    validate_rules(rules)
    return rules


def parse_rules(document: dict[str, Any], path: Path) -> Rules:
    third_party = {
        name: tuple(REPOSITORY_ROOT / directory for directory in _strings(directories, f"ThirdParty.{name}"))
        for name, directories in _object(document["ThirdParty"], "ThirdParty").items()
    }
    global_third_party = _object(document["GlobalThirdParty"], "GlobalThirdParty")
    default_private = _strings(document.get("PrivateDirectories", []), "PrivateDirectories")

    modules: dict[str, ModuleRule] = {}
    for entry in document["Modules"]:
        entry = _object(entry, "Modules[]")
        name = entry["Name"]
        party = _object(entry.get("ThirdParty", {"Public": [], "Private": []}), f"{name}.ThirdParty")
        system = entry.get("SystemHeaders", "None")
        if isinstance(system, list):
            system = _strings(system, f"{name}.SystemHeaders")
        elif system not in ("None", "Private"):
            raise UsageError(f"Modules.{name}.SystemHeaders must be 'None', 'Private' or a list of directories")
        modules[name] = ModuleRule(
            name=name,
            layer=int(entry["Layer"]),
            includes=_strings(entry.get("Includes", []), f"{name}.Includes"),
            max_layer=int(entry["MaxLayer"]) if "MaxLayer" in entry else None,
            headers=_strings(entry.get("Headers", []), f"{name}.Headers"),
            third_party_public=_strings(party.get("Public", []), f"{name}.ThirdParty.Public"),
            third_party_private=_strings(party.get("Private", []), f"{name}.ThirdParty.Private"),
            system_headers=system,
            private_directories=default_private
            + _strings(entry.get("PrivateDirectories", []), f"{name}.PrivateDirectories"),
            restricted_third_party={
                library: GlobSet(_strings(files, f"{name}.RestrictedThirdParty.{library}"))
                for library, files in _object(
                    entry.get("RestrictedThirdParty", {}), f"{name}.RestrictedThirdParty"
                ).items()
                if library != "Notes"
            },
        )

    projects: list[ProjectRule] = []
    for entry in document["Projects"]:
        entry = _object(entry, "Projects[]")
        name = entry["Name"]
        module_access = _object(entry.get("Modules", {}), f"{name}.Modules")
        party_access = _object(entry.get("ThirdParty", {}), f"{name}.ThirdParty")
        guarded = _object(entry.get("Guarded", {}), f"{name}.Guarded")
        projects.append(
            ProjectRule(
                name=name,
                kind=entry.get("Kind", "Project"),
                paths=GlobSet(_strings(entry["Paths"], f"{name}.Paths")),
                flags_from=entry.get("FlagsFrom", name),
                files_outside_modules=dict(
                    _object(entry.get("FilesOutsideModules", {}), f"{name}.FilesOutsideModules")
                ),
                modules_allow=_strings(module_access.get("Allow", []), f"{name}.Modules.Allow"),
                modules_deny=_strings(module_access.get("Deny", []), f"{name}.Modules.Deny"),
                projects=_strings(entry.get("Projects", []), f"{name}.Projects"),
                third_party_allow=_strings(party_access.get("Allow", []), f"{name}.ThirdParty.Allow"),
                third_party_deny=_strings(party_access.get("Deny", []), f"{name}.ThirdParty.Deny"),
                restricted_third_party={
                    library: _strings(files, f"{name}.RestrictedThirdParty.{library}")
                    for library, files in _object(
                        entry.get("RestrictedThirdParty", {}), f"{name}.RestrictedThirdParty"
                    ).items()
                },
                guard_condition=guarded.get("Condition", ""),
                guarded_modules=_strings(guarded.get("Modules", []), f"{name}.Guarded.Modules"),
                guarded_third_party=_strings(guarded.get("ThirdParty", []), f"{name}.Guarded.ThirdParty"),
                system_headers=bool(entry.get("SystemHeaders", False)),
            )
        )

    platform = {
        name: _strings(systems, f"PlatformDirectories.{name}")
        for name, systems in _object(document.get("PlatformDirectories", {}), "PlatformDirectories").items()
        if name != "Notes"
    }
    snapshot = _object(document["SnapshotHeaders"], "SnapshotHeaders")
    banned = _object(document["Banned"], "Banned")
    contract = _object(document["Contract"], "Contract")
    python = _object(document["Python"], "Python")
    json_rules = _object(document["Json"], "Json")
    edges: set[tuple[str, str]] = set()
    for edge in document.get("Layer5Edges", []):
        pair = _strings(edge, "Layer5Edges[]")
        if len(pair) != 2:
            raise UsageError(f"Layer5Edges entries are [from, to] pairs, got {list(pair)}")
        edges.add((pair[0], pair[1]))

    return Rules(
        path=path,
        include_roots=_strings(document["IncludeRoots"], "IncludeRoots"),
        cxx_roots=_strings(document["CxxRoots"], "CxxRoots"),
        engine_module_root=document["EngineModuleRoot"].rstrip("/"),
        platform_directories=platform,
        third_party=third_party,
        global_public=_strings(global_third_party.get("Public", []), "GlobalThirdParty.Public"),
        global_private=_strings(global_third_party.get("Private", []), "GlobalThirdParty.Private"),
        modules=modules,
        layer5_edges=frozenset(edges),
        snapshot_files=_strings(snapshot["Files"], "SnapshotHeaders.Files"),
        snapshot_modules=_strings(snapshot["AllowedModules"], "SnapshotHeaders.AllowedModules"),
        snapshot_third_party=_strings(snapshot["AllowedThirdParty"], "SnapshotHeaders.AllowedThirdParty"),
        snapshot_transitive_third_party=_strings(
            snapshot["TransitiveThirdParty"], "SnapshotHeaders.TransitiveThirdParty"
        ),
        projects=projects,
        precompiled_headers={
            name: _strings(owners, f"PrecompiledHeaders.{name}")
            for name, owners in _object(document["PrecompiledHeaders"], "PrecompiledHeaders").items()
        },
        simulation_modules=_strings(banned["SimulationPathModules"], "Banned.SimulationPathModules"),
        simulation_exceptions=GlobSet(
            _strings(banned.get("SimulationPathExceptions", []), "Banned.SimulationPathExceptions")
        ),
        try_catch_files=GlobSet(_strings(banned["TryCatchFiles"], "Banned.TryCatchFiles")),
        json_access_files=GlobSet(_strings(banned["JsonAccessFiles"], "Banned.JsonAccessFiles")),
        abi_macro_files=GlobSet(_strings(banned.get("AbiMacroFiles", []), "Banned.AbiMacroFiles")),
        new_delete_files=GlobSet(_strings(banned.get("NewDeleteFiles", []), "Banned.NewDeleteFiles")),
        output_files=GlobSet(_strings(banned.get("OutputFiles", []), "Banned.OutputFiles")),
        contract_stub_definition_files=GlobSet(
            _strings(contract["StubDefinitionFiles"], "Contract.StubDefinitionFiles")
        ),
        contract_test_files=GlobSet(_strings(contract["TestFiles"], "Contract.TestFiles")),
        contract_python_stub_files=GlobSet(_strings(contract.get("PythonStubFiles", []), "Contract.PythonStubFiles")),
        contract_python_test_files=GlobSet(_strings(contract.get("PythonTestFiles", []), "Contract.PythonTestFiles")),
        source_includes={
            file: _strings(names, f"SourceIncludes.{file}")
            for file, names in _object(document.get("SourceIncludes", {}), "SourceIncludes").items()
            if file != "Notes"
        },
        python_roots=_strings(python["Roots"], "Python.Roots"),
        python_exclude=GlobSet(_strings(python.get("Exclude", []), "Python.Exclude")),
        python_entry_points=GlobSet(_strings(python.get("EntryPoints", []), "Python.EntryPoints")),
        python_libraries=GlobSet(_strings(python.get("Libraries", []), "Python.Libraries")),
        python_third_party=GlobSet(_strings(python.get("ThirdPartyImports", []), "Python.ThirdPartyImports")),
        json_extensions=_strings(json_rules["Extensions"], "Json.Extensions"),
        json_lines_extensions=_strings(json_rules.get("LineDelimitedExtensions", []), "Json.LineDelimitedExtensions"),
        json_exclude=GlobSet(_strings(json_rules.get("Exclude", []), "Json.Exclude")),
    )


def validate_rules(rules: Rules) -> None:
    """The rules must be internally consistent; otherwise a rule could silently never apply."""
    problems: list[str] = []
    project_names = rules.project_names()
    libraries = set(rules.third_party) | {"System"}

    def check_libraries(entries: Iterable[str], where: str) -> None:
        for entry in entries:
            library = entry.split(":", 1)[0]
            if library != "*" and library not in libraries:
                problems.append(f"{where}: unknown third-party library '{library}'")

    check_libraries(rules.global_public + rules.global_private, "GlobalThirdParty")
    for module in rules.modules.values():
        for name in module.includes:
            if name in rules.modules:
                target = rules.modules[name]
                if target.layer > module.layer:
                    problems.append(f"Modules.{module.name}: includes {name}, a higher layer")
                if target.layer == 5 and module.layer == 5:
                    problems.append(
                        f"Modules.{module.name}: layer-5 edges belong in Layer5Edges, not Includes ({name})"
                    )
            elif name not in project_names:
                problems.append(f"Modules.{module.name}: unknown module or project '{name}' in Includes")
        if module.max_layer is not None and module.max_layer >= module.layer:
            problems.append(f"Modules.{module.name}: MaxLayer must be below its own layer")
        check_libraries(
            module.third_party_public + module.third_party_private + tuple(module.restricted_third_party),
            f"Modules.{module.name}.ThirdParty",
        )
        for library in module.restricted_third_party:
            if library not in module.third_party_public + module.third_party_private:
                problems.append(
                    f"Modules.{module.name}.RestrictedThirdParty: {library} is not in the module's ThirdParty lists"
                )
    for source, target in rules.layer5_edges:
        for name in (source, target):
            if name not in rules.modules or rules.modules[name].layer != 5:
                problems.append(f"Layer5Edges: '{name}' is not a layer-5 module")
    # The edges must form a DAG, so a cycle can never pass the check (section 3 rule 1).
    graph: dict[str, list[str]] = {}
    for source, target in rules.layer5_edges:
        graph.setdefault(source, []).append(target)
    state: dict[str, int] = {}

    def visit(node: str, trail: list[str]) -> None:
        state[node] = 1
        for successor in graph.get(node, []):
            if state.get(successor) == 1:
                problems.append(f"Layer5Edges: cycle {' -> '.join(trail + [node, successor])}")
            elif successor not in state:
                visit(successor, trail + [node])
        state[node] = 2

    for node in sorted(graph):
        if node not in state:
            visit(node, [])
    for name in rules.snapshot_modules:
        if name not in rules.modules:
            problems.append(f"SnapshotHeaders.AllowedModules: unknown module '{name}'")
    check_libraries(rules.snapshot_third_party + rules.snapshot_transitive_third_party, "SnapshotHeaders")
    for project in rules.projects:
        for name in project.modules_allow + project.modules_deny + project.guarded_modules:
            if name != "*" and name not in rules.modules:
                problems.append(f"Projects.{project.name}: unknown module '{name}'")
        for name in project.projects:
            if name not in project_names:
                problems.append(f"Projects.{project.name}: unknown project '{name}'")
        check_libraries(
            project.third_party_allow
            + project.third_party_deny
            + project.guarded_third_party
            + tuple(project.restricted_third_party),
            f"Projects.{project.name}",
        )
        if project.flags_from not in project_names:
            problems.append(f"Projects.{project.name}: FlagsFrom names unknown project '{project.flags_from}'")
        for module in project.files_outside_modules.values():
            if module not in rules.modules:
                problems.append(f"Projects.{project.name}: FilesOutsideModules names unknown module '{module}'")
    for name in rules.simulation_modules:
        if name not in rules.modules:
            problems.append(f"Banned.SimulationPathModules: unknown module '{name}'")
    for owners in rules.precompiled_headers.values():
        for owner in owners:
            if owner not in project_names:
                problems.append(f"PrecompiledHeaders: unknown project '{owner}'")
    if problems:
        raise UsageError(f"invalid rules in {rules.path}:\n  " + "\n  ".join(problems))


# --------------------------------------------------------------------------------------------------------------------
# C++ source model: comments and literals blanked, preprocessor directives, includes, scopes and statements
# --------------------------------------------------------------------------------------------------------------------

IDENTIFIER_CHARACTERS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")
RAW_STRING_PREFIXES = ("R", "u8R", "uR", "UR", "LR")
RAW_STRING_DELIMITER = re.compile(r'"([^ ()\\\t\n]{0,16})\(')


def _blank(characters: list[str], start: int, end: int) -> None:
    for index in range(start, end):
        if characters[index] != "\n":
            characters[index] = " "


def _token_before(text: str, index: int) -> str:
    start = index
    while start > 0 and (text[start - 1] in IDENTIFIER_CHARACTERS or text[start - 1] in "'."):
        start -= 1
    return text[start:index]


def strip_cpp(text: str) -> tuple[str, str]:
    """Return (code, code_with_literals): comments blanked in both, string and character literal contents blanked in
    code. Offsets and line breaks are preserved, so positions map back to the original text."""
    code = list(text)
    with_literals = list(text)
    index = 0
    length = len(text)
    while index < length:
        character = text[index]
        if character == "/" and index + 1 < length and text[index + 1] == "/":
            end = text.find("\n", index)
            end = length if end < 0 else end
            _blank(code, index, end)
            _blank(with_literals, index, end)
            index = end
        elif character == "/" and index + 1 < length and text[index + 1] == "*":
            end = text.find("*/", index + 2)
            end = length if end < 0 else end + 2
            _blank(code, index, end)
            _blank(with_literals, index, end)
            index = end
        elif character == '"':
            prefix = _token_before(text, index)
            match = RAW_STRING_DELIMITER.match(text, index) if prefix in RAW_STRING_PREFIXES else None
            if match:
                terminator = ")" + match.group(1) + '"'
                end = text.find(terminator, match.end())
                end = length if end < 0 else end + len(terminator)
                _blank(code, index + 1, end - 1)
                index = end
                continue
            end = index + 1
            while end < length and text[end] not in '"\n':
                end += 2 if text[end] == "\\" else 1
            _blank(code, index + 1, min(end, length))
            index = end + 1
        elif character == "'":
            token = _token_before(text, index)
            if token[:1].isdigit():  # digit separator: 1'000'000
                index += 1
                continue
            end = index + 1
            while end < length and text[end] not in "'\n":
                end += 2 if text[end] == "\\" else 1
            _blank(code, index + 1, min(end, length))
            index = end + 1
        else:
            index += 1
    return "".join(code), "".join(with_literals)


@dataclasses.dataclass(frozen=True)
class Directive:
    line: int
    offset: int
    name: str  # include, define, if, ifdef, ...
    argument: str  # comment-free text after the directive name, continuation lines joined


@dataclasses.dataclass(frozen=True)
class IncludeDirective:
    line: int
    offset: int
    name: str
    angled: bool
    conditions: tuple[tuple[str, str, bool], ...]  # enclosing (directive, expression, in #else branch)


@dataclasses.dataclass(eq=False)
class Scope:
    kind: str  # file, namespace, linkage, class, enum, function, block, init
    name: str
    start: int
    parent: Scope | None
    end: int = -1
    class_key: str = ""

    def in_code(self) -> bool:
        scope: Scope | None = self
        while scope is not None:
            if scope.kind in ("function", "block"):
                return True
            scope = scope.parent
        return False


@dataclasses.dataclass
class Statement:
    text: str
    start: int
    end: int
    scope: Scope
    access: str
    terminator: str  # ";" or "{"
    opened: Scope | None = None


DIRECTIVE_PATTERN = re.compile(r"^[ \t]*#[ \t]*([A-Za-z_]\w*)?(.*)$")
INCLUDE_NAME_PATTERN = re.compile(r'#\s*include\s*([<"])([^>"\n]+)[>"]')


class SourceFile:
    """A C++ file of the linted tree with its blanked code, directives and lazily parsed structure."""

    def __init__(self, root: Path, relative: str) -> None:
        self.relative = relative
        self.path = root / relative
        self.text = read_text(self.path)
        self.code, self.code_with_literals = strip_cpp(self.text)
        self.line_starts = [0] + [match.end() for match in re.finditer("\n", self.text)]
        self.directives, self.structure_code = self._scan_directives()
        self.includes = self._parse_includes()
        self._structure: tuple[list[Scope], list[Statement]] | None = None

    @property
    def is_header(self) -> bool:
        return self.relative.endswith(CXX_HEADER_EXTENSION)

    def line_of(self, offset: int) -> int:
        return bisect.bisect_right(self.line_starts, offset)

    def _scan_directives(self) -> tuple[list[Directive], str]:
        directives: list[Directive] = []
        structure = list(self.code)
        lines = self.code.split("\n")
        offset = 0
        index = 0
        while index < len(lines):
            line = lines[index]
            match = DIRECTIVE_PATTERN.match(line)
            if not match:
                offset += len(line) + 1
                index += 1
                continue
            start_offset = offset
            start_line = index + 1
            text = line
            end_offset = offset + len(line)
            while text.endswith("\\") and index + 1 < len(lines):
                index += 1
                offset += len(lines[index - 1]) + 1
                text = text[:-1] + " " + lines[index]
                end_offset = offset + len(lines[index])
            _blank(structure, start_offset, end_offset)
            full = DIRECTIVE_PATTERN.match(text)
            assert full is not None
            directives.append(Directive(start_line, start_offset, full.group(1) or "", full.group(2).strip()))
            offset = end_offset + 1
            index += 1
        return directives, "".join(structure)

    def _parse_includes(self) -> list[IncludeDirective]:
        includes: list[IncludeDirective] = []
        stack: list[list[Any]] = []  # [directive, expression, in_else]
        for directive in self.directives:
            if directive.name in ("if", "ifdef", "ifndef"):
                stack.append([directive.name, directive.argument, False])
            elif directive.name == "elif" and stack:
                stack[-1] = ["if", directive.argument, False]
            elif directive.name in ("elifdef", "elifndef") and stack:
                stack[-1] = [directive.name[2:], directive.argument, False]
            elif directive.name == "else" and stack:
                stack[-1][2] = True
            elif directive.name == "endif" and stack:
                stack.pop()
            elif directive.name == "include":
                original = self.text[
                    directive.offset : self.line_starts[directive.line]
                    if directive.line < len(self.line_starts)
                    else None
                ]
                match = INCLUDE_NAME_PATTERN.search(original)
                if match:
                    includes.append(
                        IncludeDirective(
                            directive.line,
                            directive.offset,
                            match.group(2).strip(),
                            match.group(1) == "<",
                            tuple((entry[0], entry[1], entry[2]) for entry in stack),
                        )
                    )
        return includes

    def structure(self) -> tuple[list[Scope], list[Statement]]:
        if self._structure is None:
            self._structure = parse_structure(self.structure_code)
        return self._structure

    def scope_at(self, offset: int) -> Scope:
        scopes, _ = self.structure()
        innermost = scopes[0]
        for scope in scopes[1:]:
            if scope.start < offset and (scope.end < 0 or offset < scope.end) and scope.start >= innermost.start:
                innermost = scope
        return innermost


ATTRIBUTE_PATTERN = re.compile(r"\[\[.*?\]\]|\balignas\s*\([^()]*\)|__declspec\s*\([^()]*\)", re.DOTALL)
CLASS_HEAD_PATTERN = re.compile(
    r"^(?P<key>class|struct|union)\b\s*(?P<name>[A-Za-z_][\w:]*)?\s*(?P<arguments><.*>)?"
    r"\s*(?:final\b)?\s*(?::(?!:).*)?$",
    re.DOTALL,
)
ENUM_HEAD_PATTERN = re.compile(r"\benum\b(?:\s+(?:class|struct)\b)?\s*(?P<name>[A-Za-z_]\w*)?")
FUNCTION_TAIL_PATTERN = re.compile(
    r"^\s*(?:(?:const|volatile|&&|&|override|final|mutable|try|noexcept(?:\s*\(.*\))?)\s*)*"
    r"(?:->.*?)?\s*(?:requires\b.*)?$",
    re.DOTALL,
)
CONTROL_HEAD_PATTERN = re.compile(r"^(?:if|for|while|switch|catch|else|do|try)\b")


def strip_template_header(text: str) -> str:
    """Remove leading attributes, `export` and `template<...>` headers (balanced angle brackets)."""
    text = text.strip()
    while True:
        attribute = ATTRIBUTE_PATTERN.match(text)
        if attribute:
            text = text[attribute.end() :].lstrip()
            continue
        if text.startswith("export") and (len(text) == 6 or text[6] not in IDENTIFIER_CHARACTERS):
            text = text[6:].lstrip()
            continue
        match = re.match(r"template\s*<", text)
        if not match:
            return text
        depth = 0
        index = match.end() - 1
        while index < len(text):
            if text[index] in "<(":
                depth += 1
            elif text[index] in ">)":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        text = text[index + 1 :].lstrip()
        # A requires-clause directly after the template header.
        if text.startswith("requires"):
            text = _strip_requires_clause(text)


def _strip_requires_clause(text: str) -> str:
    index = len("requires")
    depth = 0
    while index < len(text):
        character = text[index]
        if character in "(<":
            depth += 1
        elif character in ")>":
            depth -= 1
        elif depth == 0 and character == "\n":
            return text[index:].lstrip()
        index += 1
    return ""


def has_top_level_assignment(text: str) -> bool:
    depth = 0
    for index, character in enumerate(text):
        if character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
        elif character == "=" and depth == 0:
            before = text[index - 1] if index > 0 else ""
            after = text[index + 1] if index + 1 < len(text) else ""
            if before in "=!<>" or after == "=":
                continue
            if re.search(r"\boperator\s*\S{0,2}$", text[:index]):
                continue
            return True
    return False


def last_top_level_close(text: str) -> int:
    """Index of the last top-level ')' that closes a parameter list, or -1."""
    depth = 0
    for index in range(len(text) - 1, -1, -1):
        character = text[index]
        if character == ")":
            if depth == 0:
                tail = text[index + 1 :]
                if FUNCTION_TAIL_PATTERN.match(tail):
                    return index
            depth += 1
        elif character == "(":
            depth -= 1
    return -1


def classify_brace(head: str, parent: Scope, in_parentheses: bool) -> tuple[str, str, str]:
    """Kind, name and class key of the scope that a '{' after `head` opens."""
    if in_parentheses or parent.kind in ("init", "enum"):
        return "init", "", ""
    text = strip_template_header(head)
    in_code = parent.in_code()
    if re.search(r"\bnamespace\b", text) and not text.startswith("using") and not in_code:
        names = re.findall(r"[A-Za-z_]\w*", re.sub(r"\b(?:inline|namespace)\b", " ", text))
        return "namespace", "::".join(names), ""
    if re.fullmatch(r'extern\s*"\s*"', text):
        return "linkage", "", ""
    if re.match(r"(?:typedef\s+)?enum\b", text):
        match = ENUM_HEAD_PATTERN.search(text)
        return "enum", (match.group("name") or "") if match else "", ""
    match = CLASS_HEAD_PATTERN.match(re.sub(r"^typedef\s+", "", ATTRIBUTE_PATTERN.sub(" ", text)).strip())
    if match:
        name = match.group("name") or ""
        if match.group("arguments"):
            name += match.group("arguments")
        return "class", name, match.group("key")
    if has_top_level_assignment(text):
        return "init", "", ""
    if in_code and (not text or CONTROL_HEAD_PATTERN.match(text)):
        return "block", "", ""
    # Constructor with an initializer list: a '{' right after an identifier is a member's brace initializer.
    if re.search(r"\)\s*(?:noexcept\s*(?:\([^()]*\))?\s*)?:(?!:)", text) and not in_code:
        if re.search(r"[\w>]\s*$", text):
            return "init", "", ""
        return "function", "", ""
    if "(" in text and last_top_level_close(text) >= 0:
        return ("block" if in_code else "function"), "", ""
    return ("block" if in_code else "init"), "", ""


def parse_structure(code: str) -> tuple[list[Scope], list[Statement]]:
    """Scopes and statements of preprocessor-free, comment-free, literal-free C++ code (a heuristic parser: enough to
    find namespaces, classes with their access sections, enums, function bodies and declarations)."""
    root = Scope("file", "", -1, None)
    scopes = [root]
    statements: list[Statement] = []
    stack = [root]
    parentheses = [0]
    access = ["public"]
    statement_start = [0]

    def emit(end: int, terminator: str, opened: Scope | None = None) -> None:
        start = statement_start[-1]
        text = code[start:end]
        stripped = text.strip()
        if stripped:
            leading = len(text) - len(text.lstrip())
            statements.append(Statement(stripped, start + leading, end, stack[-1], access[-1], terminator, opened))

    index = 0
    length = len(code)
    while index < length:
        character = code[index]
        if character in "([":
            parentheses[-1] += 1
        elif character in ")]":
            parentheses[-1] = max(0, parentheses[-1] - 1)
        elif character == ";" and parentheses[-1] == 0 and stack[-1].kind != "enum":
            emit(index, ";")
            statement_start[-1] = index + 1
        elif (
            character == ":"
            and stack[-1].kind == "class"
            and parentheses[-1] == 0
            and code[index + 1 : index + 2] != ":"
            and code[index - 1 : index] != ":"
        ):
            label = code[statement_start[-1] : index].strip()
            if label in ("public", "protected", "private"):
                access[-1] = label
                statement_start[-1] = index + 1
        elif character == "{":
            head = code[statement_start[-1] : index]
            kind, name, key = classify_brace(head, stack[-1], parentheses[-1] > 0)
            scope = Scope(kind, name, index, stack[-1], class_key=key)
            scopes.append(scope)
            if kind != "init":
                emit(index, "{", scope)
            stack.append(scope)
            parentheses.append(0)
            access.append("private" if key == "class" else "public")
            statement_start.append(index + 1)
        elif character == "}" and len(stack) > 1:
            scope = stack.pop()
            scope.end = index
            parentheses.pop()
            access.pop()
            statement_start.pop()
            if scope.kind in ("namespace", "linkage", "function", "block"):
                statement_start[-1] = index + 1
        index += 1
    return scopes, statements


# --------------------------------------------------------------------------------------------------------------------
# Tree model: which files exist, which rules apply to them
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class PathRules:
    project: ProjectRule | None
    module: ModuleRule | None
    private: bool  # in a private directory of its module, or a source file


class Tree:
    """The linted tree: its files, the rules that apply to each and the include resolution."""

    def __init__(self, root: Path, rules: Rules, paths: list[str] | None) -> None:
        self.root = root
        self.rules = rules
        self.files = list_tree_files(root)
        self.file_set = set(self.files)
        self.paths = paths
        self.system = host_system()
        self._sources: dict[str, SourceFile] = {}
        self._third_party_cache: dict[str, str | None] = {}
        modules_by_length = sorted(rules.modules, key=len, reverse=True)
        self._module_prefixes = [
            (rules.engine_module_root + "/" + name + "/", rules.modules[name]) for name in modules_by_length
        ]

    def in_scope(self, relative: str) -> bool:
        return self.paths is None or any(is_under(relative, path) for path in self.paths)

    def cxx_files(self) -> list[str]:
        return [
            relative
            for relative in self.files
            if relative.endswith((CXX_HEADER_EXTENSION, CXX_SOURCE_EXTENSION))
            and any(is_under(relative, root) for root in self.rules.cxx_roots)
        ]

    def source(self, relative: str) -> SourceFile:
        if relative not in self._sources:
            self._sources[relative] = SourceFile(self.root, relative)
        return self._sources[relative]

    def path_rules(self, relative: str) -> PathRules:
        project = next((candidate for candidate in self.rules.projects if candidate.paths.matches(relative)), None)
        if project is None:
            return PathRules(None, None, False)
        is_source = not relative.endswith(CXX_HEADER_EXTENSION)
        if project.kind != "Engine":
            return PathRules(project, None, is_source)
        if relative in project.files_outside_modules:
            return PathRules(project, self.rules.modules[project.files_outside_modules[relative]], is_source)
        for prefix, module in self._module_prefixes:
            if relative.startswith(prefix):
                inner = relative[len(prefix) :].split("/")[:-1]
                private = any(part in module.private_directories for part in inner)
                return PathRules(project, module, is_source or private)
        return PathRules(project, None, is_source)

    def builds_on_host(self, relative: str) -> bool:
        """False for files in a platform directory of another system (Platform/Linux/ on Windows, ...)."""
        for part in relative.split("/")[:-1]:
            systems = self.rules.platform_directories.get(part)
            if systems is not None and self.system not in systems:
                return False
        return True

    def resolve_first_party(self, name: str) -> str | None:
        for include_root in self.rules.include_roots:
            candidate = f"{include_root}/{name}"
            if candidate in self.file_set:
                return candidate
        return None

    def resolve_third_party(self, name: str) -> str | None:
        if name not in self._third_party_cache:
            self._third_party_cache[name] = next(
                (
                    library
                    for library, directories in self.rules.third_party.items()
                    if any((directory / name).is_file() for directory in directories)
                ),
                None,
            )
        return self._third_party_cache[name]

    def include_name(self, relative: str) -> str:
        """How first-party code includes this header: its path below the deepest include root."""
        for include_root in sorted(self.rules.include_roots, key=len, reverse=True):
            if relative.startswith(include_root + "/"):
                return relative[len(include_root) + 1 :]
        return relative


# --------------------------------------------------------------------------------------------------------------------
# Step: includes (CheckIncludes)
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class IncludeTarget:
    kind: str  # module, project, pch, third-party, std, system, unresolved, unknown
    name: str
    path: str | None = None
    private: bool = False


def third_party_allowed(entries: Iterable[str], library: str, header: str) -> bool:
    return any(entry in ("*", library) or entry == f"{library}:{header}" for entry in entries)


def is_dist_guarded(conditions: tuple[tuple[str, str, bool], ...], condition: str) -> bool:
    """True when an enclosing #if branch requires `condition` (only '!defined(MACRO)' forms are supported)."""
    match = re.fullmatch(r"!\s*defined\s*\(?\s*(\w+)\s*\)?", condition.strip())
    if not match:
        return False
    macro = match.group(1)
    negated = re.compile(rf"^!\s*defined\s*\(?\s*{macro}\s*\)?$")
    positive = re.compile(rf"^defined\s*\(?\s*{macro}\s*\)?$")
    for directive, expression, in_else in conditions:
        expression = expression.strip()
        if directive == "ifndef" and expression == macro and not in_else:
            return True
        if directive == "ifdef" and expression == macro and in_else:
            return True
        if directive == "if":
            if in_else and positive.match(strip_outer_parentheses(expression)):
                return True
            if not in_else and "||" not in expression:
                if any(negated.match(strip_outer_parentheses(term)) for term in expression.split("&&")):
                    return True
    return False


def strip_outer_parentheses(expression: str) -> str:
    expression = expression.strip()
    while (
        expression.startswith("(")
        and expression.endswith(")")
        and balanced_arguments(expression, 0)[1] == len(expression) - 1
    ):
        expression = expression[1:-1].strip()
    return expression


class IncludeChecker:
    def __init__(self, tree: Tree) -> None:
        self.tree = tree
        self.rules = tree.rules

    def classify(self, source: SourceFile, include: IncludeDirective) -> tuple[IncludeTarget, list[tuple[str, str]]]:
        """The include's target and spelling problems as (code, message) pairs."""
        problems: list[tuple[str, str]] = []
        name = include.name
        if name.startswith(("./", "../")) or "/../" in name:
            problems.append(
                (
                    "include-relative",
                    f"'{name}' is a relative path; include repository headers by their full path from an include root "
                    "(CodeStyle section 4.4)",
                )
            )
        if name.endswith((".cpp", ".c", ".cc", ".cxx")) and name not in self.rules.source_includes.get(
            source.relative, ()
        ):
            problems.append(
                ("include-source", f"'{name}' is a source file; never include a .cpp file (CodeStyle section 4.4)")
            )

        first_party = self.tree.resolve_first_party(name)
        if include.angled:
            library = self.tree.resolve_third_party(name)
            if library is not None:
                return IncludeTarget("third-party", library), problems
            if name in STANDARD_HEADERS:
                return IncludeTarget("std", name), problems
            if first_party is not None:
                problems.append(
                    (
                        "include-spelling",
                        f"repository header <{name}> must be included with quotes (CodeStyle section 4.4)",
                    )
                )
                return self.classify_path(first_party), problems
            return IncludeTarget("system", name), problems

        if first_party is None:
            local = (Path(source.relative).parent / name).as_posix()
            if local in self.tree.file_set:
                problems.append(
                    (
                        "include-spelling",
                        f"'{name}' resolves only next to the including file; use its full path from an include root "
                        "(CodeStyle section 4.4)",
                    )
                )
                return self.classify_path(local), problems
            library = self.tree.resolve_third_party(name)
            if library is not None:
                problems.append(
                    (
                        "include-spelling",
                        f'third-party header "{name}" must be included with angle brackets (CodeStyle section 4.4)',
                    )
                )
                return IncludeTarget("third-party", library), problems
            return IncludeTarget("unresolved", name), problems
        return self.classify_path(first_party), problems

    def classify_path(self, relative: str) -> IncludeTarget:
        directory, _, base = relative.rpartition("/")
        if base in self.rules.precompiled_headers and directory in self.rules.include_roots:
            return IncludeTarget("pch", base, relative)
        path_rules = self.tree.path_rules(relative)
        if path_rules.project is None:
            return IncludeTarget("unknown", relative, relative)
        if path_rules.project.kind == "Engine":
            if path_rules.module is None:
                return IncludeTarget("unknown", relative, relative)
            private = path_rules.private and relative.endswith(CXX_HEADER_EXTENSION)
            return IncludeTarget("module", path_rules.module.name, relative, private)
        return IncludeTarget("project", path_rules.project.name, relative)

    def check(self, source: SourceFile) -> list[Finding]:
        findings: list[Finding] = []
        path_rules = self.tree.path_rules(source.relative)

        def report(line: int, code: str, message: str) -> None:
            findings.append(Finding(source.relative, line, code, message))

        if path_rules.project is None:
            return findings
        if path_rules.project.kind == "Engine" and path_rules.module is None:
            report(
                1,
                "include-unknown-module",
                f"'{source.relative}' is not in a module listed in Scripts/ModuleRules.json; add the module (with its "
                "layer and rules) or move the file",
            )
            return findings

        for include in source.includes:
            target, problems = self.classify(source, include)
            for code, message in problems:
                report(include.line, code, message)
            scope = source.scope_at(include.offset)
            if scope.kind != "file" and scope.kind != "linkage":
                report(
                    include.line,
                    "include-in-namespace",
                    f"'{include.name}' is included inside a {scope.kind}; never #include inside a namespace (CodeStyle "
                    "section 4.4)",
                )
            message = self.violation(source, path_rules, include, target)
            if message is not None:
                report(include.line, message[0], message[1])
        if source.relative in self.rules.snapshot_files:
            findings += self.check_snapshot_header(source)
        return findings

    def violation(
        self, source: SourceFile, path_rules: PathRules, include: IncludeDirective, target: IncludeTarget
    ) -> tuple[str, str] | None:
        project = path_rules.project
        assert project is not None
        spelled = f"<{include.name}>" if include.angled else f"'{include.name}'"
        if target.kind == "unresolved":
            return (
                "include-unresolved",
                f"{spelled} is not found below any include root ({', '.join(self.rules.include_roots)}) nor in a "
                "vendored library",
            )
        if target.kind == "unknown":
            return (
                "include-unknown-module",
                f"{spelled} is not in a module or project listed in Scripts/ModuleRules.json",
            )
        if target.kind == "std":
            return None
        if target.kind == "pch":
            if source.is_header:
                return "include-pch", f"headers never include a precompiled header ({spelled}, CodeStyle section 4.4)"
            owners = self.rules.precompiled_headers.get(target.name, ())
            if project.name not in owners:
                return (
                    "include-pch",
                    f"{spelled} is the precompiled header of {', '.join(owners)}, not of {project.name}",
                )
            return None
        if path_rules.module is not None:
            return self.module_violation(source, path_rules.module, path_rules.private, include, target, spelled)
        return self.project_violation(source, project, include, target, spelled)

    def module_violation(
        self,
        source: SourceFile,
        module: ModuleRule,
        private: bool,
        include: IncludeDirective,
        target: IncludeTarget,
        spelled: str,
    ) -> tuple[str, str] | None:
        rules = self.rules
        if target.kind == "module":
            if target.name == module.name:
                return None
            if target.private:
                return (
                    "include-private",
                    f"{spelled} is a private header of {target.name}; only {target.name} itself may include it",
                )
            other = rules.modules[target.name]
            if target.path in module.headers:
                return None
            if target.name in module.includes or (module.max_layer is not None and other.layer <= module.max_layer):
                return None
            if module.layer == 5 and other.layer == 5:
                if (target.name, module.name) in rules.layer5_edges:
                    return None
                allowed = sorted(source_name for source_name, sink in rules.layer5_edges if sink == module.name)
                return "include-layer5-edge", (
                    f"{module.name} must not include {target.name} ({spelled}): the layer-5 edge {target.name} -> "
                    f"{module.name} "
                    f"is not in Layer5Edges; {module.name} may include "
                    f"{', '.join(allowed) if allowed else 'no other layer-5 module'}"
                )
            allowed_text = self.describe_module_access(module)
            if other.layer > module.layer:
                return (
                    "include-layer",
                    f"upward include: {module.name} (layer {module.layer}) must not include {target.name} (layer "
                    f"{other.layer}) via {spelled}; {module.name} may include {allowed_text}",
                )
            relation = "the same layer" if other.layer == module.layer else f"layer {other.layer}"
            return (
                "include-layer",
                f"{module.name} (layer {module.layer}) must not include {target.name} ({relation}) via {spelled}; "
                f"{module.name} may include {allowed_text}",
            )
        if target.kind == "project":
            if target.name in module.includes:
                return None
            return "include-project", f"Engine module {module.name} must not include {target.name} headers ({spelled})"
        if target.kind == "third-party":
            entries = (
                rules.global_public + module.third_party_public
                if not private
                else (rules.global_private + module.third_party_private + module.third_party_public)
            )
            restricted = module.restricted_third_party.get(target.name)
            if restricted is not None and third_party_allowed(entries, target.name, include.name):
                if restricted.matches(source.relative):
                    return None
                return (
                    "include-third-party",
                    f"{target.name} header {spelled} is allowed in {module.name} only in "
                    f"{', '.join(restricted.patterns)}",
                )
            if third_party_allowed(entries, target.name, include.name):
                return None
            where = "private files" if private else "public headers"
            allowed = sorted(set(entries))
            hint = (
                ""
                if private
                or not third_party_allowed(rules.global_private + module.third_party_private, target.name, include.name)
                else (f"; {target.name} is allowed in its .cpp files and Private/ headers only")
            )
            return (
                "include-third-party",
                f"{target.name} header {spelled} is not allowed in {module.name} {where} (allowed: "
                f"{', '.join(allowed) or 'none'}){hint}",
            )
        if target.kind == "system":
            if module.system_headers == "Private" and private:
                return None
            if isinstance(module.system_headers, tuple):
                inner = source.relative[len(rules.engine_module_root) + len(module.name) + 2 :]
                if inner.split("/", 1)[0] in module.system_headers and "/" in inner:
                    return None
            return "include-system", (
                f"OS or compiler header {spelled} in {module.name}; platform code lives only in Platform/<OS>/ and the "
                f"Graphics device setup (Architecture section 16)"
            )
        return None

    def describe_module_access(self, module: ModuleRule) -> str:
        names = [name for name in module.includes]
        if module.max_layer is not None:
            names.append(f"layers <= {module.max_layer}")
        names += [Path(header).name for header in module.headers]
        if module.layer == 5:
            names += sorted(source for source, sink in self.rules.layer5_edges if sink == module.name)
        return ", ".join(names) if names else "nothing outside its own module"

    def project_violation(
        self, source: SourceFile, project: ProjectRule, include: IncludeDirective, target: IncludeTarget, spelled: str
    ) -> tuple[str, str] | None:
        guarded = is_dist_guarded(include.conditions, project.guard_condition) if project.guard_condition else False
        if target.kind == "module":
            if target.private:
                return (
                    "include-private",
                    f"{spelled} is a private header of {target.name}; only {target.name} itself may include it",
                )
            if target.name in project.guarded_modules:
                if guarded:
                    return None
                return (
                    "include-project",
                    f"{project.name} may include {target.name} only under #if {project.guard_condition} ({spelled})",
                )
            allowed = (
                "*" in project.modules_allow or target.name in project.modules_allow
            ) and target.name not in project.modules_deny
            if allowed:
                return None
            return "include-project", f"{project.name} must not include the {target.name} module ({spelled})"
        if target.kind == "project":
            if target.name in project.projects:
                return None
            return (
                "include-project",
                f"{project.name} must not include {target.name} headers ({spelled}); it may include "
                f"{', '.join(project.projects)}",
            )
        if target.kind == "third-party":
            if target.name in project.restricted_third_party:
                if source.relative in project.restricted_third_party[target.name]:
                    return None
                return (
                    "include-third-party",
                    f"{target.name} ({spelled}) is allowed in {project.name} only in "
                    f"{', '.join(project.restricted_third_party[target.name])}",
                )
            if target.name in project.guarded_third_party:
                if guarded:
                    return None
                return (
                    "include-third-party",
                    f"{project.name} may include {target.name} only under #if {project.guard_condition} ({spelled})",
                )
            entries = self.rules.global_private + project.third_party_allow
            if third_party_allowed(entries, target.name, include.name) and target.name not in project.third_party_deny:
                return None
            return "include-third-party", f"{target.name} header {spelled} is not allowed in {project.name}"
        if target.kind == "system":
            if project.system_headers:
                return None
            return (
                "include-system",
                f"OS or compiler header {spelled} in {project.name}; platform code lives only in Engine/Platform/<OS>/ "
                "(Architecture section 16)",
            )
        return None

    def check_snapshot_header(self, source: SourceFile) -> list[Finding]:
        """Section 3 rule 3: snapshot headers include only Core, Asset, glm and the standard library, also
        transitively."""
        rules = self.rules
        findings: list[Finding] = []
        allowed_paths = set(rules.snapshot_files)

        def first_party_allowed(target: IncludeTarget) -> bool:
            return (target.kind == "module" and target.name in rules.snapshot_modules) or target.path in allowed_paths

        def describe(target: IncludeTarget, include: IncludeDirective) -> str:
            spelled = f"<{include.name}>" if include.angled else f"'{include.name}'"
            kinds = {
                "module": f"module {target.name}",
                "project": f"project {target.name}",
                "third-party": target.name,
                "system": "an OS header",
                "pch": "a precompiled header",
            }
            return f"{spelled} ({kinds.get(target.kind, target.kind)})"

        summary = (
            f"may include only {', '.join(rules.snapshot_modules)}, {', '.join(rules.snapshot_third_party)} and the "
            "standard library (Architecture section 3 rule 3)"
        )
        for include in source.includes:
            target, _ = self.classify(source, include)
            direct_ok = (
                target.kind == "std"
                or (target.kind in ("module", "project", "pch") and first_party_allowed(target))
                or (
                    target.kind == "third-party"
                    and third_party_allowed(rules.snapshot_third_party, target.name, include.name)
                )
            )
            if not direct_ok:
                findings.append(
                    Finding(
                        source.relative,
                        include.line,
                        "include-snapshot-header",
                        f"{Path(source.relative).name} {summary}; it includes {describe(target, include)}",
                    )
                )
            if target.path is None:
                continue
            # Transitive closure through first-party headers.
            visited = {source.relative, target.path}
            queue: list[tuple[str, list[str]]] = [(target.path, [target.path])]
            reported: set[str] = set()
            while queue:
                current, chain = queue.pop(0)
                if current not in self.tree.file_set:
                    continue
                header = self.tree.source(current)
                for nested in header.includes:
                    nested_target, _ = self.classify(header, nested)
                    bad = (
                        nested_target.kind in ("system", "pch", "unknown", "project")
                        or (nested_target.kind == "module" and not first_party_allowed(nested_target))
                        or (
                            nested_target.kind == "third-party"
                            and not third_party_allowed(
                                rules.snapshot_transitive_third_party, nested_target.name, nested.name
                            )
                        )
                    )
                    key = f"{nested_target.kind}:{nested_target.name}"
                    if bad and key not in reported:
                        reported.add(key)
                        via = " -> ".join(self.tree.include_name(step) for step in chain)
                        findings.append(
                            Finding(
                                source.relative,
                                include.line,
                                "include-snapshot-header",
                                f"{Path(source.relative).name} transitively includes {describe(nested_target, nested)} "
                                f"via {via} ({current}:{nested.line}); snapshot headers stay NVRHI-free and include "
                                "nothing beyond Core, Asset and the standard library (Architecture section 3 rule 3)",
                            )
                        )
                    if nested_target.path is not None and nested_target.path not in visited:
                        visited.add(nested_target.path)
                        queue.append((nested_target.path, chain + [nested_target.path]))
        return findings


def run_includes(tree: Tree, report: StepReport) -> None:
    checker = IncludeChecker(tree)
    files = [relative for relative in tree.cxx_files() if tree.in_scope(relative)]
    report.files = len(files)
    for relative in files:
        report.findings += checker.check(tree.source(relative))


# --------------------------------------------------------------------------------------------------------------------
# Step: banned APIs and constructs
# --------------------------------------------------------------------------------------------------------------------

UNQUALIFIED = r"(?<![\w.>:])"  # not a member access and not qualified by another namespace or class
CRT_TRANSCENDENTALS = (
    "sin cos tan asin acos atan atan2 sinh cosh tanh asinh acosh atanh exp exp2 expm1 log log2 log10 log1p pow cbrt "
    "hypot erf erfc tgamma lgamma"
).split()
GLM_ROTATION_FUNCTIONS = (
    r"angleAxis|eulerAngles|slerp|rotate|yaw|pitch|roll|orientate[234]|eulerAngle[XYZ]{1,3}|yawPitchRoll|"
    r"perspective\w*|frustum|axis|angle"
)
# glm::mix and glm::lerp interpolate quaternions with acos and sin (and are the same call for vectors), so the
# simulation path uses DetMath's interpolation instead.
GLM_INTERPOLATION_FUNCTIONS = r"mix|lerp"
# glm quaternion types; constructing one from a single argument converts Euler angles (cos/sin) or a matrix.
GLM_QUATERNION_TYPES = r"(?:quat|dquat|fquat|highp_quat|mediump_quat|lowp_quat|qua\s*<[^<>;{}()]*>)"
# namespace aliases of std and glm, which qualify the calls above just like the namespace itself.
STD_ALIAS_PATTERN = re.compile(r"\bnamespace\s+([A-Za-z_]\w*)\s*=\s*(?:::)?std\s*;")
GLM_ALIAS_PATTERN = re.compile(r"\bnamespace\s+([A-Za-z_]\w*)\s*=\s*(?:::)?glm\s*;")


@dataclasses.dataclass(frozen=True)
class SimulationMathPatterns:
    """The simulation-path patterns of one file, with its namespace aliases of std and glm as qualifiers."""

    crt: re.Pattern[str]
    glm_rotation: re.Pattern[str]
    glm_interpolation: re.Pattern[str]
    glm_quaternion: re.Pattern[str]


def simulation_math_patterns(code: str) -> SimulationMathPatterns:
    std_aliases = [rf"{re.escape(alias)}::" for alias in sorted(set(STD_ALIAS_PATTERN.findall(code)))]
    glm_aliases = [rf"{re.escape(alias)}::" for alias in sorted(set(GLM_ALIAS_PATTERN.findall(code)))]
    crt_qualifiers = "|".join([r"(?:::)?std::", "::", r"(?:::)?glm::"] + std_aliases + glm_aliases)
    glm_qualifiers = "|".join([r"(?:::)?glm::"] + glm_aliases)
    return SimulationMathPatterns(
        crt=re.compile(UNQUALIFIED + rf"((?:{crt_qualifiers})?(?:" + "|".join(CRT_TRANSCENDENTALS) + r")[fl]?)\s*\("),
        glm_rotation=re.compile(UNQUALIFIED + rf"((?:{glm_qualifiers})(?:{GLM_ROTATION_FUNCTIONS}))\s*\("),
        glm_interpolation=re.compile(UNQUALIFIED + rf"((?:{glm_qualifiers})(?:{GLM_INTERPOLATION_FUNCTIONS}))\s*\("),
        glm_quaternion=re.compile(
            UNQUALIFIED + rf"((?:{glm_qualifiers}){GLM_QUATERNION_TYPES})(?:\s+([A-Za-z_]\w*))?\s*([({{])"
        ),
    )


# A parameter declaration ("const glm::vec3& eulerAngles"): a function returning a quaternion, not a construction.
PARAMETER_DECLARATION_PATTERN = re.compile(r"^\s*(?:const\s+)?[A-Za-z_][\w:<>,\s]*?[\s&*]+[A-Za-z_]\w*\s*$")

# Configuration macros that change the ABI or the declarations of a vendored library, or that first-party
# translation units must all agree on (Architecture section 2.2, each Vendor/<Lib>/VENDOR.md): defining or undefining
# one in a source file changes it for that translation unit only (an ODR violation). They are set only in premake.
ABI_MACRO_PATTERN = re.compile(
    r"^(?:JPH_\w+|SPDLOG_\w+|MA_\w+|MINIAUDIO_\w+|VULKAN_HPP_\w+|VK_USE_PLATFORM_\w+|VK_ENABLE_BETA_EXTENSIONS|"
    r"NVRHI_SHARED_LIBRARY_\w+|NOMINMAX|NDEBUG|_DEBUG|_ITERATOR_DEBUG_LEVEL|_HAS_EXCEPTIONS|_GLIBCXX_DEBUG\w*|"
    r"_GLIBCXX_ASSERTIONS|_GLIBCXX_USE_CXX11_ABI|_LIBCPP_ABI_\w+|_LIBCPP_HARDENING_MODE|LUA_USE_LONGJMP|LUA_API|"
    r"LUACODE_API|LUA_VECTOR_\w+|LUAU_\w+|IMGUI_\w+|ImTextureID|ImDrawIdx|USE_IMGUI_API|IMGUIZMO_NAMESPACE|"
    r"_GLFW_\w+|GLFW_DLL|GLM_\w+|JSON_\w+|NLOHMANN_\w+|ENTT_\w+)$"
)
# Per-translation-unit switches of those families that are meant to be set in source: they enable an operator set
# in the file that asks for it and change no layout.
ABI_MACRO_ALLOWED = frozenset({"IMGUI_DEFINE_MATH_OPERATORS"})

FILESYSTEM_FUNCTIONS = (
    "absolute canonical weakly_canonical relative proximate copy copy_file copy_symlink create_directory "
    "create_directories create_hard_link create_symlink create_directory_symlink current_path equivalent exists "
    "file_size hard_link_count last_write_time permissions read_symlink remove remove_all rename resize_file space "
    "status symlink_status temp_directory_path is_block_file is_character_file is_directory is_empty is_fifo is_other "
    "is_regular_file is_socket is_symlink"
).split()
DIRECTORY_ENTRY_MEMBERS = (
    "exists is_block_file is_character_file is_directory is_fifo is_other is_regular_file is_socket is_symlink "
    "file_size hard_link_count last_write_time status symlink_status refresh"
).split()

JSON_TYPE = r"(?:(?:::)?nlohmann::)?(?:json|ordered_json|basic_json\s*<[^;{}()]*>)"
# Type aliases of the json types: `using Json = nlohmann::json;`, `typedef nlohmann::json Json;`.
JSON_TYPE_ALIAS_PATTERNS = (
    re.compile(r"\busing\s+([A-Za-z_]\w*)\s*=\s*(?:typename\s+)?" + JSON_TYPE + r"\s*;"),
    re.compile(r"\btypedef\s+" + JSON_TYPE + r"\s+([A-Za-z_]\w*)\s*;"),
)


def json_type_pattern(aliases: Iterable[str]) -> str:
    """JSON_TYPE plus the type aliases declared for it."""
    names = sorted(set(aliases))
    if not names:
        return JSON_TYPE
    return r"(?:" + JSON_TYPE + "|" + "|".join(re.escape(name) for name in names) + r")"


def json_declaration_pattern(json_type: str) -> re.Pattern[str]:
    return re.compile(
        r"(?<![\w:])" + json_type + r"(?![\w:])\s*(?:const\b\s*)?(?:&&|&|\*)?\s*([A-Za-z_]\w*)\s*(?=[=;,)\[{(]|:(?!:))"
    )


def json_factory_pattern(json_type: str) -> re.Pattern[str]:
    return re.compile(
        r"\bauto\s*(?:&&|&)?\s*([A-Za-z_]\w*)\s*=\s*" + json_type + r"\s*::\s*(?:parse|object|array|from_\w+)\s*\("
    )


JSON_ALIAS_PATTERN = re.compile(r"\bauto\s*(?:&&|&)?\s*([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*)\s*(?:\[|;)")
JSON_RANGE_PATTERN = re.compile(r"\bfor\s*\(\s*(?:const\s+)?auto\s*(?:&&|&)?\s*([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\b")
JSON_ITEMS_PATTERN = re.compile(
    r"\bfor\s*\(\s*(?:const\s+)?auto\s*(?:&&|&)?\s*\[\s*\w+\s*,\s*(\w+)\s*\]\s*:\s*"
    r"(\w+)(?:\s*\[[^\]]*\])*\s*\.\s*items\s*\("
)
JSON_ACCESS_PATTERN = re.compile(
    r"(?<![\w:])([A-Za-z_]\w*)((?:\s*\[[^\[\]]*(?:\[[^\[\]]*\][^\[\]]*)*\])*)\s*(?:\.|->)\s*(?:template\s+)?"
    r"(at\s*\(|get_to\s*\(|get_ref\s*<|get\s*<)"
)
JSON_ACCESSORS = ("at", "get", "get_to", "get_ref")
# clang-query matcher for the same calls, resolved by type: any member call of nlohmann's basic_json named like an
# accessor, whatever the receiver's spelling (an alias, a value returned by a function, a call chain).
JSON_ACCESS_MATCHER = (
    "cxxMemberCallExpr(callee(cxxMethodDecl(hasAnyName("
    + ", ".join(f'"{name}"' for name in JSON_ACCESSORS)
    + '), ofClass(matchesName("^::nlohmann::(.+::)?basic_json$")))))'
)
CLANG_QUERY_MATCH_PATTERN = re.compile(r"^(?P<file>.+?):(?P<line>\d+):(?P<column>\d+): note: \"root\" binds here")


@dataclasses.dataclass(frozen=True)
class BannedPattern:
    code: str
    pattern: re.Pattern[str]
    message: str


BANNED_PATTERNS = (
    BannedPattern(
        "banned-throw",
        re.compile(
            r"\bthrow\b|"
            + UNQUALIFIED
            + r"(?:(?:::)?std::)?(?:rethrow_exception|throw_with_nested|rethrow_if_nested)\b"
        ),
        "first-party code never throws, rethrows or nests exceptions (Architecture section 4.6): return Result<T>/"
        "Status, or assert a programmer error",
    ),
    BannedPattern(
        "banned-rand",
        re.compile(UNQUALIFIED + r"(?:(?:::)?std::|::)?(?:rand|srand|random_shuffle)\s*\("),
        "rand()/srand() are forbidden (CodeStyle section 13): use Engine::Random with an explicit seed (Architecture "
        "section 4.12)",
    ),
    BannedPattern(
        "banned-rtti",
        re.compile(r"\bdynamic_cast\s*<|\btypeid\s*\("),
        "first-party code never uses dynamic_cast or typeid (Architecture section 2.2, Appendix A)",
    ),
    BannedPattern(
        "banned-assert",
        re.compile(UNQUALIFIED + r"assert\s*\("),
        "never use assert() from <cassert>: use ENGINE_CORE_ASSERT / ENGINE_ASSERT (CodeStyle section 11, Architecture "
        "section 4.5)",
    ),
    BannedPattern("banned-goto", re.compile(r"\bgoto\b"), "goto is forbidden (CodeStyle section 13)"),
    BannedPattern(
        "banned-endl", re.compile(r"\bstd::endl\b"), "std::endl is forbidden: write '\\n' (CodeStyle section 13)"
    ),
    BannedPattern(
        "banned-bind", re.compile(r"\bstd::bind\s*\("), "std::bind is forbidden: use a lambda (CodeStyle section 13)"
    ),
    BannedPattern(
        "banned-using-namespace-std",
        re.compile(r"\busing\s+namespace\s+(?:::)?std\s*;"),
        "'using namespace std;' is forbidden everywhere (CodeStyle section 5)",
    ),
    BannedPattern("banned-null", re.compile(r"\bNULL\b"), "use nullptr, never NULL (CodeStyle section 6)"),
    BannedPattern(
        "banned-unsupported-feature",
        re.compile(
            r"\bstd::(?:basic_)?stacktrace\b|\bstd::flat_(?:multi)?(?:map|set)\b|"
            r"\bstd::generator\b|\bco_(?:await|yield|return)\b"
        ),
        "not available on every supported toolchain (CodeStyle section 13): std::stacktrace, std::flat_map/flat_set, "
        "std::generator and coroutines are not used",
    ),
    BannedPattern(
        "banned-unsupported-feature",
        re.compile(r"^\s*(?:export\s+)?(?:import\s+[\w.:<\"]|module\s*[\w;:])", re.MULTILINE),
        "C++20 modules and 'import std;' are not used (CodeStyle section 13)",
    ),
)
OUTPUT_PATTERN = re.compile(
    r"\bstd::(?:w?cout|w?cerr|w?clog|print|println|printf|fprintf|puts|vprintf|vfprintf)\b|"
    + UNQUALIFIED
    + r"(?:::)?(?:printf|fprintf|puts|putchar|vprintf|vfprintf|wprintf)\s*\("
)
NEW_DELETE_PATTERN = re.compile(r"\b(new|delete)\b")
BANNED_HEADERS = {
    "cassert": (
        "banned-assert",
        "never use assert() from <cassert>: use ENGINE_CORE_ASSERT / ENGINE_ASSERT (CodeStyle section 11)",
    ),
    "assert.h": (
        "banned-assert",
        "never use assert() from <assert.h>: use ENGINE_CORE_ASSERT / ENGINE_ASSERT (CodeStyle section 11)",
    ),
    "stacktrace": (
        "banned-unsupported-feature",
        "<stacktrace> is not available on every supported toolchain (CodeStyle section 13)",
    ),
    "flat_map": ("banned-unsupported-feature", "<flat_map> is not in libstdc++ 14 (CodeStyle section 13)"),
    "flat_set": ("banned-unsupported-feature", "<flat_set> is not in libstdc++ 14 (CodeStyle section 13)"),
    "generator": ("banned-unsupported-feature", "<generator> and coroutines are not used (CodeStyle section 13)"),
    "coroutine": ("banned-unsupported-feature", "coroutines are not used (CodeStyle section 13)"),
}


def balanced_arguments(code: str, open_index: int) -> tuple[list[str], int]:
    """Top-level comma-separated arguments of the parenthesis or brace at open_index, and the closing index."""
    closing = {"(": ")", "{": "}"}[code[open_index]]
    depth = 0
    arguments: list[str] = []
    current_start = open_index + 1
    index = open_index
    while index < len(code):
        character = code[index]
        if character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
            if depth == 0:
                if character == closing:
                    piece = code[current_start:index].strip()
                    if piece or arguments:
                        arguments.append(piece)
                    return arguments, index
        elif character == "," and depth == 1:
            arguments.append(code[current_start:index].strip())
            current_start = index + 1
        index += 1
    return arguments, len(code)


def own_header(tree: Tree, source: SourceFile) -> SourceFile | None:
    """The header a .cpp file implements (same stem, included by the file)."""
    if source.is_header:
        return None
    stem = Path(source.relative).stem
    for include in source.includes:
        if include.angled or Path(include.name).stem != stem:
            continue
        resolved = tree.resolve_first_party(include.name)
        if resolved is not None:
            return tree.source(resolved)
    return None


class BannedChecker:
    def __init__(self, tree: Tree) -> None:
        self.tree = tree
        self.rules = tree.rules
        self.product_name = workspace_name()

    def check(self, source: SourceFile, semantic_json: list[tuple[int, str]] | None = None) -> list[Finding]:
        """The banned-API findings of one C++ file. `semantic_json` holds its json access findings from clang-query
        as (line, message); None selects the regex checker."""
        findings: list[Finding] = []
        seen: set[tuple[int, str]] = set()

        def report(offset_or_line: int, code: str, message: str, is_line: bool = False) -> None:
            line = offset_or_line if is_line else source.line_of(offset_or_line)
            if (line, code) not in seen:
                seen.add((line, code))
                findings.append(Finding(source.relative, line, code, message))

        code = self.code_without_includes(source)
        relative = source.relative
        for banned in BANNED_PATTERNS:
            for match in banned.pattern.finditer(code):
                report(match.start(), banned.code, banned.message)
        for include in source.includes:
            if include.angled and include.name in BANNED_HEADERS:
                code_name, message = BANNED_HEADERS[include.name]
                report(include.line, code_name, message, is_line=True)
        if not self.rules.try_catch_files.matches(relative):
            for match in re.finditer(r"\btry\b|\bcatch\s*\(", code):
                report(
                    match.start(),
                    "banned-try-catch",
                    "try/catch appears only in the boundary files listed in Architecture section 4.6 "
                    "(Banned.TryCatchFiles)",
                )
        if not self.rules.output_files.matches(relative):
            for match in OUTPUT_PATTERN.finditer(code):
                report(
                    match.start(),
                    "banned-output",
                    f"'{match.group(0).rstrip('( ')}' bypasses the logger: diagnostics use the "
                    "ENGINE_*_INFO/WARN/ERROR macros (CodeStyle section 11, section 13)",
                )
        if not self.rules.new_delete_files.matches(relative):
            for match in NEW_DELETE_PATTERN.finditer(code):
                before = code[max(0, match.start() - 40) : match.start()]
                if re.search(r"\boperator\s*$", before):
                    continue  # declaring or calling an allocation function by name
                if match.group(1) == "delete" and before.rstrip().endswith("="):
                    continue  # a deleted function
                report(
                    match.start(),
                    "banned-new-delete",
                    (
                        f"naked {match.group(1)}: ownership lives in Scope/Ref (CreateScope/CreateRef), containers or "
                        "a "
                        f"dedicated allocator (CodeStyle section 7)"
                    ),
                )
        for directive in source.directives:
            if directive.name == "if" and re.fullmatch(r"0+", directive.argument.strip()):
                report(
                    directive.line,
                    "banned-if-0",
                    "no '#if 0' blocks: version control keeps old code (CodeStyle section 10)",
                    is_line=True,
                )
            if directive.name in ("define", "undef") and not self.rules.abi_macro_files.matches(relative):
                name_match = re.match(r"\s*([A-Za-z_]\w*)", directive.argument)
                name = name_match.group(1) if name_match else ""
                if ABI_MACRO_PATTERN.match(name) and name not in ABI_MACRO_ALLOWED:
                    report(
                        directive.line,
                        "banned-abi-macro",
                        f"#{directive.name} {name}: this configuration macro changes the ABI or the declarations "
                        "of a library (or of first-party code) for this translation unit only, an ODR violation; it "
                        "is set once, for every project, in premake (Architecture section 2.2, Vendor/<Lib>/VENDOR.md)",
                        is_line=True,
                    )
        if self.is_simulation_path(relative):
            for offset, message in self.simulation_math(code):
                report(offset, "banned-crt-math", message)
        if not self.rules.json_access_files.matches(relative):
            if semantic_json is None:
                for offset, message in self.json_access(source, code):
                    report(offset, "banned-json-access", message)
            else:
                for line, message in semantic_json:
                    report(line, "banned-json-access", message, is_line=True)
        header = own_header(self.tree, source)
        for offset, message in self.filesystem_calls(code, header.code if header is not None else ""):
            report(offset, "banned-filesystem", message)
        if source.is_header:
            _, statements = source.structure()
            for statement in statements:
                if (
                    statement.terminator == ";"
                    and re.match(r"using\s+namespace\b", statement.text)
                    and not statement.scope.in_code()
                ):
                    report(
                        statement.start,
                        "banned-using-namespace-header",
                        "no 'using namespace' at namespace or global scope in a header (CodeStyle section 5)",
                    )
        if self.product_name:
            pattern = re.compile(re.escape(self.product_name), re.IGNORECASE)
            for match in pattern.finditer(source.code_with_literals):
                report(
                    match.start(),
                    "banned-product-name",
                    f"the product name '{self.product_name}' never appears in code; display names come from "
                    "configuration (CodeStyle section 2)",
                )
        return findings

    @staticmethod
    def code_without_includes(source: SourceFile) -> str:
        code = list(source.code)
        for directive in source.directives:
            if directive.name == "include":
                end = source.code.find("\n", directive.offset)
                _blank(code, directive.offset, len(code) if end < 0 else end)
        return "".join(code)

    def is_simulation_path(self, relative: str) -> bool:
        path_rules = self.tree.path_rules(relative)
        if path_rules.project is None or path_rules.project.kind != "Engine" or path_rules.module is None:
            return False
        if relative in path_rules.project.files_outside_modules:
            return False
        return path_rules.module.name in self.rules.simulation_modules and not self.rules.simulation_exceptions.matches(
            relative
        )

    def simulation_math(self, code: str) -> list[tuple[int, str]]:
        """CRT transcendental functions and the glm functions built on them (section 4.12), including calls qualified
        by a namespace alias of std or glm."""
        patterns = simulation_math_patterns(code)
        where = f"on the simulation path ({', '.join(self.rules.simulation_modules)})"
        results: list[tuple[int, str]] = []
        for pattern in (patterns.crt, patterns.glm_rotation):
            for match in pattern.finditer(code):
                results.append(
                    (
                        match.start(),
                        f"'{match.group(1)}' uses C runtime transcendental functions {where}; use Engine::DetMath "
                        "(Architecture section 4.12)",
                    )
                )
        for match in patterns.glm_interpolation.finditer(code):
            results.append(
                (
                    match.start(),
                    f"'{match.group(1)}' interpolates quaternions with acos and sin {where}; use Engine::DetMath's "
                    "interpolation (Architecture section 4.12)",
                )
            )
        for match in patterns.glm_quaternion.finditer(code):
            arguments, _ = balanced_arguments(code, match.end(3) - 1)
            declares_function = match.group(2) is not None and all(
                PARAMETER_DECLARATION_PATTERN.match(argument) for argument in arguments
            )
            if len(arguments) == 1 and not declares_function:
                results.append(
                    (
                        match.start(),
                        f"'{match.group(1)}' constructed from one argument converts Euler angles with cos and sin "
                        f"{where}; use Engine::DetMath (a rotation matrix converts with glm::quat_cast) "
                        "(Architecture section 4.12)",
                    )
                )
        return results

    def json_access(self, source: SourceFile, code: str) -> list[tuple[int, str]]:
        """nlohmann json at()/get<>() calls (section 4.6), regex fallback. Receivers are recognised by their declared
        json type (or a type alias of it) in this file or in the header it implements, through auto copies,
        subscripts and range-for loops. Values returned by functions and call chains need the clang-query checker."""
        texts = [code]
        header = own_header(self.tree, source)
        if header is not None:
            texts.append(header.code)
        aliases = {alias for text in texts for pattern in JSON_TYPE_ALIAS_PATTERNS for alias in pattern.findall(text)}
        json_type = json_type_pattern(aliases)
        declaration_pattern = json_declaration_pattern(json_type)
        factory_pattern = json_factory_pattern(json_type)
        names: set[str] = set()
        for text in texts:
            names.update(declaration_pattern.findall(text))
            names.update(factory_pattern.findall(text))
        changed = True
        while changed:
            changed = False
            for pattern in (JSON_ALIAS_PATTERN, JSON_RANGE_PATTERN, JSON_ITEMS_PATTERN):
                for alias, origin in pattern.findall(code):
                    if origin in names and alias not in names:
                        names.add(alias)
                        changed = True
        results: list[tuple[int, str]] = []
        for match in JSON_ACCESS_PATTERN.finditer(code):
            if match.group(1) in names:
                accessor = re.sub(r"\s+", "", match.group(3))
                call = accessor[:-1] + ("<>()" if accessor.endswith("<") else "()")
                results.append(
                    (
                        match.start(3),
                        (
                            f"json {call} on '{match.group(1)}' outside Core/Json: read through JsonReader, which "
                            "reports "
                            f"located errors instead of throwing (Architecture section 4.6)"
                        ),
                    )
                )
        for match in re.finditer(json_type + r"\s*::\s*(at|get|get_to|get_ref)\b", code):
            results.append(
                (
                    match.start(),
                    f"json::{match.group(1)} outside Core/Json: read through JsonReader (Architecture section 4.6)",
                )
            )
        return results

    def filesystem_calls(self, code: str, header_code: str) -> list[tuple[int, str]]:
        """std::filesystem functions called without their std::error_code overload (section 4.6). Error codes are
        recognised by their declarations in the file or in the header it implements (members)."""
        aliases = set(re.findall(r"\bnamespace\s+(\w+)\s*=\s*(?:::)?std::filesystem\s*;", code))
        qualifiers = [r"(?:::)?std::filesystem::"] + [rf"(?<![\w:]){re.escape(alias)}::" for alias in sorted(aliases)]
        if re.search(r"\busing\s+namespace\s+(?:::)?std::filesystem\s*;", code):
            qualifiers.append(r"(?<![\w:.>])")
        qualifier = "(?:" + "|".join(qualifiers) + ")"
        declarations = code + "\n" + header_code
        error_codes = set(re.findall(r"\b(?:std::)?error_code\b\s*(?:const\s*)?[&*]?\s*([A-Za-z_]\w*)", declarations))
        statuses = set(re.findall(r"\bfile_status\b\s*(?:const\s*)?[&*]?\s*([A-Za-z_]\w*)", declarations))
        accepted = error_codes | statuses
        results: list[tuple[int, str]] = []

        def has_error_code(arguments: list[str]) -> bool:
            return any(re.sub(r"[\s()&*]", "", argument) in accepted for argument in arguments)

        for match in re.finditer(qualifier + r"(" + "|".join(FILESYSTEM_FUNCTIONS) + r")\s*\(", code):
            arguments, _ = balanced_arguments(code, match.end() - 1)
            if not has_error_code(arguments):
                results.append(
                    (
                        match.start(1),
                        f"std::filesystem::{match.group(1)} without an std::error_code: use the error_code overload "
                        "(Architecture section 4.6)",
                    )
                )

        iterators: set[str] = set()
        for match in re.finditer(
            qualifier + r"((?:recursive_)?directory_iterator)\b\s*(?:([A-Za-z_]\w*)\s*)?([({])", code
        ):
            if match.group(2):
                iterators.add(match.group(2))
            arguments, _ = balanced_arguments(code, match.end() - 1)
            if arguments and not has_error_code(arguments):
                results.append(
                    (
                        match.start(1),
                        f"std::filesystem::{match.group(1)} constructed without an std::error_code: use the error_code "
                        "overload (Architecture section 4.6)",
                    )
                )
        for match in re.finditer(r"\bfor\s*\([^;:()]*:\s*" + qualifier + r"(?:recursive_)?directory_iterator\b", code):
            results.append(
                (
                    match.start(),
                    "range-for over a directory iterator uses its throwing operator++: advance with "
                    "increment(errorCode) (Architecture section 4.6)",
                )
            )
        for name in sorted(iterators):
            for match in re.finditer(rf"\+\+\s*{re.escape(name)}\b|(?<![\w.]){re.escape(name)}\s*\+\+", code):
                results.append(
                    (
                        match.start(),
                        f"operator++ on directory iterator '{name}' throws: use {name}.increment(errorCode) "
                        "(Architecture section 4.6)",
                    )
                )

        entries = set(re.findall(qualifier + r"directory_entry\b\s*(?:const\s*)?&?\s*([A-Za-z_]\w*)", code))
        for name in sorted(iterators):
            entries.update(re.findall(rf"\bauto\s*&?\s*([A-Za-z_]\w*)\s*=\s*\*\s*{re.escape(name)}\b", code))
        for name in sorted(entries):
            pattern = rf"(?<![\w.]){re.escape(name)}\s*(?:\.|->)\s*({'|'.join(DIRECTORY_ENTRY_MEMBERS)})\s*\(\s*\)"
            for match in re.finditer(pattern, code):
                results.append(
                    (
                        match.start(1),
                        f"directory_entry::{match.group(1)}() throws: pass an std::error_code (Architecture section "
                        "4.6)",
                    )
                )
        return results


def workspace_name() -> str:
    """The product name: the premake workspace name, defined once in premake5.lua (Scripts/Lib/paths.py)."""
    try:
        return repository_paths.workspace_name()
    except repository_paths.WorkspaceScriptError:
        return ""


def python_code_lines(text: str) -> list[tuple[int, str]]:
    """(line, text) of every Python line, with comments removed (strings and docstrings are code)."""
    lines = text.split("\n")
    try:
        for token in tokenize.generate_tokens(io.StringIO(text).readline):
            if token.type == tokenize.COMMENT:
                row, column = token.start
                lines[row - 1] = lines[row - 1][:column]
    except (tokenize.TokenError, SyntaxError, IndentationError):
        pass  # the python step reports invalid syntax; scan the text as it is
    return list(enumerate(lines, 1))


def lua_code_lines(text: str) -> list[tuple[int, str]]:
    """(line, text) of every Lua line, with "--" comments (line and long-bracket comments) removed."""
    result: list[tuple[int, str]] = []
    in_block_comment = False
    for number, line in enumerate(text.split("\n"), 1):
        code: list[str] = []
        index = 0
        quote = ""
        while index < len(line):
            if in_block_comment:
                end = line.find("]]", index)
                if end < 0:
                    index = len(line)
                    break
                in_block_comment = False
                index = end + 2
                continue
            character = line[index]
            if quote:
                code.append(character)
                if character == "\\" and index + 1 < len(line):
                    code.append(line[index + 1])
                    index += 2
                    continue
                if character == quote:
                    quote = ""
            elif character in "\"'":
                quote = character
                code.append(character)
            elif line.startswith("--[[", index):
                in_block_comment = True
                index += 4
                continue
            elif line.startswith("--", index):
                break
            else:
                code.append(character)
            index += 1
        result.append((number, "".join(code)))
    return result


def product_name_findings(tree: Tree, product_name: str) -> tuple[int, list[Finding]]:
    """The product name in Slang, Python and Lua code (comments excluded). premake5.lua's WorkspaceName line is its
    single definition. Vendored code and the deliberately defective lint fixtures are not scanned."""
    pattern = re.compile(re.escape(product_name), re.IGNORECASE)
    python = set(python_files(tree))
    scanned = 0
    findings: list[Finding] = []
    for relative in tree.files:
        if not tree.in_scope(relative) or is_under(relative, "Vendor") or is_under(relative, "Tests/Data/Lint"):
            continue
        if relative.endswith(".slang"):
            lines = list(enumerate(strip_cpp(read_text(tree.root / relative))[1].split("\n"), 1))
            kind = "shader code"
        elif relative in python:
            lines = python_code_lines(read_text(tree.root / relative))
            kind = "Python code"
        elif relative.endswith(".lua"):
            lines = lua_code_lines(read_text(tree.root / relative))
            if relative == "premake5.lua":
                lines = [(number, line) for number, line in lines if not re.match(r"\s*WorkspaceName\s*=", line)]
            kind = "premake (Lua) code"
        else:
            continue
        scanned += 1
        for number, line in lines:
            if pattern.search(line):
                findings.append(
                    Finding(
                        relative,
                        number,
                        "banned-product-name",
                        f"the product name '{product_name}' never appears in {kind}: premake5.lua defines it once "
                        "(WorkspaceName); everything else derives it or says Engine (CodeStyle section 2)",
                    )
                )
    return scanned, findings


def semantic_json_access(
    context: LintContext, files: list[str]
) -> tuple[dict[str, list[tuple[int, str]]] | None, list[Finding], str]:
    """json at()/get<>() calls found by type with clang-query: {file: [(line, message)]} for every file it checked
    (C++ sources directly, headers through a one-line translation unit), or None when the regex checker applies;
    findings for translation units clang-query could not parse; and a description of the mode for the report."""
    tree = context.tree
    options = context.options
    if options.mode == "regex":
        return None, [], "json access: regex checker (--mode regex)"
    clang_query = find_llvm_tool("clang-query", options.clang_query, "CLANG_QUERY", MINIMUM_CLANG_TIDY_MAJOR)
    if clang_query is None:
        if options.mode == "clang":
            raise ToolMissing("--mode clang: clang-query not found (it ships with clang-tidy in the LLVM tools)")
        return None, [], "json access: regex checker (clang-query not found)"

    candidates = [
        relative
        for relative in files
        if tree.builds_on_host(relative) and not tree.rules.json_access_files.matches(relative)
    ]
    if not candidates:
        return {}, [], f"json access: clang-query ({clang_query}); no files to check"
    database = context.database()
    results: dict[str, list[tuple[int, str]]] = {}
    failures: list[Finding] = []
    temporary = Path(tempfile.mkdtemp(prefix="Lint-JsonAccess-"))
    try:
        jobs: list[tuple[str, Path, list[str]]] = []
        for index, relative in enumerate(candidates):
            flags = database.flags_for(relative)
            if flags is None:
                continue  # no project flags (reported by the naming and headers steps); the regex checker applies
            if relative.endswith(CXX_HEADER_EXTENSION):
                unit = header_translation_unit(temporary, index, tree.include_name(relative))
            else:
                unit = tree.root / relative
            jobs.append((relative, unit, flags))
            results[relative] = []

        def run_query(job: tuple[str, Path, list[str]]) -> tuple[str, subprocess.CompletedProcess[str]]:
            relative, unit, flags = job
            command = [
                str(clang_query),
                "-c",
                "set output diag",
                "-c",
                f"match {JSON_ACCESS_MATCHER}",
                str(unit),
                "--",
            ] + flags[1:]
            return relative, subprocess.run(
                command,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                cwd=temporary,
                timeout=TOOL_TIMEOUT_SECONDS,
                check=False,
            )

        message = (
            "nlohmann json accessor (at, get, get_to or get_ref) called outside Core/Json: read through JsonReader, "
            "which reports located errors instead of throwing (Architecture section 4.6)"
        )
        for relative, result in run_parallel(run_query, jobs, options.jobs):
            output = (result.stdout + "\n" + result.stderr).splitlines()
            for line in output:
                match = CLANG_QUERY_MATCH_PATTERN.match(line.strip())
                if match is None:
                    continue
                where = relative_to_tree(tree, match.group("file"))
                if where is not None and where in results and (int(match.group("line")), message) not in results[where]:
                    results[where].append((int(match.group("line")), message))
            if result.returncode != 0:
                errors = [m for m in map(DIAGNOSTIC_PATTERN.match, (line.strip() for line in output)) if m]
                first = next((m for m in errors if m.group("severity") != "warning"), None)
                detail = (
                    f"{first.group('file')}:{first.group('line')}: {first.group('message')}"
                    if first
                    else (result.stderr or result.stdout).strip()[-400:]
                )
                failures.append(
                    Finding(relative, 1, "banned-compile-error", f"clang-query failed (exit code {result.returncode}): "
                                                                 f"{detail}")
                )
    finally:
        shutil.rmtree(temporary, ignore_errors=True)
    return results, failures, f"json access: clang-query ({clang_query}); flags from {context.database_origin}"


def run_banned(context: LintContext, report: StepReport) -> None:
    tree = context.tree
    checker = BannedChecker(tree)
    files = [relative for relative in tree.cxx_files() if tree.in_scope(relative)]
    report.files = len(files)
    semantic, failures, mode = semantic_json_access(context, files)
    for relative in files:
        report.findings += checker.check(tree.source(relative), None if semantic is None else semantic.get(relative))
    report.findings += failures
    report.detail = mode
    # Shaders, Python and premake scripts are code too: the product name never appears in them.
    if checker.product_name:
        scanned, findings = product_name_findings(tree, checker.product_name)
        report.files += scanned
        report.findings += findings


# --------------------------------------------------------------------------------------------------------------------
# Step: contract (contract stubs and skipped test cases left at the end of a milestone)
# --------------------------------------------------------------------------------------------------------------------

# The contract-stub marker of Engine/Source/Engine/Core/Base.h: the first statement of every stub body a milestone's
# contract task writes (Roadmap rule 3).
CONTRACT_STUB_PATTERN = re.compile(r"\bENGINE_CONTRACT_STUB\b")
CONTRACT_STUB_DEFINITION_PATTERN = re.compile(r"[ \t]*#[ \t]*define[ \t]+(ENGINE_CONTRACT_STUB)\b")
CONTRACT_STUB_MESSAGE = (
    "ENGINE_CONTRACT_STUB marks a stub written by a milestone contract task (Roadmap rule 3): implement the function, "
    "replacing the whole stub body, before the milestone ends. Outside contract mode (Lint.py --allow-contract-stubs, "
    "PreCommit.py --contract) the marker appears only in its definition (ModuleRules.json Contract.StubDefinitionFiles)"
)
# doctest's macros that take a decorator expression ("name" * doctest::skip() * ...), with or without DOCTEST_.
DOCTEST_DECORATED_MACRO_PATTERN = re.compile(
    r"(?<![\w])(?:DOCTEST_)?(?:TEST_CASE(?:_FIXTURE|_CLASS|_TEMPLATE(?:_DEFINE)?)?|SUBCASE|TEST_SUITE(?:_BEGIN)?|"
    r"SCENARIO(?:_CLASS|_TEMPLATE(?:_DEFINE)?)?)\s*\("
)
DOCTEST_ALIAS_PATTERN = re.compile(r"\bnamespace\s+([A-Za-z_]\w*)\s*=\s*(?:::)?doctest\s*;")
DOCTEST_USING_DIRECTIVE_PATTERN = re.compile(r"\busing\s+namespace\s+(?:::)?doctest\s*;")
# The suite of the test cases that only run in a child process spawned by another test (Test::ChildTargetSuite,
# Tests/Source/Support/TestOptions.h): the only test cases that stay skipped
# (Docs/Decisions/0003-m1-contract-decisions.md decision 7).
CHILD_TARGET_SUITE = r"(?:(?:::)?Engine\s*::\s*)?Test\s*::\s*ChildTargetSuite"
TEST_SKIP_MESSAGE = (
    "leaves a test case unrun: the only permanent skips are the child-process targets, whose decorator expression "
    "also carries doctest::test_suite(Test::ChildTargetSuite) (Docs/Decisions/0003-m1-contract-decisions.md decision "
    "7). A contract task's skipped tests are un-skipped when their implementation lands (Roadmap rule 3); outside "
    "contract mode (Lint.py --allow-contract-stubs, PreCommit.py --contract) none may remain"
)


@dataclasses.dataclass(frozen=True)
class DoctestPatterns:
    """doctest::skip and the child-target suite decorator of one file, qualified by doctest or a namespace alias of it,
    or unqualified after `using namespace doctest;`."""

    skip: re.Pattern[str]
    child_target: re.Pattern[str]


def doctest_patterns(code: str) -> DoctestPatterns:
    names = [r"(?:::)?doctest"] + [re.escape(alias) for alias in sorted(set(DOCTEST_ALIAS_PATTERN.findall(code)))]
    qualified = r"(?<![\w:.>])(?:" + "|".join(names) + r")\s*::\s*"
    skip = qualified + r"skip\b"
    child_target = qualified + rf"test_suite\s*\(\s*{CHILD_TARGET_SUITE}\s*\)"
    if DOCTEST_USING_DIRECTIVE_PATTERN.search(code):
        skip += r"|(?<![\w:.>])skip\s*[({]"
        child_target += rf"|(?<![\w:.>])test_suite\s*\(\s*{CHILD_TARGET_SUITE}\s*\)"
    return DoctestPatterns(re.compile(skip), re.compile(child_target))


class ContractChecker:
    """What a milestone's contract task leaves for its implementation streams (Roadmap rule 3): stub bodies marked
    with ENGINE_CONTRACT_STUB and test cases marked doctest::skip. At the end of a milestone none may remain, except the
    skipped child-process targets."""

    def __init__(self, tree: Tree) -> None:
        self.rules = tree.rules

    def check(self, source: SourceFile) -> tuple[list[Finding], int]:
        """The findings of one C++ file, and how many of its skips are allowed child-target skips."""
        findings = self.contract_stubs(source)
        if not self.rules.contract_test_files.matches(source.relative):
            return findings, 0
        skip_findings, child_targets = self.test_skips(source)
        return findings + skip_findings, child_targets

    def contract_stubs(self, source: SourceFile) -> list[Finding]:
        """Every use of the marker in code (comments and string literals do not count), except the name in its
        `#define` in a Contract.StubDefinitionFiles file."""
        definitions: set[int] = set()
        if self.rules.contract_stub_definition_files.matches(source.relative):
            for directive in source.directives:
                match = (
                    CONTRACT_STUB_DEFINITION_PATTERN.match(source.code, directive.offset)
                    if directive.name == "define"
                    else None
                )
                if match:
                    definitions.add(match.start(1))
        lines = sorted(
            {
                source.line_of(match.start())
                for match in CONTRACT_STUB_PATTERN.finditer(source.code)
                if match.start() not in definitions
            }
        )
        return [Finding(source.relative, line, "contract-stub", CONTRACT_STUB_MESSAGE) for line in lines]

    @staticmethod
    def test_skips(source: SourceFile) -> tuple[list[Finding], int]:
        """Each doctest::skip must lie in the decorator expression of a doctest test macro (up to the macro's closing
        parenthesis, across lines) that also carries doctest::test_suite(Test::ChildTargetSuite)."""
        code = source.code
        patterns = doctest_patterns(code)
        decorators: list[tuple[int, int]] = []
        for match in DOCTEST_DECORATED_MACRO_PATTERN.finditer(code):
            _, close = balanced_arguments(code, match.end() - 1)
            decorators.append((match.end() - 1, close))
        findings: dict[int, Finding] = {}
        child_targets = 0
        for match in patterns.skip.finditer(code):
            enclosing = [span for span in decorators if span[0] < match.start() < span[1]]
            if enclosing and patterns.child_target.search(code, *max(enclosing)):
                child_targets += 1
                continue
            line = source.line_of(match.start())
            spelling = re.sub(r"\s+", "", match.group(0)).rstrip("({")
            where = (
                "" if enclosing else " outside the decorator expression of a TEST_CASE, TEST_CASE_FIXTURE or SUBCASE"
            )
            findings.setdefault(
                line, Finding(source.relative, line, "test-skip", f"'{spelling}'{where} {TEST_SKIP_MESSAGE}")
            )
        return list(findings.values()), child_targets


# The Python contract markers (Docs/Decisions/0008-m4-decisions.md decision 16): what a contract task leaves in Python
# code.
PYTHON_CONTRACT_STUB_PREFIX = "contract stub"
PYTHON_CONTRACT_STUB_MESSAGE = (
    'raise NotImplementedError("contract stub ...") marks a Python stub written by a milestone contract task '
    "(Roadmap rule 3): implement the function before the milestone ends. Outside contract mode (Lint.py "
    "--allow-contract-stubs, PreCommit.py --contract) none may remain"
)
PYTHON_TEST_SKIP_MESSAGE = (
    "leaves a Python test unrun: automation and MCP tests never skip (a missing editor build or virtual environment "
    "is a failure, not a skip). A contract task's skipped tests are un-skipped when their implementation lands "
    "(Roadmap rule 3); outside contract mode (Lint.py --allow-contract-stubs, PreCommit.py --contract) none may remain"
)
# unittest's and pytest's skip markers, by their final attribute or name.
PYTHON_SKIP_DECORATORS = frozenset({"skip", "skipIf", "skipUnless", "expectedFailure", "skipif", "xfail"})
PYTHON_SKIP_EXCEPTIONS = frozenset({"SkipTest"})
# pytest's imperative skips, by the end of their dotted name.
PYTHON_SKIP_CALLS = ("pytest.skip", "pytest.importorskip")


def dotted_name(node: ast.AST) -> str:
    """"unittest.skip" for the expression unittest.skip, "skip" for a bare name; "" for anything else."""
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        base = dotted_name(node.value)
        return f"{base}.{node.attr}" if base else ""
    return ""


def literal_prefix(node: ast.AST) -> str | None:
    """The text a string expression starts with: a str constant's value, or the leading literal parts of an f-string
    (f"contract stub: {name}" starts with "contract stub: "); None for anything else."""
    if isinstance(node, ast.Constant):
        return node.value if isinstance(node.value, str) else None
    if isinstance(node, ast.JoinedStr):
        prefix = ""
        for part in node.values:
            if not (isinstance(part, ast.Constant) and isinstance(part.value, str)):
                break
            prefix += part.value
        return prefix
    return None


def python_contract_findings(tree: Tree, relative: str) -> list[Finding]:
    """The Python contract markers of one file: contract-stub in Contract.PythonStubFiles, test-skip in
    Contract.PythonTestFiles. Comments and strings that merely spell a marker do not count (the AST is checked)."""
    rules = tree.rules
    is_stub_file = rules.contract_python_stub_files.matches(relative)
    is_test_file = rules.contract_python_test_files.matches(relative)
    if not (is_stub_file or is_test_file):
        return []
    try:
        module = ast.parse(read_text(tree.root / relative), filename=relative)
    except SyntaxError:
        return []  # the python step reports it
    findings: list[Finding] = []
    for node in ast.walk(module):
        if is_stub_file and isinstance(node, ast.Raise) and isinstance(node.exc, ast.Call):
            call = node.exc
            message = literal_prefix(call.args[0]) if call.args else None
            if (
                dotted_name(call.func) == "NotImplementedError"
                and message is not None
                and message.startswith(PYTHON_CONTRACT_STUB_PREFIX)
            ):
                findings.append(Finding(relative, node.lineno, "contract-stub", PYTHON_CONTRACT_STUB_MESSAGE))
        if not is_test_file:
            continue
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            for decorator in node.decorator_list:
                target = decorator.func if isinstance(decorator, ast.Call) else decorator
                name = dotted_name(target)
                if name.rsplit(".", 1)[-1] in PYTHON_SKIP_DECORATORS and (
                    "." not in name or name.startswith(("unittest.", "pytest.mark."))
                ):
                    findings.append(
                        Finding(relative, decorator.lineno, "test-skip", f"'{name}' {PYTHON_TEST_SKIP_MESSAGE}")
                    )
        elif isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == "skipTest":
            findings.append(Finding(relative, node.lineno, "test-skip", f"'skipTest' {PYTHON_TEST_SKIP_MESSAGE}"))
        elif isinstance(node, ast.Call) and dotted_name(node.func).endswith(PYTHON_SKIP_CALLS):
            name = dotted_name(node.func)
            findings.append(Finding(relative, node.lineno, "test-skip", f"'{name}' {PYTHON_TEST_SKIP_MESSAGE}"))
        elif isinstance(node, ast.Raise) and node.exc is not None:
            raised = node.exc.func if isinstance(node.exc, ast.Call) else node.exc
            if dotted_name(raised).rsplit(".", 1)[-1] in PYTHON_SKIP_EXCEPTIONS:
                findings.append(Finding(relative, node.lineno, "test-skip", f"'SkipTest' {PYTHON_TEST_SKIP_MESSAGE}"))
    return sorted(findings, key=lambda finding: finding.line)


def run_contract(context: LintContext, report: StepReport) -> None:
    tree = context.tree
    checker = ContractChecker(tree)
    files = [relative for relative in tree.cxx_files() if tree.in_scope(relative)]
    python = python_files(tree)
    report.files = len(files) + len(python)
    findings: list[Finding] = []
    child_targets = 0
    for relative in files:
        file_findings, file_child_targets = checker.check(tree.source(relative))
        findings += file_findings
        child_targets += file_child_targets
    for relative in python:
        findings += python_contract_findings(tree, relative)
    stubs = sum(1 for finding in findings if finding.code == "contract-stub")
    if context.options.allow_contract_stubs:
        report.detail = (
            f"contract mode (--allow-contract-stubs): {stubs} contract stub(s) and {len(findings) - stubs} test "
            f"skip(s) allowed; {child_targets} child-target skip(s)"
        )
    else:
        report.findings += findings
        report.detail = f"{child_targets} child-target skip(s) (Test::ChildTargetSuite)"


# --------------------------------------------------------------------------------------------------------------------
# Compilation database (compile_commands.json from premake5 compile-commands)
# --------------------------------------------------------------------------------------------------------------------


def find_premake(override: str | None) -> Path:
    """The pinned premake (Scripts/Lib/premake.py), or --premake when it reports the pinned version."""
    try:
        return premake.require_pinned(Path(override) if override else None)
    except (ToolNotFoundError, premake.PremakeError) as error:
        raise ToolMissing(str(error)) from None


class CompileDatabase:
    """Compile flags per first-party file: exact entries for files in the database, and for other files (headers,
    fixture trees) the flags of their project with the first-party include roots moved to the linted tree."""

    def __init__(self, entries: list[dict[str, Any]], tree: Tree) -> None:
        self.tree = tree
        self.by_file: dict[str, list[str]] = {}
        self.by_project: dict[str, list[str]] = {}
        for entry in sorted(entries, key=lambda item: item.get("file", "")):
            file = Path(os.path.normpath(os.path.join(entry.get("directory", ""), entry["file"])))
            arguments = entry.get("arguments") or split_command(entry.get("command", ""))
            flags = self.strip_input(arguments, entry["file"])
            relative = repository_relative(file)
            if relative is None:
                continue
            self.by_file[self.key(relative)] = flags
            project = next((candidate for candidate in tree.rules.projects if candidate.paths.matches(relative)), None)
            if project is not None and project.name not in self.by_project:
                self.by_project[project.name] = flags

    @staticmethod
    def key(relative: str) -> str:
        return relative.lower() if os.name == "nt" else relative

    @staticmethod
    def strip_input(arguments: list[str], file: str) -> list[str]:
        """Drop the input file, -c and -o <object>; keep the compiler and every flag."""
        flags: list[str] = []
        skip = False
        normalized = os.path.normcase(os.path.normpath(file))
        for argument in arguments:
            if skip:
                skip = False
                continue
            if argument == "-o":
                skip = True
                continue
            if argument == "-c" or argument.startswith("-o") and len(argument) > 2 and not argument.startswith("-O"):
                continue
            if os.path.normcase(os.path.normpath(argument)) == normalized:
                continue
            flags.append(argument)
        return flags

    def flags_for(self, relative: str) -> list[str] | None:
        if self.tree.root == REPOSITORY_ROOT and self.key(relative) in self.by_file:
            return list(self.by_file[self.key(relative)])
        project = next((candidate for candidate in self.tree.rules.projects if candidate.paths.matches(relative)), None)
        if project is None or project.flags_from not in self.by_project:
            return None
        return self.relocate(self.by_project[project.flags_from])

    def relocate(self, flags: list[str]) -> list[str]:
        """Point first-party include directories at the linted tree (a no-op for the repository itself). Paths keep
        their case: clang-tidy's header filter compares them case-sensitively."""
        if self.tree.root == REPOSITORY_ROOT:
            return list(flags)

        def move(directory: str) -> str:
            relative = repository_relative(Path(os.path.normpath(directory)))
            if relative is not None and not is_under(relative, "Vendor"):
                return (self.tree.root / relative).as_posix()
            return directory

        result: list[str] = []
        pending = False
        for argument in flags:
            if pending:
                result.append(move(argument))
                pending = False
            elif argument in ("-I", "-isystem", "-iquote", "-idirafter"):
                result.append(argument)
                pending = True
            elif argument.startswith("-I") and len(argument) > 2:
                result.append("-I" + move(argument[2:]))
            else:
                result.append(argument)
        return result


def repository_relative(path: Path) -> str | None:
    """POSIX path relative to the repository root, keeping the path's own case, or None when outside it."""
    try:
        return path.relative_to(REPOSITORY_ROOT).as_posix()
    except ValueError:
        return None


def split_command(command: str) -> list[str]:
    return shlex.split(command, posix=os.name != "nt")


# Database entries per source (an existing file, or a premake binary): the self-test lints many trees in one process.
_DATABASE_ENTRIES: dict[tuple[str, str], tuple[list[dict[str, Any]], str]] = {}


def load_compile_database(
    tree: Tree, existing: str | None, premake_override: str | None
) -> tuple[CompileDatabase, str]:
    """Generate (or read) the compilation database. Returns it with a description of its origin."""
    if existing:
        cache_key = ("file", str(Path(existing).resolve()))
        if cache_key not in _DATABASE_ENTRIES:
            try:
                _DATABASE_ENTRIES[cache_key] = (json.loads(Path(existing).read_text(encoding="utf-8")), existing)
            except (OSError, json.JSONDecodeError) as error:
                raise UsageError(f"cannot read {existing}: {error}") from error
    else:
        premake_executable = find_premake(premake_override)
        cache_key = ("premake", str(premake_executable))
        if cache_key not in _DATABASE_ENTRIES:
            entries = generate_compile_commands(premake_executable)
            _DATABASE_ENTRIES[cache_key] = (entries, "premake5 compile-commands")
    entries, origin = _DATABASE_ENTRIES[cache_key]
    return CompileDatabase(entries, tree), origin


def generate_compile_commands(premake_executable: Path) -> list[dict[str, Any]]:
    """Run the compile-commands action (Scripts/Premake/CompileCommands.lua, which premake5.lua includes) into a
    temporary directory, with --fatal like Scripts/Generate.py."""
    temporary = Path(tempfile.mkdtemp(prefix="Lint-CompileCommands-"))
    try:
        arguments = [f"--file={REPOSITORY_ROOT / 'premake5.lua'}", "--fatal", f"--to={temporary}", "compile-commands"]
        result = premake.run(arguments, timeout=PREMAKE_TIMEOUT_SECONDS, premake=premake_executable)
        if result.timed_out:
            raise subprocess.TimeoutExpired(["premake5", *arguments], PREMAKE_TIMEOUT_SECONDS)
        output = temporary / "compile_commands.json"
        if not result.succeeded or not output.is_file():
            raise ToolFailure(f"'premake5 compile-commands' failed ({result.describe_exit()}):\n{result.tail(20)}")
        try:
            entries = json.loads(output.read_text(encoding="utf-8"))
        except json.JSONDecodeError as error:
            raise ToolFailure(f"premake5 compile-commands wrote invalid JSON: {error}") from error
        if not isinstance(entries, list):
            raise ToolFailure("premake5 compile-commands did not write a JSON array")
        return entries
    finally:
        shutil.rmtree(temporary, ignore_errors=True)


# --------------------------------------------------------------------------------------------------------------------
# Tools (clang-tidy, clang++)
# --------------------------------------------------------------------------------------------------------------------


_LLVM_DIRECTORIES: list[Path] | None = None


def visual_studio_llvm_directories() -> list[Path]:
    """Visual Studio's bundled LLVM directories (Scripts/Lib/toolchain.py), looked up once per run."""
    global _LLVM_DIRECTORIES
    if _LLVM_DIRECTORIES is None:
        _LLVM_DIRECTORIES = toolchain.visual_studio_llvm_directories()
    return _LLVM_DIRECTORIES


def find_llvm_tool(name: str, override: str | None, environment: str | None,
                   minimum_major: int | None = None) -> Path | None:
    """An LLVM tool: the override, the environment variable, Visual Studio's bundled LLVM, then PATH. On PATH the
    unversioned name is preferred when it is at least `minimum_major`, else the newest name-NN that is: Ubuntu 24.04's
    default clang++ is Clang 18 even when clang++-19 is installed, and Clang 18 cannot parse libstdc++'s <expected>."""
    for candidate in (override, os.environ.get(environment) if environment else None):
        if candidate:
            path = Path(candidate)
            if not path.is_file():
                raise ToolMissing(f"{name} not found at {path}")
            return path
    executable = name + (".exe" if os.name == "nt" else "")
    for directory in visual_studio_llvm_directories():
        if (directory / executable).is_file():
            return directory / executable
    unversioned = shutil.which(name)
    if unversioned and (minimum_major is None or tool_major_version(Path(unversioned))[0] >= minimum_major):
        return Path(unversioned)
    for major in LLVM_MAJOR_VERSIONS:
        if minimum_major is not None and major < minimum_major:
            break
        found = shutil.which(f"{name}-{major}")
        if found:
            return Path(found)
    # Last resort: the unversioned tool even when older, so the caller can name its version (Apple's clang++ reports
    # Apple's own version numbers, which are not LLVM majors).
    return Path(unversioned) if unversioned else None


def tool_major_version(tool: Path) -> tuple[int, str]:
    result = subprocess.run(
        [str(tool), "--version"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=TOOL_TIMEOUT_SECONDS,
        check=False,
    )
    match = re.search(r"version\s+(\d+)\.(\d+)\.(\d+)", result.stdout + result.stderr)
    if not match:
        return 0, "unknown"
    return int(match.group(1)), ".".join(match.groups())


def find_compiler(flags: list[str], override: str | None) -> Path:
    """The compiler named by the database (clang++ by default), found like the other LLVM tools."""
    if override:
        path = Path(override)
        if not path.is_file():
            found = shutil.which(override)
            if not found:
                raise ToolMissing(f"compiler not found: {override}")
            path = Path(found)
        return path
    name = Path(flags[0]).name if flags else "clang++"
    name = re.sub(r"\.exe$", "", name)
    if name.startswith("clang"):
        found = find_llvm_tool(name, None, None, toolchain.MINIMUM_CLANG)
        if found is not None:
            return found
    found_path = shutil.which(name)
    if found_path:
        return Path(found_path)
    if os.name != "nt":
        for fallback in ("clang++", "g++"):
            found_path = shutil.which(fallback)
            if found_path:
                return Path(found_path)
    raise ToolMissing(f"no C++ compiler found for '{name}' (install the LLVM tools, or pass --cxx)")


def run_parallel(function: Callable[[Any], Any], items: list[Any], jobs: int) -> list[Any]:
    if jobs <= 1 or len(items) <= 1:
        return [function(item) for item in items]
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        return list(executor.map(function, items))


DIAGNOSTIC_PATTERN = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<column>\d+): (?P<severity>fatal error|error|warning): (?P<message>.*?)(?: "
    r"\[(?P<check>[\w.,-]+)\])?$"
)


def relative_to_tree(tree: Tree, file: str) -> str | None:
    try:
        path = Path(file).resolve()
        relative = path.relative_to(tree.root)
    except (ValueError, OSError):
        return None
    return relative.as_posix()


def header_translation_unit(directory: Path, index: int, include: str) -> Path:
    unit = directory / f"Header{index:04d}.cpp"
    unit.write_text(f'#include "{include}"\n#include "{include}"\n', encoding="utf-8")
    return unit


# --------------------------------------------------------------------------------------------------------------------
# Step: naming (clang-tidy, or the regex fallback)
# --------------------------------------------------------------------------------------------------------------------

CASE_PATTERNS = {
    "CamelCase": re.compile(r"^[A-Z][a-zA-Z0-9]*$"),
    "camelBack": re.compile(r"^[a-z][a-zA-Z0-9]*$"),
    "UPPER_CASE": re.compile(r"^[A-Z][A-Z0-9_]*$"),
    "lower_case": re.compile(r"^[a-z][a-z0-9_]*$"),
    "aNy_CasE": re.compile(r"^.*$"),
}
FILE_NAME_PATTERN = re.compile(r"^[A-Z][A-Za-z0-9]*\.(?:h|cpp)$")
DIRECTORY_NAME_PATTERN = re.compile(r"^[A-Z][A-Za-z0-9]*$")


def load_naming_options(path: Path) -> dict[str, str]:
    """readability-identifier-naming options from .clang-tidy (the CheckOptions mapping), shared by both modes."""
    options: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise UsageError(f"cannot read {path}: {error}") from error
    for line in lines:
        match = re.match(r"^\s+readability-identifier-naming\.(\w+):\s*(.*?)\s*$", line)
        if match:
            value = match.group(2)
            if len(value) >= 2 and value[0] == value[-1] == "'":
                value = value[1:-1].replace("''", "'")
            elif len(value) >= 2 and value[0] == value[-1] == '"':
                value = value[1:-1]
            options[match.group(1)] = value
    return options


class NamingRule:
    def __init__(self, options: dict[str, str], kind: str, fallback: str | None = None) -> None:
        self.kind = kind
        key = kind if f"{kind}Case" in options or fallback is None else fallback
        self.prefix = options.get(f"{key}Prefix", "")
        self.case = options.get(f"{key}Case", "aNy_CasE")
        ignored = options.get(f"{key}IgnoredRegexp", "")
        self.ignored = re.compile(ignored) if ignored else None

    def problem(self, name: str) -> str | None:
        if self.ignored is not None and self.ignored.search(name):
            return None
        if self.prefix and not name.startswith(self.prefix):
            return f"expected prefix '{self.prefix}' + {self.case}"
        if not CASE_PATTERNS.get(self.case, CASE_PATTERNS["aNy_CasE"]).match(name[len(self.prefix) :]):
            return f"expected {(repr(self.prefix) + ' + ') if self.prefix else ''}{self.case}"
        return None


DISPLAY_KINDS = {
    "Namespace": "namespace",
    "Class": "class",
    "Struct": "struct",
    "Union": "union",
    "Enum": "enum",
    "EnumConstant": "enum constant",
    "Concept": "concept",
    "TypeAlias": "type alias",
    "Typedef": "typedef",
    "Function": "function",
    "Method": "method",
    "ConstexprVariable": "constexpr variable",
    "GlobalConstant": "global constant",
    "ClassConstant": "class constant",
    "StaticConstant": "static constant",
    "PublicMember": "public member",
    "ProtectedMember": "protected member",
    "PrivateMember": "private member",
    "ClassMember": "class member",
    "StaticVariable": "static variable",
    "GlobalVariable": "global variable",
    "MacroDefinition": "macro definition",
}
# Keywords followed by a parenthesized operand that can precede a declarator; the name follows them.
FUNCTION_SPECIFIER_KEYWORDS = frozenset(
    {"requires", "decltype", "alignas", "noexcept", "sizeof", "alignof", "__declspec", "__attribute__", "explicit"}
)


class RegexNamingChecker:
    """The naming fallback: the .clang-tidy prefixes and cases applied to declarations at namespace and class scope,
    static locals and macros, found by the heuristic structure parser."""

    def __init__(self, options: dict[str, str]) -> None:
        self.rules = {kind: NamingRule(options, kind) for kind in DISPLAY_KINDS}
        self.rules["EnumConstant"] = NamingRule(options, "ScopedEnumConstant", "EnumConstant")

    def check(self, source: SourceFile) -> list[Finding]:
        findings: list[Finding] = []
        seen: set[tuple[int, str]] = set()

        def verify(kind: str, name: str, offset: int) -> None:
            if not name or name.upper() == name and kind in ("Function", "Method") and len(name) > 1:
                return  # unnamed, or a macro invocation such as TEST_CASE(...)
            problem = self.rules[kind].problem(name)
            line = source.line_of(offset)
            if problem is not None and (line, name) not in seen:
                seen.add((line, name))
                findings.append(
                    Finding(
                        source.relative,
                        line,
                        "naming",
                        f"invalid case style for {DISPLAY_KINDS[kind]} '{name}' ({problem})",
                    )
                )

        scopes, statements = source.structure()
        for statement in statements:
            self.check_statement(source, statement, verify)
        for scope in scopes:
            if scope.kind == "enum" and scope.end > scope.start:
                body = source.structure_code[scope.start + 1 : scope.end]
                for piece_match in re.finditer(r"[^,]+", body):
                    name_match = re.match(r"\s*([A-Za-z_]\w*)", piece_match.group(0))
                    if name_match:
                        verify(
                            "EnumConstant",
                            name_match.group(1),
                            scope.start + 1 + piece_match.start() + name_match.start(1),
                        )
        for directive in source.directives:
            if directive.name == "define":
                match = re.match(r"([A-Za-z_]\w*)", directive.argument)
                if match:
                    offset = source.text.find(match.group(1), directive.offset)
                    verify("MacroDefinition", match.group(1), offset)
        return findings

    def check_statement(
        self, source: SourceFile, statement: Statement, verify: Callable[[str, str, int], None]
    ) -> None:
        scope = statement.scope
        text = strip_template_header(statement.text)
        offset = statement.start + statement.text.find(text[:20]) if text else statement.start
        if statement.terminator == "{" and statement.opened is not None:
            opened = statement.opened
            if opened.kind == "namespace":
                for name in opened.name.split("::"):
                    if name:
                        verify("Namespace", name, statement.start + statement.text.find(name))
            elif opened.kind == "class" and opened.name and "::" not in opened.name and "<" not in opened.name:
                if not statement.text.lstrip().startswith("template<>") and not re.match(
                    r"template\s*<\s*>", statement.text.lstrip()
                ):
                    kind = {"class": "Class", "struct": "Struct", "union": "Union"}[opened.class_key]
                    verify(kind, opened.name, statement.start + statement.text.find(opened.name))
            elif opened.kind == "enum" and opened.name:
                verify("Enum", opened.name, statement.start + statement.text.find(opened.name))
            elif opened.kind == "function" and scope.kind in ("file", "namespace", "linkage", "class"):
                self.check_function(text, offset, scope, verify)
            return
        if scope.kind == "enum":
            return
        if scope.in_code():
            if re.match(r"(?:static|thread_local)\b", text):
                self.check_variables(text, offset, scope, statement.access, verify)
            return
        if scope.kind not in ("file", "namespace", "linkage", "class"):
            return
        text = re.sub(r'^extern\s*"\s*"\s*', "", text)
        if not text or re.match(
            r"(?:friend|static_assert|namespace|extern\s+template|template\b|public|protected|private|return)\b", text
        ):
            return
        alias = re.match(r"using\s+([A-Za-z_]\w*)\s*=", text)
        if alias:
            verify("TypeAlias", alias.group(1), offset + text.find(alias.group(1)))
            return
        if text.startswith("using"):
            return
        concept = re.match(r"concept\s+([A-Za-z_]\w*)", text)
        if concept:
            verify("Concept", concept.group(1), offset + text.find(concept.group(1), 7))
            return
        if text.startswith("typedef"):
            pointer = re.search(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)", text)
            name_match = pointer or re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*$", text)
            if name_match:
                verify("Typedef", name_match.group(1), offset + text.rfind(name_match.group(1)))
            return
        if re.match(r"(?:enum|class|struct|union)\b[^{(=]*$", text):
            return  # forward or opaque declaration; the definition is checked
        if re.match(r"(?:enum|class|struct|union)\b", text) and "{" in text:
            # The end of a class or enum definition (its head was checked at '{'): only declarators after the closing
            # brace, as in `struct { ... } s_Instance;`, declare variables.
            tail_start = text.rfind("}") + 1
            tail = text[tail_start:]
            if tail.strip():
                leading = len(tail) - len(tail.lstrip())
                self.check_variables(tail.strip(), offset + tail_start + leading, scope, statement.access, verify)
            return
        if re.match(r"[A-Z][A-Z0-9_]*\s*\(", text):
            return  # macro invocation
        pointer = re.search(r"\(\s*[*&]+\s*(?:[A-Za-z_]\w*::)*\s*([A-Za-z_]\w*)\s*\)\s*\(", text)
        if pointer is not None:
            # A pointer to function: the declarator in parentheses names a variable.
            specifiers = set(re.findall(r"\b(static|constexpr|thread_local|extern|inline)\b", text[: pointer.start()]))
            kind = variable_kind(scope, statement.access, specifiers, is_const_object(text[: pointer.start(1)]))
            if kind is not None:
                verify(kind, pointer.group(1), offset + pointer.start(1))
            return
        name, position = function_name(text)
        if name is not None and not re.match(r"[smg]_", name) and not has_top_level_assignment(text[:position]):
            self.check_function(text, offset, scope, verify)
            return
        special = re.search(r"(?:~\s*[A-Za-z_]\w*|\boperator\b[^(]*)\s*\(", text)
        if name is None and special is not None and not has_top_level_assignment(text[: special.start()]):
            return  # a destructor or operator declares no checked name (`~Application() override;` is no variable)
        self.check_variables(text, offset, scope, statement.access, verify)

    def check_function(self, text: str, offset: int, scope: Scope, verify: Callable[[str, str, int], None]) -> None:
        name, position = function_name(text)
        if name is None:
            return
        if scope.kind == "class" and declares_override(text, position):
            return  # an override keeps the name of the function it overrides, as clang-tidy assumes too
        qualified = text[:position].rstrip().endswith("::")
        kind = "Method" if scope.kind == "class" or qualified else "Function"
        verify(kind, name, offset + position)

    def check_variables(
        self, text: str, offset: int, scope: Scope, access: str, verify: Callable[[str, str, int], None]
    ) -> None:
        declarators = split_declarators(text)
        if not declarators:
            return
        first_type = declarators[0][2]
        specifiers = set(re.findall(r"\b(static|constexpr|constinit|thread_local|extern|inline|mutable)\b", first_type))
        for name, position, type_text in declarators:
            is_const = is_const_object(type_text if type_text else first_type)
            kind = variable_kind(scope, access, specifiers, is_const)
            if kind is not None:
                verify(kind, name, offset + position)


def declares_override(text: str, name_position: int) -> bool:
    """True when the member function whose name starts at name_position is declared override or final: the
    virt-specifier follows its parameter list. Only an in-class declaration can carry it, so an override of a
    third-party interface with a name outside the naming rules (spdlog's sink_it_, doctest's test_case_start) is defined
    in its class."""
    open_index = text.find("(", name_position)
    if open_index < 0:
        return False
    _, closing = balanced_arguments(text, open_index)
    return re.search(r"\b(?:override|final)\b", text[closing + 1 :]) is not None


def function_name(text: str) -> tuple[str | None, int]:
    """Name and position of the function a declaration declares, or (None, -1) for operators and destructors."""
    depth_angle = 0
    index = 0
    while index < len(text):
        character = text[index]
        if character == "<" and index > 0 and (text[index - 1] in IDENTIFIER_CHARACTERS or text[index - 1] in " >:"):
            if re.search(r"\boperator\s*<*$", text[:index]):
                return None, -1
            depth_angle += 1
        elif character == ">" and depth_angle > 0:
            depth_angle -= 1
        elif character == "=" and depth_angle == 0:
            return None, -1
        elif character == "(" and depth_angle == 0:
            prefix = text[:index]
            if re.search(r"\boperator\b", prefix):
                return None, -1
            match = re.search(r"(~?)([A-Za-z_]\w*)\s*$", prefix)
            if match is None:
                return None, -1
            if match.group(2) in FUNCTION_SPECIFIER_KEYWORDS:
                _, closing = balanced_arguments(text, index)
                index = closing + 1
                continue
            if match.group(1) or match.group(2) in CPP_KEYWORDS:
                return None, -1
            return match.group(2), match.start(2)
        index += 1
    return None, -1


def split_declarators(text: str) -> list[tuple[str, int, str]]:
    """(name, position, type text) of each declarator of a variable declaration."""
    results: list[tuple[str, int, str]] = []
    depth = 0
    angle = 0
    in_initializer = False
    piece_start = 0
    pieces: list[tuple[int, int]] = []
    for index, character in enumerate(text + ","):
        if character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
        elif not in_initializer and character == "<":
            angle += 1
        elif not in_initializer and character == ">" and angle > 0:
            angle -= 1
        elif character == "=" and depth == 0 and angle == 0:
            in_initializer = True
        elif character == "," and depth == 0 and angle == 0:
            pieces.append((piece_start, index))
            piece_start = index + 1
            in_initializer = False
    for start, end in pieces:
        piece = text[start:end]
        cut = len(piece)
        local_depth = 0
        local_angle = 0
        for index, character in enumerate(piece):
            if character == "<":
                local_angle += 1
            elif character == ">" and local_angle > 0:
                local_angle -= 1
            elif (
                local_angle == 0
                and local_depth == 0
                and (
                    character in "={["
                    or (character == ":" and piece[index - 1 : index] != ":" and piece[index + 1 : index + 2] != ":")
                )
            ):
                cut = index
                break
        declarator = piece[:cut].rstrip()
        match = re.search(r"([A-Za-z_]\w*)$", declarator)
        if match and match.group(1) not in CPP_KEYWORDS:
            results.append((match.group(1), start + match.start(1), declarator[: match.start(1)]))
    return results


def is_const_object(type_text: str) -> bool:
    """True when the declared object itself is const (const T x, T const x, T* const x; not const T* x)."""
    if not re.search(r"\bconst\b", type_text):
        return False
    last_const = [match.end() for match in re.finditer(r"\bconst\b", type_text)][-1]
    return "*" not in type_text[last_const:] and ("*" not in type_text or type_text.rfind("*") < last_const)


def variable_kind(scope: Scope, access: str, specifiers: set[str], is_const: bool) -> str | None:
    if "constexpr" in specifiers:
        return "ConstexprVariable"
    if scope.in_code():
        if "static" in specifiers or "thread_local" in specifiers:
            return "StaticConstant" if is_const else "StaticVariable"
        return None
    if scope.kind == "class":
        if "static" in specifiers:
            return "ClassConstant" if is_const else "ClassMember"
        return {"private": "PrivateMember", "protected": "ProtectedMember"}.get(access, "PublicMember")
    return "GlobalConstant" if is_const else "GlobalVariable"


def check_file_names(tree: Tree, files: list[str]) -> list[Finding]:
    """C++ files and directories under the source roots are PascalCase; .h/.cpp only (CodeStyle sections 2, 4.1)."""
    findings: list[Finding] = []
    reported_directories: set[str] = set()
    for relative in files:
        root = next(root for root in tree.rules.cxx_roots if is_under(relative, root))
        parts = relative[len(root) + 1 :].split("/")
        for depth, part in enumerate(parts[:-1]):
            directory = root + "/" + "/".join(parts[: depth + 1])
            if not DIRECTORY_NAME_PATTERN.match(part) and directory not in reported_directories:
                reported_directories.add(directory)
                findings.append(
                    Finding(
                        relative, 1, "naming-file", f"directory '{directory}' is not PascalCase (CodeStyle section 2)"
                    )
                )
        if not FILE_NAME_PATTERN.match(parts[-1]):
            findings.append(
                Finding(
                    relative,
                    1,
                    "naming-file",
                    f"file name '{parts[-1]}' is not PascalCase .h/.cpp (CodeStyle section 2, section 4.1)",
                )
            )
    for relative in tree.files:
        if (
            relative.endswith(FORBIDDEN_CXX_EXTENSIONS)
            and any(is_under(relative, root) for root in tree.rules.cxx_roots)
            and tree.in_scope(relative)
        ):
            findings.append(
                Finding(
                    relative,
                    1,
                    "naming-file",
                    f"'{Path(relative).suffix}' is not used: headers are .h and sources .cpp (CodeStyle section 4.1)",
                )
            )
    return findings


@dataclasses.dataclass
class LintOptions:
    root: Path
    rules_path: Path
    steps: list[str]
    paths: list[str] | None
    jobs: int
    mode: str  # auto, clang or regex (MODES)
    clang_tidy: str | None
    clang_query: str | None
    cxx: str | None
    premake: str | None
    compile_commands: str | None
    allow_contract_stubs: bool = False  # contract mode: the contract step reports what it finds as allowed


class LintContext:
    """State shared by the steps of one run: the tree and, generated on first use, the compilation database."""

    def __init__(self, options: LintOptions, rules: Rules) -> None:
        self.options = options
        self.tree = Tree(options.root, rules, options.paths)
        self._database: CompileDatabase | None = None
        self.database_origin = ""

    def database(self) -> CompileDatabase:
        if self._database is None:
            self._database, self.database_origin = load_compile_database(
                self.tree, self.options.compile_commands, self.options.premake
            )
        return self._database

    def header_filter(self) -> str:
        root = re.escape(self.tree.root.as_posix()).replace("/", r"[\\/]")
        roots = "|".join(re.escape(root_path).replace("/", r"[\\/]") for root_path in self.tree.rules.cxx_roots)
        return f"^{root}[\\\\/]({roots})[\\\\/].*"


def run_naming(context: LintContext, report: StepReport) -> None:
    tree = context.tree
    options = context.options
    files = [relative for relative in tree.cxx_files() if tree.in_scope(relative)]
    report.files = len(files)
    report.findings += check_file_names(tree, files)
    naming_options = load_naming_options(CLANG_TIDY_CONFIG_PATH)

    clang_tidy: Path | None = None
    reason = ""
    if options.mode != "regex":
        clang_tidy = find_llvm_tool("clang-tidy", options.clang_tidy, "CLANG_TIDY", MINIMUM_CLANG_TIDY_MAJOR)
        if clang_tidy is None:
            reason = "clang-tidy not found"
        else:
            major, version = tool_major_version(clang_tidy)
            if major < MINIMUM_CLANG_TIDY_MAJOR:
                reason = f"clang-tidy {version} at {clang_tidy} is older than {MINIMUM_CLANG_TIDY_MAJOR}"
                clang_tidy = None
            else:
                reason = f"clang-tidy {version} ({clang_tidy})"
        if clang_tidy is None and options.mode == "clang":
            raise ToolMissing(f"--mode clang: {reason}")

    regex_checker = RegexNamingChecker(naming_options)
    if clang_tidy is None:
        report.detail = f"mode: regex fallback ({reason or '--mode regex'})"
        for relative in files:
            report.findings += regex_checker.check(tree.source(relative))
        return

    database = context.database()
    on_host = [relative for relative in files if tree.builds_on_host(relative)]
    other_systems = [relative for relative in files if relative not in on_host]
    temporary = Path(tempfile.mkdtemp(prefix="Lint-Naming-"))
    try:
        jobs: list[tuple[str, Path, list[str]]] = []
        for index, relative in enumerate(on_host):
            flags = database.flags_for(relative)
            if flags is None:
                report.findings.append(
                    Finding(
                        relative,
                        1,
                        "naming-compile-error",
                        "no compile flags for this file (is its project in compile_commands.json?)",
                    )
                )
                continue
            if relative.endswith(CXX_HEADER_EXTENSION):
                unit = header_translation_unit(temporary, index, tree.include_name(relative))
            else:
                unit = tree.root / relative
            jobs.append((relative, unit, flags))

        def run_tidy(job: tuple[str, Path, list[str]]) -> tuple[str, subprocess.CompletedProcess[str]]:
            relative, unit, flags = job
            command = [
                str(clang_tidy),
                "--quiet",
                f"--config-file={CLANG_TIDY_CONFIG_PATH}",
                f"--header-filter={context.header_filter()}",
                str(unit),
                "--",
            ] + flags[1:]
            return relative, subprocess.run(
                command,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                cwd=temporary,
                timeout=TOOL_TIMEOUT_SECONDS,
                check=False,
            )

        seen: set[tuple[str, int, str]] = set()
        for relative, result in run_parallel(run_tidy, jobs, options.jobs):
            parsed = False
            for line in (result.stdout + "\n" + result.stderr).splitlines():
                match = DIAGNOSTIC_PATTERN.match(line.strip())
                if not match:
                    continue
                parsed = True
                where = relative_to_tree(tree, match.group("file")) or relative
                if not tree.in_scope(where):
                    continue
                check = match.group("check") or ""
                code = "naming" if "readability-identifier-naming" in check else "naming-compile-error"
                message = match.group("message")
                if code == "naming":
                    message = re.sub(r"\s*\[.*$", "", message)
                key = (where, int(match.group("line")), message)
                if key not in seen:
                    seen.add(key)
                    report.findings.append(Finding(where, int(match.group("line")), code, message))
            if result.returncode != 0 and not parsed:
                report.findings.append(
                    Finding(
                        relative,
                        1,
                        "naming-compile-error",
                        f"clang-tidy failed (exit code {result.returncode}): "
                        f"{(result.stderr or result.stdout).strip()[-400:]}",
                    )
                )
    finally:
        shutil.rmtree(temporary, ignore_errors=True)

    # Files for another operating system do not compile here; the regex checker covers them.
    for relative in other_systems:
        report.findings += regex_checker.check(tree.source(relative))
    report.detail = f"mode: {reason}; flags from {context.database_origin}" + (
        f"; regex fallback for {len(other_systems)} file(s) of other systems" if other_systems else ""
    )


# --------------------------------------------------------------------------------------------------------------------
# Step: header self-containment
# --------------------------------------------------------------------------------------------------------------------


def run_headers(context: LintContext, report: StepReport) -> None:
    tree = context.tree
    headers = [
        relative for relative in tree.cxx_files() if relative.endswith(CXX_HEADER_EXTENSION) and tree.in_scope(relative)
    ]
    report.files = len(headers)
    for relative in headers:
        source = tree.source(relative)
        first = next((directive for directive in source.directives), None)
        code_before = source.structure_code[: first.offset].strip() if first else source.structure_code.strip()
        if first is None or first.name != "pragma" or first.argument != "once" or code_before:
            report.findings.append(
                Finding(
                    relative,
                    first.line if first else 1,
                    "header-pragma-once",
                    "a header starts with '#pragma once' (CodeStyle section 4.2)",
                )
            )

    compiled = [relative for relative in headers if tree.builds_on_host(relative)]
    skipped = len(headers) - len(compiled)
    if not compiled:
        report.detail = "no headers to compile"
        return
    database = context.database()
    first_flags = next((flags for flags in (database.flags_for(relative) for relative in compiled) if flags), None)
    if first_flags is None:
        raise UsageError("compile_commands.json has no flags for any first-party project")
    compiler = find_compiler(first_flags, context.options.cxx)
    temporary = Path(tempfile.mkdtemp(prefix="Lint-Headers-"))
    try:
        jobs: list[tuple[str, Path, list[str]]] = []
        for index, relative in enumerate(compiled):
            flags = database.flags_for(relative)
            if flags is None:
                report.findings.append(
                    Finding(
                        relative,
                        1,
                        "header-self-contained",
                        "no compile flags for this header's project in compile_commands.json",
                    )
                )
                continue
            jobs.append((relative, header_translation_unit(temporary, index, tree.include_name(relative)), flags))

        def compile_header(job: tuple[str, Path, list[str]]) -> tuple[str, subprocess.CompletedProcess[str]]:
            relative, unit, flags = job
            command = [str(compiler)] + flags[1:] + ["-fsyntax-only", str(unit)]
            return relative, subprocess.run(
                command,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                cwd=temporary,
                timeout=TOOL_TIMEOUT_SECONDS,
                check=False,
            )

        for relative, result in run_parallel(compile_header, jobs, context.options.jobs):
            if result.returncode == 0:
                continue
            first_error = None
            for line in (result.stderr + "\n" + result.stdout).splitlines():
                match = DIAGNOSTIC_PATTERN.match(line.strip())
                if match and match.group("severity") != "warning":
                    first_error = match
                    break
            line_number = 1
            detail = (result.stderr or result.stdout).strip().splitlines()[-1:] or ["compiler failed"]
            if first_error is not None:
                where = relative_to_tree(tree, first_error.group("file"))
                if where == relative:
                    line_number = int(first_error.group("line"))
                location = where or Path(first_error.group("file")).name
                detail = [f"{location}:{first_error.group('line')}: {first_error.group('message')}"]
            report.findings.append(
                Finding(relative, line_number, "header-self-contained", f"does not compile on its own: {detail[0]}")
            )
    finally:
        shutil.rmtree(temporary, ignore_errors=True)
    report.detail = f"compiler {compiler}; flags from {context.database_origin}" + (
        f"; {skipped} header(s) of other systems not compiled" if skipped else ""
    )


# --------------------------------------------------------------------------------------------------------------------
# Step: Python
# --------------------------------------------------------------------------------------------------------------------

PYTHON_FUNCTION_NAME = re.compile(r"^_{0,2}[a-z][a-z0-9_]*$|^__[a-z][a-z0-9_]*__$")
PYTHON_CLASS_NAME = re.compile(r"^_?[A-Z][A-Za-z0-9]*$")
PYTHON_CONSTANT_NAME = re.compile(r"^_?[A-Z][A-Z0-9_]*$|^_?[A-Z][A-Za-z0-9]*$|^__[a-z][a-z0-9_]*__$")
PYTHON_ENTRY_POINT_FILE = re.compile(r"^[A-Z][A-Za-z0-9]*\.py$")
PYTHON_LIBRARY_FILE = re.compile(r"^[a-z_][a-z0-9_]*\.py$")
# Methods whose names a standard-library base class dictates.
PYTHON_PROTOCOL_METHODS = frozenset(
    {
        "setUp",
        "tearDown",
        "setUpClass",
        "tearDownClass",
        "setUpModule",
        "tearDownModule",
        "asyncSetUp",
        "asyncTearDown",
        "addCleanup",
        "doCleanups",
        "do_GET",
        "do_POST",
        "do_PUT",
        "do_DELETE",
        "do_HEAD",
        "do_OPTIONS",
        "log_message",
    }
)


def python_files(tree: Tree) -> list[str]:
    rules = tree.rules
    return [
        relative
        for relative in tree.files
        if relative.endswith(".py")
        and any(is_under(relative, root) for root in rules.python_roots)
        and not rules.python_exclude.matches(relative)
        and tree.in_scope(relative)
    ]


class PythonChecker(ast.NodeVisitor):
    def __init__(self, relative: str, local_modules: set[str], third_party_allowed: bool) -> None:
        self.relative = relative
        self.local_modules = local_modules
        self.third_party_allowed = third_party_allowed
        self.findings: list[Finding] = []
        self.parents: dict[ast.AST, ast.AST] = {}

    def report(self, node: ast.AST, code: str, message: str) -> None:
        self.findings.append(Finding(self.relative, getattr(node, "lineno", 1), code, message))

    def visit_Module(self, node: ast.Module) -> None:
        for parent in ast.walk(node):
            for child in ast.iter_child_nodes(parent):
                self.parents[child] = parent
        self.generic_visit(node)

    def visit_ClassDef(self, node: ast.ClassDef) -> None:
        if not PYTHON_CLASS_NAME.match(node.name):
            self.report(node, "python-naming", f"class '{node.name}' is not PascalCase (PEP 8, CodeStyle section 15)")
        self.generic_visit(node)

    def visit_FunctionDef(self, node: ast.FunctionDef) -> None:
        self.check_function(node)

    def visit_AsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:
        self.check_function(node)

    def check_function(self, node: ast.FunctionDef | ast.AsyncFunctionDef) -> None:
        is_method = isinstance(self.parents.get(node), ast.ClassDef)
        # visit_* methods of ast.NodeVisitor subclasses take the node class name, which is PascalCase.
        dictated = is_method and (node.name in PYTHON_PROTOCOL_METHODS or node.name.startswith("visit_"))
        if not PYTHON_FUNCTION_NAME.match(node.name) and not dictated:
            self.report(
                node, "python-naming", f"function '{node.name}' is not snake_case (PEP 8, CodeStyle section 15)"
            )
        arguments = node.args.posonlyargs + node.args.args + node.args.kwonlyargs
        decorators = {decorator.id for decorator in node.decorator_list if isinstance(decorator, ast.Name)}
        skip_first = is_method and "staticmethod" not in decorators
        missing = [
            argument.arg
            for index, argument in enumerate(arguments)
            if argument.annotation is None and not (skip_first and index == 0 and argument.arg in ("self", "cls"))
        ]
        for variadic in (node.args.vararg, node.args.kwarg):
            if variadic is not None and variadic.annotation is None:
                missing.append(variadic.arg)
        if missing:
            self.report(
                node,
                "python-annotations",
                f"function '{node.name}' lacks type hints for {', '.join(missing)} (CodeStyle section 15)",
            )
        if node.returns is None:
            self.report(
                node, "python-annotations", f"function '{node.name}' lacks a return type hint (CodeStyle section 15)"
            )
        self.generic_visit(node)

    def visit_Call(self, node: ast.Call) -> None:
        for keyword in node.keywords:
            if keyword.arg == "shell" and isinstance(keyword.value, ast.Constant) and keyword.value.value is True:
                self.report(
                    node, "python-shell", "subprocess with shell=True: pass an argument list (CodeStyle section 15)"
                )
        self.generic_visit(node)

    def visit_ExceptHandler(self, node: ast.ExceptHandler) -> None:
        if node.type is None:
            self.report(
                node,
                "python-bare-except",
                "bare 'except:' catches SystemExit and KeyboardInterrupt: name the exception",
            )
        self.generic_visit(node)

    def visit_ImportFrom(self, node: ast.ImportFrom) -> None:
        if any(alias.name == "*" for alias in node.names):
            self.report(node, "python-wildcard-import", f"'from {node.module} import *' hides where names come from")
        if node.level == 0 and node.module:
            self.check_import(node, node.module)
        self.generic_visit(node)

    def visit_Import(self, node: ast.Import) -> None:
        for alias in node.names:
            self.check_import(node, alias.name)
        self.generic_visit(node)

    def check_import(self, node: ast.AST, module: str) -> None:
        top = module.split(".", 1)[0]
        if (
            self.third_party_allowed
            or top in sys.stdlib_module_names
            or top in self.local_modules
            or top == "__future__"
        ):
            return
        self.report(
            node,
            "python-import",
            f"'{top}' is not in the standard library; scripts use the standard library only (CodeStyle section 15)",
        )


def fstring_delimiter(start: str) -> str:
    """The quote sequence of an FSTRING_START token such as f", rf''' or F'."""
    return start.lstrip("fFrRbBuU")


def newer_fstring_syntax(text: str) -> list[tuple[int, str]]:
    """f-strings that only Python 3.12+ accepts (PEP 701): inside a replacement field, the enclosing f-string's own
    quote, a backslash or a comment. ast.parse(feature_version=(3, 10)) does not reject them. Only the 3.12+ tokenizer
    exposes f-string structure; older interpreters reject these files at compile time anyway."""
    if not hasattr(tokenize, "FSTRING_START"):
        return []
    problems: list[tuple[int, str]] = []
    enclosing: list[str] = []
    try:
        for token in tokenize.generate_tokens(io.StringIO(text).readline):
            kind = token.type
            if kind == tokenize.FSTRING_END:  # type: ignore[attr-defined]
                if enclosing:
                    enclosing.pop()
                continue
            in_field = enclosing[:-1] if kind == tokenize.FSTRING_MIDDLE else enclosing  # type: ignore[attr-defined]
            if in_field:
                if kind in (tokenize.STRING, tokenize.FSTRING_START):  # type: ignore[attr-defined]
                    quote = (
                        fstring_delimiter(token.string)
                        if kind == tokenize.FSTRING_START
                        else token.string.lstrip("rRbBuUfF")[:3]
                    )  # type: ignore[attr-defined]
                    for outer in in_field:
                        if (len(outer) == 1 and quote[:1] == outer) or (len(outer) == 3 and quote.startswith(outer)):
                            problems.append(
                                (
                                    token.start[0],
                                    f"an f-string reuses its own quote ({outer}) inside a replacement field, which "
                                    "needs Python 3.12",
                                )
                            )
                            break
                if "\\" in token.string:
                    problems.append(
                        (token.start[0], "a backslash inside an f-string replacement field needs Python 3.12")
                    )
                if kind == tokenize.COMMENT:
                    problems.append(
                        (token.start[0], "a comment inside an f-string replacement field needs Python 3.12")
                    )
            if kind == tokenize.FSTRING_START:  # type: ignore[attr-defined]
                enclosing.append(fstring_delimiter(token.string))
    except (tokenize.TokenError, SyntaxError):
        return problems
    return problems


def check_python_file(tree: Tree, relative: str, local_modules: set[str]) -> list[Finding]:
    rules = tree.rules
    findings: list[Finding] = []
    raw = (tree.root / relative).read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        return [Finding(relative, 1, "python-encoding", f"not UTF-8: {error}")]
    try:
        compile(text, relative, "exec", dont_inherit=True)
    except SyntaxError as error:
        return [Finding(relative, error.lineno or 1, "python-syntax", error.msg)]
    try:
        module = ast.parse(text, filename=relative, feature_version=(3, 10))
    except SyntaxError as error:
        return [
            Finding(relative, error.lineno or 1, "python-version", f"needs syntax newer than Python 3.10: {error.msg}")
        ]
    for line, message in newer_fstring_syntax(text):
        findings.append(Finding(relative, line, "python-version", f"{message} (scripts support Python 3.10+)"))
    try:
        for token in tokenize.generate_tokens(io.StringIO(text).readline):
            if token.type == tokenize.INDENT and "\t" in token.string:
                findings.append(
                    Finding(
                        relative,
                        token.start[0],
                        "python-indentation",
                        "indentation uses tabs; Python uses 4 spaces (PEP 8, .editorconfig)",
                    )
                )
                break
    except (tokenize.TokenError, IndentationError) as error:
        findings.append(Finding(relative, 1, "python-syntax", str(error)))

    checker = PythonChecker(relative, local_modules, rules.python_third_party.matches(relative))
    checker.visit(module)
    findings += checker.findings

    for node in module.body:
        targets: list[ast.expr] = []
        if isinstance(node, ast.Assign):
            targets = node.targets
        elif isinstance(node, ast.AnnAssign):
            targets = [node.target]
        for target in targets:
            for name in assigned_names(target):
                if not PYTHON_CONSTANT_NAME.match(name):
                    findings.append(
                        Finding(
                            relative,
                            node.lineno,
                            "python-naming",
                            f"module-level name '{name}' is not an UPPER_SNAKE_CASE constant (PEP 8, CodeStyle section "
                            "15); keep state inside functions",
                        )
                    )

    name = Path(relative).name
    if rules.python_entry_points.matches(relative):
        if not PYTHON_ENTRY_POINT_FILE.match(name):
            findings.append(
                Finding(
                    relative,
                    1,
                    "python-entry-point",
                    f"entry-point script '{name}' is not PascalCase (CodeStyle section 15)",
                )
            )
        if not any(
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == "main" for node in module.body
        ):
            findings.append(
                Finding(
                    relative, 1, "python-entry-point", "entry-point script defines no main() (CodeStyle section 15)"
                )
            )
        last = module.body[-1] if module.body else None
        if not (last is not None and is_main_guard(last)):
            findings.append(
                Finding(
                    relative,
                    getattr(last, "lineno", 1),
                    "python-entry-point",
                    'entry-point script must end with: if __name__ == "__main__": sys.exit(main()) (CodeStyle section '
                    "15)",
                )
            )
    elif rules.python_libraries.matches(relative) and not PYTHON_LIBRARY_FILE.match(name):
        findings.append(Finding(relative, 1, "python-naming", f"library module '{name}' is not snake_case (PEP 8)"))
    return findings


def assigned_names(target: ast.expr) -> list[str]:
    """Names bound by an assignment target (unpacking included; attributes and subscripts bind no name)."""
    if isinstance(target, ast.Name):
        return [target.id]
    if isinstance(target, (ast.Tuple, ast.List)):
        return [name for element in target.elts for name in assigned_names(element)]
    if isinstance(target, ast.Starred):
        return assigned_names(target.value)
    return []


def is_main_guard(node: ast.stmt) -> bool:
    if not isinstance(node, ast.If) or len(node.body) != 1 or node.orelse:
        return False
    test = node.test
    if not (
        isinstance(test, ast.Compare)
        and isinstance(test.left, ast.Name)
        and test.left.id == "__name__"
        and len(test.ops) == 1
        and isinstance(test.ops[0], ast.Eq)
        and len(test.comparators) == 1
        and isinstance(test.comparators[0], ast.Constant)
        and test.comparators[0].value == "__main__"
    ):
        return False
    statement = node.body[0]
    if not (isinstance(statement, ast.Expr) and isinstance(statement.value, ast.Call)):
        return False
    call = statement.value
    return (
        isinstance(call.func, ast.Attribute)
        and call.func.attr == "exit"
        and isinstance(call.func.value, ast.Name)
        and call.func.value.id == "sys"
        and len(call.args) == 1
        and isinstance(call.args[0], ast.Call)
        and isinstance(call.args[0].func, ast.Name)
        and call.args[0].func.id == "main"
    )


def run_python(tree: Tree, report: StepReport) -> None:
    files = python_files(tree)
    report.files = len(files)
    # First-party modules and packages anywhere in the tree count as local imports.
    local_modules = {Path(relative).stem for relative in tree.files if relative.endswith(".py")}
    local_modules |= {Path(relative).parent.name for relative in tree.files if relative.endswith("__init__.py")}
    local_modules.add("Lib")
    for relative in files:
        report.findings += check_python_file(tree, relative, local_modules)


# --------------------------------------------------------------------------------------------------------------------
# Step: JSON
# --------------------------------------------------------------------------------------------------------------------


class DuplicateKeyError(ValueError):
    pass


def _reject_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateKeyError(f"duplicate key '{key}'")
        result[key] = value
    return result


def _reject_constant(value: str) -> Any:
    raise ValueError(f"'{value}' is not valid JSON")


def parse_strict_json(text: str) -> None:
    json.loads(text, object_pairs_hook=_reject_duplicates, parse_constant=_reject_constant)


def check_json_text(relative: str, text: str, line_offset: int) -> list[Finding]:
    try:
        parse_strict_json(text)
    except json.JSONDecodeError as error:
        return [Finding(relative, line_offset + error.lineno, "json-invalid", error.msg)]
    except DuplicateKeyError as error:
        return [
            Finding(
                relative,
                line_offset + 1,
                "json-duplicate-key",
                f"{error}: readers are strict and the canonical writer never writes one (Architecture section 6)",
            )
        ]
    except ValueError as error:
        return [Finding(relative, line_offset + 1, "json-invalid", str(error))]
    return []


def run_json(tree: Tree, report: StepReport) -> None:
    rules = tree.rules
    files = [
        relative
        for relative in tree.files
        if relative.endswith(rules.json_extensions + rules.json_lines_extensions)
        and not rules.json_exclude.matches(relative)
        and not relative.startswith(("Vendor/", "bin/", "bin-int/"))
        and tree.in_scope(relative)
    ]
    report.files = len(files)
    for relative in files:
        raw = (tree.root / relative).read_bytes()
        if raw.startswith(b"\xef\xbb\xbf"):
            report.findings.append(
                Finding(
                    relative, 1, "json-bom", "JSON files are UTF-8 without a byte-order mark (Architecture section 16)"
                )
            )
            raw = raw[3:]
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as error:
            report.findings.append(Finding(relative, 1, "json-encoding", f"not UTF-8: {error}"))
            continue
        if relative.endswith(rules.json_lines_extensions):
            for number, line in enumerate(text.split("\n")):
                if line.strip():
                    report.findings += check_json_text(relative, line, number)
        else:
            report.findings += check_json_text(relative, text, 0)


# --------------------------------------------------------------------------------------------------------------------
# Running
# --------------------------------------------------------------------------------------------------------------------


def run_lint(options: LintOptions) -> tuple[list[StepReport], LintContext]:
    rules = load_rules(options.rules_path)
    context = LintContext(options, rules)
    reports: list[StepReport] = []
    for step in STEPS:
        report = StepReport(step)
        reports.append(report)
        if step not in options.steps:
            report.status = "skipped"
            continue
        if step == "includes":
            run_includes(context.tree, report)
        elif step == "banned":
            run_banned(context, report)
        elif step == "contract":
            run_contract(context, report)
        elif step == "naming":
            run_naming(context, report)
        elif step == "headers":
            run_headers(context, report)
        elif step == "python":
            run_python(context.tree, report)
        elif step == "json":
            run_json(context.tree, report)
        report.findings.sort()
        report.status = "failed" if report.findings else "passed"
    return reports, context


def clang_tools_missing(options: LintOptions) -> str:
    """Why --mode clang cannot run here (empty when clang-tidy and clang-query are both available)."""
    if options.mode == "regex":
        return "--mode regex"
    clang_tidy = find_llvm_tool("clang-tidy", options.clang_tidy, "CLANG_TIDY", MINIMUM_CLANG_TIDY_MAJOR)
    if clang_tidy is None:
        return "clang-tidy not available"
    major, version = tool_major_version(clang_tidy)
    if major < MINIMUM_CLANG_TIDY_MAJOR:
        return f"clang-tidy {version} is older than {MINIMUM_CLANG_TIDY_MAJOR}"
    if find_llvm_tool("clang-query", options.clang_query, "CLANG_QUERY", MINIMUM_CLANG_TIDY_MAJOR) is None:
        return "clang-query not available"
    return ""


def run_self_test(options: LintOptions) -> dict[str, Any]:
    """Every fixture must fail with exactly its expected findings (code, file, line), in each mode it lists. An
    expected finding with its own Modes is expected only in those modes (what only a clang tool can find)."""
    try:
        manifest = json.loads(FIXTURE_MANIFEST_PATH.read_text(encoding="utf-8"))
        fixtures = [
            (
                fixture["Name"],
                list(fixture["Steps"]),
                list(fixture.get("Modes", ["auto"])),
                [
                    (item["Code"], item["File"], int(item["Line"]), tuple(item.get("Modes", ())))
                    for item in fixture["Expected"]
                ],
            )
            for fixture in manifest["Fixtures"]
        ]
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as error:
        raise UsageError(f"cannot read the fixture manifest {FIXTURE_MANIFEST_PATH}: {error!r}") from error
    results: list[dict[str, Any]] = []
    clang_unavailable = clang_tools_missing(options)
    for name, steps, modes, expectations in fixtures:
        root = FIXTURE_MANIFEST_PATH.parent / name
        if not root.is_dir():
            raise UsageError(f"fixture directory {root} does not exist")
        if not expectations:
            raise UsageError(f"fixture {name} expects no findings; every fixture must fail")
        unknown = sorted(set(steps) - set(STEPS)) + sorted(set(modes) - set(MODES))
        unknown += sorted({mode for *_, item_modes in expectations for mode in item_modes} - set(modes) - {"auto"})
        if unknown:
            raise UsageError(f"fixture {name}: unknown steps or modes {', '.join(unknown)}")
        for mode in modes:
            if mode == "clang" and clang_unavailable:
                results.append(
                    {
                        "fixture": name,
                        "mode": mode,
                        "status": "skipped",
                        "detail": clang_unavailable,
                    }
                )
                continue
            expected = {
                (code, file, line)
                for code, file, line, item_modes in expectations
                if not item_modes or mode in item_modes
            }
            # Fixtures always run strict: they prove that every rule still finds its seeded defect.
            fixture_options = dataclasses.replace(
                options,
                root=root,
                steps=steps,
                paths=None,
                mode=mode if mode != "auto" else options.mode,
                allow_contract_stubs=False,
            )
            reports, _ = run_lint(fixture_options)
            findings = [finding for report in reports for finding in report.findings]
            actual = {(finding.code, finding.file, finding.line) for finding in findings}
            missing = sorted(expected - actual)
            unexpected = actual - expected
            status = "passed" if not missing and not unexpected else "failed"
            results.append(
                {
                    "fixture": name,
                    "mode": mode,
                    "status": status,
                    "missing": [f"{file}:{line}: {code}" for code, file, line in missing],
                    "unexpected": [
                        finding.format()
                        for finding in findings
                        if (finding.code, finding.file, finding.line) in unexpected
                    ],
                }
            )
    return {"success": all(result["status"] != "failed" for result in results), "fixtures": results}


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Static checks for first-party code: include rules (Scripts/ModuleRules.json), banned APIs, "
        "contract stubs and skipped tests, naming (clang-tidy or regex fallback), header self-containment, Python "
        "style and JSON validity (Docs/Architecture.md section 2.3).",
        epilog="Steps: "
        + ", ".join(f"{step} ({STEP_TITLES[step]})" for step in STEPS)
        + ". Exit codes: 0 no findings, 1 findings, a failed self-test or a failed tool, 2 usage error or invalid "
        "rules, 3 a required tool missing or of the wrong version, 5 a tool timed out.",
    )
    parser.add_argument(
        "--steps", default=",".join(STEPS), help=f"comma-separated steps to run (default: all of {','.join(STEPS)})"
    )
    parser.add_argument(
        "--paths",
        nargs="+",
        metavar="PATH",
        help="only report findings for files under these paths (relative to --root, or absolute)",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=REPOSITORY_ROOT,
        help="tree to lint (default: the repository; e.g. a fixture under Tests/Data/Lint)",
    )
    parser.add_argument(
        "--rules", type=Path, default=DEFAULT_RULES_PATH, help="rules file (default: Scripts/ModuleRules.json)"
    )
    parser.add_argument(
        "--mode",
        choices=MODES,
        default="auto",
        help="checkers that need clang tools (clang-tidy naming, clang-query json access): auto uses each one that is "
        "installed, clang requires them, regex forces the fallbacks",
    )
    parser.add_argument(
        "--clang-tidy", help="path to clang-tidy (default: $CLANG_TIDY, Visual Studio's LLVM, then PATH)"
    )
    parser.add_argument(
        "--clang-query", help="path to clang-query (default: $CLANG_QUERY, Visual Studio's LLVM, then PATH)"
    )
    parser.add_argument(
        "--cxx", help="compiler for header self-containment (default: the database's compiler, clang++)"
    )
    parser.add_argument(
        "--premake", help="premake5 to use; it must be the pinned version (default: Vendor/premake/bin)"
    )
    parser.add_argument("--compile-commands", help="use this compile_commands.json instead of generating one")
    parser.add_argument(
        "--jobs", type=int, default=os.cpu_count() or 1, help="parallel tool processes (default: CPU count)"
    )
    parser.add_argument(
        "--allow-contract-stubs",
        action="store_true",
        help="contract mode, only for the commit of a milestone's contract task (Roadmap rule 3; PreCommit.py "
        "--contract): the contract step counts ENGINE_CONTRACT_STUB and skipped test cases as allowed instead of "
        "reporting them. The self-test always runs strict",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run the fixtures of Tests/Data/Lint/Fixtures.json and check their expected findings",
    )
    parser.add_argument("--json", action="store_true", help="print a machine-readable report on stdout")
    return parser.parse_args(argv)


def build_options(arguments: argparse.Namespace) -> LintOptions:
    steps = [step.strip() for step in arguments.steps.split(",") if step.strip()]
    unknown = sorted(set(steps) - set(STEPS))
    if unknown:
        raise UsageError(f"unknown step(s) {', '.join(unknown)}; choose from {', '.join(STEPS)}")
    root = arguments.root.resolve()
    if not root.is_dir():
        raise UsageError(f"--root {root} is not a directory")
    paths: list[str] | None = None
    if arguments.paths:
        paths = []
        for path in arguments.paths:
            candidate = Path(path)
            absolute = (candidate if candidate.is_absolute() else root / candidate).resolve()
            if not absolute.exists():
                raise UsageError(f"--paths: {path} does not exist")
            try:
                paths.append(absolute.relative_to(root).as_posix().rstrip("/") or ".")
            except ValueError as error:
                raise UsageError(f"--paths: {path} is outside {root}") from error
        if "." in paths:
            paths = None
    return LintOptions(
        root=root,
        rules_path=arguments.rules.resolve(),
        steps=steps,
        paths=paths,
        jobs=max(1, arguments.jobs),
        mode=arguments.mode,
        clang_tidy=arguments.clang_tidy,
        clang_query=arguments.clang_query,
        cxx=arguments.cxx,
        premake=arguments.premake,
        compile_commands=arguments.compile_commands,
        allow_contract_stubs=arguments.allow_contract_stubs,
    )


def main(argv: list[str] | None = None) -> int:
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")  # type: ignore[union-attr]
    try:
        options = build_options(arguments)
        if arguments.self_test:
            result = run_self_test(options)
            if arguments.json:
                print(json.dumps(result, indent=2))
            else:
                for entry in result["fixtures"]:
                    label = {"passed": "PASS", "failed": "FAIL", "skipped": "SKIP"}[entry["status"]]
                    print(
                        f"{label} {entry['fixture']} (mode {entry['mode']})"
                        + (f": {entry['detail']}" if entry.get("detail") else "")
                    )
                    for item in entry.get("missing", []):
                        print(f"    missing:    {item}")
                    for item in entry.get("unexpected", []):
                        print(f"    unexpected: {item}")
                passed = sum(1 for entry in result["fixtures"] if entry["status"] == "passed")
                print(
                    f"Lint self-test: {'passed' if result['success'] else 'FAILED'} ({passed} of "
                    f"{len(result['fixtures'])} fixture runs passed)"
                )
                if not result["success"]:
                    failing = [entry["fixture"] for entry in result["fixtures"] if entry["status"] == "failed"]
                    print(f"[FAILED] lint self-test: fixture(s) without exactly their expected findings: "
                          f"{', '.join(failing)}")
            return EXIT_SUCCESS if result["success"] else EXIT_FINDINGS
        reports, context = run_lint(options)
    except (UsageError, ToolMissing, ToolFailure, OSError, subprocess.TimeoutExpired) as error:
        if arguments.json:
            print(json.dumps({"success": False, "error": str(error)}, indent=2))
        print(f"Lint.py: error: {error}", file=sys.stderr)
        if not arguments.json:
            print(f"[FAILED] lint: {error}")
        if isinstance(error, ToolMissing):
            return EXIT_INIT_FAILED
        if isinstance(error, subprocess.TimeoutExpired):
            return EXIT_TIMEOUT
        return EXIT_USAGE if isinstance(error, UsageError) else EXIT_FAILED

    findings = [finding for report in reports for finding in report.findings]
    success = not findings
    if arguments.json:
        print(
            json.dumps(
                {
                    "success": success,
                    "root": options.root.as_posix(),
                    "rules": options.rules_path.as_posix(),
                    "allowContractStubs": options.allow_contract_stubs,
                    "steps": [report.as_dict() for report in reports],
                    "findings": [finding.as_dict() for finding in findings],
                },
                indent=2,
            )
        )
    else:
        for finding in findings:
            print(finding.format())
        for report in reports:
            if report.status == "skipped":
                continue
            state = "passed" if report.status == "passed" else f"FAILED ({len(report.findings)} finding(s))"
            detail = f"; {report.detail}" if report.detail else ""
            print(f"Lint: {STEP_TITLES[report.name]}: {state} [{report.files} file(s){detail}]")
        print(f"Lint: {'passed' if success else 'FAILED'} ({len(findings)} finding(s); root {options.root.as_posix()})")
        if findings:
            # The summary line CI.py and PreCommit.py quote for a failing step.
            print(f"[FAILED] lint: {len(findings)} finding(s), first: {findings[0].format()}")
    return EXIT_SUCCESS if success else EXIT_FINDINGS


if __name__ == "__main__":
    sys.exit(main())
