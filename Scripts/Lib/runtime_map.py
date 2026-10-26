"""The Dist map-file check (Docs/Architecture.md §14.4; Roadmap M15 acceptance "Dist: map file contains no automation,
Analysis, ImGui or AssetPipeline symbols"; Docs/Decisions/0024-m15-contract.md decision 11). Scripts/CheckRuntimeMap.py
is its command line; CI.py runs the check right after its Dist build, as a step named with the acceptance name verbatim,
and the static checks run CheckRuntimeMap.py --self-test over Tests/Data/MapFiles/.

The Runtime links a map file in every configuration (Runtime/premake5.lua). The parser reads the four formats the
supported linkers write:
  - "msvc": link.exe /MAP ("Publics by Value" and "Static symbols": address, decorated name, Rva+Base, Lib:Object);
  - "lld": LLVM lld's map, from ld.lld -Map (the Linux Clang job: "VMA LMA Size Align Out In Symbol") and lld-link /MAP
    when a toolset links with it ("Address Size Align Out In Symbol"); input sections name archive(member), and the
    symbols listed below an input section come from it;
  - "gnu": GNU ld -Map (the Linux GCC job): the "Linker script and memory map" part, input sections with their
    archive(member), then the symbol lines (discarded input sections are not read);
  - "ld64": Apple ld -map ("# Object files:" with [index] archive(member), "# Symbols:" address size [index] name; the
    dead-stripped symbols are not read).
LTO partitions lose the archive in every format, so families are matched on symbol names alone there.

Symbol names are matched only through namespace or type components of decorated, mangled or demangled names, never raw
substrings. Each symbol name is read into the qualified names it spells: its own (the namespaces, enclosing types and
the entity, so a local static or a lambda counts as its enclosing function's) and those of its template arguments
(MSVC "?Begin@ImGui@@..." is ImGui::Begin, Itanium "_ZN6Engine8Watchdog4BeatEv" is Engine::Watchdog::Beat, demangled
"Luau::Frontend::check(...)" is Luau::Frontend::check). A name pattern matches a symbol when it fullmatches one of the
symbol's scope paths, each leading part of one of those qualified names written with "::" ("Engine",
"Engine::Watchdog", "Engine::Watchdog::Beat"). So string literals ("??_C@_05...ImGui...") never count, and neither does
"Engine::ScriptWatchdog" for "Engine::Watchdog". A symbol's origin counts only through its library component (the
"<library>" of "<library>:<object>" or "<archive>(<member>)": ImGui, imgui.lib, libLuauAnalysis.a), never through its
object-file stem, so MSVC's "Watchdog1.obj" never counts. The families, derived from the headers so a new type or
function is covered without editing this module (every type defined and every free function declared at namespace
scope, with its namespaces, except the shared Utils helpers, whose names other modules' file-local helpers reuse;
nested types are covered by their enclosing type; declared_names):
  - automation: every type and free function declared in Engine/Source/Engine/Automation/**.h
    (RuntimeAutomationServer, Engine::Watchdog and the RPC parsers such as ParseRpcRequest among them);
  - Analysis: Luau::Frontend and any symbol whose origin is the LuauAnalysis library;
  - ImGui: the ImGui namespace, any symbol whose origin is the ImGui library (or ImGuizmo, its extension), and every
    type and free function declared in Engine/Source/Engine/ImGui/**.h (ImGuiLayer, ImGuiRenderer,
    CaptureImGuiScreenshot, StatsOverlayToggle and their peers);
  - AssetPipeline: every type and free function declared in Engine/Source/Engine/AssetPipeline/**.h;
  - LuauCompiler (M19, appended by Docs/Decisions/0030-m19-contract.md decision 22, the extension of the frozen API that
    ADR 0024 decision 11 approved): Luau's compiler, its bytecode builder, its configuration reader (which compiles
    .config.luau files) and its require library, which the sandbox's first-party require replaced in every
    configuration, so Dist links none of them: Luau::compile, Luau::compileOrThrow, Luau::Compiler, Luau::Compile,
    Luau::CompileOptions, Luau::CompileError, Luau::buildTypeMap (the compiler's type map), Luau::BytecodeBuilder,
    Luau::BytecodeEncoder, Luau::Config, Luau::ConfigOptions, Luau::parseConfig, Luau::extractLuauConfig,
    Luau::LintOptions, Luau::LintWarning (the configuration's lint rules), Luau::Require, luau_compile, luaopen_require
    and every luarequire_* function, so each object of those four Luau modules shows. Names only: they share the Luau
    library with the VM, which stays, and so do Luau's Parser, Ast and Lexer, which ScriptApiRegistry's signature
    validation keeps in Dist.
The acceptance step keeps its M15 name, which names the first four. REQUIRED_SYMBOLS must be present, as a component
of some symbol's qualified names, so the FeatureTest harness provably stays in Dist (§14.4).
"""

from __future__ import annotations

import dataclasses
import re
from pathlib import Path

FORBIDDEN_FAMILIES = ("automation", "Analysis", "ImGui", "AssetPipeline", "LuauCompiler")
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
    """How one family is recognized: a symbol belongs to it when a `names` pattern fullmatches one of its scope paths
    (see the module docstring) or a `libraries` pattern fullmatches the file name of its library component."""

    names: tuple[re.Pattern[str], ...]
    libraries: tuple[re.Pattern[str], ...]


@dataclasses.dataclass(frozen=True)
class MapFile:
    format: str  # one of MAP_FORMATS
    symbols: tuple[MapSymbol, ...]


# --------------------------------------------------------------------------------------------------------------------
# The families
# --------------------------------------------------------------------------------------------------------------------

# The header trees whose declarations form a family, relative to the repository root.
_FAMILY_HEADER_ROOTS = {
    "automation": "Engine/Source/Engine/Automation",
    "ImGui": "Engine/Source/Engine/ImGui",
    "AssetPipeline": "Engine/Source/Engine/AssetPipeline",
}
# Qualified names that belong to a family whatever the headers say.
_FAMILY_FIXED_NAMES = {
    "Analysis": ("Luau::Frontend",),
    "ImGui": ("ImGui",),
    "LuauCompiler": ("Luau::compile", "Luau::compileOrThrow", "Luau::Compiler", "Luau::Compile", "Luau::CompileOptions",
                     "Luau::CompileError", "Luau::buildTypeMap", "Luau::BytecodeBuilder", "Luau::BytecodeEncoder",
                     "Luau::Config", "Luau::ConfigOptions", "Luau::parseConfig", "Luau::extractLuauConfig",
                     "Luau::LintOptions", "Luau::LintWarning", "Luau::Require", "luau_compile", "luaopen_require"),
}
# Name patterns (regular expressions, fullmatched like the names) that belong to a family whatever the headers say.
_FAMILY_FIXED_EXPRESSIONS = {
    "LuauCompiler": (r"luarequire_\w+",),
}
# Library components (file names without "lib" or the extension, case-insensitively) that belong to a family.
_FAMILY_LIBRARIES = {
    "Analysis": ("LuauAnalysis",),
    "ImGui": ("ImGui", "ImGuizmo"),
}


def family_patterns(repository_root: Path) -> dict[str, FamilyPatterns]:
    """The patterns of each FORBIDDEN_FAMILIES entry, derived from the headers the module docstring names (so a new
    Automation, ImGui or AssetPipeline type or function is covered without editing this module). Raises ValueError when
    a family's header tree is missing or declares nothing."""
    patterns: dict[str, FamilyPatterns] = {}
    for family in FORBIDDEN_FAMILIES:
        names = set(_FAMILY_FIXED_NAMES.get(family, ()))
        root = _FAMILY_HEADER_ROOTS.get(family)
        if root is not None:
            directory = repository_root / root
            headers = sorted(directory.rglob("*.h")) if directory.is_dir() else []
            declared: set[str] = set()
            for header in headers:
                declared.update(declared_names(header.read_text(encoding="utf-8")))
            if not declared:
                raise ValueError(f"{root}: no header declares a type or a free function of the {family} family")
            names.update(declared)
        libraries = tuple(re.compile(rf"(?:lib)?{re.escape(library)}(?:\.lib|\.a)?", re.IGNORECASE)
                          for library in _FAMILY_LIBRARIES.get(family, ()))
        expressions = tuple(re.compile(expression) for expression in _FAMILY_FIXED_EXPRESSIONS.get(family, ()))
        patterns[family] = FamilyPatterns(tuple(re.compile(re.escape(name)) for name in sorted(names)) + expressions,
                                          libraries)
    return patterns


# Comments, string and character literals, and preprocessor lines, which the declaration scanner removes first.
_SOURCE_NOISE = re.compile(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'|^[ \t]*#[^\n]*",
                           re.DOTALL | re.MULTILINE)
_NAMESPACE_STATEMENT = re.compile(r"^(?:inline\s+)?namespace\s*([A-Za-z_][\w:]*)?\s*$")
_TYPE_STATEMENT = re.compile(r"^(?:class|struct|union|enum(?:\s+class|\s+struct)?)\s+(?:\[\[[^\]]*\]\]\s*)*"
                             r"(?:alignas\s*\([^)]*\)\s*)*([A-Za-z_]\w*)\b")
_IDENTIFIER_BEFORE_PARENTHESIS = re.compile(r"([A-Za-z_]\w*)\s*$")
_NOT_FUNCTION_NAMES = frozenset({
    "if", "for", "while", "switch", "return", "sizeof", "alignof", "alignas", "decltype", "noexcept", "static_assert",
    "requires", "operator", "throw", "typeid", "catch",
})
_NOT_DECLARATION_STARTS = ("using", "typedef", "static_assert", "friend", "return", "template class",
                           "template struct", "extern template")


def _strip_template_prefix(statement: str) -> str:
    """`statement` without a leading template<...> parameter list (nested angle brackets included)."""
    text = statement.lstrip()
    while text.startswith("template"):
        rest = text[len("template"):].lstrip()
        if not rest.startswith("<"):
            return text
        depth = 0
        for index, char in enumerate(rest):
            if char == "<":
                depth += 1
            elif char == ">":
                depth -= 1
                if depth == 0:
                    text = rest[index + 1:].lstrip()
                    break
        else:
            return text
    return text


def _strip_attributes(statement: str) -> str:
    return re.sub(r"\[\[.*?\]\]", " ", statement).strip()


def _function_name(statement: str) -> str | None:
    """The name of the free function `statement` declares (or defines, up to its body), else None: the identifier right
    before the first parenthesis outside template arguments, after a return type."""
    text = _strip_attributes(_strip_template_prefix(statement))
    if not text or text.startswith(_NOT_DECLARATION_STARTS):
        return None
    depth = 0
    for index, char in enumerate(text):
        if char == "<":
            depth += 1
        elif char == ">":
            depth = max(depth - 1, 0)
        elif char == "(" and depth == 0:
            head = text[:index]
            match = _IDENTIFIER_BEFORE_PARENTHESIS.search(head)
            if match is None:
                return None
            name = match.group(1)
            return_type = head[:match.start()].strip()
            if (not return_type or name in _NOT_FUNCTION_NAMES or name.startswith("ENGINE_")
                    or return_type.endswith(("operator", "::", "~", "=", ",")) or "=" in return_type):
                return None
            return name
    return None


def declared_names(header: str) -> list[str]:
    """The qualified names ("Engine::Watchdog", "Engine::Automation::RegisterAudioMethods") of every type defined (with
    a body; forward declarations name types declared elsewhere) and every free function declared at namespace scope in
    the header text `header`, sorted. Declarations inside an anonymous namespace, a type or a function are not
    included, and neither are the functions of a Utils namespace: CodeStyle puts every translation unit's file-local
    helpers there as internal-linkage functions, so a qualified name such as Engine::Utils::PrefixPointers also names
    helpers of other modules (the Scene serializer's) and identifies no family."""
    text = _SOURCE_NOISE.sub(lambda match: '""' if match.group(0)[:1] in "\"'" else " ", header)
    names: set[str] = set()
    # Each scope is (kind, namespace components): "namespace" (components named), "linkage" (extern "C"), "type" or
    # "other" (a function body or a braced initializer).
    scopes: list[tuple[str, tuple[str, ...]]] = []
    statement: list[str] = []

    def namespace_scope() -> tuple[str, ...] | None:
        path: list[str] = []
        for kind, components in scopes:
            if kind not in ("namespace", "linkage"):
                return None
            if kind == "namespace" and not components:
                return None  # an anonymous namespace
            path.extend(components)
        return tuple(path)

    def add_function(path: tuple[str, ...] | None, statement_text: str) -> None:
        if path is None or "Utils" in path:
            return
        function = _function_name(statement_text)
        if function is not None:
            names.add("::".join((*path, function)))

    for char in text:
        if char not in "{};":
            statement.append(char)
            continue
        current = " ".join("".join(statement).split())
        statement = []
        if char == "}":
            if scopes:
                scopes.pop()
            continue
        path = namespace_scope()
        if char == ";":
            add_function(path, current)
            continue
        namespace = _NAMESPACE_STATEMENT.match(current)
        if namespace is not None:
            components = tuple(part for part in (namespace.group(1) or "").split("::") if part)
            scopes.append(("namespace", components))
            continue
        if current.startswith("extern") and current.endswith('""'):
            scopes.append(("linkage", ()))
            continue
        type_definition = _TYPE_STATEMENT.match(_strip_attributes(_strip_template_prefix(current)))
        if type_definition is not None:
            if path is not None:
                names.add("::".join((*path, type_definition.group(1))))
            scopes.append(("type", ()))
            continue
        add_function(path, current)
        scopes.append(("other", ()))
    return sorted(names)


# --------------------------------------------------------------------------------------------------------------------
# Qualified names inside symbol names
# --------------------------------------------------------------------------------------------------------------------

class _ParseStop(Exception):
    """The rest of a name lies outside the grammar subset this module reads; what was read so far is kept."""


class _MsvcNameReader:
    """Reads the qualified names of an MSVC decorated name: the symbol's own (with the names of local-scope entities'
    enclosing functions) and the class, struct, union and enum names in its template arguments. Parameter and return
    types are not read."""

    def __init__(self, text: str, names: list[str] | None = None) -> None:
        self.text = text
        self.position = 0
        self.qualified_names: list[list[str]] = []
        self._names: list[str] = names if names is not None else []  # the name back-references 0 to 9

    def _peek(self, offset: int = 0) -> str:
        index = self.position + offset
        return self.text[index] if index < len(self.text) else ""

    def _memorize(self, name: str) -> None:
        if name not in self._names and len(self._names) < 10:
            self._names.append(name)

    def read_symbol(self) -> None:
        if self._peek() != "?":
            raise _ParseStop
        self.position += 1
        if self._peek() == "?" and self._peek(1) != "$":
            # A special name: an operator, constructor, destructor, vftable, string literal or RTTI descriptor.
            self.position += 1
            if self._peek() == "_":
                length = 3 if self._peek(1) in ("_", "R") else 2
            else:
                length = 1
            code = self.text[self.position:self.position + length]
            self.position += length
            if code == "_C":
                return  # a string literal: its text is no name
            if code == "_R0":
                self._read_type()
                return
            if code == "_R1":
                for _ in range(4):
                    self._read_number()
        self.qualified_names.append(self._read_qualified_name())

    def _read_qualified_name(self) -> list[str]:
        components: list[str] = []
        try:
            while True:
                char = self._peek()
                if char == "":
                    raise _ParseStop
                if char == "@":
                    self.position += 1
                    break
                components.append(self._read_fragment())
        except _ParseStop:
            if components:
                self.qualified_names.append(components[::-1])
            raise
        return components[::-1]

    def _read_identifier(self) -> str:
        end = self.text.find("@", self.position)
        if end <= self.position:
            raise _ParseStop
        name = self.text[self.position:end]
        self.position = end + 1
        return name

    def _read_fragment(self) -> str:
        char = self._peek()
        if char.isdigit():
            self.position += 1
            index = int(char)
            return self._names[index] if index < len(self._names) else "?"
        if char == "?":
            if self._peek(1) == "$":
                self.position += 2
                return self._read_template_name()
            if self.text.startswith("?A0x", self.position):
                self._read_identifier()
                return "`anonymous namespace'"
            # A local scope: '?' <number> '?' and the enclosing function's decorated name, whose end is not known
            # without its whole type grammar, so reading stops after its qualified name.
            self.position += 1
            self._read_number()
            if self._peek() != "?":
                raise _ParseStop
            nested = _MsvcNameReader(self.text, list(self._names))
            nested.position = self.position
            try:
                nested.read_symbol()
            except _ParseStop:
                pass
            self.qualified_names.extend(nested.qualified_names)
            raise _ParseStop
        name = self._read_identifier()
        self._memorize(name)
        return name

    def _read_template_name(self) -> str:
        name = self._read_identifier()
        outer = self._names
        self._names = [name]  # template arguments have a back-reference table of their own
        try:
            while self._peek() != "@":
                if self._peek() == "":
                    raise _ParseStop
                self._read_type()
            self.position += 1
        finally:
            self._names = outer
        self._memorize(name)
        return name

    def _read_number(self) -> int:
        negative = self._peek() == "?"
        if negative:
            self.position += 1
        char = self._peek()
        if char.isdigit():
            self.position += 1
            value = int(char) + 1
        else:
            value = 0
            while True:
                char = self._peek()
                if char == "@":
                    self.position += 1
                    break
                if "A" <= char <= "P":
                    value = value * 16 + ord(char) - ord("A")
                    self.position += 1
                else:
                    raise _ParseStop
        return -value if negative else value

    def _read_pointee(self) -> None:
        while self._peek() in ("E", "F", "I"):  # __ptr64, __unaligned, __restrict
            self.position += 1
        if self._peek() not in ("A", "B", "C", "D"):  # function and member pointers are not read
            raise _ParseStop
        self.position += 1
        self._read_type()

    def _read_type(self) -> None:
        char = self._peek()
        if char in ("V", "U", "T"):
            self.position += 1
            self.qualified_names.append(self._read_qualified_name())
        elif char == "W":
            self.position += 2
            self.qualified_names.append(self._read_qualified_name())
        elif char in ("P", "Q", "R", "S", "A", "B"):
            self.position += 1
            self._read_pointee()
        elif char == "?":
            self.position += 2  # a cv-qualified type
            self._read_type()
        elif char == "_":
            self.position += 2  # the two-letter builtin types (_N bool, _J __int64, _K unsigned __int64, _W wchar_t)
        elif char == "$":
            self._read_dollar_type()
        elif char.isdigit():
            self.position += 1  # a parameter-type back-reference
        elif char != "" and char in "CDEFGHIJKMNOXZ":
            self.position += 1  # the one-letter builtin types
        elif char == "Y":
            self.position += 1
            for _ in range(self._read_number()):
                self._read_number()
            self._read_type()
        else:
            raise _ParseStop

    def _read_dollar_type(self) -> None:
        code = self.text[self.position:self.position + 3]
        if code in ("$$Q", "$$R"):
            self.position += 3
            self._read_pointee()
        elif code in ("$$T", "$$V", "$$Z"):
            self.position += 3
        elif code == "$$B":
            self.position += 3
            self._read_type()
        elif code == "$$C":
            self.position += 4
            self._read_type()
        elif code[:2] == "$0":
            self.position += 2
            self._read_number()
        elif code[:2] == "$M":
            self.position += 2
            self._read_type()
            self._read_type()
        else:
            raise _ParseStop


_ITANIUM_STANDARD_SUBSTITUTIONS = {
    "St": ["std"],
    "Sa": ["std", "allocator"],
    "Sb": ["std", "basic_string"],
    "Ss": ["std", "basic_string"],
    "Si": ["std", "basic_istream"],
    "So": ["std", "basic_ostream"],
    "Sd": ["std", "basic_iostream"],
}


class _ItaniumNameReader:
    """Reads the qualified names of an Itanium mangled name: the symbol's own (or, for a local entity, its enclosing
    function's; for a vtable, typeinfo or guard variable, its type's or variable's) and the class names in its template
    arguments. Parameter types are not read, and substitutions other than the std ones stay unknown ("?")."""

    def __init__(self, text: str) -> None:
        self.text = text
        self.position = 0
        self.qualified_names: list[list[str]] = []

    def _peek(self, offset: int = 0) -> str:
        index = self.position + offset
        return self.text[index] if index < len(self.text) else ""

    def read_symbol(self) -> None:
        if not self.text.startswith("_Z"):
            raise _ParseStop
        self.position = 2
        if self._peek() == "T":
            code = self._peek(1)
            if code in ("V", "T", "I", "S"):  # vtable, VTT, typeinfo, typeinfo name
                self.position += 2
                self._read_type()
            elif code in ("h", "v"):  # thunks
                self.position += 1
                self._read_call_offset()
                self._read_name()
            elif code == "c":
                self.position += 2
                self._read_call_offset()
                self._read_call_offset()
                self._read_name()
            elif code in ("W", "H"):  # thread-local wrapper and init functions
                self.position += 2
                self._read_name()
            else:
                raise _ParseStop
            return
        if self._peek() == "G":
            if self._peek(1) not in ("V", "R"):  # guard variables and reference temporaries
                raise _ParseStop
            self.position += 2
        self._read_name()

    def _read_name(self) -> None:
        char = self._peek()
        if char == "N":
            self._read_nested_name()
        elif char == "Z":
            # A local entity: Z <enclosing function> E <entity>. The function's type comes before the E, so reading
            # stops after the function's name.
            self.position += 1
            self._read_name()
            raise _ParseStop
        elif char == "S":
            components = self._read_substitution()
            if self._peek() not in ("I", "E", "") and components == ["std"]:
                components = [*components, self._read_unqualified_name()]
            if self._peek() == "I":
                self._read_template_args()
            self.qualified_names.append(components)
        else:
            components = [self._read_unqualified_name()]
            if self._peek() == "I":
                self._read_template_args()
            self.qualified_names.append(components)

    def _read_nested_name(self) -> None:
        self.position += 1
        while self._peek() in ("r", "V", "K"):
            self.position += 1
        if self._peek() in ("R", "O"):
            self.position += 1
        components: list[str] = []
        try:
            while self._peek() != "E":
                char = self._peek()
                if char == "":
                    raise _ParseStop
                if char == "I":
                    self._read_template_args()
                elif char == "S":
                    components.extend(self._read_substitution())
                elif char == "T":
                    self._read_template_param()
                    components.append("?")
                else:
                    components.append(self._read_unqualified_name())
            self.position += 1
        except _ParseStop:
            if components:
                self.qualified_names.append(components)
            raise
        self.qualified_names.append(components)

    def _read_source_name(self) -> str:
        start = self.position
        while self._peek().isdigit():
            self.position += 1
        if start == self.position:
            raise _ParseStop
        length = int(self.text[start:self.position])
        name = self.text[self.position:self.position + length]
        if len(name) != length:
            raise _ParseStop
        self.position += length
        while self._peek() == "B" and self._peek(1).isdigit():  # ABI tags ("B5cxx11")
            self.position += 1
            self._read_source_name()
        return "(anonymous namespace)" if name.startswith("_GLOBAL__N") else name

    def _read_unqualified_name(self) -> str:
        char = self._peek()
        if char.isdigit():
            return self._read_source_name()
        if char == "L":  # an internal-linkage name
            self.position += 1
            return self._read_source_name()
        if char == "C" and self._peek(1) in ("1", "2", "3"):
            self.position += 2
            return "{constructor}"
        if char == "D" and self._peek(1) in ("0", "1", "2"):
            self.position += 2
            return "{destructor}"
        if char == "U" and self._peek(1) == "t":
            end = self.text.find("_", self.position)
            if end < 0:
                raise _ParseStop
            self.position = end + 1
            return "{unnamed}"
        if char.islower() and self._peek(1).isalpha() and not (char == "c" and self._peek(1) == "v"):
            self.position += 2
            return "operator"
        raise _ParseStop

    def _read_substitution(self) -> list[str]:
        code = self.text[self.position:self.position + 2]
        if code in _ITANIUM_STANDARD_SUBSTITUTIONS:
            self.position += 2
            return list(_ITANIUM_STANDARD_SUBSTITUTIONS[code])
        end = self.text.find("_", self.position)
        sequence = self.text[self.position + 1:end] if end >= 0 else ""
        if end < 0 or not all(char.isdigit() or char.isupper() for char in sequence):
            raise _ParseStop
        self.position = end + 1
        return ["?"]

    def _read_template_param(self) -> None:
        end = self.text.find("_", self.position)
        if end < 0:
            raise _ParseStop
        self.position = end + 1

    def _read_template_args(self) -> None:
        self.position += 1
        while self._peek() != "E":
            if self._peek() == "":
                raise _ParseStop
            self._read_template_arg()
        self.position += 1

    def _read_template_arg(self) -> None:
        char = self._peek()
        if char == "L":
            if self._peek(1) == "_":
                raise _ParseStop  # an external name as a template argument
            end = self.text.find("E", self.position)
            if end < 0:
                raise _ParseStop
            self.position = end + 1
        elif char == "J":
            self.position += 1
            while self._peek() != "E":
                if self._peek() == "":
                    raise _ParseStop
                self._read_template_arg()
            self.position += 1
        elif char == "X":
            raise _ParseStop  # an expression
        else:
            self._read_type()

    def _read_type(self) -> None:
        char = self._peek()
        if char != "" and char in "vwbcahstijlmxynofdegz":
            self.position += 1
        elif char == "u":
            self.position += 1
            self._read_source_name()
        elif char != "" and char in "PROCGrVK":
            self.position += 1
            self._read_type()
        elif char == "D":
            code = self._peek(1)
            if code != "" and code in "dfehiscnau":
                self.position += 2
            elif code == "p":
                self.position += 2
                self._read_type()
            else:
                raise _ParseStop
        elif char == "T":
            self._read_template_param()
            if self._peek() == "I":
                self._read_template_args()
        elif char in ("N", "S", "Z") or char.isdigit():
            self._read_name()
        else:
            raise _ParseStop  # function, array, member-pointer and vector types

    def _read_call_offset(self) -> None:
        kind = self._peek()
        if kind not in ("h", "v"):
            raise _ParseStop
        self.position += 1
        for _ in range(1 if kind == "h" else 2):
            end = self.text.find("_", self.position)
            if end < 0:
                raise _ParseStop
            self.position = end + 1


# Operator names hold the characters the demangled reader uses as separators, so they become plain "operator" first;
# conversion operators lose their target type.
_DEMANGLED_OPERATOR = re.compile(r"\boperator(?:\s*(?:\(\)|\[\]|new\[\]|delete\[\]|->\*|->|<=>|<<=|>>=|<<|>>|<=|>="
                                 r"|==|!=|&&|\|\||\+\+|--|[-+*/%^&|~!=<>,]=?)|\s+[\w:]+)")
_DEMANGLED_QUALIFIER_WORDS = frozenset({"const", "volatile", "class", "struct", "union", "enum", "typename", "unsigned",
                                        "signed", "__ptr64", "__restrict"})


def _split_top_level(text: str, separators: str) -> list[str]:
    """`text` split at `separators` outside angle brackets and parentheses."""
    parts: list[str] = []
    depth = 0
    current: list[str] = []
    for char in text:
        if char in "<(":
            depth += 1
        elif char in ">)":
            depth = max(depth - 1, 0)
        if char in separators and depth == 0:
            parts.append("".join(current))
            current = []
        else:
            current.append(char)
    parts.append("".join(current))
    return [part for part in (part.strip() for part in parts) if part]


def _demangled_names(text: str) -> list[list[str]]:
    """The qualified name `text` spells ("std::unique_ptr<Engine::Watchdog>::reset") and those of its template
    arguments, each as its components without template arguments."""
    names: list[list[str]] = []
    components: list[str] = []
    current: list[str] = []
    depth = 0
    arguments_start = -1
    index = 0
    while index < len(text):
        char = text[index]
        if char == "<":
            if depth == 0:
                arguments_start = index + 1
            depth += 1
        elif char == ">":
            depth -= 1
            if depth < 0:
                break
            if depth == 0 and arguments_start >= 0:
                for argument in _split_top_level(text[arguments_start:index], ","):
                    words = [word.rstrip("*&")
                             for word in _split_top_level(argument.replace("*", " ").replace("&", " "), " ")]
                    words = [word for word in words if word and word not in _DEMANGLED_QUALIFIER_WORDS]
                    if words:
                        names.extend(_demangled_names(words[-1]))
                arguments_start = -1
        elif depth == 0:
            if text.startswith("::", index):
                components.append("".join(current).strip())
                current = []
                index += 2
                continue
            current.append(char)
        index += 1
    components.append("".join(current).strip())
    if any(components):
        names.append([component or "?" for component in components])
    return names


def _demangled_qualified_names(name: str) -> list[list[str]]:
    if "`string'" in name:
        return []  # a string literal
    text = _DEMANGLED_OPERATOR.sub("operator", name)
    text = text.replace("(anonymous namespace)", "{anonymous}").replace("`anonymous namespace'", "{anonymous}")
    # The parameter list, and with it the rest of a local entity's name, starts at the first parenthesis outside
    # template arguments.
    depth = 0
    for index, char in enumerate(text):
        if char == "<":
            depth += 1
        elif char == ">":
            depth = max(depth - 1, 0)
        elif char == "(" and depth == 0:
            text = text[:index]
            break
    # Return types, access specifiers, calling conventions and "vtable for" come first: the name is the last token.
    tokens = _split_top_level(text, " \t")
    return _demangled_names(tokens[-1]) if tokens else []


_PLAIN_NAME = re.compile(r"[A-Za-z_$.][\w$.@]*")
# ld64 lists C string literals by their text, and unwind and pointer entries by the name of what they refer to.
_LD64_LITERAL_PREFIXES = ("literal string: ", "cstring=")
_LD64_REFERENCE_PREFIXES = ("FDE for: ", "CIE for: ", "non-lazy-pointer-to-local: ", "lazy-pointer-to-local: ",
                            "non-lazy-pointer-to: ")


def qualified_names(name: str, map_format: str) -> list[list[str]]:
    """The qualified names a symbol name spells (see the module docstring), each as its components, outermost first:
    MSVC decorated names ("?..."), Itanium mangled names ("_Z...", "__Z..." in ld64 maps), demangled names and plain C
    names. A name outside the grammar subset read here yields what was read before it."""
    if map_format == "ld64":
        if name.startswith(_LD64_LITERAL_PREFIXES):
            return []  # a string literal: its text is no name
        for prefix in _LD64_REFERENCE_PREFIXES:
            if name.startswith(prefix):
                name = name[len(prefix):]
                break
        if name.startswith("_"):
            name = name[1:]  # Mach-O's underscore before every C-level name
    if name.startswith("?"):
        msvc = _MsvcNameReader(name)
        try:
            msvc.read_symbol()
        except _ParseStop:
            pass
        return msvc.qualified_names
    if name.startswith("_Z"):
        itanium = _ItaniumNameReader(name)
        try:
            itanium.read_symbol()
        except _ParseStop:
            pass
        return itanium.qualified_names
    if any(char in name for char in " :(<`"):
        return _demangled_qualified_names(name)
    return [[name]] if _PLAIN_NAME.fullmatch(name) else []


# --------------------------------------------------------------------------------------------------------------------
# Map files
# --------------------------------------------------------------------------------------------------------------------

_MSVC_HEADER = re.compile(r"^\s*Address\s+Publics by Value\s+Rva\+Base\s+Lib:Object\s*$", re.MULTILINE)
_MSVC_SYMBOL = re.compile(r"^\s*[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(?P<name>\S+)\s+[0-9A-Fa-f]{8,16}\s+(?:[fi]\s+)*"
                          r"(?P<origin>\S.*?)\s*$")
_LLD_ELF_HEADER = re.compile(r"^\s*VMA\s+LMA\s+Size\s+Align\s+Out\s+In\s+Symbol\s*$", re.MULTILINE)
_LLD_COFF_HEADER = re.compile(r"^\s*Address\s+Size\s+Align\s+Out\s+In\s+Symbol\s*$", re.MULTILINE)
_LLD_ELF_LINE = re.compile(r"^\s*[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+\d+ (?P<rest>.*)$")
_LLD_COFF_LINE = re.compile(r"^\s*[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+\d+ (?P<rest>.*)$")
_GNU_MEMORY_MAP = re.compile(r"^Linker script and memory map\s*$", re.MULTILINE)
_GNU_INPUT = re.compile(r"^ (?P<section>\S+)\s+0x[0-9a-fA-F]+\s+0x[0-9a-fA-F]+\s+(?P<origin>\S.*?)\s*$")
_GNU_INPUT_NAME = re.compile(r"^ (?P<section>[.*]\S*)\s*$")
_GNU_INPUT_DATA = re.compile(r"^\s+0x[0-9a-fA-F]+\s+0x[0-9a-fA-F]+\s+(?P<origin>\S.*?)\s*$")
_GNU_SYMBOL = re.compile(r"^\s+0x[0-9a-fA-F]+\s+(?!0x)(?P<name>\S.*?)\s*$")
_GNU_ASSIGNMENT = re.compile(r"^(?:PROVIDE|ASSERT|\.\s)|(?<![=!<>])\s=\s")
_LD64_PATH = re.compile(r"^# Path:", re.MULTILINE)
_LD64_SYMBOLS = re.compile(r"^# Symbols:", re.MULTILINE)
_LD64_OBJECT = re.compile(r"^\[\s*(?P<index>\d+)\]\s+(?P<origin>.*?)\s*$")
_LD64_SYMBOL = re.compile(r"^0x[0-9a-fA-F]+\s+0x[0-9a-fA-F]+\s+\[\s*(?P<index>\d+)\]\s+(?P<name>.*?)\s*$")
_ARCHIVE_MEMBER = re.compile(r"^(?P<archive>.*?)(?:\[[^\]]*\])*\((?P<member>[^()]*)\)$")


def _archive_library(origin: str) -> str:
    """The archive of an "<archive>(<member>)" origin ("libImGui.a", "ImGui.lib"), as the map spells it; ""
    otherwise."""
    match = _ARCHIVE_MEMBER.match(origin)
    return match.group("archive") if match is not None and match.group("archive") else ""


def _msvc_library(origin: str) -> str:
    if origin.startswith(("<", "*")) or re.match(r"^[A-Za-z]:[\\/]", origin):
        return ""
    library, separator, _ = origin.partition(":")
    return library if separator else ""


def _parse_msvc(text: str, header: re.Match[str]) -> list[MapSymbol]:
    symbols: list[MapSymbol] = []
    for line in text[header.end():].splitlines():
        match = _MSVC_SYMBOL.match(line)
        if match is not None:
            origin = match.group("origin")
            symbols.append(MapSymbol(match.group("name"), origin, _msvc_library(origin)))
    return symbols


def _parse_lld(text: str, header: re.Match[str], line_pattern: re.Pattern[str]) -> list[MapSymbol]:
    """lld writes each output section at no indentation, each input section 8 spaces in and each symbol 16 spaces in."""
    symbols: list[MapSymbol] = []
    origin = ""
    for line in text[header.end():].splitlines():
        match = line_pattern.match(line)
        if match is None:
            continue
        rest = match.group("rest")
        indentation = len(rest) - len(rest.lstrip(" "))
        content = rest.strip()
        if indentation < 8:
            origin = ""
        elif indentation < 16:
            origin = content.split(":(", 1)[0] if ":(" in content else content
        elif content:
            symbols.append(MapSymbol(content, origin, _archive_library(origin)))
    return symbols


def _parse_gnu(text: str, header: re.Match[str]) -> list[MapSymbol]:
    symbols: list[MapSymbol] = []
    origin = ""
    discarding = False
    pending_section = False
    for line in text[header.end():].splitlines():
        if not line.strip():
            continue
        if not line[0].isspace():
            # An output section, a LOAD line or a script statement at the top level.
            discarding = line.split()[0] == "/DISCARD/"
            origin = ""
            pending_section = False
            continue
        if discarding:
            continue
        if pending_section:
            pending_section = False
            data = _GNU_INPUT_DATA.match(line)
            if data is not None:
                origin = data.group("origin")
                continue
        input_section = _GNU_INPUT.match(line)
        if input_section is not None:
            origin = input_section.group("origin")
            continue
        if _GNU_INPUT_NAME.match(line) is not None:
            pending_section = True
            origin = ""
            continue
        symbol = _GNU_SYMBOL.match(line)
        if symbol is not None and origin and _GNU_ASSIGNMENT.search(symbol.group("name")) is None:
            symbols.append(MapSymbol(symbol.group("name"), origin, _archive_library(origin)))
    return symbols


def _parse_ld64(text: str) -> list[MapSymbol]:
    objects: dict[str, str] = {}
    symbols: list[MapSymbol] = []
    section = ""
    for line in text.splitlines():
        if line.startswith("#"):
            heading = line[1:].strip()
            if heading.startswith("Object files:"):
                section = "objects"
            elif heading.startswith("Symbols:"):
                section = "symbols"
            elif heading.startswith(("Dead Stripped Symbols:", "Sections:")):
                section = ""
            continue
        if section == "objects":
            match = _LD64_OBJECT.match(line)
            if match is not None:
                objects[str(int(match.group("index")))] = match.group("origin")
        elif section == "symbols":
            match = _LD64_SYMBOL.match(line)
            if match is not None:
                origin = objects.get(str(int(match.group("index"))), "")
                symbols.append(MapSymbol(match.group("name"), origin, _archive_library(origin)))
    return symbols


def parse_map(text: str) -> MapFile:
    """Parses a map file's text, detecting its format. Raises ValueError for text that matches none of MAP_FORMATS or
    lists no symbol."""
    text = text.replace("\r\n", "\n")
    if (header := _MSVC_HEADER.search(text)) is not None:
        map_format, symbols = "msvc", _parse_msvc(text, header)
    elif (header := _LLD_ELF_HEADER.search(text)) is not None:
        map_format, symbols = "lld", _parse_lld(text, header, _LLD_ELF_LINE)
    elif (header := _LLD_COFF_HEADER.search(text)) is not None:
        map_format, symbols = "lld", _parse_lld(text, header, _LLD_COFF_LINE)
    elif (header := _GNU_MEMORY_MAP.search(text)) is not None:
        map_format, symbols = "gnu", _parse_gnu(text, header)
    elif _LD64_PATH.search(text) is not None and _LD64_SYMBOLS.search(text) is not None:
        map_format, symbols = "ld64", _parse_ld64(text)
    else:
        raise ValueError(f"not a map file of a supported linker ({', '.join(MAP_FORMATS)})")
    if not symbols:
        raise ValueError(f"the {map_format} map file lists no symbol")
    return MapFile(map_format, tuple(symbols))


def _library_file_name(library: str) -> str:
    return re.split(r"[\\/]", library)[-1]


def _literal(pattern: re.Pattern[str]) -> str | None:
    """The text a pattern matches when it is a plain escaped literal without flags that change matching, else None."""
    if pattern.flags & re.IGNORECASE:
        return None
    literal = re.sub(r"\\(.)", r"\1", pattern.pattern)
    return literal if re.escape(literal) == pattern.pattern else None


def check_map(map_file: MapFile, patterns: dict[str, FamilyPatterns]) -> list[str]:
    """The findings of a Dist map: "forbidden <family>: <symbol> (<origin>)" per matching symbol, sorted, then "missing
    required symbol: <name>" for each REQUIRED_SYMBOLS entry no symbol names."""
    families = []
    for family, family_patterns in patterns.items():
        literals = {literal for pattern in family_patterns.names if (literal := _literal(pattern)) is not None}
        expressions = [pattern for pattern in family_patterns.names if _literal(pattern) is None]
        families.append((family, literals, expressions, family_patterns.libraries))
    findings: set[str] = set()
    components: set[str] = set()
    for symbol in map_file.symbols:
        names = qualified_names(symbol.name, map_file.format)
        paths = {"::".join(name[:count]) for name in names for count in range(1, len(name) + 1)}
        for name in names:
            components.update(name)
        library = _library_file_name(symbol.library)
        for family, literals, expressions, libraries in families:
            if (not paths.isdisjoint(literals)
                    or any(expression.fullmatch(path) for expression in expressions for path in paths)
                    or (library and any(pattern.fullmatch(library) for pattern in libraries))):
                findings.add(f"forbidden {family}: {symbol.name} ({symbol.origin})")
    missing = [f"missing required symbol: {name}" for name in REQUIRED_SYMBOLS if name not in components]
    return sorted(findings) + missing
