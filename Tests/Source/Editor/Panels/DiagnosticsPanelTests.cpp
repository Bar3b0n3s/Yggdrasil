#include "TestsPCH.h"
#include "Editor/Panels/DiagnosticsPanel.h"

#include "Editor/Panels/UtilityPanelFixture.h"

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("DiagnosticsPanel: selected fixes run as queued human commands and refresh the report")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			REQUIRE(fixture.GetClient().Call("project.setSettings", Json{ { "patch", Json{ { "StartScene", "Assets/Scenes/OtherMissing.scene" }, { "Export", Json{ { "BuildScenes", Json::array({ "Assets/Scenes/Absent.scene" }) } } } } } }));
			DiagnosticsPanel panel;
			fixture.Draw(panel);
			fixture.Pump();
			REQUIRE(fixture.Draw(panel).contains("BUILD_START_SCENE_MISSING"));
			fixture.ClickAt(panel, 100.0f, 79.0f);
			ImGui::GetIO().AddInputCharactersUTF8("BUILD_SCENE_MISSING");
			const auto filtered = fixture.Draw(panel);
			REQUIRE(filtered.contains("BUILD_SCENE_MISSING"));
			CHECK_FALSE(filtered.contains("BUILD_START_SCENE_MISSING"));
			const uint64_t revision = fixture.GetEditor().GetRevision();
			fixture.GetContext().AutomationControls.SetPolicy({ .DenyMutations = true });
			fixture.ClickAt(panel, 16.0f, 140.0f);
			fixture.ClickAt(panel, 50.0f, 102.0f);
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
			CHECK(first.contains("Script type checking unavailable"));
			CHECK(fixture.Draw(panel) == first);
			CHECK(fixture.GetEditor().GetRevision() == revision);
		}
	}

}
