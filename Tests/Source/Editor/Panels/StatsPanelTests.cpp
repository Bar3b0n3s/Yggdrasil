#include "TestsPCH.h"
#include "Editor/Panels/StatsPanel.h"

#include "Editor/Panels/UtilityPanelFixture.h"
#include "Editor/SupportingPanelTestUi.h"

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

		// M47: whether `output` holds a row of `label` reading `value` (spaces, tabs and table bars ignored, as
		// HasStatsMetricRow ignores them).
		static bool HasStatsTextRow(std::string_view output, std::string_view label, std::string_view value)
		{
			const auto compact = [](std::string text)
			{
				std::erase_if(text, [](char character)
				{
					return character == ' ' || character == '\t' || character == '\r' || character == '|';
				});
				return text;
			};
			const std::string expectedRow = compact(std::format("{} {}", label, value));
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

		TEST_CASE("StatsPanel: a displayed view shows its spot and point shadow counts")
		{
			// M23 (Docs/Decisions/0034-m23-contract.md decision 21): the view metrics list the point-shadow counts stats.get
			// reports (ShadowedPointLights, DroppedPointShadows) after the spot-shadow ones.
			Test::UtilityPanelFixture fixture;
			fixture.SetHostViews({ StatsViewSummary{ .Name = "scene", .Available = true, .Frame = "12", .Width = 640, .Height = 360, .ShadowDraws = 9, .ShadowedSpotLights = 3, .DroppedSpotShadows = 1, .ShadowedPointLights = 4, .DroppedPointShadows = 2 } });
			fixture.OpenProject();
			StatsPanel panel;
			fixture.Draw(panel);
			fixture.Pump();
			const std::string text = fixture.Draw(panel);
			INFO(text);
			CHECK(text.contains("Scene view"));
			CHECK(Utils::HasStatsMetricRow(text, "Shadow draws", 9));
			CHECK(Utils::HasStatsMetricRow(text, "Shadowed spot lights", 3));
			CHECK(Utils::HasStatsMetricRow(text, "Dropped spot shadows", 1));
			CHECK(Utils::HasStatsMetricRow(text, "Shadowed point lights", 4));
			CHECK(Utils::HasStatsMetricRow(text, "Dropped point shadows", 2));
		}

		TEST_CASE("StatsPanel: a displayed view shows its static scenery")
		{
			// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): the "Static scenery" row lists the view's static items,
			// then its visible static cells over all of them (stats.get's staticItems, staticCellsVisible and staticCells).
			Test::UtilityPanelFixture fixture;
			fixture.SetHostViews({ StatsViewSummary{ .Name = "scene", .Available = true, .Frame = "12", .Width = 640, .Height = 360, .StaticItems = 9, .StaticCells = 4, .StaticCellsVisible = 3 } });
			fixture.OpenProject();
			StatsPanel panel;
			fixture.Draw(panel);
			fixture.Pump();
			const std::string text = fixture.Draw(panel);
			INFO(text);
			CHECK(text.contains("Static scenery"));
			CHECK(text.contains("9 items, 3 / 4 cells"));
		}

		TEST_CASE("StatsPanel: a displayed view shows its sky updates and relit volumes")
		{
			// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3): the view metrics' Sky row, stats.get's skyUpdates and
			// relitIrradianceVolumes.
			Test::UtilityPanelFixture fixture;
			fixture.SetHostViews({ StatsViewSummary{ .Name = "scene", .Available = true, .Frame = "12", .Width = 640, .Height = 360, .SkyUpdates = 1, .RelitIrradianceVolumes = 2 } });
			fixture.OpenProject();
			StatsPanel panel;
			fixture.Draw(panel);
			fixture.Pump();
			const std::string text = fixture.Draw(panel);
			INFO(text);
			CHECK(text.contains("Scene view"));
			CHECK(text.contains("1 updates / 2 relit volumes"));
		}

		TEST_CASE("StatsPanel: a displayed view shows its draws per level of detail, its cross-fades and its distance-culled items")
		{
			// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3K): the "Mesh LODs" row lists the view's
			// meshLodDraws, then lodCrossFades and distanceCulledMeshes; a summary without per-level counts (a view built by hand,
			// as the case above's) shows 0 draws rather than reading past them.
			Test::UtilityPanelFixture fixture;
			fixture.SetHostViews({ StatsViewSummary{ .Name = "scene", .Available = true, .Frame = "12", .Width = 640, .Height = 360, .MeshLodDraws = { 7, 3, 2, 0, 1 }, .LodCrossFades = 2, .DistanceCulledMeshes = 5 } });
			fixture.OpenProject();
			StatsPanel panel;
			fixture.Draw(panel);
			fixture.Pump();
			const std::string text = fixture.Draw(panel);
			INFO(text);
			CHECK(text.contains("Mesh LODs"));
			CHECK(text.contains("7 / 3 / 2 / 0 / 1 draws / 2 cross-fades / 5 distance-culled"));

			Test::UtilityPanelFixture bare;
			bare.SetHostViews({ StatsViewSummary{ .Name = "scene", .Available = true, .Frame = "12", .Width = 640, .Height = 360 } });
			bare.OpenProject();
			StatsPanel empty;
			bare.Draw(empty);
			bare.Pump();
			const std::string none = bare.Draw(empty);
			INFO(none);
			CHECK(none.contains("0 draws / 0 cross-fades / 0 distance-culled"));
		}

		TEST_CASE("StatsPanel: a project change discards the pending statistics action")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			StatsPanel panel;
			const uint64_t probe = fixture.ProbeActionTicket();
			fixture.Draw(panel);
			const std::optional<uint64_t> ticket = fixture.FindPendingAction(probe);
			REQUIRE(ticket.has_value());
			REQUIRE(fixture.GetEditor().CloseProject());
			fixture.Draw(panel);
			// Discarded, not merely cancelled: no Cancelled completion is left behind for a ticket nobody will take.
			const auto abandoned = fixture.GetContext().Actions.TakeResult(*ticket);
			REQUIRE_FALSE(abandoned.has_value());
			CHECK(abandoned.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("StatsPanel: updates pause without changing simulation state")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			StatsPanel panel;
			fixture.Draw(panel);
			fixture.Pump();
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const std::string sampled = fixture.Draw(panel);
			CHECK(sampled.contains("CPU frame"));
			CHECK(sampled.contains("Physics bodies"));
			CHECK(sampled.contains("Unavailable (no active script VM)"));
			CHECK(sampled.contains("Render statistics unavailable"));
			CHECK(fixture.GetEditor().GetRevision() == revision);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Live");
			fixture.Pump(); // finish the sample already in flight
			const std::string paused = fixture.Draw(panel);
			CHECK(paused.contains("Updates paused"));
			REQUIRE(fixture.GetClient().Call("entity.create", Json{ { "name", "After paused sample" } }));
			fixture.Pump();
			CHECK(fixture.Draw(panel) == paused);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Refresh");
			fixture.Pump();
			CHECK(fixture.Draw(panel) != paused);
			CHECK(fixture.Draw(panel).contains("Updates paused"));
		}

		TEST_CASE("StatsPanel: a displayed view shows its water surfaces and whether it is under water")
		{
			// M47 (Docs/Decisions/0065-m47-water.md decision 3): the view metrics' Water row reads the surfaces the view drew
			// (RenderStats::WaterSurfaces) and, while the Underwater pass recorded, " (under water)".
			for (const bool underwater : { true, false })
			{
				CAPTURE(underwater);
				Test::UtilityPanelFixture fixture;
				fixture.SetHostViews({ StatsViewSummary{ .Name = "scene", .Available = true, .Frame = "12", .Width = 640, .Height = 360, .PlanarReflections = 1, .WaterSurfaces = 3, .Underwater = underwater } });
				fixture.OpenProject();
				StatsPanel panel;
				fixture.Draw(panel);
				fixture.Pump();
				const std::string text = fixture.Draw(panel);
				INFO(text);
				CHECK(Utils::HasStatsTextRow(text, "Water", underwater ? "3 surfaces (under water)" : "3 surfaces"));
				CHECK(Utils::HasStatsTextRow(text, "Planar reflections", "1 drawn / 0 dropped"));
			}
		}
	}

}
