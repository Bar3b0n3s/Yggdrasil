#include "TestsPCH.h"

#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/VirtualFileSystem.h"

#include "Support/EditorTestFixture.h"

#include <algorithm>
#include <array>

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
	}

}
