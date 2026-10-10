#include "TestsPCH.h"
#include "Editor/Panels/StatsPanel.h"

#include "Editor/Panels/UtilityPanelFixture.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace Engine {

	namespace Utils {

		static bool HasStatsMetricRow(std::string_view output, std::string_view label, uint32_t expected)
		{
			const auto compact = [](std::string text)
			{
				std::erase_if(text, [](char character)
				{
					return character == ' ' || character == '\t' || character == '\r' || character == '|';
				});
				return text;
			};
			const std::string expectedRow = compact(std::format("{} {}", label, expected));
			while (!output.empty())
			{
				const size_t end = output.find('\n');
				if (compact(std::string(output.substr(0, end))) == expectedRow)
					return true;
				if (end == std::string_view::npos)
					break;
				output.remove_prefix(end + 1);
			}
			return false;
		}

	}

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
			INFO(text);
			CHECK(text.contains("CPU"));
			CHECK(Utils::HasStatsMetricRow(text, "Entities", 1));
			CHECK_FALSE(Utils::HasStatsMetricRow(text, "Entities", 10));
			CHECK(Utils::HasStatsMetricRow(text, "Physics bodies", 0));
			CHECK_FALSE(Utils::HasStatsMetricRow(text, "Physics bodies", 1));
			CHECK(Utils::HasStatsMetricRow(text, "Audio voices", 0));
			CHECK_FALSE(Utils::HasStatsMetricRow(text, "Audio voices", 1));
			CHECK(text.contains("Script heap"));
			CHECK(text.contains("Unavailable (no active script VM)"));
			CHECK(text.contains("Render statistics unavailable"));
			CHECK(fixture.GetEditor().GetRevision() == revision);
			REQUIRE(fixture.GetClient().Call("entity.create", Json{ { "name", "Second stats target" } }));
			fixture.Pump();
			const std::string refreshed = fixture.Draw(panel);
			INFO(refreshed);
			CHECK(Utils::HasStatsMetricRow(refreshed, "Entities", 2));
			CHECK_FALSE(Utils::HasStatsMetricRow(refreshed, "Entities", 1));
			CHECK(Utils::HasStatsMetricRow(refreshed, "Physics bodies", 0));
			CHECK(Utils::HasStatsMetricRow(refreshed, "Audio voices", 0));
		}
	}

}
