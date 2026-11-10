#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Scene's half of the tilemaps' one rule (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 24): where
// render extraction places each tilemap in the one 2D draw order of sprites and tilemaps (RenderSnapshot::Tilemaps), which
// 2D picking follows, and the quads a tilemap's tiles are drawn with, which tilemap bounds and 2D picking take: each tile a
// sprite of its palette entry's frame (SpriteDrawing.h's ResolveSpriteCorners) at its cell's centre, in the order of
// Asset/TilemapDrawOrder.h. The Sprites stage (Renderer/SceneRenderer.cpp) resolves the same quads through its
// GpuResourceCache, so what is drawn is what is bounded and picked.

namespace Engine {

	class AssetManager;
	class Scene;
	struct TilemapData;
	struct TilemapRendererComponent;

	namespace Utils {

		// A sprite's place in the one 2D draw order: its sorting layer, order in layer and canonical entity position.
		struct SpriteDrawKey
		{
			uint32_t SortingLayer = 0;
			int32_t OrderInLayer = 0;
			uint32_t Canonical = 0;
		};

		// The snapshot item of `tilemap` (a TilemapRendererComponent with a Tilemap), drawn by entity `entity` at the world
		// matrix `world`, its sorting layer resolved against `sortingLayers` as a sprite's; DrawPosition 0 until
		// PlaceTilemapsForDrawing.
		[[nodiscard]] TilemapItem MakeTilemapItem(const TilemapRendererComponent& tilemap, const glm::mat4& world, UUID entity,
			std::span<const std::string> sortingLayers);

		// Places `tilemaps`, listed in canonical entity order (`canonical[i]` the canonical position of tilemaps[i]'s entity),
		// in the one draw order of sprites and tilemaps by (SortingLayer, OrderInLayer, canonical entity order), the sprites'
		// keys `spriteKeys` (any order): sets each one's DrawPosition (the number of sprites before it; an entity's sprite
		// draws before its tilemap) and sorts them stably by (SortingLayer, OrderInLayer) (RenderSnapshot::Tilemaps).
		void PlaceTilemapsForDrawing(std::span<TilemapItem> tilemaps, std::span<const uint32_t> canonical, std::vector<SpriteDrawKey> spriteKeys);

		// Every effectively enabled TilemapRenderer with a Tilemap of `scene` at its rendered world matrix of `alpha`, placed
		// among the scene's sprites (CollectSprites): what ExtractRenderSnapshot lists in RenderSnapshot::Tilemaps. Main thread.
		[[nodiscard]] std::vector<TilemapItem> CollectTilemaps(const Scene& scene, std::span<const std::string> sortingLayers, float alpha);

		// The tiles of one tilemap as their sprites are drawn: the tilemap, loaded, and the local quad of each palette entry
		// (index k - 1 for the cell value k), nullopt for an entry whose tiles draw nothing (a null, missing or failed sheet,
		// a Frame the sheet lacks). Resolved by ResolveTilemapTiles.
		struct TilemapTiles
		{
			AssetRef<TilemapData> Tilemap{};
			std::vector<std::optional<std::array<glm::vec2, 4>>> Palette{};
		};

		// The tiles of `item`'s tilemap (loaded through `assets`), or nullopt when it draws nothing: a tilemap that is missing,
		// fails to load or is not a tilemap (no placeholder), or a PixelsPerUnit that is not finite and positive. Each palette
		// entry's quad is SpriteDrawing.h's ResolveSpriteCorners of the sprite its tiles draw as. Main thread.
		[[nodiscard]] std::optional<TilemapTiles> ResolveTilemapTiles(AssetManager& assets, const TilemapItem& item);

	}

}
