#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <utility>
#include <vector>

// What the Sprites stage keeps of a tilemap between renders (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md
// decision 24; Scene/Components/TilemapRendererComponent.h states the rule): each chunk's bounds, the union of the quads its
// tiles draw with in the tilemap's local XY plane, so a render draws only the chunks that meet its view without walking the
// cells of the others. CPU state only: the tiles' textures are the GpuResourceCache's mirrors, resolved by every render, so
// a device loss and its recovery leave it valid (no GPU object of its own).

namespace Engine {

	struct TilemapData;

	namespace Detail {

		// One chunk's bounds in its tilemap's local XY plane: the union of the local quads (cell centre plus the sprite quad of
		// the tile's palette entry, Asset/SpriteGeometry.h) of the tiles it draws, whatever their frames' sizes and pivots.
		struct TilemapChunkBounds
		{
			glm::vec2 Min = glm::vec2(0.0f);
			glm::vec2 Max = glm::vec2(0.0f);
			bool HasTiles = false; // false: no tile of the chunk has a quad (empty palette entries, missing sheets or frames)
		};

		// The chunk bounds of one tilemap drawn at one PixelsPerUnit, rebuilt when the tilemap (its loaded data or its asset
		// version), the asset version of one of its palette's sheets or the PixelsPerUnit changes.
		struct TilemapDrawCache
		{
			AssetHandle Tilemap{};
			float PixelsPerUnit = 0.0f;
			AssetRef<TilemapData> Data{}; // the loaded tilemap the bounds were built from (kept, so it is never another's)
			uint64_t Version = 0;         // AssetManager::GetVersion of the tilemap at the build
			// The palette's sheets with their asset versions at the build, sorted by handle and unique.
			std::vector<std::pair<AssetHandle, uint64_t>> SheetVersions{};
			std::vector<TilemapChunkBounds> Chunks{}; // parallel to the tilemap's Chunks
			bool Used = false;                        // drawn by the current render (SceneRenderer::State::ReleaseUnusedTilemaps)
		};

	}

}
