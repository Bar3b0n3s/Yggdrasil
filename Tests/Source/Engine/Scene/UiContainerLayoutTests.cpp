#include "TestsPCH.h"

#include "Engine/Scene/UiContainerLayout.h"

#include <glm/glm.hpp>

#include <vector>

// The UiLayout containers (M22, C22.1, R-UI-LAYOUT; Docs/Decisions/0033-m22-contract.md decision 5): Stack, Grid and Flex
// placement in canvas units, with exact values (IEEE-exact arithmetic on representable inputs). Skipped by the contract;
// stream D un-skips them.

namespace Engine {

	namespace {

		UiContainerChild MakeChild(glm::vec2 size, float grow = 0.0f, float shrink = 1.0f, glm::vec2 minSize = glm::vec2(0.0f))
		{
			return UiContainerChild{ .Size = size, .MinSize = minSize, .Grow = grow, .Shrink = shrink };
		}

		// The placement of a container's one child.
		UiContainerPlacement PlaceOne(const UiLayoutComponent& layout, glm::vec2 container, const UiContainerChild& child)
		{
			const std::vector<UiContainerPlacement> placements = LayoutUiContainer(layout, container, std::vector<UiContainerChild>{ child });
			REQUIRE(placements.size() == 1);
			return placements[0];
		}

		std::vector<glm::vec2> GetPositions(const std::vector<UiContainerPlacement>& placements)
		{
			std::vector<glm::vec2> positions;
			for (const UiContainerPlacement& placement : placements)
				positions.push_back(placement.Position);
			return positions;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("UiContainerLayout: a Stack places its children one after another with Spacing inside Padding" * doctest::skip(true))
		{
			const UiLayoutComponent layout{ .Kind = UiLayoutKind::Stack,
				.Direction = UiLayoutDirection::Horizontal,
				.Padding = UiPadding{ .Left = 10.0f, .Top = 5.0f, .Right = 10.0f, .Bottom = 5.0f },
				.Spacing = glm::vec2(4.0f, 0.0f) };
			const std::vector<UiContainerChild> children = { MakeChild(glm::vec2(30.0f, 20.0f)), MakeChild(glm::vec2(40.0f, 10.0f)) };
			const std::vector<UiContainerPlacement> placements = LayoutUiContainer(layout, glm::vec2(200.0f, 50.0f), children);
			REQUIRE(placements.size() == 2);
			CHECK(placements[0] == UiContainerPlacement{ .Position = glm::vec2(10.0f, 5.0f), .Size = glm::vec2(30.0f, 20.0f) });
			CHECK(placements[1] == UiContainerPlacement{ .Position = glm::vec2(44.0f, 5.0f), .Size = glm::vec2(40.0f, 10.0f) });
		}

		TEST_CASE("UiContainerLayout: MainAlignment places a Stack's run, and an overflowing run starts at the inner start" * doctest::skip(true))
		{
			UiLayoutComponent layout{ .Kind = UiLayoutKind::Stack, .Direction = UiLayoutDirection::Horizontal, .Spacing = glm::vec2(10.0f, 0.0f) };
			const std::vector<UiContainerChild> children = { MakeChild(glm::vec2(20.0f, 10.0f)), MakeChild(glm::vec2(30.0f, 10.0f)) };
			// The run is 60 units in 100: 40 left over.
			layout.MainAlignment = UiMainAlignment::Start;
			CHECK(GetPositions(LayoutUiContainer(layout, glm::vec2(100.0f, 20.0f), children)) == std::vector<glm::vec2>{ glm::vec2(0.0f), glm::vec2(30.0f, 0.0f) });
			layout.MainAlignment = UiMainAlignment::Center;
			CHECK(GetPositions(LayoutUiContainer(layout, glm::vec2(100.0f, 20.0f), children)) == std::vector<glm::vec2>{ glm::vec2(20.0f, 0.0f), glm::vec2(50.0f, 0.0f) });
			layout.MainAlignment = UiMainAlignment::End;
			CHECK(GetPositions(LayoutUiContainer(layout, glm::vec2(100.0f, 20.0f), children)) == std::vector<glm::vec2>{ glm::vec2(40.0f, 0.0f), glm::vec2(70.0f, 0.0f) });
			// A 140-unit run in 100 starts at 0 whatever the alignment.
			const std::vector<UiContainerChild> wide = { MakeChild(glm::vec2(80.0f, 10.0f)), MakeChild(glm::vec2(50.0f, 10.0f)) };
			CHECK(GetPositions(LayoutUiContainer(layout, glm::vec2(100.0f, 20.0f), wide)) == std::vector<glm::vec2>{ glm::vec2(0.0f), glm::vec2(90.0f, 0.0f) });
		}

		TEST_CASE("UiContainerLayout: CrossAlignment places each child across its line, and Stretch fills it" * doctest::skip(true))
		{
			UiLayoutComponent layout{ .Kind = UiLayoutKind::Stack, .Direction = UiLayoutDirection::Horizontal };
			const UiContainerChild child = MakeChild(glm::vec2(20.0f, 10.0f));
			const glm::vec2 container(100.0f, 40.0f);
			layout.CrossAlignment = UiCrossAlignment::Start;
			CHECK(PlaceOne(layout, container, child) == UiContainerPlacement{ .Position = glm::vec2(0.0f), .Size = glm::vec2(20.0f, 10.0f) });
			layout.CrossAlignment = UiCrossAlignment::Center;
			CHECK(PlaceOne(layout, container, child).Position == glm::vec2(0.0f, 15.0f));
			layout.CrossAlignment = UiCrossAlignment::End;
			CHECK(PlaceOne(layout, container, child).Position == glm::vec2(0.0f, 30.0f));
			layout.CrossAlignment = UiCrossAlignment::Stretch;
			CHECK(PlaceOne(layout, container, child) == UiContainerPlacement{ .Position = glm::vec2(0.0f), .Size = glm::vec2(20.0f, 40.0f) });
		}

		TEST_CASE("UiContainerLayout: a Grid fills Columns cells per row, or as many as fit with Columns 0" * doctest::skip(true))
		{
			UiLayoutComponent layout{ .Kind = UiLayoutKind::Grid,
				.Direction = UiLayoutDirection::Horizontal,
				.Spacing = glm::vec2(2.0f, 1.0f),
				.Columns = 3,
				.CellSize = glm::vec2(20.0f, 10.0f) };
			// Each child takes its cell's size, whatever its own.
			const std::vector<UiContainerChild> children(5, MakeChild(glm::vec2(7.0f, 3.0f)));
			const std::vector<UiContainerPlacement> three = LayoutUiContainer(layout, glm::vec2(100.0f), children);
			CHECK(GetPositions(three)
				== std::vector<glm::vec2>{ glm::vec2(0.0f), glm::vec2(22.0f, 0.0f), glm::vec2(44.0f, 0.0f), glm::vec2(0.0f, 11.0f), glm::vec2(22.0f, 11.0f) });
			for (const UiContainerPlacement& placement : three)
				CHECK(placement.Size == glm::vec2(20.0f, 10.0f));
			// floor((100 + 2) / (20 + 2)) = 4 cells per row.
			layout.Columns = 0;
			const std::vector<UiContainerPlacement> fitted = LayoutUiContainer(layout, glm::vec2(100.0f), children);
			CHECK(GetPositions(fitted)
				== std::vector<glm::vec2>{ glm::vec2(0.0f), glm::vec2(22.0f, 0.0f), glm::vec2(44.0f, 0.0f), glm::vec2(66.0f, 0.0f), glm::vec2(0.0f, 11.0f) });
			// A Vertical grid fills columns of Columns cells.
			layout.Direction = UiLayoutDirection::Vertical;
			layout.Columns = 2;
			const std::vector<UiContainerPlacement> columns = LayoutUiContainer(layout, glm::vec2(100.0f), children);
			CHECK(GetPositions(columns)
				== std::vector<glm::vec2>{ glm::vec2(0.0f), glm::vec2(0.0f, 11.0f), glm::vec2(22.0f, 0.0f), glm::vec2(22.0f, 11.0f), glm::vec2(44.0f, 0.0f) });
			// With Columns 0, zero-width cells without spacing fit any count: 1024 per row, so the five share the first row,
			// and the count is never a division by 0.
			const UiLayoutComponent flat{ .Kind = UiLayoutKind::Grid,
				.Direction = UiLayoutDirection::Horizontal,
				.Spacing = glm::vec2(0.0f, 1.0f),
				.Columns = 0,
				.CellSize = glm::vec2(0.0f, 10.0f) };
			const std::vector<UiContainerPlacement> zero = LayoutUiContainer(flat, glm::vec2(100.0f), children);
			REQUIRE(zero.size() == 5);
			for (const UiContainerPlacement& placement : zero)
				CHECK(placement == UiContainerPlacement{ .Position = glm::vec2(0.0f), .Size = glm::vec2(0.0f, 10.0f) });
		}

		TEST_CASE("UiContainerLayout: Flex grows children by Grow and shrinks them by Shrink times their size, never below MinSize" * doctest::skip(true))
		{
			const UiLayoutComponent layout{ .Kind = UiLayoutKind::Flex, .Direction = UiLayoutDirection::Horizontal };
			// 60 units of free space shared 1 : 3.
			const std::vector<UiContainerChild> growing = { MakeChild(glm::vec2(20.0f, 10.0f), 1.0f), MakeChild(glm::vec2(20.0f, 10.0f), 3.0f) };
			const std::vector<UiContainerPlacement> grown = LayoutUiContainer(layout, glm::vec2(100.0f, 20.0f), growing);
			REQUIRE(grown.size() == 2);
			CHECK(grown[0] == UiContainerPlacement{ .Position = glm::vec2(0.0f), .Size = glm::vec2(35.0f, 10.0f) });
			CHECK(grown[1] == UiContainerPlacement{ .Position = glm::vec2(35.0f, 0.0f), .Size = glm::vec2(65.0f, 10.0f) });
			// 20 units of overflow taken back 60 : 60, the second child held at its minimum of 55.
			const std::vector<UiContainerChild> shrinking = { MakeChild(glm::vec2(60.0f, 10.0f)), MakeChild(glm::vec2(60.0f, 10.0f), 0.0f, 1.0f, glm::vec2(55.0f, 0.0f)) };
			const std::vector<UiContainerPlacement> shrunk = LayoutUiContainer(layout, glm::vec2(100.0f, 20.0f), shrinking);
			REQUIRE(shrunk.size() == 2);
			CHECK(shrunk[0].Size == glm::vec2(50.0f, 10.0f));
			CHECK(shrunk[1] == UiContainerPlacement{ .Position = glm::vec2(50.0f, 0.0f), .Size = glm::vec2(55.0f, 10.0f) });
			// A line whose total of Shrink times size is 0 (Shrink 0 forbids shrinking; a zero size has nothing to give) keeps its
			// sizes and overflows, never dividing by 0.
			const std::vector<UiContainerChild> rigid = { MakeChild(glm::vec2(60.0f, 10.0f), 0.0f, 0.0f), MakeChild(glm::vec2(0.0f, 10.0f)),
				MakeChild(glm::vec2(60.0f, 10.0f), 0.0f, 0.0f) };
			const std::vector<UiContainerPlacement> overflowing = LayoutUiContainer(layout, glm::vec2(100.0f, 20.0f), rigid);
			REQUIRE(overflowing.size() == 3);
			CHECK(overflowing[0] == UiContainerPlacement{ .Position = glm::vec2(0.0f), .Size = glm::vec2(60.0f, 10.0f) });
			CHECK(overflowing[1] == UiContainerPlacement{ .Position = glm::vec2(60.0f, 0.0f), .Size = glm::vec2(0.0f, 10.0f) });
			CHECK(overflowing[2] == UiContainerPlacement{ .Position = glm::vec2(60.0f, 0.0f), .Size = glm::vec2(60.0f, 10.0f) });
		}

		TEST_CASE("UiContainerLayout: Flex with Wrap starts a new line for a child that does not fit" * doctest::skip(true))
		{
			const UiLayoutComponent layout{ .Kind = UiLayoutKind::Flex, .Direction = UiLayoutDirection::Horizontal, .Spacing = glm::vec2(10.0f, 5.0f), .Wrap = true };
			const std::vector<UiContainerChild> children = { MakeChild(glm::vec2(40.0f, 10.0f)), MakeChild(glm::vec2(40.0f, 20.0f)), MakeChild(glm::vec2(40.0f, 10.0f)) };
			// 40 + 10 + 40 fits 100; the third child would end at 140. The first line is 20 high (its tallest child).
			CHECK(GetPositions(LayoutUiContainer(layout, glm::vec2(100.0f), children))
				== std::vector<glm::vec2>{ glm::vec2(0.0f), glm::vec2(50.0f, 0.0f), glm::vec2(0.0f, 25.0f) });
		}
	}

}
