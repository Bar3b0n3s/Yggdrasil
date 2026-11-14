#include "TestsPCH.h"
#include "Editor/Panels/DiagnosticsPanel.h"

#include "Editor/SupportingPanelTestUi.h"
#include "EditorCore/Automation/GraphMethods.h"
#include "EditorCore/Graph/GraphEditorTarget.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"

#include <algorithm>
#include <string_view>

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

		TEST_CASE("DiagnosticsPanel: filtered diagnostic fixes remain queued human commands")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			REQUIRE(fixture.GetClient().Call("project.setSettings", Json{ { "patch", Json{ { "StartScene", "Assets/Scenes/OtherMissing.scene" }, { "Export", Json{ { "BuildScenes", Json::array({ "Assets/Scenes/Absent.scene" }) } } } } } }));
			DiagnosticsPanel panel;
			fixture.Draw(panel);
			fixture.Pump();
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Search messages, files or codes");
			ImGui::GetIO().AddInputCharactersUTF8("build_scene_missing");
			const std::string filtered = fixture.Draw(panel);
			CHECK(filtered.contains("BUILD_SCENE_MISSING"));
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
			CHECK(entries.front().Origin == CommandOrigin::User);
			fixture.Pump();
			CHECK_FALSE(fixture.Draw(panel).contains("Export.BuildScenes names"));
		}

		TEST_CASE("DiagnosticsPanel: a project change releases its abandoned validation action")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			auto& actions = fixture.GetContext().Actions;
			// Tickets are sequential, so the action the panel queues next is the one after this probe.
			const auto probe = actions.Submit("scene.tree", Json::object());
			REQUIRE(probe);
			actions.Pump();
			REQUIRE(actions.TakeResult(*probe));
			DiagnosticsPanel panel;
			fixture.Draw(panel);
			const uint64_t validation = *probe + 1;
			const auto queued = actions.TakeResult(validation);
			REQUIRE(queued);
			CHECK_FALSE(queued->has_value());
			REQUIRE(fixture.GetEditor().CloseProject());
			fixture.Draw(panel);
			const auto released = actions.TakeResult(validation);
			REQUIRE_FALSE(released);
			CHECK(released.error().GetCode() == ErrorCode::NotFound);
		}

		// M24 (Docs/Decisions/0035-m24-contract.md decision 15; C24.1's review finding 16): a finding of a graph opens the Graph
		// Editor at its node, never the external editor at the .graph JSON: the import check's GRAPH_INVALID findings by their
		// message, project.validate's by their pointer and a session's runtime errors by their line of the graph's text, each
		// resolved to the node graph.read gives.
		TEST_CASE("DiagnosticsPanel: a graph's finding opens the Graph Editor at its node instead of its source")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			EditorContext& editor = fixture.GetEditor();
			Test::AutomationTestClient& client = fixture.GetClient();
			constexpr std::string_view Path = "Assets/Scripts/Broken.graph";
			REQUIRE(client.Call("graph.create", Json{ { "path", std::string(Path) } }));
			REQUIRE(client.Call("graph.addNode", Json{ { "graph", std::string(Path) }, { "kind", "Event.OnStart" } }));
			// Compare.Equal's A and B take any value and have no default: unlinked, they leave the graph unfinished, which does not
			// import (GRAPH_INVALID at /Nodes/1/Values/A and /B).
			const Result<Json> added = client.Call("graph.addNode", Json{ { "graph", std::string(Path) }, { "kind", "Compare.Equal" } });
			REQUIRE(added.has_value());
			const uint32_t equal = JsonReader((*added)["node"]).ReadUInt32().value_or(0);
			editor.GetAssets().WaitIdle();
			const std::optional<AssetHandle> graph = editor.GetAssets().Resolve(Path);
			REQUIRE(graph.has_value());
			{
				// The import publishes its check, then fails for the graph's issues.
				const Test::ExpectLog failed(LogLevel::Error, "Broken.graph");
				CHECK_FALSE(editor.GetAssets().Reimport(*graph));
			}
			const Result<ScriptImportCheck> check = editor.GetAssets().GetScriptCheck(*graph);
			REQUIRE(check.has_value());
			REQUIRE(std::ranges::any_of(check->Diagnostics, [](const ScriptDiagnostic& finding)
			{
				return finding.Code == GraphInvalidCode;
			}));
			bool opened = false;
			fixture.GetContext().OpenSource = [&opened](const std::filesystem::path&, uint32_t) -> Status
			{
				opened = true;
				return {};
			};
			const Result<Json> readJson = client.Call("graph.read", Json{ { "graph", std::string(Path) }, { "luau", true } });
			REQUIRE(readJson.has_value());
			GraphReadResult read;
			REQUIRE(editor.GetTypeRegistry().FindStruct<GraphReadResult>()->FromJson(&read, JsonReader(*readJson), {}));

			// The import check's finding (the first entries), then project.validate's (after the report arrives).
			DiagnosticsPanel panel;
			static_cast<void>(fixture.Draw(panel));
			fixture.Pump();
			CHECK(fixture.Draw(panel).contains("Open in Graph Editor"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Open in Graph Editor");
			std::optional<GraphEditorTarget> request = panel.TakeGraphRequest();
			REQUIRE(request.has_value());
			CHECK(request->Graph == *graph);
			CHECK(request->Finding.starts_with("/Nodes/1/Values/"));
			CHECK(ResolveGraphEditorTarget(*request, read) == equal);
			CHECK_FALSE(panel.TakeGraphRequest().has_value());
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Open in Graph Editor", 2);
			request = panel.TakeGraphRequest();
			REQUIRE(request.has_value());
			CHECK(request->Graph == *graph);
			CHECK(request->JsonPointer.starts_with("/Nodes/1/Values/"));
			CHECK(request->Finding.empty());
			CHECK(ResolveGraphEditorTarget(*request, read) == equal);

			// A runtime error of the graph's text, at its line: the graph is finished first, since play refuses asset errors.
			REQUIRE(client.Call("graph.removeNodes", Json{ { "graph", std::string(Path) }, { "nodes", Json::array({ equal }) } }));
			editor.GetAssets().WaitIdle();
			REQUIRE(editor.GetAssets().Reimport(*graph));
			const Result<Json> started = client.Call("play.start", Json::object());
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			PlaySession* session = editor.GetPlay().GetSession();
			REQUIRE(session != nullptr);
			const ScriptError error{ .Kind = ScriptErrorKind::Runtime, .Script = std::string(Path), .Line = 7, .Column = 3, .Message = "graph runtime failure", .Callback = "OnStart", .EntityName = "Lamp", .Traceback = {} };
			static_cast<void>(session->GetScriptErrors().Add(error));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Search messages, files or codes");
			ImGui::GetIO().AddInputCharactersUTF8("graph runtime failure");
			static_cast<void>(fixture.Draw(panel));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Open in Graph Editor");
			request = panel.TakeGraphRequest();
			REQUIRE(request.has_value());
			CHECK(request->Graph == *graph);
			CHECK(request->Line == 7);
			CHECK_FALSE(opened);
			REQUIRE(client.Call("play.stop", Json::object()));
		}
	}

}
