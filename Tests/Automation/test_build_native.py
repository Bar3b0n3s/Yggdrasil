"""Scripts/BuildNative.py, the native build of a game's module (Docs/Decisions/0030-m19-contract.md decisions 30, 34 and
35; stream N-A), and Scripts/Lib/native_build.py, which it shares with the games suite.

The pure cases (the sources, the source hash the editor computes too, the record, the diagnostics of tool output, the
format check with the pinned clang-format) run everywhere. The script's cases run it as the editor does, as a process:
those that stop before compiling (usage, no source, the lint) run everywhere, and the builds compile real modules with
MSBuild on Windows (one of the build lock's capped slots each), proving that a module is compiled with the first-party
settings, that the editor's library exports the two entry functions, that a compile error or warning fails the build
located in its source, that a source that is not clang-format clean is a warning that fails nothing, and that the
Runtime of an export links the module instead of NoNativeModule.cpp. On Linux and macOS a build exits 3 naming the
platform (decision 34), which the same cases check there.
"""

from __future__ import annotations

import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

from harness import REPOSITORY_ROOT, automation_config

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import BuildNative  # noqa: E402  (Scripts/ is on sys.path above)
import Format  # noqa: E402
import Lint  # noqa: E402
from Lib import native_build, paths  # noqa: E402

SCRIPT = REPOSITORY_ROOT / "Scripts" / "BuildNative.py"
API_HEADER = REPOSITORY_ROOT / native_build.API_HEADER
BUILD_TIMEOUT_SECONDS = 3600.0
IS_WINDOWS = sys.platform == "win32"

# The files and hashes of "NativeBuild: the sources are the .cpp and .h files below Native/ and the source hash covers
# them in order" (Tests/Source/EditorCore/Native/NativeBuildTests.cpp): the editor and the script agree on them.
HASHED_FILES = {
    "Native/Spinner.cpp": '#include "Engine/Native/NativeApi.h"\n',
    "Native/Spinner.h": "#pragma once\n",
    "Native/Systems/Move.cpp": "// move\n",
    "Native/Notes.txt": "not a source\n",
    "Assets/Other.cpp": "not below Native/\n",
}
HASHED_HEADER = "// the interface\n"
PINNED_HASHES = {"Debug": "a3230069ed62f267", "Release": "6fddebbbd4d34c08"}

SPINNER_HEADER = textwrap.dedent("""\
    #pragma once

    #include "Engine/Native/NativeApi.h"

    namespace Spinners {

    \tstruct Spinner
    \t{
    \t\tstatic constexpr std::string_view NativeTypeName = "Spinner";
    \t\tfloat DegreesPerSecond = 90.0f;
    \t};

    }
    """)
SPINNER_SOURCE = textwrap.dedent("""\
    #include "Engine/Native/NativeApi.h"
    #include "Native/Spinner.h"

    namespace Spinners {

    \tstatic void Spin(Engine::INativeStepContext& context)
    \t{
    \t\tfor (const Engine::NativeEntity entity : context.Query<Spinner>())
    \t\t{
    \t\t\tconst Spinner* spinner = context.Get<Spinner>(entity);
    \t\t\tEngine::NativeTransform transform{};
    \t\t\tif (spinner == nullptr || !context.GetTransform(entity, transform))
    \t\t\t\tcontinue;
    \t\t\tconst float radians = spinner->DegreesPerSecond * 0.017453292f * static_cast<float>(context.GetFixedDelta());
    \t\t\ttransform.Rotation = context.AngleAxis(radians, glm::vec3(0.0f, 1.0f, 0.0f)) * transform.Rotation;
    \t\t\tstatic_cast<void>(context.SetTransform(entity, transform));
    \t\t}
    \t}

    \tstatic void Register(Engine::INativeRegistrar& registrar)
    \t{
    \t\tregistrar.Add(Engine::NativeComponent<Spinner>("Turns its entity about the up axis.")
    \t\t\t\t.Field("DegreesPerSecond", &Spinner::DegreesPerSecond, "How fast it turns.", { .Unit = "deg/s" }));
    \t\tregistrar.AddSystem({ .Name = "Spin", .Description = "Turns every Spinner.", .Run = &Spin });
    \t}

    }

    ENGINE_NATIVE_MODULE(Spinners::Register)
    """)
# SPINNER_SOURCE, which is clang-format clean, with one line indented a tab too far: the registration of its system.
MISINDENTED_TEXT = "\t\tregistrar.AddSystem("
MISINDENTED_SOURCE = SPINNER_SOURCE.replace(MISINDENTED_TEXT, "\t" + MISINDENTED_TEXT)
MISINDENTED_LINE = next(number for number, line in enumerate(SPINNER_SOURCE.splitlines(), 1)
                        if line.startswith(MISINDENTED_TEXT))


def outside_repository(test: unittest.TestCase) -> Path:
    """A new directory in the system's temporary directory, removed after the test: outside the repository, so no
    .clang-format lies above it."""
    directory = Path(tempfile.mkdtemp(prefix="BuildNative-")).resolve()
    test.addCleanup(shutil.rmtree, directory, True)
    test.assertFalse(directory.is_relative_to(REPOSITORY_ROOT.resolve()), directory)
    return directory


def write_files(root: Path, files: dict[str, str]) -> None:
    for relative, text in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))


def native_api_version() -> int:
    match = re.search(r"\bNativeApiVersion\s*=\s*(\d+)", API_HEADER.read_text(encoding="utf-8"))
    assert match is not None, "NativeApi.h defines NativeApiVersion"
    return int(match.group(1))


class SourceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="BuildNative-"))
        self.addCleanup(shutil.rmtree, self.directory, True)

    def test_sources_and_their_hash_are_the_ones_the_editor_computes(self) -> None:
        project = self.directory / "Game"
        write_files(project, HASHED_FILES)
        header = self.directory / "NativeApi.h"
        header.write_bytes(HASHED_HEADER.encode("utf-8"))
        self.assertEqual(native_build.list_sources(project),
                         ["Native/Spinner.cpp", "Native/Spinner.h", "Native/Systems/Move.cpp"])
        for configuration, expected in PINNED_HASHES.items():
            self.assertEqual(native_build.source_hash(project, configuration, header), expected, configuration)
        (project / "Native" / "Spinner.h").write_bytes(b"#pragma once\n// changed\n")
        self.assertNotEqual(native_build.source_hash(project, "Debug", header), PINNED_HASHES["Debug"])
        with self.assertRaises(FileNotFoundError):
            native_build.source_hash(project, "Debug", self.directory / "Missing.h")
        self.assertEqual(native_build.list_sources(self.directory / "NoModule"), [])


class FormatTests(unittest.TestCase):
    def test_native_modules_are_formatted_like_first_party_code(self) -> None:
        # Decision 35: Format.py formats the C++ below Projects/*/Native/ and Tests/Projects/*/Native/.
        directory = Path(tempfile.mkdtemp(prefix="BuildNative-"))
        self.addCleanup(shutil.rmtree, directory, True)
        write_files(directory, {"Projects/Game/Native/Spinner.cpp": "", "Tests/Projects/Game/Native/Systems/Move.h": "",
                                "Projects/Game/Assets/Notes.txt": "", "Vendor/Lib/Lib.cpp": ""})
        self.assertEqual(sorted(Format.first_party_files(directory, None)),
                         ["Projects/Game/Native/Spinner.cpp", "Tests/Projects/Game/Native/Systems/Move.h"])


class FormatCheckTests(unittest.TestCase):
    # The native test game's friction NT-02: Format.py --check fails the gate on a native source that is not
    # clang-format clean, which a builder working through automation alone could not learn before the gate. The build's
    # format check reports each such source as a warning, in the repository's style also for a project outside the
    # repository, where clang-format --style=file finds no .clang-format and would pass every source as it is.
    def setUp(self) -> None:
        self.project = outside_repository(self)
        write_files(self.project, {"Native/Spinner.h": SPINNER_HEADER, "Native/Spinner.cpp": MISINDENTED_SOURCE})
        self.sources = native_build.list_sources(self.project)

    def test_a_source_that_is_not_clang_format_clean_is_one_warning_at_its_first_differing_line(self) -> None:
        diagnostics = BuildNative.format_diagnostics(self.project, self.sources)
        self.assertEqual([(diagnostic.severity, diagnostic.code, diagnostic.file, diagnostic.line)
                          for diagnostic in diagnostics],
                         [("Warning", native_build.FORMAT_WARNING, "Native/Spinner.cpp", MISINDENTED_LINE)])
        # The warning quotes the line as clang-format writes it, which is how the source must read there.
        expected = SPINNER_SOURCE.splitlines()[MISINDENTED_LINE - 1]
        self.assertIn(json.dumps(expected, ensure_ascii=False), diagnostics[0].message)
        write_files(self.project, {"Native/Spinner.cpp": SPINNER_SOURCE})
        self.assertEqual(BuildNative.format_diagnostics(self.project, self.sources), [])

    def test_a_source_with_crlf_line_endings_is_told_so_instead_of_quoting_its_own_line(self) -> None:
        # The repository's .clang-format writes LF line endings, so every line of a CRLF source differs from
        # clang-format's by its invisible carriage return alone: quoting clang-format's line 1 would show the builder
        # the text it wrote, and rewriting the same bytes would never clear the warning.
        write_files(self.project, {"Native/Spinner.cpp": SPINNER_SOURCE.replace("\n", "\r\n")})
        diagnostics = BuildNative.format_diagnostics(self.project, self.sources)
        self.assertEqual([(diagnostic.severity, diagnostic.code, diagnostic.file, diagnostic.line)
                          for diagnostic in diagnostics],
                         [("Warning", native_build.FORMAT_WARNING, "Native/Spinner.cpp", 1)])
        self.assertIn("CRLF", diagnostics[0].message)
        self.assertNotIn(json.dumps(SPINNER_SOURCE.splitlines()[0], ensure_ascii=False), diagnostics[0].message)
        # A source whose first differing line differs in its text, and that ends a later line in CRLF, quotes that line
        # and names the line endings too, so one rewrite clears both.
        last_line = "ENGINE_NATIVE_MODULE(Spinners::Register)\n"
        self.assertTrue(MISINDENTED_SOURCE.endswith(last_line))
        crlf_last_line = MISINDENTED_SOURCE.replace(last_line, last_line[:-1] + "\r\n")
        write_files(self.project, {"Native/Spinner.cpp": crlf_last_line})
        diagnostics = BuildNative.format_diagnostics(self.project, self.sources)
        self.assertEqual([diagnostic.line for diagnostic in diagnostics], [MISINDENTED_LINE])
        self.assertIn(json.dumps(SPINNER_SOURCE.splitlines()[MISINDENTED_LINE - 1], ensure_ascii=False),
                      diagnostics[0].message)
        self.assertIn("CRLF", diagnostics[0].message)

    def test_a_differing_end_and_an_unreadable_source_are_warnings_too(self) -> None:
        # A source whose lines all match clang-format's but whose end differs (no final newline) is located at its last
        # line, and a source the check cannot read is a warning without a line: the check never stops the build.
        header = SPINNER_HEADER.rstrip("\n")
        write_files(self.project, {"Native/Spinner.h": header, "Native/Spinner.cpp": SPINNER_SOURCE})
        diagnostics = BuildNative.format_diagnostics(self.project, ["Native/Gone.cpp", *self.sources])
        self.assertEqual([(diagnostic.severity, diagnostic.code, diagnostic.file, diagnostic.line)
                          for diagnostic in diagnostics],
                         [("Warning", native_build.FORMAT_WARNING, "Native/Gone.cpp", 0),
                          ("Warning", native_build.FORMAT_WARNING, "Native/Spinner.h", len(header.splitlines()))])

    def test_without_clang_format_the_check_is_one_warning_without_a_location(self) -> None:
        missing = self.project / "Missing" / "clang-format.exe"
        with mock.patch.dict(os.environ, {"CLANG_FORMAT": str(missing)}):
            diagnostics = BuildNative.format_diagnostics(self.project, self.sources)
        self.assertEqual([(diagnostic.severity, diagnostic.code, diagnostic.file, diagnostic.line)
                          for diagnostic in diagnostics], [("Warning", native_build.FORMAT_WARNING, "", 0)])


class LintTests(unittest.TestCase):
    def test_the_editors_backups_beside_the_sources_are_not_module_files(self) -> None:
        # Decision 35's file rule and the editor's own backups: every project write keeps the previous bytes as
        # "<file>.bak" (NativeDirectoryMount), so native.write leaves one beside each source it overwrites. No build
        # reads it and the project's .gitignore leaves it out, so the lint of a build does not refuse it, while any
        # other file below Native/ still is a finding.
        directory = Path(tempfile.mkdtemp(prefix="BuildNative-"))
        self.addCleanup(shutil.rmtree, directory, True)
        write_files(directory, {"Native/Spinner.cpp": '#include "Engine/Native/NativeApi.h"\n',
                                "Native/Spinner.cpp.bak": "// the source before native.write replaced it\n",
                                "Native/Notes.txt": "a note\n"})
        findings = Lint.lint_native_module(directory)
        self.assertEqual(sorted({(finding.file, finding.code) for finding in findings}),
                         [("Native/Notes.txt", "native-file")])


class RecordTests(unittest.TestCase):
    def record(self) -> native_build.BuildRecord:
        return native_build.BuildRecord(
            "Module", "Debug", "0123456789abcdef", ["Native/Spinner.cpp", "Native/Spinner.h"], False, "",
            native_build.LOG_FILE,
            [native_build.Diagnostic("Error", native_build.COMPILE_ERROR, "Native/Spinner.cpp", 3, 5, "an error")])

    def test_the_record_round_trips_and_is_read_strictly(self) -> None:
        record = self.record()
        text = native_build.record_text(record)
        self.assertEqual(native_build.parse_record(text), record)
        self.assertTrue(text.startswith('{\n\t"Format": "NativeBuild",\n\t"Version": 1,\n'))
        self.assertIn('\t"Sources": ["Native/Spinner.cpp", "Native/Spinner.h"],\n', text)
        document = json.loads(text)
        for change in ({"Logs": "Build.log"}, {"Version": 2}, {"SourceHash": "0123456789ABCDEF"}, {"Kind": "Library"},
                       {"Succeeded": True}, {"Log": "../Build.log"}):
            changed = dict(document)
            changed.update(change)
            with self.assertRaises(native_build.RecordError, msg=str(change)):
                native_build.parse_record(json.dumps(changed))
        with self.assertRaises(native_build.RecordError):
            native_build.parse_record("{")

    def test_tool_output_becomes_located_diagnostics(self) -> None:
        project = Path(tempfile.gettempdir()).resolve() / "Game"
        source = project / "Native" / "Spinner.cpp"
        compile_error = native_build.parse_tool_diagnostic(
            f"  3>{source}(12,9): error C2065: 'speed': undeclared identifier [{project / 'Workspace' / 'X.vcxproj'}]",
            project, REPOSITORY_ROOT)
        self.assertEqual(compile_error, native_build.Diagnostic(
            "Error", native_build.COMPILE_ERROR, "Native/Spinner.cpp", 12, 9, "C2065: 'speed': undeclared identifier"))
        warning = native_build.parse_tool_diagnostic(
            f"{source}(4): warning C4189: 'unused': local variable is initialized but not referenced", project,
            REPOSITORY_ROOT)
        assert warning is not None
        self.assertEqual((warning.code, warning.file, warning.line, warning.column),
                         (native_build.COMPILE_WARNING, "Native/Spinner.cpp", 4, 0))
        api = native_build.parse_tool_diagnostic(f"{API_HEADER}(40,1): error C2059: syntax error", project,
                                                 REPOSITORY_ROOT)
        assert api is not None
        self.assertEqual(api.file, native_build.API_HEADER_HASH_NAME)
        link = native_build.parse_tool_diagnostic(
            "Spinner.obj : error LNK2019: unresolved external symbol GetLinkedNativeModule", project, REPOSITORY_ROOT)
        assert link is not None
        self.assertEqual((link.code, link.file, link.line), (native_build.LINK_ERROR, "", 0))
        self.assertIn("Spinner.obj", link.message)
        other = native_build.parse_tool_diagnostic("C:/VS/Microsoft.Cpp.targets(10,5): error MSB3073: exited", project,
                                                   REPOSITORY_ROOT)
        assert other is not None
        self.assertEqual((other.code, other.file), (native_build.BUILD_ERROR, ""))
        self.assertIn("Microsoft.Cpp.targets(10)", other.message)
        self.assertEqual(other.severity, "Error")
        temporary = native_build.parse_tool_diagnostic(
            "C:/VS/Microsoft.CppBuild.targets(561,5): warning MSB8029: The Intermediate directory or Output directory "
            "cannot reside under the Temporary directory", project, REPOSITORY_ROOT)
        assert temporary is not None
        self.assertEqual((temporary.code, temporary.severity), (native_build.BUILD_ERROR, "Warning"))
        self.assertIsNone(native_build.parse_tool_diagnostic("Build succeeded.", project, REPOSITORY_ROOT))


class ScriptTests(unittest.TestCase):
    def setUp(self) -> None:
        # Below bin-int, not the system's temporary directory: MSBuild warns about outputs there (MSB8029), which the
        # record would carry as a warning.
        paths.BIN_INT_ROOT.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix="BuildNative-", dir=paths.BIN_INT_ROOT))
        self.addCleanup(shutil.rmtree, self.directory, True)
        self.project = self.directory / "Game"
        (self.project / native_build.NATIVE_DIRECTORY).mkdir(parents=True)

    def build(self, kind: str, config: str, name: str) -> tuple[int, native_build.BuildRecord | None, Path, str]:
        """Runs the script as the editor does; (exit code, the record or None, the output directory, its output)."""
        out = self.directory / name
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--project", str(self.project), "--kind", kind, "--config", config, "--out",
             str(out), "--json"], capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=BUILD_TIMEOUT_SECONDS, check=False)
        record = native_build.read_record(out) if (out / native_build.RECORD_FILE).is_file() else None
        if record is not None:
            self.assertEqual(native_build.parse_record(result.stdout), record, "--json prints the record")
        return result.returncode, record, out, result.stdout + result.stderr

    def assert_not_built_here(self, code: int, record: native_build.BuildRecord | None, output: str) -> None:
        """Linux and macOS (decision 34): exit code 3 naming the platform, and a record that says so."""
        self.assertEqual(code, 3, output)
        assert record is not None
        self.assertFalse(record.succeeded)
        self.assertEqual([diagnostic.code for diagnostic in record.diagnostics], [native_build.BUILD_ERROR])
        self.assertIn("Windows only", record.diagnostics[0].message)

    def test_usage_errors_exit_2_and_write_no_record(self) -> None:
        code, record, out, output = self.build("module", "Dist", "Dist")
        self.assertEqual(code, 2, output)
        self.assertIsNone(record)
        occupied = self.directory / "Occupied"
        occupied.mkdir()
        (occupied / "Other.txt").write_text("kept\n", encoding="utf-8")
        code, record, _, output = self.build("module", "Debug", "Occupied")
        self.assertEqual(code, 2, output)
        self.assertEqual(sorted(path.name for path in occupied.iterdir()), ["Other.txt"])

    def test_a_module_without_sources_exits_3_with_its_reason_in_the_record(self) -> None:
        code, record, out, output = self.build("module", "Debug", "Empty")
        self.assertEqual(code, 3, output)
        assert record is not None
        self.assertFalse(record.succeeded)
        self.assertEqual(record.output, "")
        self.assertEqual(record.sources, [])
        self.assertEqual(record.source_hash, native_build.source_hash(self.project, "Debug", API_HEADER))
        self.assertEqual([diagnostic.code for diagnostic in record.diagnostics], [native_build.BUILD_ERROR])
        self.assertTrue((out / record.log).is_file())

    def test_lint_findings_fail_the_build_before_anything_compiles(self) -> None:
        # The format check runs whatever the lint found (the native test game's friction NT-02), so a source that is not
        # clang-format clean is reported beside the lint's findings, not one build later.
        write_files(self.project, {
            "Native/Counter.cpp": '#include "Engine/Native/NativeApi.h"\n#include <windows.h>\n\nnamespace Game {\n\n'
                                  "\tstatic int s_Count = 0;\n\n}\n",
            "Native/Readme.md": "a note\n",
            "Native/Spinner.h": SPINNER_HEADER,
            "Native/Spinner.cpp": MISINDENTED_SOURCE,
        })
        code, record, out, output = self.build("module", "Debug", "Lint")
        assert record is not None
        if not IS_WINDOWS:
            self.assert_not_built_here(code, record, output)
            return
        self.assertEqual(code, 1, output)
        self.assertFalse(record.succeeded)
        found = sorted((diagnostic.code, diagnostic.file, diagnostic.line, diagnostic.message.split(":", 1)[0])
                       for diagnostic in record.diagnostics if diagnostic.code == native_build.LINT_ERROR)
        self.assertEqual(found, [(native_build.LINT_ERROR, "Native/Counter.cpp", 2, "native-include"),
                                 (native_build.LINT_ERROR, "Native/Counter.cpp", 6, "native-mutable-state"),
                                 (native_build.LINT_ERROR, "Native/Readme.md", 1, "native-file")])
        self.assertEqual([(diagnostic.severity, diagnostic.code, diagnostic.file, diagnostic.line)
                          for diagnostic in record.diagnostics if diagnostic.code != native_build.LINT_ERROR],
                         [("Warning", native_build.FORMAT_WARNING, "Native/Spinner.cpp", MISINDENTED_LINE)])
        self.assertFalse((out / native_build.WORKSPACE_DIRECTORY).exists(), "nothing was generated or compiled")

    def test_the_editors_library_is_compiled_like_engine_code_and_exports_the_entry_functions(self) -> None:
        write_files(self.project, {"Native/Spinner.h": SPINNER_HEADER, "Native/Spinner.cpp": SPINNER_SOURCE})
        code, record, out, output = self.build("module", "Debug", "Module")
        assert record is not None
        if not IS_WINDOWS:
            self.assert_not_built_here(code, record, output)
            return
        self.assertEqual(code, 0, output)
        self.assertTrue(record.succeeded, record.diagnostics)
        self.assertEqual(record.output, native_build.MODULE_LIBRARY)
        self.assertEqual(record.sources, ["Native/Spinner.cpp", "Native/Spinner.h"])
        self.assertEqual(record.source_hash, native_build.source_hash(self.project, "Debug", API_HEADER))
        self.assertEqual(record.diagnostics, [])

        # The first-party settings (Dependencies.lua): warnings as errors, precise floating point, the engine's
        # instruction set, the shared-library define, and no library linked.
        project = (out / native_build.WORKSPACE_DIRECTORY / "NativeModule.vcxproj").read_text(encoding="utf-8")
        self.assertIn("<TreatWarningAsError>true</TreatWarningAsError>", project)
        self.assertNotIn("<FloatingPointModel>Fast", project)
        self.assertIn("/arch:SSE4.2", project)
        self.assertIn("ENGINE_NATIVE_SHARED_LIBRARY", project)
        self.assertNotIn("Engine.lib", project)
        self.assertNotIn("JoltPhysics.lib", project)

        library = ctypes.CDLL(str(out / record.output))
        try:
            version = library.EngineNativeModuleApiVersion
            version.restype = ctypes.c_uint32
            self.assertEqual(version(), native_api_version())
            self.assertIsNotNone(library.EngineNativeModuleRegister)
        finally:
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(library._handle))  # type: ignore[attr-defined]

    def test_compile_errors_and_warnings_fail_the_build_located_in_their_sources(self) -> None:
        write_files(self.project, {
            "Native/Broken.cpp": "namespace Game {\n\n\tint Broken()\n\t{\n\t\treturn Undeclared;\n\t}\n\n}\n",
            "Native/Unused.cpp": "namespace Game {\n\n\tint Unused()\n\t{\n\t\tconst int unused = 1;\n"
                                 "\t\treturn 0;\n\t}\n\n}\n",
        })
        code, record, _, output = self.build("module", "Debug", "Broken")
        assert record is not None
        if not IS_WINDOWS:
            self.assert_not_built_here(code, record, output)
            return
        self.assertEqual(code, 1, output)
        self.assertFalse(record.succeeded)
        self.assertEqual(record.output, "")
        located = {(diagnostic.code, diagnostic.file, diagnostic.line) for diagnostic in record.diagnostics}
        self.assertIn((native_build.COMPILE_ERROR, "Native/Broken.cpp", 5), located)
        self.assertIn((native_build.COMPILE_WARNING, "Native/Unused.cpp", 5), located)

    def test_a_source_that_is_not_clang_format_clean_builds_with_one_format_warning(self) -> None:
        # The friction NT-02 (FormatCheckTests): the record carries the format check's warning, located in the source of
        # a project outside the repository, and the build still succeeds. Its output stays below bin-int (setUp), so
        # MSBuild adds no warning of its own.
        self.project = outside_repository(self) / "Game"
        write_files(self.project, {"Native/Spinner.h": SPINNER_HEADER, "Native/Spinner.cpp": MISINDENTED_SOURCE})
        code, record, out, output = self.build("module", "Debug", "Unformatted")
        assert record is not None
        if not IS_WINDOWS:
            self.assert_not_built_here(code, record, output)
            return
        self.assertEqual(code, 0, output)
        self.assertTrue(record.succeeded, record.diagnostics)
        self.assertTrue((out / record.output).is_file())
        self.assertEqual([(diagnostic.severity, diagnostic.code, diagnostic.file, diagnostic.line)
                          for diagnostic in record.diagnostics],
                         [("Warning", native_build.FORMAT_WARNING, "Native/Spinner.cpp", MISINDENTED_LINE)])

    def test_an_exports_runtime_links_the_module_instead_of_no_native_module(self) -> None:
        write_files(self.project, {"Native/Spinner.h": SPINNER_HEADER, "Native/Spinner.cpp": SPINNER_SOURCE})
        config = automation_config()
        code, record, out, output = self.build("runtime", config, "Runtime")
        assert record is not None
        if not IS_WINDOWS:
            self.assert_not_built_here(code, record, output)
            return
        self.assertEqual(code, 0, output)
        self.assertTrue(record.succeeded, record.diagnostics)
        self.assertEqual(record.output, native_build.RUNTIME_EXECUTABLE)
        self.assertTrue((out / record.output).is_file())
        # The linker map names the object that defines GetLinkedNativeModule: the module's, never NoNativeModule.obj.
        symbols = (out / "Runtime.map").read_text(encoding="utf-8", errors="replace").splitlines()
        defining = [line for line in symbols if "?GetLinkedNativeModule@Engine@@" in line]
        self.assertTrue(defining, "the Runtime links GetLinkedNativeModule")
        self.assertTrue(all(line.rstrip().endswith("Spinner.obj") for line in defining), defining)
        self.assertFalse(any("NoNativeModule" in line for line in symbols))
        self.assertTrue(any("RuntimeApp.obj" in line for line in symbols), "the Runtime's own code is linked")
        self.assertEqual(paths.output_directory_name(config).split("-", 1)[0], config)


if __name__ == "__main__":
    unittest.main()
