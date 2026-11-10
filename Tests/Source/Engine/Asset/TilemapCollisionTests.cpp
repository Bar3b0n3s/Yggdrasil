#include "TestsPCH.h"

#include "Engine/Asset/TilemapCollision.h"

#include "Engine/Core/Random.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

// The tilemap collider's boxes (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 25, stream T2).

namespace Engine {

	namespace {

		// A tilemap of `chunks` chunks of `chunkSize` cells, at seeded chunk coordinates in canonical order, each cell empty,
		// a solid entry or the non-solid one; a chunk the draw leaves empty keeps one solid cell (chunks are never empty).
		TilemapData MakeRandomTilemap(Random& random, uint32_t chunkSize, size_t chunks)
		{
			TilemapData tilemap;
			tilemap.ChunkSize = chunkSize;
			tilemap.Palette = { TilemapTile{ .Solid = true }, TilemapTile{ .Solid = false }, TilemapTile{ .Solid = true } };
			std::map<std::pair<int32_t, int32_t>, TilemapChunk> sorted;
			while (sorted.size() < chunks)
			{
				const auto x = static_cast<int32_t>(random.RangeInt(-3, 3));
				const auto y = static_cast<int32_t>(random.RangeInt(-3, 3));
				TilemapChunk chunk{ .X = x, .Y = y, .Tiles = std::vector<uint32_t>(static_cast<size_t>(chunkSize) * chunkSize, 0) };
				for (uint32_t& tile : chunk.Tiles)
				{
					// Mostly solid, so boxes grow over several cells.
					const int64_t draw = random.RangeInt(0, 9);
					tile = draw < 2 ? 0 : (draw < 3 ? 2 : (draw < 6 ? 1 : 3));
				}
				chunk.Tiles[0] = chunk.Tiles[0] == 0 ? 1 : chunk.Tiles[0];
				sorted.emplace(std::make_pair(y, x), std::move(chunk));
			}
			for (auto& [key, chunk] : sorted)
				tilemap.Chunks.push_back(std::move(chunk));
			return tilemap;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("TilemapCollision: solid cells merge into boxes per chunk in scan order")
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

		// Decision 25's canonical merge, over seeded maps of every kind of chunk size, negative chunks included: the boxes are
		// exactly the solid cells (every solid cell in one box, no other cell in any), each inside one chunk, listed chunk by
		// chunk in the tilemap's order and, within a chunk, by their first cell in scan order; and each box is the greedy
		// scan's (it cannot grow right on its bottom row, nor up by a whole row), so the shape depends on the cells alone.
		TEST_CASE("TilemapCollision: merged boxes are exactly the solid cells, chunk by chunk in scan order")
		{
			Random random(2117);
			for (const uint32_t chunkSize : { 1u, 2u, 4u, 8u, 16u })
			{
				for (int map = 0; map < 6; ++map)
				{
					CAPTURE(chunkSize);
					CAPTURE(map);
					const TilemapData tilemap = MakeRandomTilemap(random, chunkSize, 4);
					const std::vector<TilemapBox> boxes = MergeTilemapBoxes(tilemap);
					CHECK(MergeTilemapBoxes(tilemap) == boxes);

					// Every cell's box, -1 for none.
					std::map<std::pair<int32_t, int32_t>, int64_t> owners;
					const auto chunkOf = [chunkSize](glm::ivec2 cell)
					{
						const auto size = static_cast<int32_t>(chunkSize);
						return glm::ivec2(cell.x >= 0 ? cell.x / size : -((-cell.x + size - 1) / size), cell.y >= 0 ? cell.y / size : -((-cell.y + size - 1) / size));
					};
					size_t boxIndex = 0;
					std::vector<TilemapBox> concatenated;
					for (const TilemapChunk& chunk : tilemap.Chunks)
					{
						const std::vector<TilemapBox> chunkBoxes = MergeTilemapChunkBoxes(tilemap, chunk);
						concatenated.insert(concatenated.end(), chunkBoxes.begin(), chunkBoxes.end());
						for (size_t index = 0; index < chunkBoxes.size(); ++index)
						{
							const TilemapBox& box = chunkBoxes[index];
							REQUIRE(box.Size.x >= 1);
							REQUIRE(box.Size.y >= 1);
							// Inside the chunk.
							CHECK(chunkOf(box.Min) == glm::ivec2(chunk.X, chunk.Y));
							CHECK(chunkOf(box.Min + glm::ivec2(box.Size) - glm::ivec2(1)) == glm::ivec2(chunk.X, chunk.Y));
							// Scan order of the first cells.
							if (index > 0)
							{
								const TilemapBox& previous = chunkBoxes[index - 1];
								CHECK(std::make_pair(previous.Min.y, previous.Min.x) < std::make_pair(box.Min.y, box.Min.x));
							}
							for (int32_t y = box.Min.y; y < box.Min.y + static_cast<int32_t>(box.Size.y); ++y)
							{
								for (int32_t x = box.Min.x; x < box.Min.x + static_cast<int32_t>(box.Size.x); ++x)
									CHECK(owners.emplace(std::make_pair(x, y), static_cast<int64_t>(boxIndex + index)).second);
							}
						}
						boxIndex += chunkBoxes.size();
					}
					CHECK(concatenated == boxes);

					// The boxes hold exactly the solid cells.
					const auto size = static_cast<int32_t>(chunkSize);
					const auto ownerOf = [&owners](int32_t x, int32_t y)
					{
						const auto found = owners.find(std::make_pair(x, y));
						return found != owners.end() ? found->second : int64_t{ -1 };
					};
					const auto solidAt = [&tilemap, size](int32_t x, int32_t y)
					{
						for (const TilemapChunk& chunk : tilemap.Chunks)
						{
							if (x >= chunk.X * size && x < (chunk.X + 1) * size && y >= chunk.Y * size && y < (chunk.Y + 1) * size)
							{
								const auto index = static_cast<size_t>((y - chunk.Y * size) * size + (x - chunk.X * size));
								return IsTilemapCellSolid(tilemap, chunk.Tiles[index]);
							}
						}
						return false;
					};
					size_t solidCells = 0;
					for (const TilemapChunk& chunk : tilemap.Chunks)
					{
						for (int32_t y = chunk.Y * size; y < (chunk.Y + 1) * size; ++y)
						{
							for (int32_t x = chunk.X * size; x < (chunk.X + 1) * size; ++x)
							{
								const bool solid = solidAt(x, y);
								CHECK(solid == (ownerOf(x, y) >= 0));
								solidCells += solid ? 1 : 0;
							}
						}
					}
					CHECK(owners.size() == solidCells);

					// Greedy: no box could have grown right along its bottom row, nor up by the whole row above it, over cells
					// solid, in its chunk and not in a box that starts before it.
					for (size_t index = 0; index < boxes.size(); ++index)
					{
						const TilemapBox& box = boxes[index];
						const glm::ivec2 chunk = chunkOf(box.Min);
						const auto growable = [&](int32_t x, int32_t y)
						{
							return chunkOf(glm::ivec2(x, y)) == chunk && solidAt(x, y) && ownerOf(x, y) >= static_cast<int64_t>(index);
						};
						CHECK_FALSE(growable(box.Min.x + static_cast<int32_t>(box.Size.x), box.Min.y));
						bool rowAbove = true;
						for (int32_t x = box.Min.x; x < box.Min.x + static_cast<int32_t>(box.Size.x); ++x)
							rowAbove = rowAbove && growable(x, box.Min.y + static_cast<int32_t>(box.Size.y));
						CHECK_FALSE(rowAbove);
					}
				}
			}
		}
	}

}
