#include "EnginePCH.h"
#include "Engine/Asset/SpriteGeometry.h"

#include "Engine/Core/Assert.h"

#include <cmath>

namespace Engine {

	std::optional<SpriteFrameGeometry> GetSpriteFrameGeometry(const SpriteSheetData* sheet, uint32_t frame, glm::uvec2 textureSize)
	{
		ENGINE_CORE_ASSERT(textureSize.x >= 1 && textureSize.y >= 1, "a sprite's texture is at least 1 x 1 texels, got {} x {}", textureSize.x, textureSize.y);
		if (sheet == nullptr)
			return SpriteFrameGeometry{ .TextureSize = textureSize, .RectMin = glm::uvec2(0), .RectSize = textureSize, .Pivot = glm::vec2(0.5f) };
		if (frame >= sheet->Frames.size())
			return std::nullopt;
		const SpriteFrame& source = sheet->Frames[frame];
		return SpriteFrameGeometry{
			.TextureSize = textureSize,
			.RectMin = glm::uvec2(source.Rect.X, source.Rect.Y),
			.RectSize = glm::uvec2(source.Rect.Width, source.Rect.Height),
			.Pivot = source.Pivot,
		};
	}

	SpriteLocalQuad ComputeSpriteLocalQuad(const SpriteFrameGeometry& frame, float pixelsPerUnit, bool flipX, bool flipY)
	{
		ENGINE_CORE_ASSERT(std::isfinite(pixelsPerUnit) && pixelsPerUnit > 0.0f, "a sprite needs finite, positive pixels per unit, got {}", pixelsPerUnit);
		ENGINE_CORE_ASSERT(frame.TextureSize.x >= 1 && frame.TextureSize.y >= 1 && frame.RectSize.x >= 1 && frame.RectSize.y >= 1,
			"a sprite frame and its texture are at least 1 x 1 texels");
		// The edges in texels from the pivot first, then one division each, so a pivot on the texel grid gives corners that are
		// exact multiples of 1 / pixelsPerUnit (the file comment).
		const glm::vec2 size(static_cast<float>(frame.RectSize.x), static_cast<float>(frame.RectSize.y));
		const glm::vec2 pivot = frame.Pivot * size;
		float left = -pivot.x / pixelsPerUnit;
		float right = (size.x - pivot.x) / pixelsPerUnit;
		float top = pivot.y / pixelsPerUnit;
		float bottom = -(size.y - pivot.y) / pixelsPerUnit;
		if (flipX)
		{
			left = -left;
			right = -right;
		}
		if (flipY)
		{
			top = -top;
			bottom = -bottom;
		}

		const glm::vec2 texture(static_cast<float>(frame.TextureSize.x), static_cast<float>(frame.TextureSize.y));
		SpriteLocalQuad quad;
		quad.Corners = { glm::vec2(left, top), glm::vec2(right, top), glm::vec2(right, bottom), glm::vec2(left, bottom) };
		quad.TexCoordMin = glm::vec2(static_cast<float>(frame.RectMin.x), static_cast<float>(frame.RectMin.y)) / texture;
		quad.TexCoordMax = (glm::vec2(static_cast<float>(frame.RectMin.x), static_cast<float>(frame.RectMin.y)) + size) / texture;
		return quad;
	}

}
