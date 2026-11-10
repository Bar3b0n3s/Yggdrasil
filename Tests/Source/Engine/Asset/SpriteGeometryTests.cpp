#include "TestsPCH.h"

#include "Engine/Asset/SpriteGeometry.h"

#include <optional>

// The sprite geometry (Docs/Decisions/0032-m21-contract.md decision 3, stream A).

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("SpriteGeometry: a sheet frame gives its texels and pivot, and a texture is whole and centred")
		{
			SpriteSheetData sheet;
			sheet.Frames.push_back(SpriteFrame{ .Name = "Idle", .Rect = { .X = 8, .Y = 4, .Width = 8, .Height = 12 }, .Pivot = glm::vec2(0.5f, 1.0f) });
			const std::optional<SpriteFrameGeometry> frame = GetSpriteFrameGeometry(&sheet, 0, glm::uvec2(32, 16));
			REQUIRE(frame.has_value());
			CHECK(frame->TextureSize == glm::uvec2(32, 16));
			CHECK(frame->RectMin == glm::uvec2(8, 4));
			CHECK(frame->RectSize == glm::uvec2(8, 12));
			CHECK(frame->Pivot == glm::vec2(0.5f, 1.0f));
			// SPRITE_FRAME_OUT_OF_RANGE: an index the sheet lacks draws nothing.
			CHECK_FALSE(GetSpriteFrameGeometry(&sheet, 1, glm::uvec2(32, 16)).has_value());

			const std::optional<SpriteFrameGeometry> texture = GetSpriteFrameGeometry(nullptr, 0, glm::uvec2(16, 8));
			REQUIRE(texture.has_value());
			CHECK(texture->TextureSize == glm::uvec2(16, 8));
			CHECK(texture->RectMin == glm::uvec2(0));
			CHECK(texture->RectSize == glm::uvec2(16, 8));
			CHECK(texture->Pivot == glm::vec2(0.5f));
		}

		TEST_CASE("SpriteGeometry: a frame's corners follow its pivot, size, pixels per unit and flips")
		{
			// 16 x 8 texels of a 32 x 16 texture at 8 pixels per unit: 2 x 1 units, the pivot at the bottom-left corner.
			const SpriteFrameGeometry frame{ .TextureSize = glm::uvec2(32, 16), .RectMin = glm::uvec2(16, 8), .RectSize = glm::uvec2(16, 8), .Pivot = glm::vec2(0.0f, 1.0f) };
			const SpriteLocalQuad quad = ComputeSpriteLocalQuad(frame, 8.0f, false, false);
			CHECK(quad.Corners[0] == glm::vec2(0.0f, 1.0f));
			CHECK(quad.Corners[1] == glm::vec2(2.0f, 1.0f));
			CHECK(quad.Corners[2] == glm::vec2(2.0f, 0.0f));
			CHECK(quad.Corners[3] == glm::vec2(0.0f, 0.0f));
			CHECK(quad.TexCoordMin == glm::vec2(0.5f, 0.5f));
			CHECK(quad.TexCoordMax == glm::vec2(1.0f, 1.0f));

			// FlipX mirrors about the pivot: the image's left edge is now on the right, the texture rectangle unchanged.
			const SpriteLocalQuad flipped = ComputeSpriteLocalQuad(frame, 8.0f, true, false);
			CHECK(flipped.Corners[0] == glm::vec2(0.0f, 1.0f));
			CHECK(flipped.Corners[1] == glm::vec2(-2.0f, 1.0f));
			CHECK(flipped.TexCoordMin == quad.TexCoordMin);
			const SpriteLocalQuad upsideDown = ComputeSpriteLocalQuad(frame, 8.0f, false, true);
			CHECK(upsideDown.Corners[0] == glm::vec2(0.0f, -1.0f));
			CHECK(upsideDown.Corners[2] == glm::vec2(2.0f, 0.0f));

			// A centred 16 x 16 frame at 16 pixels per unit lies on the texel grid: corners at +-0.5 exactly.
			const SpriteLocalQuad centred = ComputeSpriteLocalQuad(SpriteFrameGeometry{ .TextureSize = glm::uvec2(16), .RectSize = glm::uvec2(16) }, 16.0f, false, false);
			CHECK(centred.Corners[0] == glm::vec2(-0.5f, 0.5f));
			CHECK(centred.Corners[2] == glm::vec2(0.5f, -0.5f));
		}
	}

}
