#include "TestsPCH.h"
#include "EditorCore/Scripting/EditorScriptService.h"

#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/EditorTestFixture.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		class EmptyScriptDiagnostics final : public IScriptDiagnosticsProvider
		{
		public:
			uint64_t GetEnvironmentHash() const override { return 1; }
			std::vector<ScriptDiagnostic> CheckScript(const ScriptCheckRequest& /*request*/) override
			{
				return {};
			}
		};

		VfsPath ScriptServicePath(std::string_view path)
		{
			auto parsed = VfsPath::Create("project", path);
			REQUIRE(parsed.has_value());
			return *parsed;
		}

		void WriteScriptServiceFile(Test::EditorTestFixture& fixture, std::string_view path, std::string_view source)
		{
			REQUIRE(fixture.GetEditor().GetVfs().WriteFileAtomic(ScriptServicePath(path), AsBytes(source)));
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorScriptService: the editor owns the service only while a project is open")
		{
			Test::EditorTestFixture fixture("ScriptServiceLifetime");
			auto& editor = fixture.GetEditor();
			CHECK(editor.GetScriptService() == nullptr);
			fixture.CreateAndOpenProject();
			REQUIRE(editor.GetScriptService() != nullptr);
			const EditorContext& view = editor;
			REQUIRE(view.GetScriptService() != nullptr);
			const auto checked = view.GetScriptService()->Check({});
			REQUIRE(checked.has_value());
			CHECK(checked->Paths.empty());
			REQUIRE(editor.CloseProject());
			CHECK(editor.GetScriptService() == nullptr);
		}

		TEST_CASE("EditorScriptService: configuration changes replace diagnostics without retaining stale strict findings")
		{
			Test::EditorTestFixture fixture("ScriptConfigurationRefresh");
			fixture.CreateAndOpenProject();
			WriteScriptServiceFile(fixture, "Assets/ConfigCheck.luau", "local value: number = 'wrong'\nreturn { Value = value }\n");
			auto& editor = fixture.GetEditor();
			const std::array paths{ ScriptServicePath("Assets/ConfigCheck.luau") };
			REQUIRE(editor.GetScriptService() != nullptr);
			const auto strict = editor.GetScriptService()->Check(paths);
			REQUIRE(strict.has_value());
			CHECK(std::ranges::any_of(strict->Diagnostics, [](const ScriptDiagnostic& finding)
			{
				return finding.Severity == DiagnosticSeverity::Error;
			}));
			WriteScriptServiceFile(fixture, ".luaurc", "{\"languageMode\":\"nocheck\"}");
			REQUIRE(editor.GetScriptService() != nullptr);
			const auto unchecked = editor.GetScriptService()->Check(paths);
			REQUIRE(unchecked.has_value());
			CHECK_FALSE(std::ranges::any_of(unchecked->Diagnostics, [](const ScriptDiagnostic& finding)
			{
				return finding.Severity == DiagnosticSeverity::Error;
			}));
			WriteScriptServiceFile(fixture, ".luaurc", "{\"languageMode\":\"strict\"}");
			REQUIRE(editor.GetScriptService() != nullptr);
			const auto strictAgain = editor.GetScriptService()->Check(paths);
			REQUIRE(strictAgain.has_value());
			CHECK(strictAgain->Diagnostics == strict->Diagnostics);
		}

		TEST_CASE("EditorScriptService: published field snapshots retain their old schema across source replacement")
		{
			Test::EditorTestFixture fixture("ScriptSchemaLifetime");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			REQUIRE(editor.GetScriptService() != nullptr);
			const auto path = ScriptServicePath("Assets/Schema.luau");
			const auto first = editor.GetScriptService()->Write(path, "return Script.Define('Schema', { Fields = { First = Field.Number(2) } })");
			REQUIRE(first.has_value());
			const auto before = editor.GetScriptSchemaSnapshot();
			REQUIRE(before.has_value());
			REQUIRE((*before)->FindSchema(first->Script, "First").has_value());
			const auto second = editor.GetScriptService()->Write(path, "return Script.Define('Schema', { Fields = { Second = Field.Number(3) } })");
			REQUIRE(second.has_value());
			CHECK(second->Script == first->Script);
			// Writes return current diagnostics; immutable schemas change when the scheduled import is published.
			editor.GetAssets().WaitIdle();
			const auto after = editor.GetScriptSchemaSnapshot();
			REQUIRE(after.has_value());
			CHECK((*after)->FindSchema(first->Script, "Second").has_value());
			CHECK_FALSE((*after)->FindSchema(first->Script, "First").has_value());
			CHECK((*before)->FindSchema(first->Script, "First").has_value());
			CHECK_FALSE((*before)->FindSchema(first->Script, "Second").has_value());
		}
		TEST_CASE("EditorScriptService: source operations require an open project")
		{
			Test::EditorTestFixture fixture("ScriptServiceLauncher");
			EmptyScriptDiagnostics diagnostics;
			EditorScriptService service(fixture.GetEditor(), diagnostics);
			const VfsPath path = ScriptServicePath("Assets/Scripts/Module.luau");
			const auto read = service.Read(path);
			REQUIRE_FALSE(read.has_value());
			CHECK(read.error().GetCode() == ErrorCode::InvalidState);
			const auto write = service.Write(path, "return 1");
			REQUIRE_FALSE(write.has_value());
			CHECK(write.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("EditorScriptService: writes reject files outside Assets and invalid source encoding")
		{
			Test::EditorTestFixture fixture("ScriptServiceInput");
			fixture.CreateAndOpenProject();
			EmptyScriptDiagnostics diagnostics;
			EditorScriptService service(fixture.GetEditor(), diagnostics);
			for (const std::string_view name : std::array{ "Library/Module.luau", "Assets/Module.txt" })
			{
				const auto written = service.Write(ScriptServicePath(name), "return 1");
				REQUIRE_FALSE(written.has_value());
				CHECK(written.error().GetCode() == ErrorCode::InvalidArgument);
			}
			const std::string invalidUtf8(1, static_cast<char>(0xff));
			const auto written = service.Write(ScriptServicePath("Assets/Module.luau"), invalidUtf8);
			REQUIRE_FALSE(written.has_value());
			CHECK(written.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK_FALSE(service.Read(ScriptServicePath("Assets/Module.luau")).has_value());
		}

		TEST_CASE("EditorScriptService: writing unchanged source preserves the handle without another undo step")
		{
			Test::EditorTestFixture fixture("ScriptServiceNoOp");
			fixture.CreateAndOpenProject();
			EmptyScriptDiagnostics diagnostics;
			EditorScriptService service(fixture.GetEditor(), diagnostics);
			const VfsPath path = ScriptServicePath("Assets/Scripts/Module.luau");
			const auto first = service.Write(path, "return 42\n");
			REQUIRE(first.has_value());
			const auto same = service.Write(path, "return 42\n");
			REQUIRE(same.has_value());
			CHECK(same->Script == first->Script);
			CHECK(same->UndoIndex == 0);
			const auto source = service.Read(path);
			REQUIRE(source.has_value());
			CHECK(*source == "return 42\n");
		}

		TEST_CASE("EditorScriptService: a missing or misspelled required module is a located finding, not a failed check")
		{
			Test::EditorTestFixture fixture("ScriptServiceMissingRequire");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			REQUIRE(editor.GetScriptService() != nullptr);
			WriteScriptServiceFile(fixture, "Assets/Scripts/Board.luau", "return { Value = 1 }\n");
			WriteScriptServiceFile(fixture, "Assets/Scripts/Clean.luau", "return { Value = 2 }\n");
			for (const std::string_view request : std::array{ "./Missing", "./Bord" })
			{
				CAPTURE(request);
				WriteScriptServiceFile(fixture, "Assets/Scripts/Game.luau", std::format("local Board = require(\"{}\")\nreturn Script.Define(\"Game\", {{}})\n", request));
				// Checking every project script still reports every file, the unresolved require among the findings.
				const auto checked = editor.GetScriptService()->Check({});
				REQUIRE_MESSAGE(checked.has_value(), (checked ? std::string() : checked.error().ToString()));
				CHECK(checked->Paths == std::vector<std::string>{ "Assets/Scripts/Board.luau", "Assets/Scripts/Clean.luau", "Assets/Scripts/Game.luau" });
				CHECK(std::ranges::any_of(checked->Diagnostics, [](const ScriptDiagnostic& finding)
				{
					return finding.Severity == DiagnosticSeverity::Error && finding.File.starts_with("Assets/Scripts/");
				}));
			}
			// A missing requested path is still NotFound.
			const std::array missing{ ScriptServicePath("Assets/Scripts/Absent.luau") };
			const auto absent = editor.GetScriptService()->Check(missing);
			REQUIRE_FALSE(absent.has_value());
			CHECK(absent.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("EditorScriptService: a syntax error is one compile finding and load-time failures use the compile code")
		{
			Test::EditorTestFixture fixture("ScriptServiceCompileCodes");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			REQUIRE(editor.GetScriptService() != nullptr);
			const auto syntax = editor.GetScriptService()->Write(ScriptServicePath("Assets/Syntax.luau"), "-- first\n-- second\nlocal = 1\n");
			REQUIRE(syntax.has_value());
			const auto errors = std::ranges::count_if(syntax->Diagnostics, [](const ScriptDiagnostic& finding)
			{
				return finding.Severity == DiagnosticSeverity::Error;
			});
			CHECK(errors == 1);
			REQUIRE_FALSE(syntax->Diagnostics.empty());
			CHECK(syntax->Diagnostics[0].Code == "SCRIPT_COMPILE_ERROR");
			CHECK(syntax->Diagnostics[0].File == "Assets/Syntax.luau");
			CHECK(syntax->Diagnostics[0].Line == 3);
			CHECK(syntax->Diagnostics[0].Column == 7);
			const auto loadTime = editor.GetScriptService()->Write(ScriptServicePath("Assets/LoadTime.luau"), "local T = {}\n\nT.Axis = Input.GetAxis(\"Move\")\nreturn Script.Define(\"LoadTime\", T)\n");
			REQUIRE(loadTime.has_value());
			REQUIRE(loadTime->Diagnostics.size() == 1);
			CHECK(loadTime->Diagnostics[0].Code == "SCRIPT_COMPILE_ERROR");
			CHECK(loadTime->Diagnostics[0].File == "Assets/LoadTime.luau");
			CHECK(loadTime->Diagnostics[0].Line == 3);
			CHECK(loadTime->Diagnostics[0].Message.find("load time") != std::string::npos);
		}

		TEST_CASE("EditorScriptService: the Test template names its suite after the file stem")
		{
			Test::EditorTestFixture fixture("ScriptServiceSuiteName", {}, nullptr, std::nullopt, true);
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			REQUIRE(editor.GetScriptService() != nullptr);
			const auto suite = editor.GetScriptService()->Create(ScriptServicePath("Assets/Tests/Tetris.test.luau"), ScriptTemplate::Test);
			REQUIRE_MESSAGE(suite.has_value(), (suite ? std::string() : suite.error().ToString()));
			CHECK(suite->Diagnostics.empty());
			const auto source = editor.GetScriptService()->Read(ScriptServicePath("Assets/Tests/Tetris.test.luau"));
			REQUIRE(source.has_value());
			CHECK(source->find("Test.Suite(\"Tetris\",") != std::string::npos);
			// Behaviour and Module names stay prefixed identifiers.
			const auto behaviour = editor.GetScriptService()->Create(ScriptServicePath("Assets/Scripts/Player-Controller.luau"), ScriptTemplate::Behaviour);
			REQUIRE(behaviour.has_value());
			const auto behaviourSource = editor.GetScriptService()->Read(ScriptServicePath("Assets/Scripts/Player-Controller.luau"));
			REQUIRE(behaviourSource.has_value());
			CHECK(behaviourSource->find("Script.Define(\"Script_Player_Controller\", Script_Player_Controller)") != std::string::npos);
		}
	}

}
