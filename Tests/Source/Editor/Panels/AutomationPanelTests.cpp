#include "TestsPCH.h"
#include "Editor/Panels/AutomationPanel.h"

#include "Editor/SupportingPanelTestUi.h"
#include "EditorCore/EditorPreferences.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/ExpectLog.h"

#include <string>
#include <vector>

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("AutomationPanel: request details keep distinct client names with matching hash suffixes")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			AutomationServer& server = fixture.GetClient().GetServer();
			for (const char* name : { "First###same", "Second###same" })
			{
				const ClientId client = server.ConnectInProcess(name);
				server.SubmitInProcess(client, RpcRequest{ .Id = "job###same", .Method = "project.info", .Params = Json::object(), .TranscriptLine = {} });
				server.Pump();
				const auto responses = server.TakeInProcessResponses(client);
				REQUIRE(responses.size() == 1);
				REQUIRE(responses.front().contains("result"));
				server.DisconnectInProcess(client);
			}
			REQUIRE(fixture.GetContext().AutomationControls.GetRecentRequests().size() == 2);
			struct RequestDetails
			{
				AutomationPanel Panel{};
				[[nodiscard]] Status Draw(EditorPanelContext& context)
				{
					ImGui::LogFinish(); // Logging expands every tree; exercise the actual user-controlled Details state.
					return Panel.Draw(context);
				}
			} panel;
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Search client or method");
			fixture.ReplaceFocusedText(panel, "-Second");
			CHECK_FALSE(Test::SupportingPanelTestUi::FindText(fixture, panel, "Method: project.info").has_value());
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Details");
			CHECK(Test::SupportingPanelTestUi::FindText(fixture, panel, "Method: project.info").has_value());
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "-Second");
			fixture.ReplaceFocusedText(panel, "-First");
			CHECK_FALSE(Test::SupportingPanelTestUi::FindText(fixture, panel, "Method: project.info").has_value());
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Details");
			CHECK(Test::SupportingPanelTestUi::FindText(fixture, panel, "Method: project.info").has_value());
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "-First");
			fixture.ReplaceFocusedText(panel, "-Second");
			CHECK(Test::SupportingPanelTestUi::FindText(fixture, panel, "Method: project.info").has_value());
		}

		TEST_CASE("AutomationPanel: human controls pause and deny agent work")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			AutomationPanel panel;
			fixture.Draw(panel);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Pause agent requests");
			CHECK(fixture.GetContext().AutomationControls.GetPolicy().Paused);
			const auto paused = fixture.GetClient().Request("project.info", Json::object());
			CHECK(paused["error"]["code"] == Json(std::to_underlying(RpcErrorCode::Busy)));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Pause agent requests");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Deny agent mutations");
			CHECK(fixture.GetContext().AutomationControls.GetPolicy().DenyMutations);
			const auto denied = fixture.GetClient().Call("entity.create", Json{ { "name", "Denied from panel" } });
			REQUIRE_FALSE(denied);
			CHECK(denied.error().GetCode() == ErrorCode::PermissionDenied);
			CHECK(fixture.Draw(panel).contains("Failed"));
		}

		TEST_CASE("AutomationPanel: listener preference waits for the host safe point")
		{
			Test::UtilityPanelFixture fixture;
			AutomationPanel panel;
			fixture.Draw(panel);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Allow AI automation");
			const auto path = VfsPath::Parse("user://Editor.json");
			REQUIRE(path);
			CHECK_FALSE(fixture.GetEditor().GetVfs().Exists(*path));
			CHECK(fixture.GetClient().GetServer().GetPort() == 0);
			fixture.Pump();
			const auto preferences = ReadEditorPreferences(fixture.GetEditor().GetVfs());
			REQUIRE(preferences);
			CHECK(preferences->AllowAiAutomation);
			CHECK(fixture.GetClient().GetServer().GetPort() != 0);
			CHECK(fixture.Draw(panel).contains("Listening on localhost:"));
		}

		TEST_CASE("AutomationPanel: outcome and method filters compose")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			REQUIRE(fixture.GetClient().Call("project.info", Json::object()));
			REQUIRE(fixture.GetClient().Call("entity.create", Json{ { "name", "Allowed" } }));
			AutomationPanel panel;
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Deny agent mutations");
			CHECK(fixture.GetContext().AutomationControls.GetPolicy().DenyMutations);
			const auto denied = fixture.GetClient().Call("entity.create", Json{ { "name", "Denied" } });
			REQUIRE_FALSE(denied);
			CHECK(denied.error().GetCode() == ErrorCode::PermissionDenied);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Search client or method");
			ImGui::GetIO().AddInputCharactersUTF8("ENTITY.CREATE");
			fixture.Draw(panel);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "All requests");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Failed");
			const std::string failures = fixture.Draw(panel);
			CHECK(failures.contains("entity.create"));
			CHECK_FALSE(failures.contains("project.info"));
			CHECK_FALSE(failures.contains("Completed"));
		}

		TEST_CASE("AutomationPanel: the external editor setting queues one preference write for the safe point")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			AutomationPanel panel;
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Executable, such as code");
			fixture.ReplaceFocusedText(panel, "code");
			fixture.Press(panel, ImGuiKey_Tab); // the arguments field: one argument per line
			ImGui::GetIO().AddInputCharactersUTF8("--goto");
			fixture.Draw(panel);
			fixture.Press(panel, ImGuiKey_Enter);
			ImGui::GetIO().AddInputCharactersUTF8("{path}:{line}");
			fixture.Draw(panel);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Save editor");
			CHECK(fixture.GetPreferences().GetExternalEditorState().Pending);
			CHECK(fixture.Draw(panel).contains("Saving the external editor"));
			// Drawing and clicking never write the preference; the host's safe point does.
			const auto queued = ReadEditorPreferences(fixture.GetEditor().GetVfs());
			REQUIRE(queued);
			CHECK(queued->ExternalEditorExecutable.empty());
			fixture.Pump();
			const auto saved = ReadEditorPreferences(fixture.GetEditor().GetVfs());
			REQUIRE(saved);
			CHECK(saved->ExternalEditorExecutable == "code");
			CHECK(saved->ExternalEditorArguments == std::vector<std::string>{ "--goto", "{path}:{line}" });
			const EditorExternalEditorState state = fixture.GetPreferences().GetExternalEditorState();
			CHECK_FALSE(state.Pending);
			CHECK_FALSE(state.Failure.has_value());
			CHECK_FALSE(fixture.Draw(panel).contains("Saving the external editor"));
		}

		TEST_CASE("AutomationPanel: an external editor without {path} is refused and explained in the panel")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			AutomationPanel panel;
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Executable, such as code");
			fixture.ReplaceFocusedText(panel, "code");
			fixture.Press(panel, ImGuiKey_Tab);
			ImGui::GetIO().AddInputCharactersUTF8("--new-window");
			fixture.Draw(panel);
			const Test::ExpectLog refused(LogLevel::Error, "{path}");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Save editor");
			CHECK_FALSE(fixture.GetPreferences().GetExternalEditorState().Pending);
			CHECK(fixture.Draw(panel).contains("must contain {path}"));
			CHECK(refused.GetMatchCount() == 1);
			fixture.Pump();
			const auto stored = ReadEditorPreferences(fixture.GetEditor().GetVfs());
			REQUIRE(stored);
			CHECK(stored->ExternalEditorExecutable.empty());
		}

		TEST_CASE("AutomationPanel: two different preference failures are each logged once and both shown")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			// Both host failures persist until another change of their preference is queued.
			fixture.GetContext().AutomationPreferences.GetState = []()
			{
				return EditorAutomationPreferenceState{ .Allowed = false, .Pending = false, .Failure = Error(ErrorCode::Io, "listener port unavailable") };
			};
			fixture.GetContext().ExternalEditor.GetState = []()
			{
				return EditorExternalEditorState{ .Executable = {}, .Arguments = {}, .Pending = false, .Failure = Error(ErrorCode::Io, "Editor.json is read-only") };
			};
			AutomationPanel panel;
			const Test::ExpectLog listener(LogLevel::Error, "listener port unavailable");
			const Test::ExpectLog preferences(LogLevel::Error, "Editor.json is read-only");
			std::string text;
			for (int frame = 0; frame < 5; ++frame)
				text = fixture.Draw(panel);
			CHECK(listener.GetMatchCount() == 1);
			CHECK(preferences.GetMatchCount() == 1);
			CHECK(text.contains("listener port unavailable"));
			CHECK(text.contains("Editor.json is read-only"));
		}

		TEST_CASE("AutomationPanel: editor arguments are split one per line without a shell")
		{
			CHECK(AutomationPanel::SplitEditorArguments("").empty());
			CHECK(AutomationPanel::SplitEditorArguments("--goto\r\n{path}:{line}\n\n\"quoted words\" & ;")
				== std::vector<std::string>{ "--goto", "{path}:{line}", "\"quoted words\" & ;" });
			CHECK(AutomationPanel::SplitEditorArguments("{path}\n") == std::vector<std::string>{ "{path}" });
		}
	}

}
