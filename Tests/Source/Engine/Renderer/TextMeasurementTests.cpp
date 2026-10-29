#include "TestsPCH.h"

#include "Engine/Renderer/TextMeasurement.h"

#include "Engine/Asset/FontData.h"

#include <glm/glm.hpp>

#include <array>
#include <string>

// The text-measurement layer (M22, C22.1, R-UI-TEXT; Docs/Decisions/0033-m22-contract.md decision 6) on TextLayoutTests'
// synthetic font: positions, word wrapping, line feeds and spacing, alignment, the font chain and byte offsets. Skipped by
// the contract; stream B un-skips them.

namespace Engine {

	namespace {

		// Ascent 0.8, descent -0.2, line gap 0.1 (a line is 1.1 em); 'A' advances 0.5, 'B' 0.6, ' ' 0.25 (no outline); the
		// pair "AB" kerns by -0.1. Plane boxes: 'A' (0, 0)-(0.5, 0.7), 'B' (0.05, 0)-(0.55, 0.7). Without B or without A when asked.
		FontData MakeFont(bool withA = true, bool withB = true)
		{
			FontData font;
			font.Ascent = 0.8f;
			font.Descent = -0.2f;
			font.LineGap = 0.1f;
			font.AtlasWidth = 4;
			font.AtlasHeight = 4;
			font.AtlasPixels.assign(16, std::byte{ 0 });
			font.Glyphs.push_back(FontGlyph{ .Codepoint = ' ', .Advance = 0.25f });
			if (withA)
				font.Glyphs.push_back(FontGlyph{ .Codepoint = 'A', .Advance = 0.5f, .PlaneMin = glm::vec2(0.0f), .PlaneMax = glm::vec2(0.5f, 0.7f), .AtlasMin = glm::vec2(0.0f), .AtlasMax = glm::vec2(0.5f) });
			if (withB)
				font.Glyphs.push_back(FontGlyph{ .Codepoint = 'B', .Advance = 0.6f, .PlaneMin = glm::vec2(0.05f, 0.0f), .PlaneMax = glm::vec2(0.55f, 0.7f), .AtlasMin = glm::vec2(0.5f), .AtlasMax = glm::vec2(1.0f) });
			if (withA && withB)
				font.Kerning = { FontKerningPair{ .First = 'A', .Second = 'B', .Advance = -0.1f } };
			REQUIRE(ValidateFontData(font).has_value());
			return font;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("TextMeasurement: a line measures its glyph advances and kerning at the size" * doctest::skip(true))
		{
			const FontData font = MakeFont();
			const std::array<const FontData*, 1> chain = { &font };
			const TextMeasurement measured = MeasureText(chain, "AB A", { .Size = 10.0f });
			REQUIRE(measured.Glyphs.size() == 3); // the space has no glyph
			REQUIRE(measured.Lines.size() == 1);
			CHECK(measured.SkippedCodepoints == 0);
			// 'A' at pen 0; 'B' at (0.5 - 0.1) * 10 = 4, its box from 4.5; the second 'A' at 4 + 6 + 2.5 = 12.5. The baseline is at
			// 8 pixels (0.8 em), a 0.7 em box above it spans y 1 to 8.
			CHECK(measured.Glyphs[0].Min.x == doctest::Approx(0.0f));
			CHECK(measured.Glyphs[0].Min.y == doctest::Approx(1.0f));
			CHECK(measured.Glyphs[0].Max.y == doctest::Approx(8.0f));
			CHECK(measured.Glyphs[1].Min.x == doctest::Approx(4.5f));
			CHECK(measured.Glyphs[1].AtlasMin == glm::vec2(0.5f));
			CHECK(measured.Glyphs[2].Min.x == doctest::Approx(12.5f));
			CHECK(measured.Glyphs[0].ByteOffset == 0);
			CHECK(measured.Glyphs[1].ByteOffset == 1);
			CHECK(measured.Glyphs[2].ByteOffset == 3);
			CHECK(measured.Lines[0].Width == doctest::Approx(17.5f));
			CHECK(measured.Lines[0].Baseline == doctest::Approx(8.0f));
			CHECK(measured.Lines[0].ByteBegin == 0);
			CHECK(measured.Lines[0].ByteEnd == 4);
			CHECK(measured.Size.x == doctest::Approx(17.5f));
			CHECK(measured.Size.y == doctest::Approx(10.0f)); // one line: Ascent - Descent
			CHECK(MeasureTextSize(chain, "AB A", { .Size = 10.0f }) == measured.Size);
			// Identical input, identical output.
			CHECK(MeasureText(chain, "AB A", { .Size = 10.0f }) == measured);
		}

		TEST_CASE("TextMeasurement: Word wraps at the last space that fits MaxWidth and breaks a longer word between glyphs" * doctest::skip(true))
		{
			const FontData font = MakeFont();
			const std::array<const FontData*, 1> chain = { &font };
			// "AB" is 10 pixels wide; "AB A" would be 17.5: the line breaks at the space, which neither line keeps.
			const TextMeasurement wrapped = MeasureText(chain, "AB A", { .Size = 10.0f, .Wrap = TextWrap::Word, .MaxWidth = 12.0f });
			REQUIRE(wrapped.Lines.size() == 2);
			CHECK(wrapped.Lines[0].ByteBegin == 0);
			CHECK(wrapped.Lines[0].ByteEnd == 2);
			CHECK(wrapped.Lines[1].ByteBegin == 3);
			CHECK(wrapped.Lines[1].ByteEnd == 4);
			CHECK(wrapped.Lines[1].Top == doctest::Approx(11.0f));
			CHECK(wrapped.Size.x == doctest::Approx(12.0f));
			CHECK(wrapped.Size.y == doctest::Approx(21.0f));
			// "ABAB" is 20 pixels wide: its third glyph would end at 15, beyond 12, so the word breaks before it.
			const TextMeasurement broken = MeasureText(chain, "ABAB", { .Size = 10.0f, .Wrap = TextWrap::Word, .MaxWidth = 12.0f });
			REQUIRE(broken.Lines.size() == 2);
			CHECK(broken.Lines[0].GlyphCount == 2);
			CHECK(broken.Lines[1].GlyphCount == 2);
			CHECK(broken.Lines[1].FirstGlyph == 2);
			// Without wrapping, one line whatever MaxWidth.
			CHECK(MeasureText(chain, "AB A", { .Size = 10.0f, .MaxWidth = 12.0f }).Lines.size() == 1);
		}

		TEST_CASE("TextMeasurement: line feeds break lines, carriage returns are ignored, and LineSpacing scales the pitch" * doctest::skip(true))
		{
			const FontData font = MakeFont();
			const std::array<const FontData*, 1> chain = { &font };
			const TextMeasurement measured = MeasureText(chain, "A\r\nB", { .Size = 10.0f, .LineSpacing = 2.0f });
			REQUIRE(measured.Lines.size() == 2);
			// The second line is 1.1 em * 2 lower: top 22, baseline 30; the block is 22 + Ascent - Descent high.
			CHECK(measured.Lines[1].Top == doctest::Approx(22.0f));
			CHECK(measured.Lines[1].Baseline == doctest::Approx(30.0f));
			CHECK(measured.Size.y == doctest::Approx(32.0f));
			CHECK(measured.Lines[1].ByteBegin == 3);
			CHECK(MeasureText(chain, "", { .Size = 10.0f }).Lines.empty());
		}

		TEST_CASE("TextMeasurement: Alignment places each line within MaxWidth, or within the widest line" * doctest::skip(true))
		{
			const FontData font = MakeFont();
			const std::array<const FontData*, 1> chain = { &font };
			// "AB" is 10 wide and "A" 5: right-aligned in the widest line, the second starts at 5.
			const TextMeasurement right = MeasureText(chain, "AB\nA", { .Size = 10.0f, .Alignment = RenderTextAlignment::Right });
			REQUIRE(right.Lines.size() == 2);
			CHECK(right.Size.x == doctest::Approx(10.0f));
			CHECK(right.Lines[0].Left == doctest::Approx(0.0f));
			CHECK(right.Lines[1].Left == doctest::Approx(5.0f));
			// Centred in a 20-pixel block.
			const TextMeasurement centred = MeasureText(chain, "AB\nA", { .Size = 10.0f, .MaxWidth = 20.0f, .Alignment = RenderTextAlignment::Center });
			REQUIRE(centred.Lines.size() == 2);
			CHECK(centred.Size.x == doctest::Approx(20.0f));
			CHECK(centred.Lines[0].Left == doctest::Approx(5.0f));
			CHECK(centred.Lines[1].Left == doctest::Approx(7.5f));
			CHECK(centred.Glyphs[2].Min.x == doctest::Approx(7.5f));
		}

		TEST_CASE("TextMeasurement: a codepoint takes the first font of the chain with its glyph, and the rest are skipped and counted" * doctest::skip(true))
		{
			const FontData first = MakeFont(true, false);
			const FontData second = MakeFont(false, true);
			const std::array<const FontData*, 2> chain = { &first, &second };
			// 'A' from the first font, 'B' from the second without the pair's kerning (the fonts differ), 'é' and the invalid
			// byte skipped without advancing, 'B' again.
			const TextMeasurement measured = MeasureText(chain, std::string("AB\xC3\xA9") + "\xFF" + "B", { .Size = 10.0f });
			REQUIRE(measured.Glyphs.size() == 3);
			CHECK(measured.SkippedCodepoints == 2);
			CHECK(measured.Glyphs[0].Font == 0);
			CHECK(measured.Glyphs[1].Font == 1);
			CHECK(measured.Glyphs[1].Min.x == doctest::Approx(5.5f));
			CHECK(measured.Glyphs[2].Font == 1);
			CHECK(measured.Glyphs[2].ByteOffset == 5);
			CHECK(measured.Glyphs[2].Min.x == doctest::Approx(11.5f));
		}
	}

}
