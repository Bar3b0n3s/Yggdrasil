#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The editor's tile brush (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 26): in the scene view, with
// an entity holding a TilemapRenderer selected, the brush paints the palette entry chosen in its toolbar into the cells the
// cursor drags over (left button) or erases them (right button), and each stroke, from press to release, is one tilemap.paint
// of every cell it crossed, so one undo step; the tilemap draws the stroke's cells only once it is applied (no preview of
// unapplied cells). These pure functions turn the view's rays into cells and a stroke into the method's params, so
// automation paints exactly as the brush does (Roadmap rule 5). The brush itself (Editor/Viewport/TilemapBrush.{h,cpp},
// called from SceneViewportPanel, M21's file) and the tilemap asset's inspector section (InspectorPanel: its palette, cell
// size and chunk size through asset.setProperties) are stream T3's; T3 implements these functions.

namespace Engine {

	// The cell (Asset/TilemapData.h's cell coordinates) under `ray` (a unit direction) for a tilemap whose entity has the world
	// matrix `tilemapWorld` and whose cells are `cellSize` local units: the point where the ray meets the entity's local plane
	// z = 0, taken to local space, divided by the cell size and floored. nullopt when the ray is parallel to the plane, meets
	// it behind its origin, the matrix is not invertible or the cell lies outside MaxTilemapCellCoordinate. Pure.
	[[nodiscard]] std::optional<glm::ivec2> IntersectTilemapCell(const RenderRay& ray, const glm::mat4& tilemapWorld, const glm::vec2& cellSize);

	// The cells of a drag from cell `from` to cell `to` (both included, `from` first): the cells of the line between their
	// centres in Bresenham's order, so a fast drag that skips cells between two frames still paints a connected line. Pure.
	[[nodiscard]] std::vector<glm::ivec2> GetTilemapStrokeCells(glm::ivec2 from, glm::ivec2 to);

	struct TilemapPaintRequest
	{
		AssetHandle Tilemap{};               // the selected TilemapRenderer's Tilemap
		std::span<const glm::ivec2> Cells{}; // the stroke's cells in the order the brush crossed them
		uint32_t Tile = 0;                   // the brush's cell value: 0 erases
	};

	// The tilemap.paint params of a stroke: {tilemap: the handle, cells: [{x, y, tile}]}, each cell once, at its first place
	// in the stroke. InvalidArgument for an invalid handle, no cell or more than MaxTilemapPaintCells distinct cells.
	[[nodiscard]] Result<Json> MakeTilemapPaintParams(const TilemapPaintRequest& request);

}
