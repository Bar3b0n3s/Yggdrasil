#pragma once

#include "Engine/Asset/TilemapData.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// The order and the places a TilemapRenderer draws its tiles at (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md
// decision 24; Scene/Components/TilemapRendererComponent.h states the rule): the one walk over a tilemap's cells that the
// Sprites stage (Renderer), 2D picking and tilemap bounds (Scene) share, so what is drawn is what is picked and bounded. In
// the Asset module beside TilemapData, which both Renderer and Scene may include (§3). Pure; thread-safe.

namespace Engine {

	// The world matrix a tile of `tilemap` at cell `cell` draws with: the tilemap's rendered world matrix `world` times the
	// translation to the cell's centre (GetTilemapCellCenter), exactly the world matrix of a sprite drawn there.
	[[nodiscard]] inline glm::mat4 GetTilemapTileWorld(const glm::mat4& world, const TilemapData& tilemap, glm::ivec2 cell)
	{
		return world * glm::translate(glm::mat4(1.0f), glm::vec3(GetTilemapCellCenter(tilemap, cell), 0.0f));
	}

	// Calls `visit(chunk, cell, value)` (`chunk` the index in tilemap.Chunks, `value` the cell's value, never 0) for every
	// non-empty cell of `tilemap` (valid: ValidateTilemap) whose chunk `includeChunk(chunk)` accepts, in the tilemap's draw
	// order: row by row from the top row (highest y) down, each row left to right, across the whole tilemap and never chunk
	// by chunk; with `reverse`, exactly backwards (the last-drawn tile first). Stops as soon as `visit` returns false.
	template<typename IncludeChunk, typename Visit>
	void ForEachTilemapTileInDrawOrder(const TilemapData& tilemap, bool reverse, IncludeChunk&& includeChunk, Visit&& visit)
	{
		const std::vector<TilemapChunk>& chunks = tilemap.Chunks;
		const int64_t size = tilemap.ChunkSize;
		const size_t cells = static_cast<size_t>(size * size);
		// The chunk rows: [first, last) ranges of chunks of one Y, in ascending Y (the chunks are sorted by (Y, X)).
		std::vector<std::pair<size_t, size_t>> rows;
		for (size_t index = 0; index < chunks.size();)
		{
			size_t end = index + 1;
			while (end < chunks.size() && chunks[end].Y == chunks[index].Y)
				++end;
			rows.emplace_back(index, end);
			index = end;
		}
		const auto visitCell = [&chunks, size, cells, &visit](size_t chunk, int64_t column, int64_t row) -> bool
		{
			const TilemapChunk& data = chunks[chunk];
			const size_t index = static_cast<size_t>(row * size + column);
			if (index >= cells || index >= data.Tiles.size() || data.Tiles[index] == 0)
				return true;
			const glm::ivec2 cell(static_cast<int32_t>(data.X * size + column), static_cast<int32_t>(data.Y * size + row));
			return visit(chunk, cell, data.Tiles[index]);
		};
		if (!reverse)
		{
			for (size_t rowIndex = rows.size(); rowIndex-- > 0;)
			{
				const auto [first, last] = rows[rowIndex];
				for (int64_t row = size - 1; row >= 0; --row)
				{
					for (size_t chunk = first; chunk < last; ++chunk)
					{
						if (!includeChunk(chunk))
							continue;
						for (int64_t column = 0; column < size; ++column)
						{
							if (!visitCell(chunk, column, row))
								return;
						}
					}
				}
			}
			return;
		}
		for (const auto [first, last] : rows)
		{
			for (int64_t row = 0; row < size; ++row)
			{
				for (size_t chunk = last; chunk-- > first;)
				{
					if (!includeChunk(chunk))
						continue;
					for (int64_t column = size - 1; column >= 0; --column)
					{
						if (!visitCell(chunk, column, row))
							return;
					}
				}
			}
		}
	}

}
