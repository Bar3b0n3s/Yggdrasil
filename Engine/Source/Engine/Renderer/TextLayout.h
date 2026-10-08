#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

// Text layout for the TextRenderer (Architecture §8.10): pure CPU functions from a font's metrics (Asset/FontData.h) and a
// UTF-8 string to glyph quads, and the placement of a screen text block on the viewport. Deterministic: identical input
// gives identical quads in every configuration (only IEEE-exact operations on the font's floats). Thread-safe (no state).
// Frozen by the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 11); stream D implements it.

namespace Engine {

	struct FontData;

	// The 1080p reference height that screen text sizes and offsets are given at (§8.10: "scaled by viewport height / 1080").
	inline constexpr float TextReferenceHeight = 1080.0f;

	// One glyph quad of a laid-out text block, in em units relative to the block's top-left corner (+x right, +y down), and
	// its rectangle in the font's atlas (normalized, origin top left; FontGlyph::AtlasMin and AtlasMax).
	struct TextGlyphQuad
	{
		glm::vec2 PositionMin = glm::vec2(0.0f);
		glm::vec2 PositionMax = glm::vec2(0.0f);
		glm::vec2 AtlasMin = glm::vec2(0.0f);
		glm::vec2 AtlasMax = glm::vec2(0.0f);

		bool operator==(const TextGlyphQuad&) const = default;
	};

	struct TextLayoutResult
	{
		// One quad per glyph with an outline (spaces advance the pen but have no quad), in string order.
		std::vector<TextGlyphQuad> Quads{};
		// The block's size in em units: the widest line's advance width, and LineCount * (Ascent - Descent + LineGap) minus
		// the last line's LineGap.
		glm::vec2 Size = glm::vec2(0.0f);
		uint32_t LineCount = 0; // lines of the text ('\n' separates them; an empty text has 0)
		// Codepoints the font has no glyph for, plus invalid UTF-8 sequences (each skipped without advancing the pen; kerning
		// pairs the glyphs on either side of them).
		uint32_t SkippedCodepoints = 0;
	};

	// Lays out `text` with `font`: each line's glyphs placed along its baseline at y = Ascent + lineIndex * (Ascent - Descent
	// + LineGap) (em units, down from the block's top), each pen step the glyph's Advance plus the kerning of the pair
	// (FontKerningPair) when the next glyph follows, and every line aligned within the block's width by `alignment`.
	// '\r' is ignored. Pure.
	[[nodiscard]] TextLayoutResult LayoutText(const FontData& font, std::string_view text, RenderTextAlignment alignment);

	// The top-left pixel of a screen text block (+x right, +y down from the viewport's top left) whose size is `blockSize`
	// pixels: Anchor * viewport + Offset * scale - Pivot * blockSize, with scale = viewportHeight / TextReferenceHeight.
	// `viewportWidth` and `viewportHeight` >= 1 (asserted). Pure.
	[[nodiscard]] glm::vec2 PlaceScreenText(const TextItem& item, const glm::vec2& blockSize, uint32_t viewportWidth, uint32_t viewportHeight);

}
