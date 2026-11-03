#pragma once

#include "Engine/Asset/TilemapData.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// The collision boxes of a tilemap (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 25): the solid cells
// of each chunk merged into as few rectangles as a greedy scan gives, the shape the TilemapCollider lays into the 2D world as
// one compound (Scene/Components/TilemapColliderComponent.h). In Asset beside TilemapData, a pure function of the data, so the
// play session's Physics2DSystem, the edit-time validator and the collider debug draw build the same boxes in the same
// order. Pure; deterministic in every configuration (integers only).

namespace Engine {

	// The most boxes one TilemapCollider makes: beyond it the collider is PHYSICS_INVALID_SHAPE (a compound of more sub-shapes
	// costs Jolt's broad phase more than any map needs).
	inline constexpr uint32_t MaxTilemapColliderBoxes = 65536;

	// One merged box: the cells [Min.x, Min.x + Size.x) x [Min.y, Min.y + Size.y) of the tilemap's grid (cell coordinates,
	// TilemapData.h), both sizes >= 1.
	struct TilemapBox
	{
		glm::ivec2 Min = glm::ivec2(0);
		glm::uvec2 Size = glm::uvec2(1);

		bool operator==(const TilemapBox&) const = default;
	};

	// Whether cell value `tile` of `tilemap` is solid: not empty, a palette entry (a value beyond the palette is not), and that
	// entry's Solid set.
	[[nodiscard]] bool IsTilemapCellSolid(const TilemapData& tilemap, uint32_t tile);

	// The boxes of `chunk` (a chunk of `tilemap`, valid): its solid cells visited row by row from its bottom row up, each row
	// left to right; a solid cell in no box yet starts one, which grows right while the next cell of its row is solid and in no
	// box, then up while every cell of the next row across its width is solid and in no box; its cells join it. Boxes in the
	// order they start, never reaching outside the chunk. Pure.
	[[nodiscard]] std::vector<TilemapBox> MergeTilemapChunkBoxes(const TilemapData& tilemap, const TilemapChunk& chunk);

	// Every chunk's boxes, the chunks in the tilemap's order (sorted by (Y, X)): the TilemapCollider's sub-shapes in their
	// order. Pure.
	[[nodiscard]] std::vector<TilemapBox> MergeTilemapBoxes(const TilemapData& tilemap);

}
