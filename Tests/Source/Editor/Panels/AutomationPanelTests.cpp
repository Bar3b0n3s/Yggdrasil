#include "TestsPCH.h"
#include "Editor/Panels/AutomationPanel.h"

#include "Editor/Panels/UtilityPanelFixture.h"
#include "Engine/Core/VirtualFileSystem.h"

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("AutomationPanel: human controls pause and deny agent work")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			AutomationPanel panel;
			fixture.Draw(panel);
			fixture.ClickAt(panel, 16.0f, 39.0f);
			CHECK(fixture.GetContext().AutomationControls.GetPolicy().Paused);
			const auto paused = fixture.GetClient().Request("project.info", Json::object());
			CHECK(paused["error"]["code"] == Json(std::to_underlying(RpcErrorCode::Busy)));
			fixture.ClickAt(panel, 16.0f, 39.0f);
			fixture.ClickAt(panel, 16.0f, 62.0f);
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
			fixture.ClickFirstItem(panel);
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
