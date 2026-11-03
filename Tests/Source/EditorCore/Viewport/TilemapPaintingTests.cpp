#include "TestsPCH.h"

#include "EditorCore/Viewport/TilemapPainting.h"

#include "EditorCore/Automation/TilemapMethods.h"

#include <glm/gtc/matrix_transform.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <vector>

// The editor's tile brush (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 26, stream T3): the view's rays
// to cells and a stroke to its one tilemap.paint. Skipped until stream T3 implements them.

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("TilemapPainting: a scene-view ray finds the cell under the cursor and a stroke becomes one tilemap.paint" * doctest::skip(true))
		{
			// A tilemap entity at (10, 5) with cells of 2 x 0.5 units: the ray down through (13.5, 4.9) meets local (3.5, -0.1), cell
			// (1, -1); one through (9.99, 5) lies just left of cell 0, in cell (-1, 0).
			const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 5.0f, 0.0f));
			const glm::vec2 cellSize(2.0f, 0.5f);
			const auto down = [](float x, float y)
			{
				return RenderRay{ .Origin = glm::vec3(x, y, 10.0f), .Direction = glm::vec3(0.0f, 0.0f, -1.0f) };
			};
			CHECK(IntersectTilemapCell(down(13.5f, 4.9f), world, cellSize) == std::optional<glm::ivec2>(glm::ivec2(1, -1)));
			CHECK(IntersectTilemapCell(down(9.99f, 5.0f), world, cellSize) == std::optional<glm::ivec2>(glm::ivec2(-1, 0)));
			// A ray parallel to the plane, or meeting it behind its origin, finds no cell.
			CHECK_FALSE(IntersectTilemapCell(RenderRay{ .Origin = glm::vec3(0.0f, 0.0f, 10.0f), .Direction = glm::vec3(1.0f, 0.0f, 0.0f) }, world, cellSize).has_value());
			CHECK_FALSE(IntersectTilemapCell(RenderRay{ .Origin = glm::vec3(0.0f, 0.0f, 10.0f), .Direction = glm::vec3(0.0f, 0.0f, 1.0f) }, world, cellSize).has_value());
			// A rotated tilemap: its cells turn with it.
			const glm::mat4 turned = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			CHECK(IntersectTilemapCell(down(-0.25f, 1.0f), turned, glm::vec2(1.0f)) == std::optional<glm::ivec2>(glm::ivec2(1, 0)));

			// A fast drag from (0, 0) to (3, 1) paints the connected line between them, both ends included.
			const std::vector<glm::ivec2> stroke = GetTilemapStrokeCells(glm::ivec2(0, 0), glm::ivec2(3, 1));
			CHECK(stroke == std::vector<glm::ivec2>{ glm::ivec2(0, 0), glm::ivec2(1, 0), glm::ivec2(2, 1), glm::ivec2(3, 1) });
			CHECK(GetTilemapStrokeCells(glm::ivec2(2, 2), glm::ivec2(2, 2)) == std::vector<glm::ivec2>{ glm::ivec2(2, 2) });

			// The stroke's tilemap.paint: each cell once, at its first place, with the brush's value.
			const AssetHandle tilemap(0x7113000000000009ull);
			const std::vector<glm::ivec2> crossed = { glm::ivec2(0, 0), glm::ivec2(1, 0), glm::ivec2(0, 0), glm::ivec2(-1, 2) };
			const Result<Json> params = MakeTilemapPaintParams({ .Tilemap = tilemap, .Cells = crossed, .Tile = 3 });
			REQUIRE_MESSAGE(params.has_value(), params.error().ToString());
			CHECK((*params)["tilemap"] == Json(tilemap.ToString()));
			REQUIRE((*params)["cells"].size() == 3);
			CHECK((*params)["cells"][0] == Json{ { "x", 0 }, { "y", 0 }, { "tile", 3 } });
			CHECK((*params)["cells"][2] == Json{ { "x", -1 }, { "y", 2 }, { "tile", 3 } });
			CHECK_FALSE(MakeTilemapPaintParams({ .Tilemap = AssetHandle(), .Cells = crossed, .Tile = 1 }).has_value());
			CHECK_FALSE(MakeTilemapPaintParams({ .Tilemap = tilemap, .Cells = {}, .Tile = 1 }).has_value());
			const std::vector<glm::ivec2> tooMany(MaxTilemapPaintCells + 1, glm::ivec2(0));
			std::vector<glm::ivec2> distinct;
			for (uint32_t index = 0; index <= MaxTilemapPaintCells; ++index)
				distinct.push_back(glm::ivec2(static_cast<int32_t>(index), 0));
			CHECK_FALSE(MakeTilemapPaintParams({ .Tilemap = tilemap, .Cells = distinct, .Tile = 1 }).has_value());
			// Repeats count once: a stroke that crosses one cell many times stays within the limit.
			CHECK(MakeTilemapPaintParams({ .Tilemap = tilemap, .Cells = tooMany, .Tile = 1 }).has_value());
		}
	}

}
