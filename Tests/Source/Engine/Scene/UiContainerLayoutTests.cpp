#include "TestsPCH.h"

#include "Engine/Scene/UiContainerLayout.h"

#include <glm/glm.hpp>

#include <limits>
#include <vector>

// The UiLayout containers (M22, C22.1, R-UI-LAYOUT; Docs/Decisions/0033-m22-contract.md decision 5): Stack, Grid and Flex
// placement in canvas units, with exact values (IEEE-exact arithmetic on representable inputs), so the unit suite's Debug and
// Release runs check that the placements are identical across configurations (Roadmap M22's acceptance).

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
		TEST_CASE("UiContainerLayout: a Stack places its children one after another with Spacing inside Padding")
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

		// M38 (Docs/Decisions/0055-m38-runtime-locale.md decision 12; Roadmap M38's acceptance): HorizontalReverse is Horizontal
		// mirrored inside the padded rectangle: the first child at the right edge, MainAlignment Start at the right, a Grid's
		// rows and Flex's lines from the right; Horizontal itself is unchanged.
		TEST_CASE("UiContainerLayout: a HorizontalReverse row lays its children out from the right")
		{
			UiLayoutComponent layout{ .Kind = UiLayoutKind::Stack,
				.Direction = UiLayoutDirection::HorizontalReverse,
				.Padding = UiPadding{ .Left = 10.0f, .Top = 5.0f, .Right = 20.0f, .Bottom = 5.0f },
				.Spacing = glm::vec2(4.0f, 0.0f) };
			const std::vector<UiContainerChild> children = { MakeChild(glm::vec2(30.0f, 20.0f)), MakeChild(glm::vec2(40.0f, 10.0f)) };
			// The padded rectangle is [10, 180] along X: the first child ends at 180, the second 4 units before it.
			std::vector<UiContainerPlacement> placements = LayoutUiContainer(layout, glm::vec2(200.0f, 50.0f), children);
			REQUIRE(placements.size() == 2);
			CHECK(placements[0] == UiContainerPlacement{ .Position = glm::vec2(150.0f, 5.0f), .Size = glm::vec2(30.0f, 20.0f) });
			CHECK(placements[1] == UiContainerPlacement{ .Position = glm::vec2(106.0f, 5.0f), .Size = glm::vec2(40.0f, 10.0f) });
			// MainAlignment Start sits at the right edge, End at the left.
			layout.MainAlignment = UiMainAlignment::End;
			placements = LayoutUiContainer(layout, glm::vec2(200.0f, 50.0f), children);
			CHECK(placements[1].Position.x == 10.0f);
			// Every kind is the Horizontal placement mirrored.
			for (const UiLayoutKind kind : { UiLayoutKind::Stack, UiLayoutKind::Grid, UiLayoutKind::Flex })
			{
				CAPTURE(static_cast<int>(kind));
				UiLayoutComponent forward = layout;
				forward.Kind = kind;
				forward.Direction = UiLayoutDirection::Horizontal;
				forward.Columns = 2;
				forward.CellSize = glm::vec2(50.0f, 20.0f);
				UiLayoutComponent reverse = forward;
				reverse.Direction = UiLayoutDirection::HorizontalReverse;
				const std::vector<UiContainerChild> three = { MakeChild(glm::vec2(30.0f, 20.0f)), MakeChild(glm::vec2(40.0f, 10.0f)), MakeChild(glm::vec2(20.0f, 10.0f)) };
				const std::vector<UiContainerPlacement> ahead = LayoutUiContainer(forward, glm::vec2(200.0f, 50.0f), three);
				const std::vector<UiContainerPlacement> mirrored = LayoutUiContainer(reverse, glm::vec2(200.0f, 50.0f), three);
				REQUIRE(ahead.size() == mirrored.size());
				for (size_t index = 0; index < ahead.size(); ++index)
				{
					CAPTURE(index);
					CHECK(mirrored[index].Size == ahead[index].Size);
					CHECK(mirrored[index].Position.y == ahead[index].Position.y);
					CHECK(mirrored[index].Position.x == 10.0f + (170.0f - ((ahead[index].Position.x - 10.0f) + ahead[index].Size.x)));
				}
			}
		}

		TEST_CASE("UiContainerLayout: MainAlignment places a Stack's run, and an overflowing run starts at the inner start")
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

		TEST_CASE("UiContainerLayout: CrossAlignment places each child across its line, and Stretch fills it")
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

		TEST_CASE("UiContainerLayout: a Grid fills Columns cells per row, or as many as fit with Columns 0")
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

		TEST_CASE("UiContainerLayout: Flex grows children by Grow and shrinks them by Shrink times their size, never below MinSize")
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

		TEST_CASE("UiContainerLayout: Flex with Wrap starts a new line for a child that does not fit")
		{
			const UiLayoutComponent layout{ .Kind = UiLayoutKind::Flex, .Direction = UiLayoutDirection::Horizontal, .Spacing = glm::vec2(10.0f, 5.0f), .Wrap = true };
			const std::vector<UiContainerChild> children = { MakeChild(glm::vec2(40.0f, 10.0f)), MakeChild(glm::vec2(40.0f, 20.0f)), MakeChild(glm::vec2(40.0f, 10.0f)) };
			// 40 + 10 + 40 fits 100; the third child would end at 140. The first line is 20 high (its tallest child).
			CHECK(GetPositions(LayoutUiContainer(layout, glm::vec2(100.0f), children))
				== std::vector<glm::vec2>{ glm::vec2(0.0f), glm::vec2(50.0f, 0.0f), glm::vec2(0.0f, 25.0f) });
		}

		TEST_CASE("UiContainerLayout: a Vertical container's main axis is y, and Stack and Flex never take a child below its MinSize")
		{
			UiLayoutComponent stack{ .Kind = UiLayoutKind::Stack,
				.Direction = UiLayoutDirection::Vertical,
				.Padding = UiPadding{ .Left = 2.0f, .Top = 4.0f, .Right = 6.0f, .Bottom = 8.0f },
				.Spacing = glm::vec2(0.0f, 3.0f) };
			// The inner rectangle is 42 x 88 from (2, 4). The first child is raised to its MinSize; the third is wider than the
			// inner rectangle.
			const std::vector<UiContainerChild> children = { MakeChild(glm::vec2(10.0f, 20.0f), 0.0f, 1.0f, glm::vec2(15.0f, 25.0f)),
				MakeChild(glm::vec2(30.0f, 5.0f)), MakeChild(glm::vec2(10.0f), 0.0f, 1.0f, glm::vec2(60.0f, 0.0f)) };
			const glm::vec2 container(50.0f, 100.0f);
			const std::vector<UiContainerPlacement> started = LayoutUiContainer(stack, container, children);
			REQUIRE(started.size() == 3);
			CHECK(started[0] == UiContainerPlacement{ .Position = glm::vec2(2.0f, 4.0f), .Size = glm::vec2(15.0f, 25.0f) });
			CHECK(started[1] == UiContainerPlacement{ .Position = glm::vec2(2.0f, 32.0f), .Size = glm::vec2(30.0f, 5.0f) });
			CHECK(started[2] == UiContainerPlacement{ .Position = glm::vec2(2.0f, 40.0f), .Size = glm::vec2(60.0f, 10.0f) });
			// Stretch fills the 42-unit line, but never below a child's MinSize.
			stack.CrossAlignment = UiCrossAlignment::Stretch;
			const std::vector<UiContainerPlacement> stretched = LayoutUiContainer(stack, container, children);
			REQUIRE(stretched.size() == 3);
			CHECK(stretched[0].Size == glm::vec2(42.0f, 25.0f));
			CHECK(stretched[1].Size == glm::vec2(42.0f, 5.0f));
			CHECK(stretched[2] == UiContainerPlacement{ .Position = glm::vec2(2.0f, 40.0f), .Size = glm::vec2(60.0f, 10.0f) });
			// Center places a child wider than its line over it, overflowing both sides alike.
			stack.CrossAlignment = UiCrossAlignment::Center;
			CHECK(GetPositions(LayoutUiContainer(stack, container, children))
				== std::vector<glm::vec2>{ glm::vec2(15.5f, 4.0f), glm::vec2(8.0f, 32.0f), glm::vec2(-7.0f, 40.0f) });

			// A Vertical Flex takes 20 units of overflow back along y, 50 : 50, the second child held at its minimum of 45.
			const UiLayoutComponent flex{ .Kind = UiLayoutKind::Flex, .Direction = UiLayoutDirection::Vertical };
			const std::vector<UiContainerChild> tall = { MakeChild(glm::vec2(10.0f, 50.0f)), MakeChild(glm::vec2(10.0f, 50.0f), 0.0f, 1.0f, glm::vec2(0.0f, 45.0f)) };
			const std::vector<UiContainerPlacement> shrunk = LayoutUiContainer(flex, glm::vec2(20.0f, 80.0f), tall);
			REQUIRE(shrunk.size() == 2);
			CHECK(shrunk[0] == UiContainerPlacement{ .Position = glm::vec2(0.0f), .Size = glm::vec2(10.0f, 40.0f) });
			CHECK(shrunk[1] == UiContainerPlacement{ .Position = glm::vec2(0.0f, 40.0f), .Size = glm::vec2(10.0f, 45.0f) });
		}

		TEST_CASE("UiContainerLayout: each Flex line shares its own free space and is as tall as its tallest child, and a single line spans the inner cross size")
		{
			UiLayoutComponent layout{ .Kind = UiLayoutKind::Flex,
				.Direction = UiLayoutDirection::Horizontal,
				.Spacing = glm::vec2(10.0f, 5.0f),
				.CrossAlignment = UiCrossAlignment::End,
				.Wrap = true };
			const std::vector<UiContainerChild> children = { MakeChild(glm::vec2(40.0f, 10.0f)), MakeChild(glm::vec2(40.0f, 20.0f)), MakeChild(glm::vec2(40.0f, 10.0f)) };
			// Two lines: the first 20 units high, the second (at 20 + 5) 10 units high.
			CHECK(GetPositions(LayoutUiContainer(layout, glm::vec2(100.0f), children))
				== std::vector<glm::vec2>{ glm::vec2(0.0f, 10.0f), glm::vec2(50.0f, 0.0f), glm::vec2(0.0f, 25.0f) });
			layout.CrossAlignment = UiCrossAlignment::Center;
			CHECK(GetPositions(LayoutUiContainer(layout, glm::vec2(100.0f), children))
				== std::vector<glm::vec2>{ glm::vec2(0.0f, 5.0f), glm::vec2(50.0f, 0.0f), glm::vec2(0.0f, 25.0f) });
			layout.CrossAlignment = UiCrossAlignment::Stretch;
			const std::vector<UiContainerPlacement> stretched = LayoutUiContainer(layout, glm::vec2(100.0f), children);
			REQUIRE(stretched.size() == 3);
			CHECK(stretched[0].Size == glm::vec2(40.0f, 20.0f));
			CHECK(stretched[2].Size == glm::vec2(40.0f, 10.0f));

			// Growing children: the first line shares its 10 free units, the second its 60.
			const std::vector<UiContainerChild> growing(3, MakeChild(glm::vec2(40.0f, 10.0f), 1.0f));
			layout.CrossAlignment = UiCrossAlignment::Start;
			const std::vector<UiContainerPlacement> grown = LayoutUiContainer(layout, glm::vec2(100.0f), growing);
			REQUIRE(grown.size() == 3);
			CHECK(grown[0] == UiContainerPlacement{ .Position = glm::vec2(0.0f), .Size = glm::vec2(45.0f, 10.0f) });
			CHECK(grown[1] == UiContainerPlacement{ .Position = glm::vec2(55.0f, 0.0f), .Size = glm::vec2(45.0f, 10.0f) });
			CHECK(grown[2] == UiContainerPlacement{ .Position = glm::vec2(0.0f, 15.0f), .Size = glm::vec2(100.0f, 10.0f) });

			// Every child fits one 140-unit line of 200: that single line spans the inner cross size, with Wrap or without.
			layout.CrossAlignment = UiCrossAlignment::Center;
			const std::vector<glm::vec2> centred = { glm::vec2(0.0f, 45.0f), glm::vec2(50.0f, 40.0f), glm::vec2(100.0f, 45.0f) };
			CHECK(GetPositions(LayoutUiContainer(layout, glm::vec2(200.0f, 100.0f), children)) == centred);
			layout.Wrap = false;
			CHECK(GetPositions(LayoutUiContainer(layout, glm::vec2(200.0f, 100.0f), children)) == centred);
		}

		TEST_CASE("UiContainerLayout: MainAlignment places a Grid's cell block, and a fitted Grid holds at least one cell per line")
		{
			UiLayoutComponent layout{ .Kind = UiLayoutKind::Grid,
				.Direction = UiLayoutDirection::Horizontal,
				.Spacing = glm::vec2(4.0f, 2.0f),
				.MainAlignment = UiMainAlignment::Center,
				.Columns = 2,
				.CellSize = glm::vec2(20.0f, 10.0f) };
			const std::vector<UiContainerChild> children(3, MakeChild(glm::vec2(5.0f)));
			// The block is 20 + 4 + 20 = 44 units wide in 100: 56 left over.
			CHECK(GetPositions(LayoutUiContainer(layout, glm::vec2(100.0f, 50.0f), children))
				== std::vector<glm::vec2>{ glm::vec2(28.0f, 0.0f), glm::vec2(52.0f, 0.0f), glm::vec2(28.0f, 12.0f) });
			layout.MainAlignment = UiMainAlignment::End;
			CHECK(GetPositions(LayoutUiContainer(layout, glm::vec2(100.0f, 50.0f), children))
				== std::vector<glm::vec2>{ glm::vec2(56.0f, 0.0f), glm::vec2(80.0f, 0.0f), glm::vec2(56.0f, 12.0f) });
			// floor((100 + 4) / (150 + 4)) is 0, so each row holds one cell; the 150-unit block overflows and starts at 0.
			layout.Columns = 0;
			layout.CellSize = glm::vec2(150.0f, 10.0f);
			const std::vector<UiContainerPlacement> wide = LayoutUiContainer(layout, glm::vec2(100.0f, 50.0f), children);
			CHECK(GetPositions(wide) == std::vector<glm::vec2>{ glm::vec2(0.0f), glm::vec2(0.0f, 12.0f), glm::vec2(0.0f, 24.0f) });
			for (const UiContainerPlacement& placement : wide)
				CHECK(placement.Size == glm::vec2(150.0f, 10.0f));
		}

		TEST_CASE("UiContainerLayout: Padding wider than the container leaves an empty inner rectangle, and no placed size is negative")
		{
			// The inner rectangle starts at the padding and holds nothing: a run that overflows it starts there, and Stretch
			// gives no height.
			const UiLayoutComponent padded{ .Kind = UiLayoutKind::Stack,
				.Direction = UiLayoutDirection::Horizontal,
				.Padding = UiPadding{ .Left = 30.0f, .Top = 10.0f, .Right = 30.0f, .Bottom = 10.0f },
				.MainAlignment = UiMainAlignment::Center,
				.CrossAlignment = UiCrossAlignment::Stretch };
			CHECK(PlaceOne(padded, glm::vec2(50.0f, 15.0f), MakeChild(glm::vec2(10.0f)))
				== UiContainerPlacement{ .Position = glm::vec2(30.0f, 10.0f), .Size = glm::vec2(10.0f, 0.0f) });
			// A negative or NaN size takes no space: the run of nothing is centred in the 140-unit inner width.
			const UiContainerPlacement nothing{ .Position = glm::vec2(100.0f, 10.0f), .Size = glm::vec2(0.0f, 30.0f) };
			CHECK(PlaceOne(padded, glm::vec2(200.0f, 50.0f), MakeChild(glm::vec2(-5.0f, -5.0f))) == nothing);
			CHECK(PlaceOne(padded, glm::vec2(200.0f, 50.0f), MakeChild(glm::vec2(std::numeric_limits<float>::quiet_NaN()))) == nothing);

			// Spacing that overflows more than the children's sizes: each would lose 60 of its 10 units and stops at 0.
			const UiLayoutComponent flex{ .Kind = UiLayoutKind::Flex, .Direction = UiLayoutDirection::Horizontal, .Spacing = glm::vec2(200.0f, 0.0f) };
			const std::vector<UiContainerChild> children(2, MakeChild(glm::vec2(10.0f)));
			const std::vector<UiContainerPlacement> squeezed = LayoutUiContainer(flex, glm::vec2(100.0f, 10.0f), children);
			REQUIRE(squeezed.size() == 2);
			CHECK(squeezed[0] == UiContainerPlacement{ .Position = glm::vec2(0.0f), .Size = glm::vec2(0.0f, 10.0f) });
			CHECK(squeezed[1] == UiContainerPlacement{ .Position = glm::vec2(200.0f, 0.0f), .Size = glm::vec2(0.0f, 10.0f) });

			// A container without children places nothing.
			CHECK(LayoutUiContainer(flex, glm::vec2(100.0f, 10.0f), std::vector<UiContainerChild>{}).empty());
		}
	}

}
