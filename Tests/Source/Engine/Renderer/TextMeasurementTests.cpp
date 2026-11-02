#include "TestsPCH.h"

#include "Engine/Renderer/TextMeasurement.h"

#include "Engine/Asset/FontData.h"
#include "Engine/Core/Hash.h"

#include <glm/glm.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

// The text-measurement layer (M22, C22.1, R-UI-TEXT; Docs/Decisions/0033-m22-contract.md decision 6) on TextLayoutTests'
// synthetic font: positions, word wrapping, line feeds and spacing, alignment, the font chain and byte offsets, and the
// wrapping and byte-range rules of TextMeasurement.cpp's file comment.

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
		TEST_CASE("TextMeasurement: a line measures its glyph advances and kerning at the size")
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

		TEST_CASE("TextMeasurement: Word wraps at the last space that fits MaxWidth and breaks a longer word between glyphs")
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

		TEST_CASE("TextMeasurement: line feeds break lines, carriage returns are ignored, and LineSpacing scales the pitch")
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

		TEST_CASE("TextMeasurement: Alignment places each line within MaxWidth, or within the widest line")
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

		TEST_CASE("TextMeasurement: a codepoint takes the first font of the chain with its glyph, and the rest are skipped and counted")
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

		TEST_CASE("TextMeasurement: Word breaks at the last space whose line fits, else at the first, and never before a line's first other codepoint")
		{
			const FontData font = MakeFont();
			const std::array<const FontData*, 1> chain = { &font };
			const auto wrap = [&chain](std::string_view text, float maxWidth)
			{
				return MeasureText(chain, text, { .Size = 10.0f, .Wrap = TextWrap::Word, .MaxWidth = maxWidth });
			};

			// "AB  A": the spaces start at 10 and 12.5 and the last 'A' would end at 20. Within 16 both spaces fit, so the line
			// keeps the first and breaks at the second.
			const TextMeasurement wide = wrap("AB  A", 16.0f);
			REQUIRE(wide.Lines.size() == 2);
			CHECK(wide.Lines[0].ByteEnd == 3);
			CHECK(wide.Lines[0].Width == doctest::Approx(12.5f));
			CHECK(wide.Lines[1].ByteBegin == 4);
			CHECK(wide.Lines[1].Width == doctest::Approx(5.0f));
			// Within 11 only the first fits: the line breaks there, and the second space starts the next line.
			const TextMeasurement narrow = wrap("AB  A", 11.0f);
			REQUIRE(narrow.Lines.size() == 2);
			CHECK(narrow.Lines[0].ByteEnd == 2);
			CHECK(narrow.Lines[0].Width == doctest::Approx(10.0f));
			CHECK(narrow.Lines[1].ByteBegin == 3);
			CHECK(narrow.Lines[1].Width == doctest::Approx(7.5f));
			REQUIRE(narrow.Glyphs.size() == 3);
			CHECK(narrow.Glyphs[2].Min.x == doctest::Approx(2.5f));

			// A glyph wider than MaxWidth stays alone on its line; no space fits after it, so the line breaks at the first.
			const TextMeasurement lone = wrap("B A", 4.0f);
			REQUIRE(lone.Lines.size() == 2);
			CHECK(lone.Lines[0].ByteEnd == 1);
			CHECK(lone.Lines[0].Width == doctest::Approx(6.0f));
			CHECK(lone.Lines[1].ByteBegin == 2);

			// Leading spaces are no break: the first 'A' stays on the first line although it ends at 7.5, beyond 6, and the
			// word breaks before 'B'.
			const TextMeasurement leading = wrap(" AB", 6.0f);
			REQUIRE(leading.Lines.size() == 2);
			CHECK(leading.Lines[0].GlyphCount == 1);
			CHECK(leading.Lines[0].ByteEnd == 2);
			CHECK(leading.Lines[0].Width == doctest::Approx(7.5f));
			CHECK(leading.Lines[1].ByteBegin == 2);
			CHECK(leading.Glyphs[1].Min.x == doctest::Approx(0.5f));

			// A MaxWidth that is not positive and finite neither wraps nor sizes the block.
			for (const float maxWidth : { -5.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
			{
				CAPTURE(maxWidth);
				const TextMeasurement unbounded = wrap("AB A", maxWidth);
				CHECK(unbounded.Lines.size() == 1);
				CHECK(unbounded.Size.x == doctest::Approx(17.5f));
			}
		}

		TEST_CASE("TextMeasurement: the lines of a paragraph share its bytes, a trailing line feed starts an empty line, and every direction lays out left to right")
		{
			const FontData font = MakeFont();
			const std::array<const FontData*, 1> chain = { &font };
			// The carriage return and the invalid byte belong to the lines around them; only the line feed is no line's.
			const TextMeasurement measured = MeasureText(chain, std::string("A\r\nB\xFF") + "\n", { .Size = 10.0f });
			REQUIRE(measured.Lines.size() == 3);
			CHECK(measured.SkippedCodepoints == 1);
			CHECK(measured.Lines[0].ByteBegin == 0);
			CHECK(measured.Lines[0].ByteEnd == 2);
			CHECK(measured.Lines[1].ByteBegin == 3);
			CHECK(measured.Lines[1].ByteEnd == 5);
			CHECK(measured.Lines[2].ByteBegin == 6);
			CHECK(measured.Lines[2].ByteEnd == 6);
			CHECK(measured.Lines[2].GlyphCount == 0);
			CHECK(measured.Lines[2].Width == doctest::Approx(0.0f));
			// Three lines: two pitches of 11 and the last line's 10.
			CHECK(measured.Size.y == doctest::Approx(32.0f));
			CHECK(measured.Lines[2].Baseline == doctest::Approx(30.0f));

			// An empty text has no line and no height; a bounded block keeps its width.
			const TextMeasurement empty = MeasureText(chain, "", { .Size = 10.0f, .MaxWidth = 20.0f });
			CHECK(empty.Lines.empty());
			CHECK(empty.Size.x == doctest::Approx(20.0f));
			CHECK(empty.Size.y == doctest::Approx(0.0f));

			// Shaping and bidi are W22.3's: in W22.1 every TextDirection gives the left-to-right layout.
			const TextMeasurement leftToRight = MeasureText(chain, "AB A", { .Size = 10.0f, .Direction = TextDirection::LeftToRight });
			CHECK(MeasureText(chain, "AB A", { .Size = 10.0f, .Direction = TextDirection::RightToLeft }) == leftToRight);
			CHECK(MeasureText(chain, "AB A", { .Size = 10.0f, .Direction = TextDirection::Auto }) == leftToRight);
		}

		TEST_CASE("TextMeasurement: a wrapped and centred block measures to the same bytes in every configuration")
		{
			// The pinned digest of every float and integer of the measurement, at a size, line spacing and width that binary
			// floats do not hold exactly, so every product rounds: the Debug and Release unit runs (and the milestone gate's)
			// compare the layout byte for byte.
			constexpr uint64_t ExpectedDigest = 0xea4be6a75af8cf57ull;
			const FontData font = MakeFont();
			const std::array<const FontData*, 1> chain = { &font };
			// Kerned pairs, a run of spaces, a line feed after a carriage return, a leading space and an ill-formed byte (\377).
			constexpr std::string_view Text = "AB A BAB  ABBA\nBA\r\n A \377AAAA AB";
			const TextMeasurement measured = MeasureText(chain, Text,
				{ .Size = 13.7f, .LineSpacing = 1.3f, .Wrap = TextWrap::Word, .MaxWidth = 23.9f, .Alignment = RenderTextAlignment::Center });
			REQUIRE(measured.Lines.size() > 4);

			XXH64Hasher hasher;
			const auto updateFloat = [&hasher](float value)
			{
				hasher.UpdateU64(std::bit_cast<uint32_t>(value));
			};
			for (const TextMeasuredGlyph& glyph : measured.Glyphs)
			{
				for (const glm::vec2& corner : { glyph.Min, glyph.Max, glyph.AtlasMin, glyph.AtlasMax })
				{
					updateFloat(corner.x);
					updateFloat(corner.y);
				}
				hasher.UpdateU64(glyph.Font);
				hasher.UpdateU64(glyph.ByteOffset);
			}
			for (const TextMeasuredLine& line : measured.Lines)
			{
				for (const uint32_t value : { line.FirstGlyph, line.GlyphCount, line.ByteBegin, line.ByteEnd })
					hasher.UpdateU64(value);
				for (const float value : { line.Left, line.Width, line.Top, line.Baseline })
					updateFloat(value);
			}
			updateFloat(measured.Size.x);
			updateFloat(measured.Size.y);
			hasher.UpdateU64(measured.SkippedCodepoints);
			CHECK(hasher.Digest() == ExpectedDigest);
		}
	}

}
