#pragma once

#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>

// The geometry of a sprite (M21, C21.1; Docs/Decisions/0032-m21-contract.md decision 3): where a SpriteRenderer's image lies
// in its entity's local space and which texels it shows. The one rule the Sprites stage (Renderer), sprite bounds and 2D
// picking (Scene/EntityBounds.h, Scene/SpritePicking.h) and the editor share, so what is drawn is exactly what is bounded
// and picked. In the Asset module beside SpriteSheetData, which both Renderer and Scene may include.
//
// Conventions: texel space as SpriteSheetData.h's (x right, y down, whole texels); the entity's local space has +X right and
// +Y up, so a frame's top row is its highest. A frame of Width x Height texels is Width / PixelsPerUnit x
// Height / PixelsPerUnit local units, its Pivot at the local origin. FlipX negates every corner's X and FlipY every corner's
// Y (a mirror about the pivot), keeping the texture rectangle. Integer texel sizes and a pivot on the texel grid give corners
// that are exact multiples of 1 / PixelsPerUnit, so a sprite drawn at the PixelPerfectCamera's PixelsPerUnit on a snapped
// camera maps each texel to one scene pixel. Pure functions; thread-safe; IEEE-exact arithmetic only (Asset is not on the
// simulation path, but these feed the deterministic picking of Scene).

namespace Engine {

	// The texels one sprite shows.
	struct SpriteFrameGeometry
	{
		glm::uvec2 TextureSize = glm::uvec2(1); // the texture's size in texels, both >= 1
		glm::uvec2 RectMin = glm::uvec2(0);     // the frame's top-left texel
		glm::uvec2 RectSize = glm::uvec2(1);    // the frame's size in texels, both >= 1
		glm::vec2 Pivot = glm::vec2(0.5f);      // SpriteFrame::Pivot: a fraction of the frame from its top-left corner

		bool operator==(const SpriteFrameGeometry&) const = default;
	};

	// One sprite's quad in its entity's local space (z = 0): the corners in QuadDraw's order (Renderer/QuadBatcher.h: top-left,
	// top-right, bottom-right and bottom-left as the image shows them, so FlipX swaps their sides) and the normalized texture
	// rectangle, TexCoordMin at the top-left corner and TexCoordMax at the bottom-right one (texel edges over TextureSize,
	// without a half-texel inset).
	struct SpriteLocalQuad
	{
		std::array<glm::vec2, 4> Corners{};
		glm::vec2 TexCoordMin = glm::vec2(0.0f);
		glm::vec2 TexCoordMax = glm::vec2(1.0f);

		bool operator==(const SpriteLocalQuad&) const = default;
	};

	// The texels of frame `frame` of `sheet` over a texture of `textureSize` (both >= 1; a sheet with a null Texture passes
	// the frame's extent, RectMin + RectSize, so its white texel covers the frame): the frame's Rect and Pivot. With a null
	// `sheet`, the whole texture pivoted at its centre ((0.5, 0.5)). nullopt when `frame` is not an index of the sheet's
	// Frames (SPRITE_FRAME_OUT_OF_RANGE: the sprite draws nothing).
	[[nodiscard]] std::optional<SpriteFrameGeometry> GetSpriteFrameGeometry(const SpriteSheetData* sheet, uint32_t frame, glm::uvec2 textureSize);

	// The local quad of `frame` at `pixelsPerUnit` (finite, > 0) with the flips (see the file comment): with w = RectSize.x /
	// pixelsPerUnit, h = RectSize.y / pixelsPerUnit and (px, py) the Pivot, before the flips the corners are (-px w, py h),
	// ((1 - px) w, py h), ((1 - px) w, -(1 - py) h) and (-px w, -(1 - py) h). Asserts the documented ranges.
	[[nodiscard]] SpriteLocalQuad ComputeSpriteLocalQuad(const SpriteFrameGeometry& frame, float pixelsPerUnit, bool flipX, bool flipY);

}
