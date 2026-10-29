#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace Engine {

	// The default sorting layer: the first entry of the project's SortingLayers.Layers, and the layer a sprite naming an
	// undeclared one draws in (SPRITE_UNKNOWN_SORTING_LAYER; Docs/Decisions/0032-m21-contract.md decision 4).
	inline constexpr std::string_view DefaultSortingLayerName = "Default";

	// Registry name "SpriteRenderer" (Architecture §5.3; M21, Docs/Decisions/0032-m21-contract.md decision 3): draws one
	// sprite in the entity's local XY plane, facing +Z, in the Sprites stage (Renderer/RenderStage.h) through the
	// QuadBatcher's Hdr pipeline, unlit unless Lit is set. Requires Transform.
	//   - The image: frame Frame of the sprite sheet Sheet (Asset/SpriteSheetData.h: its texel rectangle and pivot; a sheet
	//     with a null texture shows white); with a null Sheet, the whole of Texture, pivoted at its centre; with neither, the
	//     built-in white square (BuiltinAssetHandles::SpriteSquareTexture).
	//   - What is missing, one rule for drawing, picking (Scene/SpritePicking.h), bounds (Scene/EntityBounds.h) and the
	//     editor: a texture that is missing or fails to load, Texture or the sheet's own, is replaced by its placeholder, the
	//     Missing checker (engine://Textures/Missing, 64 x 64 texels), shown whole over the sprite's quad: the frame's quad
	//     with a sheet, the placeholder's own 64 x 64 texels centred with Texture. That quad is drawn, picked and bounded. A
	//     sheet that is missing or fails to load (sheets have no placeholder) and a Frame that is not an index of the sheet's
	//     frames (SPRITE_FRAME_OUT_OF_RANGE) give no quad: the sprite draws nothing, is never picked and adds no bounds.
	//   - The size: the frame's (or texture's) texels divided by PixelsPerUnit, in the entity's local units, so its world size
	//     follows the entity's scale (Asset/SpriteGeometry.h). FlipX and FlipY mirror the image about the pivot.
	//   - Color multiplies every texel: linear RGB with straight alpha in the fourth component.
	//   - Order: sprites draw in (sorting layer, OrderInLayer, canonical entity order) order, back to front, without writing
	//     depth (painter's order), depth-tested against the opaque meshes. SortingLayer names an entry of the project's
	//     SortingLayers.Layers; an undeclared name draws in the first layer (SPRITE_UNKNOWN_SORTING_LAYER).
	//   - Lit (W21.2, C21.2): the sprite opts in to 2D lights (Light2D); unlit sprites keep their texels unchanged. The flag
	//     lands with the component so the component never needs a migration for it; it has no effect until W21.2.
	struct SpriteRendererComponent
	{
		TypedAssetHandle<AssetType::SpriteSheet> Sheet{};
		uint32_t Frame = 0;
		TypedAssetHandle<AssetType::Texture> Texture{};
		glm::vec4 Color = glm::vec4(1.0f); // linear, straight alpha
		bool FlipX = false;
		bool FlipY = false;
		float PixelsPerUnit = 16.0f;
		std::string SortingLayer = std::string(DefaultSortingLayerName);
		int32_t OrderInLayer = 0;
		bool Lit = false;
	};

}
