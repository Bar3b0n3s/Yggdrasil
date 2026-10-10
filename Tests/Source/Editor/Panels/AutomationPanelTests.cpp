#include "TestsPCH.h"
#include "Editor/Panels/AutomationPanel.h"

#include "Editor/SupportingPanelTestUi.h"
#include "Engine/Core/VirtualFileSystem.h"

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
	}

}
