#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace Engine {

	// Registry name "TilemapRenderer" (Architecture §5.3; M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision
	// 24): draws a Tilemap asset's tiles (Asset/TilemapData.h) in the entity's local XY plane, facing +Z, in the Sprites stage,
	// through the sprite path. Requires Transform.
	//   - A tile is drawn exactly as a sprite: the non-empty cell (x, y) holding palette entry {Sheet, Frame} draws as a
	//     SpriteRenderer with that Sheet and Frame and this component's Color, PixelsPerUnit and Lit, unflipped, whose world
	//     matrix is the tilemap's rendered world matrix times the translation to the cell's centre (GetTilemapCellCenter:
	//     ((x + 0.5) CellSize.x, (y + 0.5) CellSize.y, 0)), so the frame's pivot sits at the cell's centre and a tilemap
	//     renders byte-identical to the same tiles drawn as sprites at those places. What is missing follows
	//     SpriteRendererComponent.h's one rule per tile: a sheet's missing texture shows its placeholder over the tile's quad; a
	//     missing sheet or a Frame the sheet lacks (TILEMAP_FRAME_OUT_OF_RANGE) draws nothing for that tile. A missing or
	//     failed Tilemap draws nothing (tilemaps have no placeholder).
	//   - Order: the tilemap is one item of the 2D draw order, sorted with the sprites by (sorting layer, OrderInLayer,
	//     canonical entity order); its own tiles draw in the tilemap's cell order, row by row from the top row (highest y)
	//     down, each row left to right, across the whole tilemap and never chunk by chunk, so a tile taller than its cell
	//     draws over the row above it and tiles overlapping across a chunk edge draw in that order.
	//   - Chunked rendering: the renderer keeps each tilemap's chunk bounds between frames (rebuilt when the tilemap, a
	//     sheet of it or PixelsPerUnit changes; the textures are looked up every render) and draws only the chunks whose
	//     bounds meet the view: a chunk's bounds are the union of its tiles' quads (each tile's whole sprite rectangle,
	//     whatever its frame's size and pivot, never only its cell), in world space, and a chunk meets the view unless they
	//     lie wholly outside its clip rectangle. So a tile drawn larger than its cell never pops in or out at the view's
	//     edge. The tiles of the chunks drawn are merged into the cell order above.
	//   - One bound a view: the QuadBatcher draws a record of more than MaxQuadsPerRecord quads in further draws
	//     (Renderer/QuadBatcher.h), and a view draws at most SceneRenderer::MaxTileQuadsPerView (2^19, two of its batches)
	//     tiles, the first in its 2D draw order across its tilemaps, of the chunks that meet it, so the renderer's CPU memory
	//     stays bounded however many tiles a zoomed-out view shows; the tiles past it are not drawn, which the renderer logs
	//     once, and every sprite still draws, those after the tilemap in the draw order included (M21's milestone review).
	// Rendering only: it makes no 2D body (TilemapColliderComponent does) and adds nothing to the state hash beyond its own
	// component data in the scene's canonical text.
	struct TilemapRendererComponent
	{
		TypedAssetHandle<AssetType::Tilemap> Tilemap{};
		glm::vec4 Color = glm::vec4(1.0f); // linear, straight alpha; multiplies every tile's texels
		float PixelsPerUnit = 16.0f;       // texels per local unit, as SpriteRenderer's
		std::string SortingLayer = std::string(DefaultSortingLayerName);
		int32_t OrderInLayer = 0;
		bool Lit = false; // its tiles take the 2D lights (Light2DComponent.h), with the flat normal
	};

}
