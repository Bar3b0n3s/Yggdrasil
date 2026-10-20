"""The shared prebuilt vendor-library cache (Scripts/Lib/vendor_cache.py; Docs/Architecture.md §2.2, §2.3;
Docs/Decisions/0028-backlog-program.md decision 16, T-VCACHE) and CheckBuildConfig.py's rule 9.

Most cases run on a small repository in a temporary directory (git init, a vendored library under Vendor/ and project
files with the shape premake writes), so they need git but no generated workspace, compiler or MSBuild. One case
generates the real workspace with the pinned premake into temporary directories, to prove that the keys of the real
vendored projects do not depend on where a workspace is generated, and that the AddressSanitizer and clang-cl
workspaces get their own entries, and one runs CheckBuildConfig.py's rule 9 against the entries Build.py publishes for
the workspace Generate.py writes. That MSBuild links the cached libraries is proven by Build.py's own builds (the
build-and-test skill's end-to-end check).
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from typing import Any
from unittest import mock

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import Build  # noqa: E402  (Scripts/ is on sys.path above)
import CheckBuildConfig  # noqa: E402
import Generate  # noqa: E402
from Lib import paths, premake, vendor_cache, workspace  # noqa: E402

TOOLCHAIN = vendor_cache.toolchain_identity("18.10.3", "14.51.36231", "10.0.26100.0", "absent")
# The Visual Studio and Windows SDK the tests that run Build.py's or CheckBuildConfig.py's key computation stand in.
VISUAL_STUDIO = Build.toolchain.VisualStudio(Path("VS"), "18.10.3", "Visual Studio", Path("MSBuild.exe"), "14.51.36231")
WINDOWS_SDK = ("10.0.26100.0", Path("."))
# Build.run_msbuild itself, which BuildVendorCacheTests replaces with a stand-in in setUp.
RUN_MSBUILD = Build.run_msbuild
NAMESPACE = {"msb": vendor_cache.MSBUILD_NAMESPACE}
JOLT_DEFINES = "JPH_OBJECT_STREAM;JPH_CROSS_PLATFORM_DETERMINISTIC;JPH_USE_SSE4_1;JPH_USE_SSE4_2"
VENDOR_RECORD = textwrap.dedent("""\
    # Fake

    | Field | Value |
    |---|---|
    | Version | 1.2.3 (tag `v1.2.3`) |
    | Commit | `0123456789abcdef0123456789abcdef01234567` (2026-01-01) |
    """)


@dataclasses.dataclass
class ProjectShape:
    """The settings of one project file written like premake's: `configs` (the configurations the file defines, `config`
    alone by default), every one with these settings, for the x64 platform; keys are computed for `config`."""

    name: str
    kind: str = "StaticLibrary"
    sources: tuple[str, ...] = ()  # repository-relative
    include_directories: tuple[str, ...] = ()  # repository-relative, as AdditionalIncludeDirectories
    external_include_directories: tuple[str, ...] = ()  # repository-relative, as ExternalIncludePath
    defines: str = JOLT_DEFINES
    options: str = "/arch:SSE4.2"
    floating_point_model: str | None = None
    asan: bool = False
    toolset: str = "v145"
    config: str = "Debug"
    configs: tuple[str, ...] = ()
    references: tuple[str, ...] = ()  # project names, written as sibling project files
    file_options: dict[str, str] = dataclasses.field(default_factory=dict)  # source -> AdditionalOptions
    output_root: str = "bin"
    # Further ClCompile settings (name, ;-separated paths: repository-relative, absolute, or $(...) and %(...) as
    # written) and further items (type, repository-relative path).
    compile_settings: tuple[tuple[str, str], ...] = ()
    other_items: tuple[tuple[str, str], ...] = ()


def project_text(shape: ProjectShape, location: Path, repository: Path, relative: bool) -> str:
    """The project file of `shape` written into `location`, its paths relative to it (premake in place) or absolute
    (premake --to on another drive)."""

    def path(value: str) -> str:
        if value.startswith(("$(", "%(")):
            return value
        absolute = repository / value
        return os.path.relpath(absolute, location) if relative else str(absolute)

    def paths_of(value: str) -> str:
        return ";".join(path(entry) for entry in value.split(";"))

    configs = shape.configs or (shape.config,)
    conditions = [f"'$(Configuration)|$(Platform)'=='{config}|x64'" for config in configs]
    asan = "\n    <EnableASAN>true</EnableASAN>" if shape.asan else ""
    external = (f"\n    <ExternalIncludePath>{';'.join(path(d) for d in shape.external_include_directories)};"
                f"$(ExternalIncludePath)</ExternalIncludePath>" if shape.external_include_directories else "")
    floating_point = (f"\n      <FloatingPointModel>{shape.floating_point_model}</FloatingPointModel>"
                      if shape.floating_point_model else "")
    includes = "".join(f"{path(d)};" for d in shape.include_directories)
    settings = "".join(f"\n      <{name}>{paths_of(value)}</{name}>" for name, value in shape.compile_settings)
    extension = ".lib" if shape.kind == "StaticLibrary" else ".exe"
    text = textwrap.dedent("""\
        <?xml version="1.0" encoding="utf-8"?>
        <Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
          <ItemGroup Label="ProjectConfigurations">
        """)
    text += "".join(f'    <ProjectConfiguration Include="{config}|x64">\n'
                    f'      <Configuration>{config}</Configuration>\n'
                    f'      <Platform>x64</Platform>\n    </ProjectConfiguration>\n' for config in configs)
    text += textwrap.dedent("""\
          </ItemGroup>
          <PropertyGroup Label="Globals">
            <Keyword>Win32Proj</Keyword>
            <PreferredToolArchitecture>x64</PreferredToolArchitecture>
            <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
          </PropertyGroup>
        """)
    for config, condition in zip(configs, conditions):
        text += textwrap.indent(textwrap.dedent(f"""\
            <PropertyGroup Condition="{condition}" Label="Configuration">
              <ConfigurationType>{shape.kind}</ConfigurationType>
              <PlatformToolset>{shape.toolset}</PlatformToolset>{asan}
            </PropertyGroup>
            <PropertyGroup Condition="{condition}">
              <OutDir>$(ProjectDir){shape.output_root}\\{config}\\{shape.name}\\</OutDir>
              <IntDir>$(ProjectDir){shape.output_root}-int\\{config}\\{shape.name}\\</IntDir>
              <TargetName>{shape.name}</TargetName>
              <TargetExt>{extension}</TargetExt>{external}
            </PropertyGroup>
            <ItemDefinitionGroup Condition="{condition}">
              <ClCompile>
                <PreprocessorDefinitions>{shape.defines};%(PreprocessorDefinitions)</PreprocessorDefinitions>
                <AdditionalIncludeDirectories>{includes}%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
                <AdditionalOptions>{shape.options} %(AdditionalOptions)</AdditionalOptions>{floating_point}{settings}
              </ClCompile>
            </ItemDefinitionGroup>
            """), "  ")
    text += "  <ItemGroup>\n"
    for source in shape.sources:
        option = shape.file_options.get(source)
        if option:
            text += f'    <ClCompile Include="{path(source)}">\n'
            text += "".join(f'      <AdditionalOptions Condition="{condition}">{option} %(AdditionalOptions)'
                            f'</AdditionalOptions>\n' for condition in conditions)
            text += "    </ClCompile>\n"
        else:
            text += f'    <ClCompile Include="{path(source)}" />\n'
    text += "".join(f'    <{kind} Include="{path(item)}" />\n' for kind, item in shape.other_items)
    text += "  </ItemGroup>\n  <ItemGroup>\n"
    text += "".join(f'    <ProjectReference Include="{reference}.vcxproj" />\n' for reference in shape.references)
    return text + '  </ItemGroup>\n  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />\n</Project>\n'


JOLT = ProjectShape("JoltPhysics", sources=("Vendor/JoltPhysics/Jolt/Core.cpp", "Vendor/JoltPhysics/Jolt/Body.cpp"),
                    include_directories=("Vendor/JoltPhysics",))
CONSUMER = ProjectShape("Tests", kind="Application", sources=("Tests/Main.cpp",),
                        external_include_directories=("Vendor/JoltPhysics",), references=("JoltPhysics",))


class FakeRepository:
    """A git work tree in a temporary directory with Vendor/JoltPhysics (VENDOR.md, premake5.lua, two sources) and
    Vendor/Other (a header directory only), and .gitignore ignoring generated project files, as the repository does."""

    def __init__(self, test: unittest.TestCase) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="EngineVendorCache")
        test.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        subprocess.run(["git", "init", "-q", str(self.root)], check=True, capture_output=True)
        files = {
            ".gitignore": "*.vcxproj\n/bin/\n",
            "Vendor/JoltPhysics/VENDOR.md": VENDOR_RECORD,
            "Vendor/JoltPhysics/premake5.lua": 'project "JoltPhysics"\n\tkind "StaticLibrary"\n',
            "Vendor/JoltPhysics/Jolt/Core.cpp": "int Core() { return 1; }\n",
            "Vendor/JoltPhysics/Jolt/Body.cpp": "int Body() { return 2; }\n",
            "Vendor/JoltPhysics/Jolt/Jolt.h": "#pragma once\n",
            "Vendor/Other/other.h": "#pragma once\n",
            "Tests/Main.cpp": "int main() { return 0; }\n",
        }
        for name, text in files.items():
            self.write(name, text)
        self.cache = vendor_cache.VendorCache(self.root / "cache")

    def write(self, name: str, text: str) -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
        return path

    def project(self, shape: ProjectShape, location: str = "Vendor/JoltPhysics", relative: bool = True) -> Path:
        directory = self.root / location
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / f"{shape.name}.vcxproj"
        path.write_text(project_text(shape, directory, self.root, relative), encoding="utf-8")
        return path

    def inputs(self, shape: ProjectShape = JOLT, toolchain: dict[str, str] | None = None,
               location: str = "Vendor/JoltPhysics", relative: bool = True) -> dict[str, Any]:
        project = vendor_cache.read_project_file(self.project(shape, location, relative))
        return vendor_cache.project_inputs(project, shape.config, "x64", toolchain or TOOLCHAIN,
                                           vendor_cache.SourceDigester(self.root))

    def key(self, shape: ProjectShape = JOLT, **arguments: Any) -> str:
        return vendor_cache.entry_key(self.inputs(shape, **arguments))

    def stage(self, files: dict[str, bytes]) -> Path:
        staged = Path(tempfile.mkdtemp(prefix="staged-", dir=self.root))
        for name, data in files.items():
            (staged / name).write_bytes(data)
        return staged

    def publish(self, inputs: dict[str, Any], warnings: tuple[str, ...] = ()) -> Path:
        staged = self.stage({"JoltPhysics.lib": b"library " * 64, "JoltPhysics.pdb": b"symbols " * 32})
        return self.cache.publish(staged, inputs, "JoltPhysics.lib", warnings, checkout=self.root)


class VendorCacheKeyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repository = FakeRepository(self)

    def test_equal_inputs_give_equal_keys(self) -> None:
        first = self.repository.inputs()
        second = self.repository.inputs()
        self.assertEqual(first, second)
        self.assertEqual(vendor_cache.entry_key(first), vendor_cache.entry_key(second))
        self.assertEqual(len(vendor_cache.entry_key(first)), vendor_cache.KEY_LENGTH)

    def test_the_key_does_not_depend_on_the_workspace_location_or_output_directories(self) -> None:
        in_place = self.repository.key()
        elsewhere = self.repository.key(location="bin-int/Asan", relative=True)
        absolute = self.repository.key(dataclasses.replace(JOLT, output_root="other"), location="generated",
                                       relative=False)
        self.assertEqual(in_place, elsewhere)
        self.assertEqual(in_place, absolute)

    def test_changing_any_keyed_input_changes_the_key(self) -> None:
        base = self.repository.key()
        settings = {
            "a define": dataclasses.replace(JOLT, defines=JOLT_DEFINES + ";JPH_DOUBLE_PRECISION"),
            "the floating-point model": dataclasses.replace(JOLT, floating_point_model="Fast"),
            "the instruction set": dataclasses.replace(JOLT, options="/arch:AVX2"),
            "the sanitizer": dataclasses.replace(JOLT, asan=True),
            "the toolset": dataclasses.replace(JOLT, toolset="ClangCL"),
            "the configuration": dataclasses.replace(JOLT, config="Release"),
            "a per-file option": dataclasses.replace(JOLT, file_options={"Vendor/JoltPhysics/Jolt/Core.cpp": "/Od"}),
            "the compiled files": dataclasses.replace(JOLT, sources=JOLT.sources[:1]),
            "an include directory": dataclasses.replace(JOLT, include_directories=("Vendor/JoltPhysics",
                                                                                  "Vendor/Other")),
        }
        for name, shape in settings.items():
            with self.subTest(name):
                self.assertNotEqual(self.repository.key(shape), base)
        for field in TOOLCHAIN:
            with self.subTest(f"the toolchain's {field}"):
                self.assertNotEqual(self.repository.key(toolchain={**TOOLCHAIN, field: "0.0"}), base)

    def test_changing_any_pinned_source_changes_the_key(self) -> None:
        base = self.repository.key()
        changes = {
            "a source": ("Vendor/JoltPhysics/Jolt/Core.cpp", "int Core() { return 3; }\n"),
            "a header": ("Vendor/JoltPhysics/Jolt/Jolt.h", "#pragma once\n#define CHANGED\n"),
            "the pin in VENDOR.md": ("Vendor/JoltPhysics/VENDOR.md", VENDOR_RECORD.replace("0123456789", "9876543210")),
            "premake5.lua": ("Vendor/JoltPhysics/premake5.lua", 'project "JoltPhysics"\n\tkind "SharedLib"\n'),
            "a new file": ("Vendor/JoltPhysics/Jolt/New.h", "#pragma once\n"),
        }
        for name, (file, text) in changes.items():
            with self.subTest(name):
                original = (self.repository.root / file).read_text(encoding="utf-8") \
                    if (self.repository.root / file).exists() else None
                self.repository.write(file, text)
                try:
                    self.assertNotEqual(self.repository.key(), base)
                finally:
                    if original is None:
                        (self.repository.root / file).unlink()
                    else:
                        self.repository.write(file, original)
                self.assertEqual(self.repository.key(), base)

    def test_an_included_vendor_directory_is_pinned_too(self) -> None:
        shape = dataclasses.replace(JOLT, include_directories=("Vendor/JoltPhysics", "Vendor/Other"))
        base = self.repository.inputs(shape)
        self.assertEqual(base["sources"]["directories"], ["Vendor/JoltPhysics", "Vendor/Other"])
        self.repository.write("Vendor/Other/other.h", "#pragma once\n#define OTHER 2\n")
        self.assertNotEqual(vendor_cache.entry_key(self.repository.inputs(shape)), vendor_cache.entry_key(base))

    def test_the_per_user_property_sheet_is_part_of_the_toolchain(self) -> None:
        local = Path(tempfile.mkdtemp(prefix="LocalAppData-"))
        self.addCleanup(shutil.rmtree, local, ignore_errors=True)
        environment = {"LOCALAPPDATA": str(local)}
        self.assertEqual(vendor_cache.user_properties_digest("x64", environment), "absent")
        sheet = local / vendor_cache.USER_PROPERTIES_DIRECTORY / "Microsoft.Cpp.x64.user.props"
        sheet.parent.mkdir(parents=True)
        sheet.write_text("<Project />\n", encoding="utf-8")
        first = vendor_cache.user_properties_digest("x64", environment)
        sheet.write_text("<Project><ItemDefinitionGroup /></Project>\n", encoding="utf-8")
        self.assertNotIn(vendor_cache.user_properties_digest("x64", environment), ("absent", first))
        self.assertNotEqual(self.repository.key(toolchain={**TOOLCHAIN, "userProperties": first}),
                            self.repository.key())

    def test_ignored_files_do_not_change_the_key(self) -> None:
        base = self.repository.key()
        self.repository.write("Vendor/JoltPhysics/Generated.vcxproj", "<Project />\n")
        self.assertEqual(self.repository.key(), base)

    def test_the_record_names_the_pinned_version_and_commit(self) -> None:
        pins = self.repository.inputs()["sources"]["pins"]
        self.assertEqual(pins["Vendor/JoltPhysics"]["commit"], "0123456789abcdef0123456789abcdef01234567")
        self.assertIn("1.2.3", pins["Vendor/JoltPhysics"]["version"])

    def test_only_static_libraries_compiling_vendored_sources_are_cached(self) -> None:
        def is_vendor(shape: ProjectShape) -> bool:
            project = vendor_cache.read_project_file(self.repository.project(shape))
            return vendor_cache.is_vendor_library(vendor_cache.project_settings(project, shape.config, "x64",
                                                                                self.repository.root))

        self.assertTrue(is_vendor(JOLT))
        self.assertFalse(is_vendor(CONSUMER))
        self.assertFalse(is_vendor(dataclasses.replace(JOLT, sources=JOLT.sources + ("Tests/Main.cpp",))))
        self.assertFalse(is_vendor(dataclasses.replace(JOLT, sources=())))


class VendorCacheUnkeyedInputTests(unittest.TestCase):
    """Fail-closed (the review's key finding): a vendored project that names an input whose content the key cannot
    digest is never cached, so an edit of that input can never leave a stale entry linked."""

    def setUp(self) -> None:
        self.repository = FakeRepository(self)
        self.repository.write("Engine/Config/config.h", "#pragma once\n")
        self.outside = Path(tempfile.gettempdir()).resolve()

    def test_inputs_the_key_cannot_digest_keep_a_project_out_of_the_cache(self) -> None:
        cases = {
            "an include directory elsewhere in the repository": (
                dataclasses.replace(JOLT, include_directories=("Vendor/JoltPhysics", "Engine/Config")),
                "AdditionalIncludeDirectories: Engine/Config"),
            "an SDK include directory built from a property": (
                dataclasses.replace(JOLT, include_directories=("Vendor/JoltPhysics", "$(VULKAN_SDK)/Include")),
                "AdditionalIncludeDirectories: $(VULKAN_SDK)/Include"),
            "an include directory outside the repository": (
                dataclasses.replace(JOLT, include_directories=("Vendor/JoltPhysics", str(self.outside))),
                f"AdditionalIncludeDirectories: {self.outside.as_posix()}"),
            "a forced include outside Vendor/": (
                dataclasses.replace(JOLT, compile_settings=(("ForcedIncludeFiles", "Engine/Config/config.h"),)),
                "ForcedIncludeFiles: Engine/Config/config.h"),
            "a #using directory outside Vendor/": (
                dataclasses.replace(JOLT, compile_settings=(("AdditionalUsingDirectories",
                                                             "Engine/Config;%(AdditionalUsingDirectories)"),)),
                "AdditionalUsingDirectories: Engine/Config"),
            "an item outside Vendor/": (
                dataclasses.replace(JOLT, other_items=(("ClInclude", "Engine/Config/config.h"),)),
                "ClInclude: Engine/Config/config.h"),
        }
        for name, (shape, named) in cases.items():
            with self.subTest(name), self.assertRaises(vendor_cache.UnkeyedInputError) as raised:
                self.repository.inputs(shape)
            self.assertIn(named, str(raised.exception))

    def test_a_forced_include_under_vendor_is_digested(self) -> None:
        shape = dataclasses.replace(JOLT, compile_settings=(("ForcedIncludeFiles", "Vendor/Other/other.h"),))
        base = self.repository.inputs(shape)
        self.assertEqual(base["sources"]["directories"], ["Vendor/JoltPhysics", "Vendor/Other"])
        self.assertEqual(base["settings"]["tools"]["ClCompile"]["ForcedIncludeFiles"], "Vendor/Other/other.h")
        self.repository.write("Vendor/Other/other.h", "#pragma once\n#define OTHER 3\n")
        self.assertNotEqual(vendor_cache.entry_key(self.repository.inputs(shape)), vendor_cache.entry_key(base))

    def test_a_list_that_only_inherits_its_earlier_value_is_keyed(self) -> None:
        # %(AdditionalIncludeDirectories) and $(ExternalIncludePath) of the setting itself, as premake writes them.
        shape = dataclasses.replace(JOLT, external_include_directories=("Vendor/Other",))
        self.assertEqual(self.repository.inputs(shape)["sources"]["directories"],
                         ["Vendor/JoltPhysics", "Vendor/Other"])

    def test_a_project_the_key_cannot_digest_compiles_in_the_workspace(self) -> None:
        unkeyed = self.repository.project(dataclasses.replace(JOLT, include_directories=("Vendor/JoltPhysics",
                                                                                         "Engine/Config")))
        keyed = self.repository.project(dataclasses.replace(JOLT, name="Other"))
        compiler = FakeCompiler()
        plan = vendor_cache.prepare(self.repository.cache, {"JoltPhysics": unkeyed, "Other": keyed}, "Debug", "x64",
                                    TOOLCHAIN, self.repository.root / "Workspace.slnx", compiler, 60.0, lambda _: None,
                                    self.repository.root)
        self.assertEqual(compiler.calls, [["Other"]])
        self.assertEqual([entry.project for entry in plan.linked], ["Other"])
        self.assertEqual(plan.compiled_in_workspace, ["JoltPhysics"])
        self.assertTrue(any("Engine/Config" in note for note in plan.notes), plan.notes)


class VendorCachePublishTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repository = FakeRepository(self)
        self.inputs = self.repository.inputs()

    def test_publishing_renames_a_complete_entry_into_place(self) -> None:
        staged = self.repository.stage({"JoltPhysics.lib": b"library", "JoltPhysics.pdb": b"symbols"})
        target = self.repository.cache.entry_directory("JoltPhysics", "Debug", vendor_cache.entry_key(self.inputs))
        renames: list[tuple[Path, Path]] = []
        real_rename = os.rename

        def checked_rename(source: Any, destination: Any) -> None:
            # At the moment of the rename the entry does not exist yet and the staged directory is complete.
            self.assertFalse(Path(destination).exists())
            self.assertEqual(sorted(child.name for child in Path(source).iterdir()),
                             ["JoltPhysics.lib", "JoltPhysics.pdb", vendor_cache.MANIFEST_NAME])
            renames.append((Path(source), Path(destination)))
            real_rename(source, destination)

        with mock.patch.object(vendor_cache.os, "rename", checked_rename):
            entry = self.repository.cache.publish(staged, self.inputs, "JoltPhysics.lib", checkout=self.repository.root)
        self.assertEqual(renames, [(staged, target)])
        self.assertEqual(entry, target)
        self.assertFalse(staged.exists())
        self.assertEqual(self.repository.cache.verify(entry, self.inputs), [])

    def test_an_entry_is_never_overwritten(self) -> None:
        entry = self.repository.publish(self.inputs)
        library = (entry / "JoltPhysics.lib").read_bytes()
        staged = self.repository.stage({"JoltPhysics.lib": b"another library"})
        with self.assertRaises(vendor_cache.EntryExistsError):
            self.repository.cache.publish(staged, self.inputs, "JoltPhysics.lib", checkout=self.repository.root)
        self.assertEqual((entry / "JoltPhysics.lib").read_bytes(), library)
        self.assertEqual(self.repository.cache.verify(entry, self.inputs), [])

    def test_a_staged_directory_without_its_library_is_not_published(self) -> None:
        staged = self.repository.stage({"JoltPhysics.pdb": b"symbols"})
        with self.assertRaises(vendor_cache.VendorCacheError):
            self.repository.cache.publish(staged, self.inputs, "JoltPhysics.lib", checkout=self.repository.root)
        self.assertFalse(self.repository.cache.entries.exists())

    def test_a_directory_without_a_record_is_not_an_entry(self) -> None:
        entry = self.repository.cache.entry_directory("JoltPhysics", "Debug", vendor_cache.entry_key(self.inputs))
        entry.mkdir(parents=True)
        (entry / "JoltPhysics.lib").write_bytes(b"library")
        problems = self.repository.cache.verify(entry, self.inputs)
        self.assertEqual(len(problems), 1)
        self.assertIn(vendor_cache.MANIFEST_NAME, problems[0])


class VendorCacheVerifyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repository = FakeRepository(self)
        self.inputs = self.repository.inputs()
        self.entry = self.repository.publish(self.inputs)
        self.assertEqual(self.repository.cache.verify(self.entry, self.inputs), [])

    def problems(self) -> str:
        return "\n".join(self.repository.cache.verify(self.entry, self.inputs))

    def edit_record(self, change: Any) -> None:
        path = self.entry / vendor_cache.MANIFEST_NAME
        manifest = json.loads(path.read_text(encoding="utf-8"))
        change(manifest)
        path.write_text(json.dumps(manifest), encoding="utf-8")

    def test_a_changed_file_of_the_same_size_is_rejected(self) -> None:
        library = self.entry / "JoltPhysics.lib"
        data = bytearray(library.read_bytes())
        data[10] ^= 0xFF
        library.write_bytes(bytes(data))
        self.assertIn("JoltPhysics.lib does not have its recorded SHA-256", self.problems())

    def test_a_truncated_file_is_rejected(self) -> None:
        (self.entry / "JoltPhysics.pdb").write_bytes(b"sym")
        self.assertIn("JoltPhysics.pdb is 3 bytes", self.problems())

    def test_a_missing_file_is_rejected(self) -> None:
        (self.entry / "JoltPhysics.pdb").unlink()
        self.assertIn("JoltPhysics.pdb is missing", self.problems())

    def test_an_edited_record_is_rejected(self) -> None:
        def drop_determinism(manifest: dict[str, Any]) -> None:
            tools = manifest["inputs"]["settings"]["tools"]["ClCompile"]
            tools["PreprocessorDefinitions"] = tools["PreprocessorDefinitions"].replace(
                "JPH_CROSS_PLATFORM_DETERMINISTIC;", "")

        self.edit_record(drop_determinism)
        problems = self.problems()
        self.assertIn("do not hash to its key", problems)
        self.assertIn("records other inputs than this build's (settings.tools.ClCompile.PreprocessorDefinitions",
                      problems)

    def test_an_entry_recorded_for_other_inputs_is_rejected(self) -> None:
        other = self.repository.inputs(dataclasses.replace(JOLT, options="/arch:AVX2"))

        def record_other_inputs(manifest: dict[str, Any]) -> None:
            manifest["inputs"] = other
            manifest["key"] = vendor_cache.entry_key(other)

        self.edit_record(record_other_inputs)
        problems = self.problems()
        self.assertIn("not the directory's", problems)
        self.assertIn("records other inputs than this build's", problems)

    def test_an_entry_of_another_format_is_rejected(self) -> None:
        self.edit_record(lambda manifest: manifest.update(format=vendor_cache.FORMAT + 1))
        self.assertIn(f"its format is {vendor_cache.FORMAT + 1}", self.problems())

    def test_an_unreadable_record_is_rejected(self) -> None:
        (self.entry / vendor_cache.MANIFEST_NAME).write_text("{", encoding="utf-8")
        self.assertIn("unreadable", self.problems())


class FakeCompiler:
    """Stands in for MSBuild in vendor_cache.prepare: writes each staged build's library and compiler PDB."""

    def __init__(self, contents: bytes = b"compiled library", warnings: tuple[str, ...] = (),
                 succeeded: bool = True, library: bool = True) -> None:
        self.contents = contents
        self.warnings = warnings
        self.succeeded = succeeded
        self.library = library  # False: a successful compile that wrote no library
        self.calls: list[list[str]] = []

    def __call__(self, project: Path, builds: list[vendor_cache.StagedBuild]) -> vendor_cache.PopulateResult:
        ElementTree.parse(project)  # the generated project is well-formed
        self.calls.append([build.project for build in builds])
        warnings = []
        for build in builds:
            build.output_directory.mkdir(parents=True, exist_ok=True)
            if self.succeeded and self.library:
                (build.output_directory / build.library).write_bytes(self.contents)
            if self.succeeded:
                (build.output_directory / f"{build.project}.pdb").write_bytes(b"compiler symbols")
            warnings += [f"{warning} [{build.project_file}]" for warning in self.warnings]
        return vendor_cache.PopulateResult(self.succeeded, warnings, [] if self.succeeded else ["error C1234: no"])


class VendorCachePrepareTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repository = FakeRepository(self)
        self.projects = {"JoltPhysics": self.repository.project(JOLT),
                         "Tests": self.repository.project(CONSUMER, location="Vendor/JoltPhysics")}
        self.log: list[str] = []

    def prepare(self, compiler: FakeCompiler) -> vendor_cache.Plan:
        return vendor_cache.prepare(self.repository.cache, self.projects, "Debug", "x64", TOOLCHAIN,
                                    self.repository.root / "Workspace.slnx", compiler, 60.0, self.log.append,
                                    self.repository.root)

    def test_a_missing_entry_is_compiled_once_then_linked(self) -> None:
        compiler = FakeCompiler()
        plan = self.prepare(compiler)
        self.assertEqual(compiler.calls, [["JoltPhysics"]])
        self.assertEqual([(entry.project, entry.published) for entry in plan.linked], [("JoltPhysics", True)])
        self.assertEqual(plan.linked[0].library.read_bytes(), b"compiled library")
        self.assertEqual(sorted(child.name for child in plan.linked[0].entry.iterdir()),
                         ["JoltPhysics.lib", "JoltPhysics.pdb", vendor_cache.MANIFEST_NAME])
        self.assertEqual(list(self.repository.cache.staging.iterdir()), [])

        again = self.prepare(compiler)
        self.assertEqual(len(compiler.calls), 1)
        self.assertEqual([(entry.project, entry.published, entry.entry) for entry in again.linked],
                         [("JoltPhysics", False, plan.linked[0].entry)])

    def test_an_entry_another_build_published_while_this_one_waited_is_linked(self) -> None:
        # One build compiles a missing entry while the others wait for populate.lock, then link it: here another build
        # held the lock and published the entry for the same inputs before this one got it.
        real_lock = vendor_cache.VendorCache.populate_lock
        published: list[Path] = []

        @contextlib.contextmanager
        def another_build_publishes_first(cache: vendor_cache.VendorCache, timeout: float,
                                          on_wait: Any) -> Any:
            published.append(self.repository.publish(self.repository.inputs()))
            with real_lock(cache, timeout, on_wait):
                yield

        compiler = FakeCompiler()
        with mock.patch.object(vendor_cache.VendorCache, "populate_lock", another_build_publishes_first):
            plan = self.prepare(compiler)
        self.assertEqual(compiler.calls, [])
        self.assertEqual([(entry.entry, entry.published) for entry in plan.linked], [(published[0], False)])
        self.assertEqual(plan.compiled_in_workspace, [])
        self.assertEqual(plan.linked[0].library.read_bytes(), b"library " * 64)

    def test_a_corrupt_entry_is_replaced_and_never_linked(self) -> None:
        first = self.prepare(FakeCompiler(b"first build")).linked[0]
        (first.entry / "JoltPhysics.lib").write_bytes(b"first buil!")
        compiler = FakeCompiler(b"second build")
        plan = self.prepare(compiler)
        self.assertEqual(compiler.calls, [["JoltPhysics"]])
        self.assertEqual(plan.linked[0].library.read_bytes(), b"second build")
        self.assertTrue(plan.linked[0].published)
        self.assertTrue(any("rejected" in note and "SHA-256" in note for note in plan.notes), plan.notes)
        self.assertEqual(list(self.repository.cache.trash.iterdir()), [])

    def test_a_failed_compile_publishes_nothing(self) -> None:
        with self.assertRaises(vendor_cache.PopulateFailedError) as raised:
            self.prepare(FakeCompiler(succeeded=False))
        self.assertEqual(raised.exception.result.errors, ["error C1234: no"])
        self.assertFalse(self.repository.cache.entries.exists())
        self.assertEqual(list(self.repository.cache.staging.iterdir()), [])

    def test_a_library_that_cannot_be_published_is_compiled_in_the_workspace(self) -> None:
        plan = self.prepare(FakeCompiler(library=False))
        self.assertEqual(plan.linked, [])
        self.assertEqual(plan.compiled_in_workspace, ["JoltPhysics"])
        self.assertTrue(any("not published" in note for note in plan.notes), plan.notes)
        self.assertFalse(self.repository.cache.entries.exists())

    def test_an_entry_in_use_is_neither_replaced_nor_linked(self) -> None:
        entry = self.prepare(FakeCompiler()).linked[0].entry
        (entry / "JoltPhysics.pdb").write_bytes(b"changed")
        compiler = FakeCompiler()
        with mock.patch.object(vendor_cache.VendorCache, "discard", return_value=False):
            plan = self.prepare(compiler)
        self.assertEqual(compiler.calls, [])
        self.assertEqual(plan.linked, [])
        self.assertEqual(plan.compiled_in_workspace, ["JoltPhysics"])
        self.assertTrue(entry.is_dir())

    def test_compile_warnings_count_in_every_build_that_links_the_entry(self) -> None:
        warning = f"{self.repository.root / 'Vendor' / 'JoltPhysics' / 'Jolt' / 'Core.cpp'}(1,1): warning C4100: x"
        plan = self.prepare(FakeCompiler(warnings=(f"3:7>{warning}",)))
        self.assertEqual(len(plan.warnings), 1)
        self.assertTrue(plan.warnings[0].startswith(warning))
        manifest = json.loads((plan.linked[0].entry / vendor_cache.MANIFEST_NAME).read_text(encoding="utf-8"))
        self.assertTrue(manifest["warnings"][0].startswith(vendor_cache.REPOSITORY_PLACEHOLDER))
        self.assertEqual(self.prepare(FakeCompiler()).warnings, plan.warnings)

    def test_compile_warnings_of_no_library_count_in_that_build_only(self) -> None:
        class TraversalWarning(FakeCompiler):
            def __call__(self, project: Path, builds: list[vendor_cache.StagedBuild]) -> vendor_cache.PopulateResult:
                result = super().__call__(project, builds)
                result.warnings.append(f"MSBUILD : warning MSB4000: x [{project}]")
                return result

        plan = self.prepare(TraversalWarning())
        self.assertEqual(len(plan.warnings), 1)
        self.assertIn("MSB4000", plan.warnings[0])
        manifest = json.loads((plan.linked[0].entry / vendor_cache.MANIFEST_NAME).read_text(encoding="utf-8"))
        self.assertEqual(manifest["warnings"], [])
        self.assertEqual(self.prepare(FakeCompiler()).warnings, [])


class VendorCacheLockTests(unittest.TestCase):
    def test_the_populate_lock_excludes_other_processes_and_names_its_holder(self) -> None:
        repository = FakeRepository(self)
        # A timeout of 0 tries the lock once: the outcome depends on who holds it, never on how long anything takes.
        script = textwrap.dedent(f"""\
            import sys
            sys.path.insert(0, {str(REPOSITORY_ROOT / 'Scripts')!r})
            from pathlib import Path
            from Lib import vendor_cache
            cache = vendor_cache.VendorCache(Path({str(repository.cache.root)!r}))
            try:
                with cache.populate_lock(0.0, print):
                    print("acquired")
            except vendor_cache.VendorCacheError as error:
                print(f"refused: {{error}}")
            """)

        def other_process() -> str:
            return subprocess.run([sys.executable, "-c", script], capture_output=True, text=True, timeout=60,
                                  check=True).stdout

        with repository.cache.populate_lock(0.0, self.fail):
            output = other_process()
            self.assertIn("refused: timed out", output)
            self.assertIn(f"held by pid {os.getpid()} in {paths.REPOSITORY_ROOT.as_posix()}", output)
        self.assertIn("acquired", other_process())


class VendorCacheOverrideTests(unittest.TestCase):
    def test_the_override_links_cached_libraries_into_the_linking_projects_that_reference_them(self) -> None:
        repository = FakeRepository(self)
        cached = repository.project(JOLT)
        other = repository.project(dataclasses.replace(JOLT, name="Other"))
        static = repository.project(dataclasses.replace(JOLT, name="Static", references=("JoltPhysics",)))
        application = repository.project(dataclasses.replace(CONSUMER, references=("JoltPhysics", "Other")))
        references, linking = vendor_cache.consumers([cached, other, static, application], "Debug", "x64")
        self.assertEqual(references[application], [cached, other])
        self.assertEqual(linking, {application})

        library = repository.root / "cache" / "JoltPhysics.lib"
        linked = [vendor_cache.LinkedEntry("JoltPhysics", cached, library.parent, library, False, [])]
        path = vendor_cache.write_link_override(repository.root / "bin-int" / "Debug.targets", linked, references,
                                                linking)
        root = ElementTree.parse(path).getroot()
        removed = [item.get("Remove") for item in root.iterfind("msb:ItemGroup/msb:ProjectReference", NAMESPACE)]
        self.assertEqual(removed, [str(cached)])
        groups = {group.get("Condition"): group for group in root.iterfind("msb:PropertyGroup", NAMESPACE)}
        self.assertEqual(groups[f"'$(MSBuildProjectFullPath)' == '{cached}'"]
                         .findtext("msb:EngineVendorCacheLibrary", namespaces=NAMESPACE), str(library))
        self.assertEqual(groups[f"'$(MSBuildProjectFullPath)' == '{application}'"]
                         .findtext("msb:EngineVendorCacheLibraries", namespaces=NAMESPACE), str(library))
        self.assertNotIn(f"'$(MSBuildProjectFullPath)' == '{static}'", groups)
        skip = ElementTree.parse(path.parent / vendor_cache.SKIP_TARGETS_NAME).getroot()
        self.assertEqual([target.get("Name") for target in skip.iterfind("msb:Target", NAMESPACE)],
                         ["Build", "Rebuild"])
        self.assertEqual(skip.findall("msb:Target/msb:CL", NAMESPACE), [])

    def test_paths_msbuild_would_interpret_are_refused(self) -> None:
        for path in (Path("C:/Work;Space/x.lib"), Path("C:/100%/x.lib"), Path("C:/it's/x.lib"), Path("C:/$(x)/a")):
            with self.subTest(str(path)), self.assertRaises(vendor_cache.VendorCacheError):
                vendor_cache.msbuild_literal(path)


class CheckBuildConfigVendorCacheTests(unittest.TestCase):
    """Rule 9: the entries a workspace would link must record the workspace's settings, and those recorded settings
    must pass rules 1 to 4 and 7 against the workspace's consumers."""

    def setUp(self) -> None:
        self.repository = FakeRepository(self)
        self.generated = self.repository.root / "generated"
        self.vendor_shape = dataclasses.replace(JOLT, configs=paths.CONFIGURATIONS)
        vendor = self.repository.project(self.vendor_shape, location="generated", relative=False)
        consumer = self.repository.project(dataclasses.replace(CONSUMER, configs=paths.CONFIGURATIONS),
                                           location="generated", relative=False)
        projects = {path.stem: CheckBuildConfig.parse_vcxproj(path) for path in (vendor, consumer)}
        self.workspace = CheckBuildConfig.GeneratedWorkspace(CheckBuildConfig.TARGET_BY_NAME["windows"], projects)
        self.context = CheckBuildConfig.VendorCacheContext(self.repository.cache, TOOLCHAIN,
                                                           vendor_cache.SourceDigester(self.repository.root))
        self.inputs = self.repository.inputs(self.vendor_shape, location="generated", relative=False)
        self.assertEqual(CheckBuildConfig.check_workspace(self.workspace, {"JoltPhysics"}, {"Tests"}), [])

    def check(self) -> tuple[list[str], int]:
        return CheckBuildConfig.check_vendor_cache(self.workspace, self.generated, {"JoltPhysics"}, {"Tests"},
                                                   self.context)

    def record(self, change: Any) -> str:
        """Publishes the workspace's entry, then changes the ClCompile settings or properties its record holds (the
        record of a library built with other flags under this key); returns the findings."""
        entry = self.repository.publish(self.inputs)
        path = entry / vendor_cache.MANIFEST_NAME
        manifest = json.loads(path.read_text(encoding="utf-8"))
        change(manifest["inputs"]["settings"])
        path.write_text(json.dumps(manifest), encoding="utf-8")
        findings, checked = self.check()
        self.assertEqual(checked, 1)
        text = "\n".join(findings)
        self.assertIn(f"vendor cache entry {entry.as_posix()}", text)
        self.assertIn("records other build settings than the workspace's JoltPhysics", text)
        return text

    def test_an_entry_recording_the_workspace_settings_passes(self) -> None:
        self.repository.publish(self.inputs)
        self.assertEqual(self.check(), ([], 1))

    def test_an_entry_recording_a_defective_workspace_adds_no_finding_of_its_own(self) -> None:
        # The workspace's own per-file defect is rule 3's finding; the entry recording the same flags adds none.
        core = "Vendor/JoltPhysics/Jolt/Core.cpp"
        shape = dataclasses.replace(self.vendor_shape, file_options={core: "/arch:AVX2"})
        vendor = self.repository.project(shape, location="generated", relative=False)
        self.workspace.projects["JoltPhysics"] = CheckBuildConfig.parse_vcxproj(vendor)
        baseline = CheckBuildConfig.check_workspace(self.workspace, {"JoltPhysics"}, {"Tests"})
        self.assertTrue(any("Core.cpp) is compiled for a different instruction set" in line for line in baseline),
                        baseline)
        self.repository.publish(self.repository.inputs(shape, location="generated", relative=False))
        self.assertEqual(self.check(), ([], 1))

    def test_entries_under_other_keys_are_not_checked(self) -> None:
        other = self.repository.inputs(dataclasses.replace(self.vendor_shape, options="/arch:AVX2"),
                                       location="elsewhere", relative=False)
        self.repository.publish(other)
        self.assertEqual(self.check(), ([], 0))

    def test_recorded_abi_defines_the_consumers_lack_are_reported(self) -> None:
        def add_define(settings: dict[str, Any]) -> None:
            compile_settings = settings["tools"]["ClCompile"]
            compile_settings["PreprocessorDefinitions"] = "JPH_DOUBLE_PRECISION;" + compile_settings[
                "PreprocessorDefinitions"]

        text = self.record(add_define)
        self.assertIn("Tests includes JoltPhysics headers but lacks JPH_DOUBLE_PRECISION that JoltPhysics defines",
                      text)
        self.assertIn("do not hash to its key", text)

    def test_a_recorded_missing_determinism_define_is_reported(self) -> None:
        def drop_define(settings: dict[str, Any]) -> None:
            compile_settings = settings["tools"]["ClCompile"]
            compile_settings["PreprocessorDefinitions"] = compile_settings["PreprocessorDefinitions"].replace(
                "JPH_CROSS_PLATFORM_DETERMINISTIC;", "")

        self.assertIn("JoltPhysics does not define JPH_CROSS_PLATFORM_DETERMINISTIC (the flags recorded by vendor "
                      "cache entry", self.record(drop_define))

    def test_a_recorded_instruction_set_other_than_the_consumers_is_reported(self) -> None:
        def wider(settings: dict[str, Any]) -> None:
            settings["tools"]["ClCompile"]["AdditionalOptions"] = "/arch:AVX2 %(AdditionalOptions)"

        self.assertIn("Tests is compiled for a different instruction set (/arch:SSE4.2) than JoltPhysics (/arch:AVX2)",
                      self.record(wider))

    def test_a_recorded_fast_floating_point_model_is_reported(self) -> None:
        def fast(settings: dict[str, Any]) -> None:
            settings["tools"]["ClCompile"]["FloatingPointModel"] = "Fast"

        self.assertIn("JoltPhysics uses a non-precise floating-point model: FloatingPointModel=Fast",
                      self.record(fast))

    def test_a_recorded_sanitizer_in_a_plain_workspace_is_reported(self) -> None:
        def instrumented(settings: dict[str, Any]) -> None:
            settings["properties"]["EnableASAN"] = "true"

        self.assertIn("JoltPhysics is built with AddressSanitizer in a plain workspace", self.record(instrumented))


class BuildVendorCacheTests(unittest.TestCase):
    """Build.py's MSBuild builds with the cache, MSBuild replaced by a stand-in: a workspace below the repository's
    bin-int (a static library compiling Vendor/MikkTSpace/mikktspace.c, and an application that links it)."""

    def setUp(self) -> None:
        paths.BIN_INT_ROOT.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="EngineBuildVendorCache", dir=paths.BIN_INT_ROOT)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        library = ProjectShape("Lib", sources=("Vendor/MikkTSpace/mikktspace.c",),
                               include_directories=("Vendor/MikkTSpace",))
        application = ProjectShape("App", kind="Application", sources=("Tests/Source/TestsMain.cpp",),
                                   references=("Lib",))
        for shape in (library, application):
            (self.root / f"{shape.name}.vcxproj").write_text(project_text(shape, self.root, REPOSITORY_ROOT, True),
                                                             encoding="utf-8")
        (self.root / "Workspace.slnx").write_text(
            '<Solution>\n  <Configurations>\n    <BuildType Name="Debug" />\n    <Platform Name="x64" />\n'
            '  </Configurations>\n  <Project Path="App.vcxproj" />\n  <Project Path="Lib.vcxproj" />\n</Solution>\n',
            encoding="utf-8")
        self.solution = workspace.read_solution(self.root / "Workspace.slnx")
        self.library_file = (self.root / "Lib.vcxproj").resolve()
        self.cache_root = self.root / "cache"
        self.compile_warnings: list[str] = []
        self.compile_errors: list[str] = []
        self.calls: list[tuple[Path, list[str]]] = []
        for patch in (mock.patch.object(Build.toolchain, "find_visual_studio", return_value=VISUAL_STUDIO),
                      mock.patch.object(Build.toolchain, "find_windows_sdk", return_value=WINDOWS_SDK),
                      mock.patch.object(Build, "run_msbuild", side_effect=self.msbuild)):
            patch.start()
            self.addCleanup(patch.stop)

    def msbuild(self, visual_studio: Any, target: Path, options: list[str], log_stem: Path, arguments: Any,
                console: Any, timeout: float) -> tuple[Any, list[str], list[str]]:
        """Stands in for MSBuild: a populate project "compiles" each staged library; the solution builds cleanly."""
        self.calls.append((target, options))
        if target.suffix != ".proj":
            return Build.ProcessResult(["MSBuild"], 0, False, 0.1, ""), [], []
        for item in ElementTree.parse(target).getroot().iterfind("msb:ItemGroup/msb:VendorProject", NAMESPACE):
            properties = dict(entry.split("=", 1) for entry in
                              item.findtext("msb:AdditionalProperties", namespaces=NAMESPACE).split(";"))
            output = Path(properties["OutDir"])
            output.mkdir(parents=True, exist_ok=True)
            if not self.compile_errors:
                (output / "Lib.lib").write_bytes(b"compiled library")
                (output / "Lib.pdb").write_bytes(b"compiler symbols")
        code = 1 if self.compile_errors else 0
        return (Build.ProcessResult(["MSBuild"], code, False, 0.1, ""),
                [f"{warning} [{self.library_file}]" for warning in self.compile_warnings], list(self.compile_errors))

    def build(self, **options: Any) -> Build.Step:
        arguments = argparse.Namespace(jobs=1, verbose=False, rebuild=False, allow_warnings=False,
                                       allow_vendor_warnings=False, vendor_cache=True, vendor_cache_dir=self.cache_root,
                                       timeout=600.0)
        for name, value in options.items():
            setattr(arguments, name, value)
        self.calls.clear()
        return Build.build_msbuild(self.solution, "Debug", None, arguments, Build.Console(True))

    def solution_options(self) -> list[str]:
        return [options for target, options in self.calls if target.suffix == ".slnx"][0]

    def test_a_build_compiles_a_missing_library_once_and_links_it_from_the_cache(self) -> None:
        first = self.build()
        self.assertFalse(first.failed, first.detail)
        self.assertEqual([target.suffix for target, _ in self.calls], [".proj", ".slnx"])
        linked = first.data["vendorCache"]["linked"]
        self.assertEqual([(entry["project"], entry["published"]) for entry in linked], [("Lib", True)])
        override = Path(next(option for option in self.solution_options()
                             if option.startswith("-p:ForceImportAfterCppTargets=")).split("=", 1)[1])
        root = ElementTree.parse(override).getroot()
        libraries = root.findtext("msb:PropertyGroup/msb:EngineVendorCacheLibraries", namespaces=NAMESPACE)
        self.assertEqual(Path(libraries), Path(linked[0]["entry"]) / "Lib.lib")

        second = self.build()
        self.assertFalse(second.failed, second.detail)
        self.assertEqual([target.suffix for target, _ in self.calls], [".slnx"])
        self.assertEqual([entry["published"] for entry in second.data["vendorCache"]["linked"]], [False])

    def test_a_cached_library_reports_its_compile_warnings_in_every_build(self) -> None:
        self.compile_warnings = [f"{REPOSITORY_ROOT / 'Vendor' / 'MikkTSpace' / 'mikktspace.c'}(1,1): warning C4100: x"]
        for build in ("compiling", "linking from the cache"):
            with self.subTest(build):
                step = self.build()
                self.assertTrue(step.failed)
                self.assertIn("1 warning(s)", step.detail)
                self.compile_warnings = []
        tolerated = self.build(allow_vendor_warnings=True)
        self.assertFalse(tolerated.failed, tolerated.detail)
        self.assertEqual(tolerated.data["vendorWarningCount"], 1)

    def test_a_failed_compile_fails_the_build_with_its_errors(self) -> None:
        self.compile_errors = ["mikktspace.c(1,1): error C2065: x"]
        step = self.build()
        self.assertTrue(step.failed)
        self.assertTrue(step.detail.startswith("vendor cache: build failed"), step.detail)
        self.assertIn("error C2065", step.detail)
        self.assertEqual([target.suffix for target, _ in self.calls], [".proj"])

    def test_the_compile_of_missing_entries_runs_with_the_builds_job_count_and_compiler_cap(self) -> None:
        # The build lock's capped slot (Lib/locks.py) holds for every MSBuild run of the build, so the compile of
        # missing entries runs with the build's -m and, when capped, its CL_MPCount too: at most -m x CL_MPCount = 12
        # compilers.
        for name, jobs, capped, parallel in (("capped", 12, True, ["-m:2", "-p:CL_MPCount=6"]),
                                             ("uncapped", 24, False, ["-m:24"])):
            commands: list[list[str]] = []

            def streamed(command: list[str], **_options: Any) -> Build.ProcessResult:
                commands.append(command)
                return Build.ProcessResult(command, 0, False, 0.1, "")

            def msbuild(*call: Any) -> tuple[Any, list[str], list[str]]:
                with mock.patch.object(Build, "run_streamed", side_effect=streamed):
                    RUN_MSBUILD(*call)
                return self.msbuild(*call)

            with self.subTest(name), mock.patch.object(Build, "run_msbuild", side_effect=msbuild):
                shutil.rmtree(self.cache_root, ignore_errors=True)
                step = self.build(jobs=jobs, capped=capped)
                self.assertFalse(step.failed, step.detail)
                self.assertEqual([Path(command[1]).suffix for command in commands], [".proj", ".slnx"])
                for command in commands:
                    self.assertEqual([option for option in command
                                      if option.startswith(("-m:", "-p:CL_MPCount="))], parallel)

    def test_msbuild_runs_without_the_variables_that_select_another_toolchain(self) -> None:
        # The review's toolchain finding: a VCToolsVersion of a Developer Prompt (-vcvars_ver=14.50) made MSBuild
        # compile entries with another MSVC than the key's; LLVMInstallDir, LLVMToolsVersion and VCToolsInstallDir
        # select clang-cl and MSVC the same way, and cl.exe adds CL and _CL_ to every compile.
        environments: list[dict[str, str]] = []

        def streamed(command: list[str], **options: Any) -> Build.ProcessResult:
            environments.append(options["env"])
            return Build.ProcessResult(command, 0, False, 0.1, "")

        selected = {"VCToolsVersion": "14.50.35717", "VCTOOLSINSTALLDIR": "C:/MSVC/14.50.35717",
                    "LLVMInstallDir": "C:/LLVM", "LLVMToolsVersion": "19.1.0", "CL": "/Od", "_CL_": "/DOTHER"}
        arguments = argparse.Namespace(jobs=1, capped=False, verbose=False)
        with mock.patch.dict(os.environ, selected), mock.patch.object(Build, "run_streamed", side_effect=streamed):
            RUN_MSBUILD(VISUAL_STUDIO, self.root / "Workspace.slnx", [], self.root / "Log", arguments,
                        Build.Console(True), 60.0)
        names = {name.upper() for name in environments[0]}
        for variable in vendor_cache.TOOLCHAIN_ENVIRONMENT_VARIABLES:
            self.assertNotIn(variable.upper(), names)
        self.assertEqual(environments[0].get("VSLANG"), "1033")
        self.assertIn("PATH", names, "the rest of the environment is kept")

    def test_without_the_cache_the_workspace_compiles_its_vendored_libraries(self) -> None:
        for name, options in (("--no-vendor-cache", {"vendor_cache": False}),
                              ("no repository", {"vendor_cache_dir": None})):
            with self.subTest(name), mock.patch.object(Build.vendor_cache, "default_root",
                                                       side_effect=vendor_cache.VendorCacheError("no repository")):
                step = self.build(**options)
                self.assertFalse(step.failed, step.detail)
                self.assertEqual([target.suffix for target, _ in self.calls], [".slnx"])
                self.assertFalse(any("ForceImportAfterCppTargets" in option for option in self.solution_options()))
                self.assertFalse(step.data["vendorCache"]["used"])


class CheckBuildConfigWiringTests(unittest.TestCase):
    """CheckBuildConfig.py's rule 9 end to end (the review's wiring finding): its own generation of the windows target,
    its --vendor-cache-dir, --no-vendor-cache and report, against the entries Build.py publishes for the workspace
    Generate.py writes (MSBuild stood in, the toolchain fixed for both), so a key either computes differently fails
    here instead of leaving rule 9 checking nothing."""

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="EngineRule9")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.cache_root = self.root / "cache"
        for patch in (mock.patch.object(Build.toolchain, "find_visual_studio", return_value=VISUAL_STUDIO),
                      mock.patch.object(Build.toolchain, "find_windows_sdk", return_value=WINDOWS_SDK),
                      mock.patch.object(Build, "run_msbuild", side_effect=self.msbuild)):
            patch.start()
            self.addCleanup(patch.stop)

    @staticmethod
    def msbuild(visual_studio: Any, target: Path, options: list[str], log_stem: Path, arguments: Any, console: Any,
                timeout: float) -> tuple[Any, list[str], list[str]]:
        """Stands in for MSBuild's compile of missing entries: each staged project's library and compiler PDB."""
        for item in ElementTree.parse(target).getroot().iterfind("msb:ItemGroup/msb:VendorProject", NAMESPACE):
            properties = dict(entry.split("=", 1) for entry in
                              item.findtext("msb:AdditionalProperties", namespaces=NAMESPACE).split(";"))
            project = vendor_cache.read_project_file(Path(item.get("Include")))
            library = vendor_cache.library_name(project, vendor_cache.project_settings(
                project, properties["Configuration"], properties["Platform"]))
            output = Path(properties["OutDir"])
            output.mkdir(parents=True, exist_ok=True)
            (output / library).write_bytes(b"compiled library")
            (output / f"{project.name}.pdb").write_bytes(b"compiler symbols")
        return Build.ProcessResult(["MSBuild"], 0, False, 0.1, ""), [], []

    def check(self, *options: str) -> tuple[int, dict[str, Any]]:
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
            code = CheckBuildConfig.main(["--json", "--targets", "windows", "--vendor-cache-dir", str(self.cache_root),
                                          *options])
        return code, json.loads(output.getvalue())

    def test_rule_9_checks_the_entries_build_py_links(self) -> None:
        location = self.root / "Workspace"
        step = Generate.generate("vs2026", "windows", None, location, Build.Console(True), False)
        self.assertFalse(step.failed, step.detail)
        solution = workspace.read_solution(workspace.workspace_file("vs2026", location))
        arguments = argparse.Namespace(vendor_cache=True, vendor_cache_dir=self.cache_root, jobs=1, capped=False,
                                       verbose=False, allow_warnings=False, allow_vendor_warnings=False)
        use = Build.link_vendor_cache(solution, "Debug", None, VISUAL_STUDIO, arguments, Build.Console(True),
                                      self.root / "Log", time.monotonic() + 600.0)
        self.assertIsNotNone(use.plan, use.reason)
        linked = {entry.project: entry.entry for entry in use.plan.linked}
        self.assertEqual(set(linked), {"GLFW", "ImGui", "ImGuizmo", "JoltPhysics", "Luau", "LuauAnalysis", "MikkTSpace",
                                       "NVRHI", "miniaudio", "nfd", "spdlog"})

        code, report = self.check()
        self.assertEqual(code, 0, report["findings"])
        self.assertEqual(report["vendorCache"]["entriesChecked"], len(linked))

        # The record of an entry built with other flags under this key: rule 9 finds it.
        record = linked["JoltPhysics"] / vendor_cache.MANIFEST_NAME
        manifest = json.loads(record.read_text(encoding="utf-8"))
        compile_settings = manifest["inputs"]["settings"]["tools"]["ClCompile"]
        compile_settings["PreprocessorDefinitions"] = compile_settings["PreprocessorDefinitions"].replace(
            "JPH_CROSS_PLATFORM_DETERMINISTIC;", "")
        record.write_text(json.dumps(manifest), encoding="utf-8")
        code, report = self.check()
        self.assertEqual(code, 1)
        self.assertTrue(any(f"vendor cache entry {linked['JoltPhysics'].as_posix()}" in finding
                            and "records other build settings" in finding for finding in report["findings"]),
                        report["findings"])

        code, report = self.check("--no-vendor-cache")
        self.assertEqual(code, 0, report["findings"])
        self.assertEqual(report["vendorCache"],
                         {"root": None, "entriesChecked": 0, "skipped": "--no-vendor-cache given"})


class VendorCacheWorkspaceTests(unittest.TestCase):
    """The real workspace, generated with the pinned premake into temporary directories."""

    def generate(self, location: Path, *options: str) -> dict[tuple[str, str], str]:
        result = premake.run(["--fatal", "--os=windows", *options, f"--to={location}", "vs2026"], timeout=300,
                             premake=premake.require_pinned())
        self.assertTrue(result.succeeded, result.tail(20))
        keys = {}
        for path in sorted(location.glob("*.vcxproj")):
            project = vendor_cache.read_project_file(path)
            for config in paths.CONFIGURATIONS:
                settings = vendor_cache.project_settings(project, config, "x64")
                if vendor_cache.is_vendor_library(settings):
                    keys[(project.name, config)] = vendor_cache.entry_key(
                        vendor_cache.project_inputs(project, config, "x64", TOOLCHAIN, self.digester))
        return keys

    def temporary_directory(self, parent: Path | None = None) -> Path:
        if parent is not None:
            parent.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="EngineVendorCacheWorkspace", dir=parent)
        self.addCleanup(temporary.cleanup)
        return Path(temporary.name)

    def test_real_keys_follow_the_build_not_the_workspace_location(self) -> None:
        self.digester = vendor_cache.SourceDigester()
        # Below bin-int premake writes paths relative to the workspace; in the system's temporary directory (another
        # drive on the development machines) absolute ones.
        inside = self.generate(self.temporary_directory(paths.BIN_INT_ROOT))
        outside = self.generate(self.temporary_directory())
        self.assertEqual({name for name, _ in inside},
                         {"GLFW", "ImGui", "ImGuizmo", "JoltPhysics", "Luau", "LuauAnalysis", "MikkTSpace", "NVRHI",
                          "miniaudio", "nfd", "spdlog"})
        self.assertEqual(inside, outside)
        self.assertEqual(len(set(inside.values())), len(inside))

        sanitized = self.generate(self.temporary_directory(), "--sanitize=address")
        for (name, config), key in inside.items():
            with self.subTest(f"AddressSanitizer {name} {config}"):
                # Only Release is instrumented: its entries are separate, Debug and Dist share the plain ones.
                if config == "Release":
                    self.assertNotEqual(sanitized[(name, config)], key)
                else:
                    self.assertEqual(sanitized[(name, config)], key)
        clang = self.generate(self.temporary_directory(), "--cc=clang")
        self.assertEqual(set(clang), set(inside))
        self.assertFalse(set(clang.values()) & set(inside.values()))


if __name__ == "__main__":
    unittest.main()
