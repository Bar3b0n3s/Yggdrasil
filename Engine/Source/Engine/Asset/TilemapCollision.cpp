#include "EnginePCH.h"
#include "Engine/Asset/TilemapCollision.h"

#include "Engine/Core/Assert.h"

#include <cstddef>

// The tilemap collider's boxes (Docs/Decisions/0032-m21-contract.md decision 25): a greedy scan of each chunk, in integers,
// so the boxes and their order are the same in every configuration.

namespace Engine {

	namespace Utils {

		// Whether `chunk` is one MergeTilemapChunkBoxes can read for chunks of `size` cells a side: ChunkSize^2 values, and its
		// coordinates within the cell range (ValidateTilemap's rules, checked again so a tilemap that skipped validation never
		// makes the cell arithmetic overflow).
		static bool IsMergeableTilemapChunk(const TilemapChunk& chunk, uint32_t size)
		{
			if (size == 0 || size > MaxTilemapChunkSize || chunk.Tiles.size() != static_cast<size_t>(size) * size)
				return false;
			const auto limit = static_cast<int32_t>(static_cast<uint32_t>(MaxTilemapCellCoordinate) / size);
			return chunk.X >= -limit && chunk.X < limit && chunk.Y >= -limit && chunk.Y < limit;
		}

	}

	bool IsTilemapCellSolid(const TilemapData& tilemap, uint32_t tile)
	{
		return tile != 0 && tile <= tilemap.Palette.size() && tilemap.Palette[tile - 1].Solid;
	}

	std::vector<TilemapBox> MergeTilemapChunkBoxes(const TilemapData& tilemap, const TilemapChunk& chunk)
	{
		const uint32_t size = tilemap.ChunkSize;
		const bool mergeable = Utils::IsMergeableTilemapChunk(chunk, size);
		ENGINE_CORE_ASSERT(mergeable, "MergeTilemapChunkBoxes: chunk ({}, {}) is not a valid chunk of a tilemap of {}-cell chunks", chunk.X, chunk.Y, size);
		if (!mergeable)
			return {};

		// Tiles[y * size + x], row y from the chunk's bottom row up (TilemapData.h).
		const auto cellIndex = [size](uint32_t x, uint32_t y)
		{
			return static_cast<size_t>(y) * size + x;
		};
		// A cell is free while it is solid and in no box yet.
		std::vector<uint8_t> freeCells(chunk.Tiles.size(), 0);
		for (size_t index = 0; index < chunk.Tiles.size(); ++index)
			freeCells[index] = IsTilemapCellSolid(tilemap, chunk.Tiles[index]) ? 1 : 0;

		std::vector<TilemapBox> boxes;
		const int32_t originX = chunk.X * static_cast<int32_t>(size);
		const int32_t originY = chunk.Y * static_cast<int32_t>(size);
		for (uint32_t y = 0; y < size; ++y)
		{
			for (uint32_t x = 0; x < size; ++x)
			{
				if (freeCells[cellIndex(x, y)] == 0)
					continue;
				// Right while the next cell of the row is free, then up while a whole row across the width is.
				uint32_t width = 1;
				while (x + width < size && freeCells[cellIndex(x + width, y)] != 0)
					++width;
				uint32_t height = 1;
				while (y + height < size)
				{
					bool rowFree = true;
					for (uint32_t column = x; column < x + width && rowFree; ++column)
						rowFree = freeCells[cellIndex(column, y + height)] != 0;
					if (!rowFree)
						break;
					++height;
				}
				for (uint32_t row = y; row < y + height; ++row)
				{
					for (uint32_t column = x; column < x + width; ++column)
						freeCells[cellIndex(column, row)] = 0;
				}
				boxes.push_back(TilemapBox{ .Min = glm::ivec2(originX + static_cast<int32_t>(x), originY + static_cast<int32_t>(y)), .Size = glm::uvec2(width, height) });
			}
		}
		return boxes;
	}

	std::vector<TilemapBox> MergeTilemapBoxes(const TilemapData& tilemap)
	{
		std::vector<TilemapBox> boxes;
		for (const TilemapChunk& chunk : tilemap.Chunks)
		{
			const std::vector<TilemapBox> chunkBoxes = MergeTilemapChunkBoxes(tilemap, chunk);
			boxes.insert(boxes.end(), chunkBoxes.begin(), chunkBoxes.end());
		}
		return boxes;
	}

}
