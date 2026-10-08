#include "TestsPCH.h"

#include "Engine/Renderer/TextLayout.h"

#include "Engine/Asset/FontData.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>

// Text layout (Architecture §8.10; Docs/Decisions/0013-m8-decisions.md decision 11) on a synthetic font with exact metrics.

namespace Engine {

	namespace {

		// Ascent 0.8, descent -0.2, line gap 0.1 (a line is 1.1 em); 'A' advances 0.5, 'B' 0.6, ' ' 0.25 (no outline); the
		// pair "AB" kerns by -0.1. Plane boxes: 'A' (0, 0)-(0.5, 0.7), 'B' (0.05, 0)-(0.55, 0.7).
		FontData MakeFont()
		{
			FontData font;
			font.Ascent = 0.8f;
			font.Descent = -0.2f;
			font.LineGap = 0.1f;
			font.AtlasWidth = 4;
			font.AtlasHeight = 4;
			font.AtlasPixels.assign(16, std::byte{ 0 });
			font.Glyphs = {
				FontGlyph{ .Codepoint = ' ', .Advance = 0.25f },
				FontGlyph{ .Codepoint = 'A', .Advance = 0.5f, .PlaneMin = glm::vec2(0.0f, 0.0f), .PlaneMax = glm::vec2(0.5f, 0.7f), .AtlasMin = glm::vec2(0.0f), .AtlasMax = glm::vec2(0.5f) },
				FontGlyph{ .Codepoint = 'B', .Advance = 0.6f, .PlaneMin = glm::vec2(0.05f, 0.0f), .PlaneMax = glm::vec2(0.55f, 0.7f), .AtlasMin = glm::vec2(0.5f), .AtlasMax = glm::vec2(1.0f) },
			};
			font.Kerning = { FontKerningPair{ .First = 'A', .Second = 'B', .Advance = -0.1f } };
			REQUIRE(ValidateFontData(font).has_value());
			return font;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("TextLayout: glyphs advance along the baseline with kerning")
		{
			const FontData font = MakeFont();
			const TextLayoutResult layout = LayoutText(font, "AB A", RenderTextAlignment::Left);
			REQUIRE(layout.Quads.size() == 3); // the space has no quad
			CHECK(layout.LineCount == 1);
			CHECK(layout.SkippedCodepoints == 0);
			// 'A' at pen 0; 'B' at 0.5 - 0.1 = 0.4; the second 'A' at 0.4 + 0.6 + 0.25 = 1.25. y is down from the block's top:
			// the baseline at Ascent 0.8, so a box from plane y 0 to 0.7 spans 0.1 to 0.8.
			CHECK(layout.Quads[0].PositionMin.x == doctest::Approx(0.0f));
			CHECK(layout.Quads[0].PositionMin.y == doctest::Approx(0.1f));
			CHECK(layout.Quads[0].PositionMax.y == doctest::Approx(0.8f));
			CHECK(layout.Quads[1].PositionMin.x == doctest::Approx(0.45f));
			CHECK(layout.Quads[2].PositionMin.x == doctest::Approx(1.25f));
			CHECK(layout.Quads[1].AtlasMin == glm::vec2(0.5f));
			CHECK(layout.Size.x == doctest::Approx(1.75f));
			CHECK(layout.Size.y == doctest::Approx(1.0f)); // one line: Ascent - Descent
		}

		TEST_CASE("TextLayout: lines stack by the line height and align within the block")
		{
			const FontData font = MakeFont();
			const TextLayoutResult right = LayoutText(font, "AB\nA", RenderTextAlignment::Right);
			CHECK(right.LineCount == 2);
			CHECK(right.Size.x == doctest::Approx(1.0f)); // "AB" is 0.5 - 0.1 + 0.6 wide
			CHECK(right.Size.y == doctest::Approx(2.1f)); // 2 * 1.1 - the last line's gap
			REQUIRE(right.Quads.size() == 3);
			CHECK(right.Quads[2].PositionMin.x == doctest::Approx(0.5f)); // the 0.5-wide second line ends at the block's right
			CHECK(right.Quads[2].PositionMin.y == doctest::Approx(1.2f)); // its baseline at 0.8 + 1.1, box top 0.7 above it
			const TextLayoutResult centred = LayoutText(font, "AB\nA", RenderTextAlignment::Center);
			CHECK(centred.Quads[2].PositionMin.x == doctest::Approx(0.25f));
		}

		TEST_CASE("TextLayout: unknown codepoints and invalid UTF-8 are skipped and counted")
		{
			const FontData font = MakeFont();
			const TextLayoutResult layout = LayoutText(font, std::string("A\xC3\xA9") + "\xFF" + "B", RenderTextAlignment::Left);
			CHECK(layout.SkippedCodepoints == 2);
			REQUIRE(layout.Quads.size() == 2);
			CHECK(layout.Quads[1].PositionMin.x == doctest::Approx(0.45f)); // skipped codepoints do not advance; kerning still applies
			CHECK(LayoutText(font, "", RenderTextAlignment::Left).LineCount == 0);
			CHECK(LayoutText(font, "A\r\nB", RenderTextAlignment::Left).LineCount == 2);
		}

		TEST_CASE("TextLayout: each maximal subpart of ill-formed UTF-8 counts as one skipped codepoint")
		{
			const FontData font = MakeFont();
			struct Case
			{
				std::string Text;
				uint32_t Skipped = 0;
			};
			const std::array<Case, 7> cases = { {
				// Adjacent literals end each hex escape before the 'B'.
				{ .Text = "A\xE2\x82"
						  "B",
					.Skipped = 1 }, // a truncated three-byte sequence
				{ .Text = "A\xF0\x9F\x98"
						  "B",
					.Skipped = 1 }, // a truncated four-byte sequence
				{ .Text = "A\xC0\x80"
						  "B",
					.Skipped = 2 }, // C0 never starts a sequence; 80 is a stray continuation
				{ .Text = "A\xE0\x80\x80"
						  "B",
					.Skipped = 3 }, // an overlong form: E0 needs A0 to BF next
				{ .Text = "A\xED\xA0\x80"
						  "B",
					.Skipped = 3 }, // a surrogate: ED needs 80 to 9F next
				{ .Text = "A\xF4\x90\x80\x80"
						  "B",
					.Skipped = 4 },                // past U+10FFFF: F4 needs 80 to 8F next
				{ .Text = "A\xC3", .Skipped = 1 }, // a lead byte at the end
			} };
			for (const Case& item : cases)
			{
				CAPTURE(item.Text);
				const TextLayoutResult layout = LayoutText(font, item.Text, RenderTextAlignment::Left);
				CHECK(layout.SkippedCodepoints == item.Skipped);
				REQUIRE(!layout.Quads.empty());
				CHECK(layout.Quads[0].PositionMin.x == doctest::Approx(0.0f));
				if (layout.Quads.size() == 2)
					CHECK(layout.Quads[1].PositionMin.x == doctest::Approx(0.45f)); // "AB" with its kerning: nothing advanced
			}
			// Well-formed sequences of every length decode to one codepoint each (none is in the font).
			CHECK(LayoutText(font, "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80", RenderTextAlignment::Left).SkippedCodepoints == 3);
		}

		TEST_CASE("TextLayout: spaces advance without quads and a trailing line feed starts an empty line")
		{
			const FontData font = MakeFont();
			const TextLayoutResult spaces = LayoutText(font, "  ", RenderTextAlignment::Left);
			CHECK(spaces.Quads.empty());
			CHECK(spaces.LineCount == 1);
			CHECK(spaces.Size.x == doctest::Approx(0.5f));
			const TextLayoutResult trailing = LayoutText(font, "A\n", RenderTextAlignment::Center);
			CHECK(trailing.LineCount == 2);
			CHECK(trailing.Size.y == doctest::Approx(2.1f));
			REQUIRE(trailing.Quads.size() == 1);
			CHECK(trailing.Quads[0].PositionMin.x == doctest::Approx(0.0f)); // the widest line is the only one with a width
		}

		TEST_CASE("TextLayout: screen text is placed from anchor, offset and pivot at the 1080p scale")
		{
			TextItem item;
			item.Anchor = glm::vec2(1.0f, 0.0f); // top right
			item.Pivot = glm::vec2(1.0f, 0.0f);
			item.Offset = glm::vec2(-20.0f, 10.0f);
			// A 1920 x 540 viewport: scale 0.5, so the offset is (-10, 5) pixels.
			const glm::vec2 placed = PlaceScreenText(item, glm::vec2(100.0f, 30.0f), 1920, 540);
			CHECK(placed.x == doctest::Approx(1920.0f - 10.0f - 100.0f));
			CHECK(placed.y == doctest::Approx(5.0f));
			item.Anchor = glm::vec2(0.5f);
			item.Pivot = glm::vec2(0.5f);
			item.Offset = glm::vec2(0.0f);
			const glm::vec2 centred = PlaceScreenText(item, glm::vec2(100.0f, 30.0f), 1920, 1080);
			CHECK(centred == glm::vec2(910.0f, 525.0f));
		}
	}

}
