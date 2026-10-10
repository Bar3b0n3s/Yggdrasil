#include "TestsPCH.h"
#include "Editor/Panels/DiagnosticsPanel.h"

#include "Editor/SupportingPanelTestUi.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("DiagnosticsPanel: script findings retain independent details with matching hash suffixes")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			EditorContext& editor = fixture.GetEditor();
			REQUIRE(editor.GetScriptService() != nullptr);
			for (const char* name : { "First###same.luau", "Second###same.luau" })
			{
				const auto path = VfsPath::Create("project", std::string("Assets/") + name);
				REQUIRE(path);
				const auto written = editor.GetScriptService()->Write(*path, "--!strict\nlocal value: number = \"type failure\"\nreturn value");
				REQUIRE_MESSAGE(written.has_value(), (written ? "" : written.error().ToString()));
				// Write returns findings directly; loading publishes the import check consumed by the panel.
				const auto loaded = editor.GetScriptService()->GetFields(written->Script);
				REQUIRE_MESSAGE(loaded.has_value(), (loaded ? "" : loaded.error().ToString()));
				const auto check = editor.GetAssets().GetScriptCheck(written->Script);
				REQUIRE_MESSAGE(check.has_value(), (check ? "" : check.error().ToString()));
				REQUIRE(check->Diagnostics.size() == 1);
			}
			std::filesystem::path opened;
			uint32_t line = 0;
			fixture.GetContext().OpenSource = [&opened, &line](const std::filesystem::path& path, uint32_t sourceLine) -> Status
			{
				opened = path;
				line = sourceLine;
				return {};
			};
			struct DiagnosticDetails
			{
				DiagnosticsPanel Panel{};
				[[nodiscard]] Status Draw(EditorPanelContext& context)
				{
					ImGui::LogFinish(); // Keep the real collapsed/expanded state instead of logging's auto-expansion.
					return Panel.Draw(context);
				}
			} panel;
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Search messages, files or codes");
			fixture.ReplaceFocusedText(panel, "-Second");
			CHECK_FALSE(Test::SupportingPanelTestUi::FindText(fixture, panel, "Code: SCRIPT_TYPE_ERROR").has_value());
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Details");
			CHECK(Test::SupportingPanelTestUi::FindText(fixture, panel, "Code: SCRIPT_TYPE_ERROR").has_value());
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Open source");
			CHECK(opened == editor.GetProject().GetRoot() / "Assets/First###same.luau");
			CHECK(line == 2);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "-Second");
			fixture.ReplaceFocusedText(panel, "-First");
			CHECK_FALSE(Test::SupportingPanelTestUi::FindText(fixture, panel, "Code: SCRIPT_TYPE_ERROR").has_value());
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Open source");
			CHECK(opened == editor.GetProject().GetRoot() / "Assets/Second###same.luau");
			CHECK(line == 2);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Details");
			CHECK(Test::SupportingPanelTestUi::FindText(fixture, panel, "Code: SCRIPT_TYPE_ERROR").has_value());
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "-First");
			fixture.ReplaceFocusedText(panel, "-Second");
			CHECK(Test::SupportingPanelTestUi::FindText(fixture, panel, "Code: SCRIPT_TYPE_ERROR").has_value());
		}

		TEST_CASE("DiagnosticsPanel: latest failed import findings replace old type ranges while retaining the last good asset")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			auto& editor = fixture.GetEditor();
			const auto path = VfsPath::Create("project", "Assets/Diagnostic.luau");
			REQUIRE(path);
			REQUIRE(editor.GetScriptService() != nullptr);
			const auto written = editor.GetScriptService()->Write(*path, "--!strict\nlocal value: number = \"old type failure\"\nreturn value");
			REQUIRE(written);
			const auto good = editor.GetScriptService()->GetFields(written->Script);
			REQUIRE(good);
			const auto first = editor.GetAssets().GetScriptCheck(written->Script);
			REQUIRE(first);
			REQUIRE_FALSE(first->Diagnostics.empty());
			DiagnosticsPanel panel;
			std::filesystem::path opened;
			uint32_t line = 0;
			fixture.GetContext().OpenSource = [&opened, &line](const std::filesystem::path& file, uint32_t sourceLine) -> Status
			{
				opened = file;
				line = sourceLine;
				return {};
			};
			const std::string initial = fixture.Draw(panel);
			CHECK(initial.contains("SCRIPT_TYPE_ERROR"));
			CHECK(initial.contains("Assets/Diagnostic.luau:2:"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Open source");
			CHECK(opened == editor.GetProject().GetRoot() / "Assets/Diagnostic.luau");
			CHECK(line == 2);
			REQUIRE(editor.GetVfs().WriteFileAtomic(*path, AsBytes("return function(\n")));
			{
				Test::ExpectLog expected(LogLevel::Error, "Diagnostic.luau");
				CHECK_FALSE(editor.GetAssets().Reimport(written->Script));
			}
			const auto failed = editor.GetAssets().GetScriptCheck(written->Script);
			REQUIRE(failed);
			CHECK(failed->SourceHash != first->SourceHash);
			REQUIRE_FALSE(failed->Diagnostics.empty());
			const std::string refreshed = fixture.Draw(panel);
			CHECK(refreshed.contains(failed->Diagnostics.front().Message));
			CHECK_FALSE(refreshed.contains(first->Diagnostics.front().Message));
			const auto retained = editor.GetScriptService()->GetFields(written->Script);
			REQUIRE(retained);
			CHECK(*retained == *good);
		}

		TEST_CASE("DiagnosticsPanel: active session errors display context counts and traceback without mutating the scene")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			REQUIRE(fixture.GetClient().Call("play.start", Json::object()));
			PlaySession* session = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(session != nullptr);
			ScriptError error{ .Kind = ScriptErrorKind::Runtime, .Script = "Assets/Runtime.luau", .Line = 8, .Column = 3, .Message = "runtime failure", .Callback = "OnFixedUpdate", .EntityName = "Scripted entity", .Traceback = { { .Script = "Assets/Runtime.luau", .Line = 8, .Function = "OnFixedUpdate" } } };
			static_cast<void>(session->GetScriptErrors().Add(error));
			static_cast<void>(session->GetScriptErrors().Add(error));
			const auto revision = session->GetScene().GetRevision();
			DiagnosticsPanel panel;
			const std::string text = fixture.Draw(panel);
			CHECK(text.contains("runtime failure"));
			CHECK(text.contains("OnFixedUpdate"));
			CHECK(text.contains("Scripted entity"));
			CHECK(text.contains("2 occurrences"));
			CHECK(session->GetScene().GetRevision() == revision);
			REQUIRE(fixture.GetClient().Call("play.stop", Json::object()));
			CHECK_FALSE(fixture.Draw(panel).contains("runtime failure"));
		}

		TEST_CASE("DiagnosticsPanel: selected fixes run as queued human commands and refresh the report")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			REQUIRE(fixture.GetClient().Call("project.setSettings", Json{ { "patch", Json{ { "StartScene", "Assets/Scenes/OtherMissing.scene" }, { "Export", Json{ { "BuildScenes", Json::array({ "Assets/Scenes/Absent.scene" }) } } } } } }));
			DiagnosticsPanel panel;
			fixture.Draw(panel);
			fixture.Pump();
			REQUIRE(fixture.Draw(panel).contains("BUILD_START_SCENE_MISSING"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Search messages, files or codes");
			ImGui::GetIO().AddInputCharactersUTF8("BUILD_SCENE_MISSING");
			const auto filtered = fixture.Draw(panel);
			REQUIRE(filtered.contains("BUILD_SCENE_MISSING"));
			CHECK_FALSE(filtered.contains("BUILD_START_SCENE_MISSING"));
			const uint64_t revision = fixture.GetEditor().GetRevision();
			fixture.GetContext().AutomationControls.SetPolicy({ .DenyMutations = true });
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Include in fix");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Fix selected");
			CHECK(fixture.GetEditor().GetRevision() == revision);
			CHECK(fixture.GetEditor().GetProject().GetSettings().Export.BuildScenes.size() == 1);
			fixture.Pump();
			fixture.Draw(panel);
			CHECK(fixture.GetEditor().GetProject().GetSettings().Export.BuildScenes.empty());
			const auto entries = fixture.GetEditor().GetHistory().GetEntries(1);
			REQUIRE(entries.size() == 1);
			CHECK(entries[0].Origin == CommandOrigin::User);
			fixture.Pump();
			CHECK_FALSE(fixture.Draw(panel).contains("Export.BuildScenes names"));
		}

		TEST_CASE("DiagnosticsPanel: queued validation displays stable fixable project diagnostics")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			REQUIRE(fixture.GetClient().Call("project.setSettings", Json{ { "patch", Json{ { "Export", Json{ { "BuildScenes", Json::array({ "Assets/Scenes/Absent.scene" }) } } } } } }));
			DiagnosticsPanel panel;
			CHECK(fixture.Draw(panel).contains("Validation queued"));
			const uint64_t revision = fixture.GetEditor().GetRevision();
			fixture.Pump();
			const std::string first = fixture.Draw(panel);
			CHECK(first.contains("BUILD_SCENE_MISSING"));
			CHECK(first.contains("Project validation:"));
			CHECK(fixture.Draw(panel) == first);
			CHECK(fixture.GetEditor().GetRevision() == revision);
		}
	}

}
