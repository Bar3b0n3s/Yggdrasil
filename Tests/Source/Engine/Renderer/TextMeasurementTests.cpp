#include "TestsPCH.h"

#include "Engine/Renderer/TextMeasurement.h"

#include "Engine/Asset/FontData.h"
#include "Engine/AssetPipeline/Importers/FontImporter.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Support/TestData.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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

		// MakeFont with two Hebrew letters (Alef U+05D0 like A, Bet U+05D1 like B, kerned by -0.1 as A and B are), a
		// combining acute accent (U+0301, advance 0, a box above the letters) and a paragraph separator (U+2029, no outline):
		// right-to-left text without shaping data. Without the accent when asked.
		FontData MakeBidiFont(bool withAccent = true)
		{
			FontData font = MakeFont();
			if (withAccent)
				font.Glyphs.push_back(FontGlyph{ .Codepoint = 0x0301, .Advance = 0.0f, .PlaneMin = glm::vec2(-0.3f, 0.8f), .PlaneMax = glm::vec2(-0.1f, 0.9f), .AtlasMin = glm::vec2(0.0f), .AtlasMax = glm::vec2(0.25f) });
			font.Glyphs.push_back(FontGlyph{ .Codepoint = 0x05D0, .Advance = 0.5f, .PlaneMin = glm::vec2(0.0f), .PlaneMax = glm::vec2(0.5f, 0.7f), .AtlasMin = glm::vec2(0.0f), .AtlasMax = glm::vec2(0.5f) });
			font.Glyphs.push_back(FontGlyph{ .Codepoint = 0x05D1, .Advance = 0.6f, .PlaneMin = glm::vec2(0.05f, 0.0f), .PlaneMax = glm::vec2(0.55f, 0.7f), .AtlasMin = glm::vec2(0.5f), .AtlasMax = glm::vec2(1.0f) });
			font.Glyphs.push_back(FontGlyph{ .Codepoint = 0x2029, .Advance = 0.25f });
			font.Kerning.push_back(FontKerningPair{ .First = 0x05D0, .Second = 0x05D1, .Advance = -0.1f });
			REQUIRE(ValidateFontData(font).has_value());
			return font;
		}

		// The first byte of each cluster, in the measurement's order (visual order, line by line).
		std::vector<uint32_t> GetVisualOrder(const TextMeasurement& measured)
		{
			std::vector<uint32_t> order;
			for (const TextMeasuredCluster& cluster : measured.Clusters)
				order.push_back(cluster.ByteBegin);
			return order;
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

			// Left-to-right text lays out left to right in every TextDirection: the bidirectional algorithm (C22.3) puts its
			// runs at an even level.
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

		// C22.3 (Docs/Decisions/0033-m22-contract.md decisions 43 and 48; stream S): clusters and carets, shaping and bidi.
		TEST_CASE("TextMeasurement: the caret of a byte offset is its cluster's leading edge in visual order")
		{
			const FontData font = MakeFont();
			const std::array<const FontData*, 1> chain = { &font };
			// "AB A" at 10 pixels per em: A 0 to 4 (5 less the AB kerning's 1), B 4 to 10, the space 10 to 12.5, A 12.5 to 17.5.
			const TextMeasurement measured = MeasureText(chain, "AB A", { .Size = 10.0f });
			REQUIRE(measured.Clusters.size() == 4);
			CHECK(measured.Clusters[1] == TextMeasuredCluster{ .ByteBegin = 1, .ByteEnd = 2, .Line = 0, .Left = 4.0f, .Right = 10.0f });
			CHECK(measured.Clusters[2] == TextMeasuredCluster{ .ByteBegin = 2, .ByteEnd = 3, .Line = 0, .Left = 10.0f, .Right = 12.5f });
			CHECK(measured.LineHeight == doctest::Approx(10.0f));
			CHECK(FindTextCaret(measured, 0) == TextCaret{ .Top = glm::vec2(0.0f), .Line = 0 });
			CHECK(FindTextCaret(measured, 1).Top.x == 4.0f);
			CHECK(FindTextCaret(measured, 4).Top.x == 17.5f);
			CHECK(FindTextCaret(measured, 99).Top.x == 17.5f);

			// The second line of "A\nB" starts at its own top; an empty text's caret sits at its alignment's start.
			const TextMeasurement lines = MeasureText(chain, "A\nB", { .Size = 10.0f });
			CHECK(FindTextCaret(lines, 2) == TextCaret{ .Top = glm::vec2(0.0f, 11.0f), .Line = 1 });
			const TextMeasurement empty = MeasureText(chain, "", { .Size = 10.0f, .MaxWidth = 40.0f, .Alignment = RenderTextAlignment::Center });
			CHECK(FindTextCaret(empty, 0) == TextCaret{ .Top = glm::vec2(20.0f, 0.0f), .Line = 0 });

			// A byte no cluster begins at, ends at or holds (the second of two skipped ill-formed bytes) takes the trailing edge
			// of the cluster that ends last before it on its line, or its line's start when none does (the review of C22.3).
			const TextMeasurement skipped = MeasureText(chain, "A\377\377", { .Size = 10.0f });
			CHECK(FindTextCaret(skipped, 2) == TextCaret{ .Top = glm::vec2(5.0f, 0.0f), .Line = 0 });
			const TextMeasurement leading = MeasureText(chain, "\377\377A", { .Size = 10.0f });
			CHECK(FindTextCaret(leading, 1) == TextCaret{ .Top = glm::vec2(0.0f), .Line = 0 });
		}

		// Stream S's rules (TextMeasurement.h, TextMeasurement.cpp's file comment) on fonts without shaping data: the
		// bidirectional algorithm reorders runs codepoint by codepoint, kerning stays between its pair in a right-to-left
		// run, a mark takes the font of the codepoint before it, and a paragraph separator other than the line feed ends
		// no line.
		TEST_CASE("TextMeasurement: right-to-left text without shaping data is reordered codepoint by codepoint, with its kerning between its pair")
		{
			const FontData font = MakeBidiFont();
			const std::array<const FontData*, 1> chain = { &font };
			// "\u05D0\u05D1" (Alef, Bet: 4 bytes) at 10 pixels per em, a right-to-left paragraph (Auto): Bet at the left, then the
			// pair's kerning (-1), then Alef: Bet 0 to 5, Alef 5 to 10.
			const TextMeasurement measured = MeasureText(chain, "\xD7\x90\xD7\x91", { .Size = 10.0f });
			REQUIRE(measured.Lines.size() == 1);
			CHECK(measured.Lines[0].Width == 10.0f);
			REQUIRE(measured.Clusters.size() == 2);
			CHECK(measured.Clusters[0] == TextMeasuredCluster{ .ByteBegin = 2, .ByteEnd = 4, .Line = 0, .Left = 0.0f, .Right = 5.0f, .RightToLeft = true });
			CHECK(measured.Clusters[1] == TextMeasuredCluster{ .ByteBegin = 0, .ByteEnd = 2, .Line = 0, .Left = 5.0f, .Right = 10.0f, .RightToLeft = true });
			REQUIRE(measured.Glyphs.size() == 2);
			CHECK(measured.Glyphs[0].ByteOffset == 2u); // glyphs in visual order
			CHECK(measured.Glyphs[0].Min.x == doctest::Approx(0.5f));
			CHECK(measured.Glyphs[1].Min.x == doctest::Approx(5.0f));
			// The caret before Alef is its right edge, before Bet Bet's right edge, and after the text the left edge.
			CHECK(FindTextCaret(measured, 0).Top.x == 10.0f);
			CHECK(FindTextCaret(measured, 2).Top.x == 5.0f);
			CHECK(FindTextCaret(measured, 4).Top.x == 0.0f);
			// A left-to-right paragraph holds the one right-to-left run the same way.
			CHECK(MeasureText(chain, "\xD7\x90\xD7\x91", { .Size = 10.0f, .Direction = TextDirection::LeftToRight }) == measured);

			// "A \u05D0\u05D1 B": left to right, the Hebrew run reversed in place; right to left, the whole line reversed but
			// each Latin letter (a run of its own) read left to right.
			const std::string mixed = "A \xD7\x90\xD7\x91 B";
			CHECK(GetVisualOrder(MeasureText(chain, mixed, { .Size = 10.0f })) == std::vector<uint32_t>{ 0, 1, 4, 2, 6, 7 });
			const TextMeasurement rightToLeft = MeasureText(chain, mixed, { .Size = 10.0f, .Direction = TextDirection::RightToLeft });
			CHECK(GetVisualOrder(rightToLeft) == std::vector<uint32_t>{ 7, 6, 4, 2, 1, 0 });
			CHECK_FALSE(rightToLeft.Clusters.front().RightToLeft);
			CHECK(rightToLeft.Clusters[1].RightToLeft);

			// A paragraph separator other than the line feed is drawn and ends no line.
			const TextMeasurement separated = MeasureText(chain, std::string("A\xE2\x80\xA9") + "B", { .Size = 10.0f });
			CHECK(separated.Lines.size() == 1);
			CHECK(separated.Clusters.size() == 3);
		}

		TEST_CASE("TextMeasurement: a combining mark takes the font of the codepoint before it when that font has it")
		{
			// The first font has A and the accent, the second B and the accent: the accent after B is the second font's, the
			// accent after A the first's; without the accent in the second font, the accent after B is the first font's.
			const FontData first = MakeBidiFont();
			FontData second = MakeBidiFont();
			std::erase_if(second.Glyphs, [](const FontGlyph& glyph)
			{
				return glyph.Codepoint == 'A';
			});
			second.Kerning.clear();
			FontData firstWithoutB = first;
			std::erase_if(firstWithoutB.Glyphs, [](const FontGlyph& glyph)
			{
				return glyph.Codepoint == 'B';
			});
			firstWithoutB.Kerning.clear();
			REQUIRE(ValidateFontData(second).has_value());
			REQUIRE(ValidateFontData(firstWithoutB).has_value());
			const std::array<const FontData*, 2> chain = { &firstWithoutB, &second };
			const TextMeasurement measured = MeasureText(chain, std::string("B\xCC\x81") + "A\xCC\x81", { .Size = 10.0f });
			REQUIRE(measured.Glyphs.size() == 4);
			CHECK(measured.Glyphs[0].Font == 1u);
			CHECK(measured.Glyphs[1].Font == 1u);
			CHECK(measured.Glyphs[1].ByteOffset == 1u);
			CHECK(measured.Glyphs[2].Font == 0u);
			CHECK(measured.Glyphs[3].Font == 0u);
			const FontData secondWithoutAccent = MakeBidiFont(false);
			const std::array<const FontData*, 2> fallback = { &firstWithoutB, &secondWithoutAccent };
			const TextMeasurement fallen = MeasureText(fallback, "B\xCC\x81", { .Size = 10.0f });
			REQUIRE(fallen.Glyphs.size() == 2);
			CHECK(fallen.Glyphs[0].Font == 1u);
			CHECK(fallen.Glyphs[1].Font == 0u);
		}

		// The generated Glyphs.ttf (MakeFormatFixtures.py: 1000 units per em, A and V advancing 600 units, its legacy kern table
		// kerning A-V by -80 and V-A by -60) baked by FontImporter version 5 carries shaping data: HarfBuzz shapes it with the
		// kern table its subset keeps, and positions are font units converted once to pixels.
		TEST_CASE("TextMeasurement: a font with shaping data is shaped in font units, with the kerning its subset keeps")
		{
			Result<Buffer> source = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Fonts") / "Glyphs.ttf");
			REQUIRE_MESSAGE(source.has_value(), source.error().ToString());
			FontBakeReport report;
			const Result<FontData> font = FontImporter::Bake(*source, {}, "Glyphs.ttf", report);
			REQUIRE_MESSAGE(font.has_value(), font.error().ToString());
			REQUIRE_FALSE(font->ShapingSource.empty());
			REQUIRE(font->UnitsPerEm == 1000u);
			const std::array<const FontData*, 1> chain = { &*font };

			// "AVA": HarfBuzz splits a kern table's adjustment between its pair, half to the first glyph's advance and the rest
			// to the second's advance and offset, so A advances 600 - 40, V 600 - 40 - 30 and the last A 600 - 30 units, V
			// drawn 40 units left of its cluster's start; every position is units / 1000 * 10, converted once.
			const TextMeasurement measured = MeasureText(chain, "AVA", { .Size = 10.0f });
			REQUIRE(measured.Clusters.size() == 3);
			const auto pixels = [](float units)
			{
				return units / 1000.0f * 10.0f;
			};
			CHECK(measured.Clusters[1].Left == pixels(560.0f));
			CHECK(measured.Clusters[2].Left == pixels(1090.0f));
			CHECK(measured.Lines[0].Width == pixels(1660.0f));
			REQUIRE(measured.Glyphs.size() == 3);
			const FontGlyph* letter = FindGlyph(*font, 'V');
			REQUIRE(letter != nullptr);
			CHECK(measured.Glyphs[1].Min.x == pixels(520.0f) + letter->PlaneMin.x * 10.0f);
			CHECK(measured.Glyphs[1].AtlasMin == FindGlyphByIndex(*font, letter->GlyphIndex)->AtlasMin);
			CHECK(measured.SkippedCodepoints == 0u);

			// Without shaping data the same font lays out codepoint by codepoint with its kerning pairs, to the same width.
			FontData unshaped = *font;
			unshaped.ShapingSource.clear();
			unshaped.UnitsPerEm = 0;
			unshaped.IndexedGlyphs.clear();
			REQUIRE(ValidateFontData(unshaped).has_value());
			const std::array<const FontData*, 1> unshapedChain = { &unshaped };
			CHECK(MeasureText(unshapedChain, "AVA", { .Size = 10.0f }).Lines[0].Width == doctest::Approx(16.6f));
		}

		// The Noto fixtures are stream F's (pinned in decision 42), so these cases run once streams S and F have both landed.
		TEST_CASE("TextMeasurement: Arabic is shaped into its joined forms and laid out right to left")
		{
			Result<Buffer> source = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Fonts") / "NotoSansArabic-Regular.ttf");
			REQUIRE_MESSAGE(source.has_value(), source.error().ToString());
			FontImportSettings settings;
			settings.Ranges = { FontCharacterRange{ .First = 0x0627, .Last = 0x064A } };
			FontBakeReport report;
			const Result<FontData> font = FontImporter::Bake(*source, settings, "NotoSansArabic-Regular.ttf", report);
			REQUIRE_MESSAGE(font.has_value(), font.error().ToString());
			const std::array<const FontData*, 1> chain = { &*font };

			// "بيت" (Beh, Yeh, Teh: 6 bytes): three joined letters, the first letter's initial form at the right.
			const TextMeasurement measured = MeasureText(chain, "\xD8\xA8\xD9\x8A\xD8\xAA", { .Size = 48.0f });
			// Noto Sans Arabic draws a dotted letter as its dotless joined form and its dots, so a letter's cluster holds several
			// glyphs (stream S of W22.3 amended the contract's count of one glyph a letter, which this font does not give).
			REQUIRE(measured.Clusters.size() == 3);
			REQUIRE(measured.Glyphs.size() >= 3);
			const auto rightmost = std::ranges::max_element(measured.Glyphs, {}, [](const TextMeasuredGlyph& glyph)
			{
				return glyph.Max.x;
			});
			CHECK(rightmost->ByteOffset == 0u);
			// Every cluster is right to left, the first in visual order is the last letter and the last the first, and no
			// glyph of the first letter is the isolated form of its codepoint (each is a joined form only shaping reaches).
			CHECK(measured.Clusters[0].ByteBegin == 4u);
			CHECK(measured.Clusters[0].RightToLeft);
			CHECK(measured.Clusters[2].ByteBegin == 0u);
			CHECK(measured.SkippedCodepoints == 0u);
			for (const TextMeasuredGlyph& glyph : measured.Glyphs)
			{
				if (glyph.ByteOffset == 0)
					CHECK(glyph.AtlasMin != FindGlyph(*font, 0x0628)->AtlasMin);
			}
			// The caret before the text is its right edge; after it, its left edge.
			CHECK(FindTextCaret(measured, 0).Top.x == measured.Lines[0].Left + measured.Lines[0].Width);
			CHECK(FindTextCaret(measured, 6).Top.x == measured.Lines[0].Left);
		}

		// M38 (Docs/Decisions/0055-m38-runtime-locale.md decision 11; Roadmap M38's acceptance): Start and End follow each line's
		// own paragraph's resolved base direction (Auto: its first strong character's), so an Arabic paragraph and a Latin one
		// of one block align to opposite edges; a paragraph without a strong character reads left to right; Left and Right are
		// unchanged; an empty text's caret takes Start as Left and End as Right.
		TEST_CASE("TextMeasurement: Start and End align each paragraph by its own direction")
		{
			Result<Buffer> arabicSource = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Fonts") / "NotoSansArabic-Regular.ttf");
			REQUIRE_MESSAGE(arabicSource.has_value(), arabicSource.error().ToString());
			Result<Buffer> latinSource = FileSystem::ReadFile(Test::GetRepositoryRoot() / "Resources/Fonts/Inter-Regular.ttf");
			REQUIRE_MESSAGE(latinSource.has_value(), latinSource.error().ToString());
			FontImportSettings arabicSettings;
			arabicSettings.Ranges = { FontCharacterRange{ .First = 0x0627, .Last = 0x064A } };
			FontBakeReport report;
			const Result<FontData> arabic = FontImporter::Bake(*arabicSource, arabicSettings, "NotoSansArabic-Regular.ttf", report);
			const Result<FontData> latin = FontImporter::Bake(*latinSource, {}, "Inter-Regular.ttf", report);
			REQUIRE(arabic.has_value());
			REQUIRE(latin.has_value());
			const std::array<const FontData*, 2> chain = { &*latin, &*arabic };

			// Line 0: Beh, Yeh, Teh (right to left); line 1: "home" (left to right); line 2: "123" (no strong character).
			const std::string text = "\xD8\xA8\xD9\x8A\xD8\xAA\nhome\n123";
			const auto measure = [&](RenderTextAlignment alignment)
			{
				const TextMeasurement measured = MeasureText(chain, text, { .Size = 24.0f, .MaxWidth = 400.0f, .Alignment = alignment });
				REQUIRE(measured.Lines.size() == 3);
				return measured;
			};
			const TextMeasurement start = measure(RenderTextAlignment::Start);
			CHECK(start.Lines[0].Left + start.Lines[0].Width == doctest::Approx(400.0f));
			CHECK(start.Lines[1].Left == 0.0f);
			CHECK(start.Lines[2].Left == 0.0f);
			const TextMeasurement end = measure(RenderTextAlignment::End);
			CHECK(end.Lines[0].Left == 0.0f);
			CHECK(end.Lines[1].Left + end.Lines[1].Width == doctest::Approx(400.0f));
			CHECK(end.Lines[2].Left + end.Lines[2].Width == doctest::Approx(400.0f));
			// Start is Left for a left-to-right line and Right for a right-to-left one, exactly.
			const TextMeasurement left = measure(RenderTextAlignment::Left);
			const TextMeasurement right = measure(RenderTextAlignment::Right);
			CHECK(start.Lines[0].Left == right.Lines[0].Left);
			CHECK(start.Lines[1].Left == left.Lines[1].Left);
			CHECK(end.Lines[0].Left == left.Lines[0].Left);
			CHECK(end.Lines[1].Left == right.Lines[1].Left);
			// Only the lines' placement differs: the glyphs move with their line.
			REQUIRE(start.Glyphs.size() == left.Glyphs.size());
			for (size_t line = 0; line < start.Lines.size(); ++line)
			{
				const float shift = start.Lines[line].Left - left.Lines[line].Left;
				for (uint32_t index = start.Lines[line].FirstGlyph; index < start.Lines[line].FirstGlyph + start.Lines[line].GlyphCount; ++index)
				{
					CAPTURE(index);
					CHECK(start.Glyphs[index].Min.x == doctest::Approx(left.Glyphs[index].Min.x + shift));
				}
			}
			// An empty text's caret: Start at the left edge, End at the right.
			const TextMeasurement empty = MeasureText(chain, "", { .Size = 24.0f, .MaxWidth = 400.0f, .Alignment = RenderTextAlignment::Start });
			CHECK(FindTextCaret(empty, 0).Top.x == 0.0f);
			const TextMeasurement emptyEnd = MeasureText(chain, "", { .Size = 24.0f, .MaxWidth = 400.0f, .Alignment = RenderTextAlignment::End });
			CHECK(FindTextCaret(emptyEnd, 0).Top.x == 400.0f);

			// The W38.2 review's finding 2: every line a paragraph wraps into takes the paragraph's direction (each line its own
			// flag), so a wrapped Arabic paragraph's lines all end at the right edge at Start, and the Latin paragraph after it
			// starts at the left.
			const std::string word = "\xD8\xA8\xD9\x8A\xD8\xAA";
			const std::string wrapped = word + " " + word + " " + word + " " + word + "\nhome";
			const TextMeasurement wrappedStart =
				MeasureText(chain, wrapped, { .Size = 24.0f, .Wrap = TextWrap::Word, .MaxWidth = 80.0f, .Alignment = RenderTextAlignment::Start });
			REQUIRE(wrappedStart.Lines.size() >= 3);
			for (size_t line = 0; line + 1 < wrappedStart.Lines.size(); ++line)
			{
				CAPTURE(line);
				CHECK(wrappedStart.Lines[line].Left + wrappedStart.Lines[line].Width == doctest::Approx(80.0f));
			}
			CHECK(wrappedStart.Lines.back().Left == 0.0f);
			// A paragraph's direction is its first strong character's, whatever runs its line holds: an Arabic word then a Latin
			// one starts at the right edge, a Latin word then an Arabic one at the left; an empty paragraph, without a strong
			// character, reads left to right.
			const std::string mixed = word + " home\nhome " + word + "\n\n" + word;
			const TextMeasurement mixedStart = MeasureText(chain, mixed, { .Size = 24.0f, .MaxWidth = 400.0f, .Alignment = RenderTextAlignment::Start });
			REQUIRE(mixedStart.Lines.size() == 4);
			CHECK(mixedStart.Lines[0].Left + mixedStart.Lines[0].Width == doctest::Approx(400.0f));
			CHECK(mixedStart.Lines[1].Left == 0.0f);
			CHECK(mixedStart.Lines[2].Left == 0.0f);
			CHECK(mixedStart.Lines[3].Left + mixedStart.Lines[3].Width == doctest::Approx(400.0f));
			const TextMeasurement mixedEnd = MeasureText(chain, mixed, { .Size = 24.0f, .MaxWidth = 400.0f, .Alignment = RenderTextAlignment::End });
			REQUIRE(mixedEnd.Lines.size() == 4);
			CHECK(mixedEnd.Lines[0].Left == 0.0f);
			CHECK(mixedEnd.Lines[1].Left + mixedEnd.Lines[1].Width == doctest::Approx(400.0f));
			CHECK(mixedEnd.Lines[2].Left == 400.0f);
			CHECK(mixedEnd.Lines[3].Left == 0.0f);
		}

		TEST_CASE("TextMeasurement: a mixed-direction line is reordered by the bidirectional algorithm in either base direction, and Alignment places it")
		{
			Result<Buffer> hebrewSource = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Fonts") / "NotoSansHebrew-Regular.ttf");
			REQUIRE_MESSAGE(hebrewSource.has_value(), hebrewSource.error().ToString());
			Result<Buffer> latinSource = FileSystem::ReadFile(Test::GetRepositoryRoot() / "Resources/Fonts/Inter-Regular.ttf");
			REQUIRE_MESSAGE(latinSource.has_value(), latinSource.error().ToString());
			FontImportSettings hebrewSettings;
			hebrewSettings.Ranges = { FontCharacterRange{ .First = 0x05D0, .Last = 0x05EA } };
			FontBakeReport report;
			const Result<FontData> hebrew = FontImporter::Bake(*hebrewSource, hebrewSettings, "NotoSansHebrew-Regular.ttf", report);
			const Result<FontData> latin = FontImporter::Bake(*latinSource, {}, "Inter-Regular.ttf", report);
			REQUIRE(hebrew.has_value());
			REQUIRE(latin.has_value());
			// Inter first: the Hebrew letters fall back to Noto Sans Hebrew.
			const std::array<const FontData*, 2> chain = { &*latin, &*hebrew };

			// "ab אבג cd" in a left-to-right paragraph: visual order a, b, space, Gimel, Bet, Alef, space, c, d.
			const std::string text = "ab \xD7\x90\xD7\x91\xD7\x92 cd";
			const TextMeasurement measured = MeasureText(chain, text, { .Size = 20.0f });
			std::vector<uint32_t> visual;
			for (const TextMeasuredCluster& cluster : measured.Clusters)
				visual.push_back(cluster.ByteBegin);
			CHECK(visual == std::vector<uint32_t>{ 0, 1, 2, 7, 5, 3, 9, 10, 11 });
			const auto alef = std::ranges::find_if(measured.Glyphs, [](const TextMeasuredGlyph& glyph)
			{
				return glyph.ByteOffset == 3;
			});
			REQUIRE(alef != measured.Glyphs.end());
			CHECK(alef->Font == 1u);

			// The same text in a right-to-left paragraph: the Latin runs keep their order inside, the runs reverse.
			const TextMeasurement rightToLeft = MeasureText(chain, text, { .Size = 20.0f, .Direction = TextDirection::RightToLeft });
			visual.clear();
			for (const TextMeasuredCluster& cluster : rightToLeft.Clusters)
				visual.push_back(cluster.ByteBegin);
			CHECK(visual == std::vector<uint32_t>{ 10, 11, 9, 7, 5, 3, 2, 0, 1 });
			// Auto takes the first strong character's direction: Latin here.
			CHECK(MeasureText(chain, text, { .Size = 20.0f, .Direction = TextDirection::Auto }) == measured);

			// Alignment places a right-to-left line as it places any other (it does not follow the direction): Right ends it at
			// the block's right edge, Left starts it at 0, and both keep the visual order.
			const TextMeasurement right = MeasureText(chain, text,
				{ .Size = 20.0f, .MaxWidth = 300.0f, .Alignment = RenderTextAlignment::Right, .Direction = TextDirection::RightToLeft });
			REQUIRE(right.Lines.size() == 1);
			REQUIRE_FALSE(right.Clusters.empty());
			CHECK(right.Lines[0].Left + right.Lines[0].Width == doctest::Approx(300.0f));
			CHECK(right.Clusters.back().Right == doctest::Approx(300.0f));
			CHECK(right.Clusters.front().ByteBegin == 10u);
			const TextMeasurement left = MeasureText(chain, text,
				{ .Size = 20.0f, .MaxWidth = 300.0f, .Alignment = RenderTextAlignment::Left, .Direction = TextDirection::RightToLeft });
			REQUIRE(left.Lines.size() == 1);
			REQUIRE_FALSE(left.Clusters.empty());
			CHECK(left.Lines[0].Left == 0.0f);
			CHECK(left.Clusters.front().Left == 0.0f);
			CHECK(left.Clusters.front().ByteBegin == 10u);
		}

		// C22.3 (the review of C22.3, its major on Roadmap M22's acceptance "shaped text is byte-identical across
		// configurations"; streams S and F, decision 43): every float and integer of the shaped measurements of a fixed set
		// of texts, with each glyph's id in its font, pinned in one digest that the Debug and Release unit runs (and the
		// milestone gate's) compare byte for byte: Latin through Inter's default features (its kerning, contextual
		// alternates and ligatures), Arabic, Hebrew, a mixed-direction line and Japanese through the fallback chain, each in
		// both base directions, wrapped and centred at a size, line spacing and width that binary floats do not hold
		// exactly, so every conversion rounds.
		TEST_CASE("TextMeasurement: shaped Latin, Arabic, Hebrew, mixed-direction and fallback CJK text measures to the same bytes in every configuration")
		{
			// Stream S recorded the digest its Debug run measured; its Release run measured the same.
			constexpr uint64_t ExpectedDigest = 0x813084c4db179ee9ull;
			const auto bake = [](const std::filesystem::path& path, std::vector<FontCharacterRange> ranges)
			{
				Result<Buffer> source = FileSystem::ReadFile(path);
				REQUIRE_MESSAGE(source.has_value(), source.error().ToString());
				FontImportSettings settings;
				settings.Ranges = std::move(ranges);
				FontBakeReport report;
				Result<FontData> font = FontImporter::Bake(*source, settings, path.filename().string(), report);
				REQUIRE_MESSAGE(font.has_value(), font.error().ToString());
				return std::move(*font);
			};
			const std::filesystem::path fonts = Test::GetTestDataPath("Assets/Fonts");
			const FontData latin = bake(Test::GetRepositoryRoot() / "Resources/Fonts/Inter-Regular.ttf", {});
			const FontData arabic = bake(fonts / "NotoSansArabic-Regular.ttf", { FontCharacterRange{ .First = 0x0621, .Last = 0x064A } });
			const FontData hebrew = bake(fonts / "NotoSansHebrew-Regular.ttf", { FontCharacterRange{ .First = 0x05D0, .Last = 0x05EA } });
			// The Japanese text's kana and ideographs only.
			const FontData japanese = bake(fonts / "NotoSansJP-Regular.otf",
				{ FontCharacterRange{ .First = 0x306E, .Last = 0x306E },
					FontCharacterRange{ .First = 0x30AD, .Last = 0x30AD },
					FontCharacterRange{ .First = 0x30B9, .Last = 0x30B9 },
					FontCharacterRange{ .First = 0x30C6, .Last = 0x30C8 },
					FontCharacterRange{ .First = 0x65E5, .Last = 0x65E5 },
					FontCharacterRange{ .First = 0x672C, .Last = 0x672C },
					FontCharacterRange{ .First = 0x8A9E, .Last = 0x8A9E } });
			// Inter first: the other scripts fall back along the chain.
			const std::array<const FontData*, 4> chain = { &latin, &arabic, &hebrew, &japanese };
			// "office AVATAR Wave -> fi"; "مرحبا بالعالم"; "שלום עולם"; "Hello שלום 123 world"; "日本語のテキスト".
			constexpr std::array<std::string_view, 5> Texts = {
				"office AVATAR Wave -> fi",
				"\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85",
				"\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D",
				"Hello \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D 123 world",
				"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\x86\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x88",
			};

			XXH64Hasher hasher;
			const auto updateFloat = [&hasher](float value)
			{
				hasher.UpdateU64(std::bit_cast<uint32_t>(value));
			};
			for (const std::string_view text : Texts)
			{
				for (const TextDirection direction : { TextDirection::LeftToRight, TextDirection::RightToLeft })
				{
					const TextMeasurement measured = MeasureText(chain, text,
						{ .Size = 23.7f,
							.LineSpacing = 1.15f,
							.Wrap = TextWrap::Word,
							.MaxWidth = 211.3f,
							.Alignment = RenderTextAlignment::Center,
							.Direction = direction });
					REQUIRE_FALSE(measured.Glyphs.empty());
					CHECK(measured.SkippedCodepoints == 0u);
					for (const TextMeasuredGlyph& glyph : measured.Glyphs)
					{
						// The glyph's id: the indexed glyph of its font with its atlas rectangle.
						REQUIRE(glyph.Font < chain.size());
						const std::vector<FontGlyph>& indexed = chain[glyph.Font]->IndexedGlyphs;
						const auto id = std::ranges::find_if(indexed, [&glyph](const FontGlyph& candidate)
						{
							return candidate.AtlasMin == glyph.AtlasMin && candidate.AtlasMax == glyph.AtlasMax;
						});
						REQUIRE(id != indexed.end());
						hasher.UpdateU64(id->GlyphIndex);
						for (const glm::vec2& corner : { glyph.Min, glyph.Max, glyph.AtlasMin, glyph.AtlasMax })
						{
							updateFloat(corner.x);
							updateFloat(corner.y);
						}
						hasher.UpdateU64(glyph.Font);
						hasher.UpdateU64(glyph.ByteOffset);
					}
					for (const TextMeasuredCluster& cluster : measured.Clusters)
					{
						for (const uint32_t value : { cluster.ByteBegin, cluster.ByteEnd, cluster.Line })
							hasher.UpdateU64(value);
						updateFloat(cluster.Left);
						updateFloat(cluster.Right);
						hasher.UpdateU64(cluster.RightToLeft ? 1u : 0u);
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
					updateFloat(measured.LineHeight);
				}
			}
			CHECK(hasher.Digest() == ExpectedDigest);
		}
	}

}
