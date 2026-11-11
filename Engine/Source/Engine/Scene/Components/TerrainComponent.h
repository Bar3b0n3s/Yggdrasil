#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Engine {

	// M26 (C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 20): a terrain's material layers, one per channel of
	// its splat map (red, green, blue, alpha).
	inline constexpr uint32_t MaxTerrainLayers = 4;
	inline constexpr float MaxTerrainSize = 100000.0f;

	// Registry struct "TerrainLayer": one material layer of a Terrain, tiled over its surface.
	struct TerrainLayer
	{
		TypedAssetHandle<AssetType::Texture> BaseColorMap{}; // null: white (the layer is its Tint)
		TypedAssetHandle<AssetType::Texture> NormalMap{};    // null: flat
		glm::vec3 Tint = glm::vec3(1.0f);                    // linear, multiplies the base colour
		float Tiling = 4.0f;                                 // metres per repeat of the layer's maps, (0, 1000]
		float Roughness = 0.9f;                              // perceptual, [0, 1]
		float Metallic = 0.0f;                               // [0, 1]
	};

	// Registry name "Terrain", version 1, category Terrain (M26, C26.1, C-TERR; decisions 4, 20, 21 and 22): a heightfield
	// laid out under the entity and drawn with up to four splatted material layers. Requires Transform.
	//   - The surface: Heightfield's samples spread over Size metres along local X and Z from the entity's origin, heights up
	//     to HeightScale metres along local +Y (Asset/TerrainGeometry.h states the placement and the triangulation that the
	//     drawing, the physics and the queries share). Null or failing to load (TERRAIN_NO_HEIGHTFIELD): nothing is drawn and a
	//     TerrainCollider makes no body.
	//   - The look: Layers (at most MaxTerrainLayers) blended per pixel by SplatMap's channels (red layer 0, green 1, blue 2,
	//     alpha 3), each weight divided by their sum (all zero, or a null SplatMap: layer 0 alone); a layer's maps tile every
	//     Tiling metres in the terrain's local XZ plane. The splat map is a data texture: a Texture imported with Usage Linear
	//     and Compression None (RGBA8Unorm), so the shader samples the painted bytes as they are (Usage Color would decode R, G
	//     and B through the sRGB EOTF) and a stroke reads back byte-identical (ADR 0028's phase-3 ledger: Compression None);
	//     every path that makes one sets both (terrain.edit's first paint, and so the editor's paint tool), and one imported
	//     otherwise is TERRAIN_SPLAT_NOT_RAW. It draws in its own passes (TerrainDepthNormal, TerrainForward), lit like a mesh
	//     by M23's light loop and the probes, receiving shadows when ReceiveShadows; it casts none (a kept limit, Backlog §6).
	//   - Detail: chunks of the heightfield drawn at a level of detail by their distance to the camera, without cracks between
	//     levels (Renderer/TerrainLod.h); LodBias scales the distances at which a chunk drops detail (above 1 keeps detail
	//     farther).
	//   - Editing: terrain.edit (and the editor's sculpt and paint tools, which call it) rewrites the heightfield's and the
	//     splat map's source files; there are no holes and no run-time edits (kept limits, Backlog §6), so a terrain is component
	//     data and assets, which the scene's serialization and the asset files cover.
	struct TerrainComponent
	{
		TypedAssetHandle<AssetType::Heightfield> Heightfield{};
		glm::vec2 Size{ 64.0f };            // metres along local X and Z, each in [0.01, MaxTerrainSize]
		float HeightScale = 16.0f;          // metres at the largest sample, [0.001, MaxTerrainSize]
		std::vector<TerrainLayer> Layers{}; // at most MaxTerrainLayers; empty draws one white layer
		TypedAssetHandle<AssetType::Texture> SplatMap{};
		bool ReceiveShadows = true;
		float LodBias = 1.0f; // [0.25, 4]
	};

}
