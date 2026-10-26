"""The Dist map-file check's parser and families (Docs/Architecture.md §14.4; Roadmap M15 acceptance "Dist: map file
contains no automation, Analysis, ImGui or AssetPipeline symbols"; Docs/Decisions/0024-m15-contract.md decision 11,
stream D), over the seeded fixtures of Tests/Data/MapFiles/<format>/: Clean.map has no finding, and each
<Family>.map has exactly the findings its Expected.json lists. The real Dist map is checked by CI.py after its Dist
build. These tests need no editor build.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

from Lib import runtime_map  # noqa: E402  (Scripts/ is on sys.path above)

MAP_FIXTURES = REPOSITORY_ROOT / "Tests" / "Data" / "MapFiles"
CHECK_SCRIPT = REPOSITORY_ROOT / "Scripts" / "CheckRuntimeMap.py"


class RuntimeMapTests(unittest.TestCase):
    def test_every_format_parses_and_every_seeded_fixture_has_exactly_its_findings(self) -> None:
        patterns = runtime_map.family_patterns(REPOSITORY_ROOT)
        self.assertEqual(sorted(patterns), sorted(runtime_map.FORBIDDEN_FAMILIES))
        for map_format in runtime_map.MAP_FORMATS:
            directory = MAP_FIXTURES / map_format
            expected = json.loads((directory / "Expected.json").read_text(encoding="utf-8"))
            self.assertEqual(sorted(expected), sorted(["Clean", *runtime_map.FORBIDDEN_FAMILIES, "MissingRequired"]))
            for case, findings in expected.items():
                with self.subTest(format=map_format, case=case):
                    parsed = runtime_map.parse_map((directory / f"{case}.map").read_text(encoding="utf-8"))
                    self.assertEqual(parsed.format, map_format)
                    self.assertEqual(runtime_map.check_map(parsed, patterns), findings)

    def test_string_literals_and_object_stems_never_count(self) -> None:
        text = (MAP_FIXTURES / "msvc" / "Clean.map").read_text(encoding="utf-8")
        parsed = runtime_map.parse_map(text)
        names = [symbol.name for symbol in parsed.symbols]
        self.assertTrue(any("??_C@" in name and "ImGui" in name for name in names))  # a string literal "ImGui"
        self.assertTrue(any(symbol.origin.endswith("Watchdog1.obj") for symbol in parsed.symbols))
        self.assertEqual(runtime_map.check_map(parsed, runtime_map.family_patterns(REPOSITORY_ROOT)), [])

    def test_text_that_is_no_map_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            runtime_map.parse_map("not a map file\n")
        # A recognized header without a single symbol is refused too.
        with self.assertRaises(ValueError):
            runtime_map.parse_map("  Address         Publics by Value              Rva+Base               Lib:Object\n")

    def test_lld_link_coff_maps_read_as_lld(self) -> None:
        text = "\n".join([
            "Address  Size     Align Out     In      Symbol",
            "00001000 00002000  4096 .text",
            "00001000 00000040    16         ImGui.lib(imgui.obj):(.text$mn)",
            "00001000 00000000     0                 ?Begin@ImGui@@YA_NPEBDPEA_NH@Z",
            "00001040 00000040    16         Engine.lib(FeatureTestRunner.obj):(.text$mn)",
            "00001040 00000000     0                 ?Run@FeatureTestRunner@Engine@@QEAAXXZ",
            "00001080 00000040    16         Engine.lib(ReplayPlayer.obj):(.text$mn)",
            "00001080 00000000     0                 ?Step@ReplayPlayer@Engine@@QEAAXXZ",
            "000010c0 00000040    16         Engine.lib(Application.obj):(.text$mn)",
            "000010c0 00000000     0                 ??_C@_05OCLPBJCB@ImGui@",
        ]) + "\n"
        parsed = runtime_map.parse_map(text)
        self.assertEqual(parsed.format, "lld")
        self.assertEqual(parsed.symbols[0], runtime_map.MapSymbol("?Begin@ImGui@@YA_NPEBDPEA_NH@Z",
                                                                  "ImGui.lib(imgui.obj)", "ImGui.lib"))
        self.assertEqual(runtime_map.check_map(parsed, runtime_map.family_patterns(REPOSITORY_ROOT)),
                         ["forbidden ImGui: ?Begin@ImGui@@YA_NPEBDPEA_NH@Z (ImGui.lib(imgui.obj))"])

    def test_names_are_read_through_their_components_in_every_encoding(self) -> None:
        cases = [
            ("?Beat@Watchdog@Engine@@QEAAXXZ", "msvc", ["Engine", "Watchdog", "Beat"]),
            ("??_7ImGuiLayer@Engine@@6B@", "msvc", ["Engine", "ImGuiLayer"]),
            ("?value@?1??0?$type_index@W4SceneTarget@Engine@@@entt@@SAIXZ@4IB", "msvc", ["Engine", "SceneTarget"]),
            ("??1?$unique_ptr@VWatchdog@Engine@@U?$default_delete@VWatchdog@Engine@@@std@@@std@@QEAA@XZ", "msvc",
             ["Engine", "Watchdog"]),
            ("_ZN6Engine8Watchdog4BeatEv", "gnu", ["Engine", "Watchdog", "Beat"]),
            ("_ZTVN6Engine10ImGuiLayerE", "gnu", ["Engine", "ImGuiLayer"]),
            ("_ZZN6Engine3FooEvE5s_Bar", "gnu", ["Engine", "Foo"]),
            ("_ZNSt10unique_ptrIN6Engine8WatchdogESt14default_deleteIS1_EE5resetEPS1_", "lld", ["Engine", "Watchdog"]),
            ("__ZN4Luau8Frontend5checkERKNSt3__112basic_stringIcEE", "ld64", ["Luau", "Frontend", "check"]),
            ("FDE for: __ZN5ImGui5BeginEPKcPbi", "ld64", ["ImGui", "Begin"]),
            ("std::__1::unique_ptr<Engine::Watchdog, std::__1::default_delete<Engine::Watchdog> >"
             "::reset(Engine::Watchdog*)", "lld", ["Engine", "Watchdog"]),
            ("Engine::Foo(int)::{lambda(int)#1}::operator()(int) const", "lld", ["Engine", "Foo"]),
            ("public: virtual void __cdecl Engine::ImGuiLayer::Render(class nvrhi::ICommandList &)", "lld",
             ["Engine", "ImGuiLayer", "Render"]),
            ("vtable for Engine::RuntimeAutomationServer", "lld", ["Engine", "RuntimeAutomationServer"]),
        ]
        for name, map_format, components in cases:
            with self.subTest(name=name):
                self.assertIn(components, runtime_map.qualified_names(name, map_format))
        # String literals spell no name, whatever their text says; neither does a similar but longer identifier.
        for name, map_format in (("??_C@_05OCLPBJCB@ImGui@", "msvc"), ("literal string: ImGui", "ld64"),
                                 ("`string'", "lld")):
            with self.subTest(name=name):
                self.assertEqual(runtime_map.qualified_names(name, map_format), [])
        self.assertEqual(runtime_map.qualified_names("??1ScriptWatchdog@Engine@@QEAA@XZ", "msvc"),
                         [["Engine", "ScriptWatchdog"]])

    def test_families_come_from_the_headers_definitions_and_free_functions(self) -> None:
        header = """
            #pragma once
            #include "Engine/Core/Base.h"
            // class CommentedOut { };
            namespace Engine {
                class TypeRegistry; // a forward declaration names a type declared elsewhere
                struct Settings
                {
                    struct Nested { int Value = 0; };
                    [[nodiscard]] bool IsValid() const;
                };
                enum class Mode : uint8_t { A, B };
                [[nodiscard]] Result<Settings> ParseSettings(std::string_view text);
                template<typename T> [[nodiscard]] T Convert(const Settings& settings);
                inline constexpr Settings DefaultSettings{ {}, };
                inline bool IsDefault(const Settings& settings) { return !settings.IsValid(); }
                namespace Utils {
                    [[nodiscard]] Error PrefixPointers(const Error& error, std::string_view prefix);
                    struct SharedHelper { int Count = 0; };
                }
                namespace Automation {
                    [[nodiscard]] MethodResult PlayStep(MethodContext& context, const PlayStepParams& params);
                }
                namespace {
                    struct Hidden { };
                }
            }
        """
        self.assertEqual(runtime_map.declared_names(header), [
            "Engine::Automation::PlayStep", "Engine::Convert", "Engine::IsDefault", "Engine::Mode",
            "Engine::ParseSettings", "Engine::Settings", "Engine::Utils::SharedHelper",
        ])
        patterns = runtime_map.family_patterns(REPOSITORY_ROOT)
        names = {family: {pattern.pattern.replace("\\", "") for pattern in family_patterns.names}
                 for family, family_patterns in patterns.items()}
        for family, name in (("automation", "Engine::ParseRpcRequest"), ("automation", "Engine::Watchdog"),
                             ("automation", "Engine::RuntimeAutomationServer"), ("ImGui", "ImGui"),
                             ("ImGui", "Engine::ImGuiLayer"), ("ImGui", "Engine::StatsOverlayToggle"),
                             ("ImGui", "Engine::CaptureImGuiScreenshot"), ("AssetPipeline", "Engine::PakWriter"),
                             ("Analysis", "Luau::Frontend")):
            with self.subTest(family=family, name=name):
                self.assertIn(name, names[family])
        self.assertNotIn("Engine::TypeRegistry", names["automation"])
        self.assertNotIn("Engine::Utils::PrefixPointers", names["automation"])

    def test_the_luau_compiler_family_finds_the_compiler_and_the_require_library_but_not_the_parser(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 22 (B-LUAU; Roadmap M19 acceptance "The Dist map holds no
        # Luau::compile, BytecodeBuilder or extractLuauConfig symbol"): once the first-party require replaces Luau's
        # Require library, nothing in Dist reaches Luau's Compiler, Config or Require objects, so the family that
        # names them joins the forbidden families; the Parser and Ast that ScriptApiRegistry's signature parsing keeps,
        # and the VM, are no findings. The compiler shares the Luau library with the VM, so the family matches names
        # only.
        self.assertIn("LuauCompiler", runtime_map.FORBIDDEN_FAMILIES)
        self.assertEqual(runtime_map.FORBIDDEN_FAMILIES[:4], ("automation", "Analysis", "ImGui", "AssetPipeline"))
        text = "?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@"
        forbidden = [
            f"?compile@Luau@@YA?AV{text}AEBV23@AEBUCompileOptions@1@AEBUParseOptions@1@PEAVBytecodeEncoder@1@@Z",
            f"?compileOrThrow@Luau@@YAXAEAVBytecodeBuilder@1@AEBV{text}AEBUCompileOptions@1@AEBUParseOptions@1@@Z",
            "?addConstantNil@BytecodeBuilder@Luau@@QEAAHXZ",
            f"?extractLuauConfig@Luau@@YA?AV?$optional@V{text}@std@@V?$basic_string_view@DU?$char_traits@D@std@@@3@"
            "AEAUConfig@1@@Z",
            "?lua_requireinternal@Require@Luau@@YAHPEAUlua_State@@PEBD@Z",
            "luau_compile",
            "luarequire_pushproxyrequire",
        ]
        kept = ["?parseType@Parser@Luau@@QEAAPEAVAstType@2@_N@Z", "luaV_execute",
                "?Run@FeatureTestRunner@Engine@@QEAAXXZ", "?Step@ReplayPlayer@Engine@@QEAAXXZ"]
        lines = ["Address  Size     Align Out     In      Symbol", "00001000 00002000  4096 .text"]
        for index, name in enumerate([*forbidden, *kept]):
            address = 0x1000 + index * 0x40
            lines.append(f"{address:08x} 00000040    16         Luau.lib(Unit{index}.obj):(.text$mn)")
            lines.append(f"{address:08x} 00000000     0                 {name}")
        parsed = runtime_map.parse_map("\n".join(lines) + "\n")
        findings = runtime_map.check_map(parsed, runtime_map.family_patterns(REPOSITORY_ROOT))
        self.assertEqual(sorted(findings), sorted(f"forbidden LuauCompiler: {name} (Luau.lib(Unit{index}.obj))"
                                                  for index, name in enumerate(forbidden)))

    def test_the_command_line_self_test_passes_and_a_missing_map_is_exit_code_3(self) -> None:
        result = subprocess.run([sys.executable, str(CHECK_SCRIPT), "--self-test", "--json"], cwd=REPOSITORY_ROOT,
                                capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, result.stderr)
        document = json.loads(result.stdout)
        self.assertTrue(document["success"])
        self.assertEqual(len(document["steps"]), len(runtime_map.MAP_FORMATS))
        with tempfile.TemporaryDirectory(prefix="RuntimeMap-") as directory:
            missing = Path(directory) / "Runtime.map"
            result = subprocess.run([sys.executable, str(CHECK_SCRIPT), "--map", str(missing)], cwd=REPOSITORY_ROOT,
                                    capture_output=True, text=True, timeout=300)
            self.assertEqual(result.returncode, 3, result.stdout)
            dirty = Path(directory) / "ImGui.map"
            dirty.write_text((MAP_FIXTURES / "msvc" / "ImGui.map").read_text(encoding="utf-8"), encoding="utf-8")
            result = subprocess.run([sys.executable, str(CHECK_SCRIPT), "--map", str(dirty), "--json"],
                                    cwd=REPOSITORY_ROOT, capture_output=True, text=True, timeout=300)
            self.assertEqual(result.returncode, 1, result.stderr)
            document = json.loads(result.stdout)
            self.assertEqual(document["steps"][0]["findings"],
                             json.loads((MAP_FIXTURES / "msvc" / "Expected.json").read_text(encoding="utf-8"))["ImGui"])


if __name__ == "__main__":
    unittest.main()
