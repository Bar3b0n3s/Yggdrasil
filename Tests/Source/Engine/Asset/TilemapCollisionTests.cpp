#include "TestsPCH.h"

#include "Engine/Asset/TilemapCollision.h"

#include <vector>

// The tilemap collider's boxes (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 25, stream T2): skipped
// until stream T2 implements them.

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("TilemapCollision: solid cells merge into boxes per chunk in scan order" * doctest::skip(true))
		{
			// Entry 1 is solid, entry 2 is not. Chunk (0, 0), rows from the bottom up:
			//   y = 3: 1 0 0 1
			//   y = 2: 0 2 1 1
			//   y = 1: 1 1 1 0
			//   y = 0: 1 1 1 0
			// Chunk (1, 0) has a full bottom row, and chunk (0, 1) its cell (0, 4) on top of chunk (0, 0)'s (0, 3).
			TilemapData tilemap;
			tilemap.ChunkSize = 4;
			tilemap.Palette = { TilemapTile{ .Solid = true }, TilemapTile{ .Solid = false } };
			tilemap.Chunks = {
				TilemapChunk{ .X = 0, .Y = 0, .Tiles = { 1, 1, 1, 0, 1, 1, 1, 0, 0, 2, 1, 1, 1, 0, 0, 1 } },
				TilemapChunk{ .X = 1, .Y = 0, .Tiles = { 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
				TilemapChunk{ .X = 0, .Y = 1, .Tiles = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
			};
			CHECK_FALSE(IsTilemapCellSolid(tilemap, 0));
			CHECK(IsTilemapCellSolid(tilemap, 1));
			CHECK_FALSE(IsTilemapCellSolid(tilemap, 2));
			CHECK_FALSE(IsTilemapCellSolid(tilemap, 3)); // beyond the palette

			// The bottom-left cell grows right over the row, then up while whole rows are solid; the non-solid entry 2 starts
			// nothing; then the boxes of the rows above in the order they start.
			const std::vector<TilemapBox> first = MergeTilemapChunkBoxes(tilemap, tilemap.Chunks[0]);
			const std::vector<TilemapBox> expectedFirst = {
				{ .Min = glm::ivec2(0, 0), .Size = glm::uvec2(3, 2) },
				{ .Min = glm::ivec2(2, 2), .Size = glm::uvec2(2, 1) },
				{ .Min = glm::ivec2(0, 3), .Size = glm::uvec2(1, 1) },
				{ .Min = glm::ivec2(3, 3), .Size = glm::uvec2(1, 1) },
			};
			CHECK(first == expectedFirst);

			// Never across a chunk's edge: chunk (1, 0)'s row and chunk (0, 1)'s cell are boxes of their own, in the chunks'
			// order (sorted by Y, then X).
			std::vector<TilemapBox> expectedAll = expectedFirst;
			expectedAll.push_back({ .Min = glm::ivec2(4, 0), .Size = glm::uvec2(4, 1) });
			expectedAll.push_back({ .Min = glm::ivec2(0, 4), .Size = glm::uvec2(1, 1) });
			CHECK(MergeTilemapBoxes(tilemap) == expectedAll);
			CHECK(MergeTilemapBoxes(TilemapData{}).empty());
		}
	}

}
