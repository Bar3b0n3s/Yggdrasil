#include "TestsPCH.h"
#include "Editor/Panels/UndoHistoryPanel.h"

#include "Editor/Panels/UtilityPanelFixture.h"

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("UndoHistoryPanel: human undo is queued and preserves agent labels")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			REQUIRE(fixture.GetClient().Call("entity.create", Json{ { "name", "History target" } }));
			UndoHistoryPanel panel;
			const std::string text = fixture.Draw(panel);
			CHECK(text.contains("[agent]"));
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const size_t before = fixture.GetEditor().GetHistory().GetUndoCount();
			fixture.ClickFirstItem(panel);
			CHECK(fixture.GetEditor().GetRevision() == revision);
			fixture.Pump();
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() + 1 == before);
			CHECK(fixture.GetEditor().GetHistory().GetRedoCount() == 1);
			CHECK(fixture.Draw(panel).contains("Current position"));
			fixture.ClickAt(panel, 70.0f, 16.0f);
			CHECK(fixture.GetEditor().GetHistory().GetRedoCount() == 1);
			fixture.Pump();
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() == before);
			CHECK(fixture.GetEditor().GetHistory().GetRedoCount() == 0);
		}
	}

}
