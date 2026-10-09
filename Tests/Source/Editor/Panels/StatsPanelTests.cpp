#include "TestsPCH.h"
#include "Editor/Panels/StatsPanel.h"

#include "Editor/Panels/UtilityPanelFixture.h"

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("StatsPanel: queued CPU statistics label absent script measurements")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			REQUIRE(fixture.GetClient().Call("entity.create", Json{ { "name", "Stats target" } }));
			StatsPanel panel;
			CHECK(fixture.Draw(panel).contains("Statistics unavailable"));
			const uint64_t revision = fixture.GetEditor().GetRevision();
			fixture.Pump();
			const std::string text = fixture.Draw(panel);
			CHECK(text.contains("CPU"));
			CHECK(text.contains("Entities 1 | Bodies 0 | Voices 0"));
			CHECK(text.contains("Luau heap unavailable"));
			CHECK(text.contains("Render statistics unavailable"));
			CHECK(fixture.GetEditor().GetRevision() == revision);
			REQUIRE(fixture.GetClient().Call("entity.create", Json{ { "name", "Second stats target" } }));
			fixture.Pump();
			CHECK(fixture.Draw(panel).contains("Entities 2 | Bodies 0 | Voices 0"));
		}
	}

}
