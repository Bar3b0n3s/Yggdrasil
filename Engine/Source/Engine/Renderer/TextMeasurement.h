#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Text measurement (M22, C22.1, R-UI-TEXT; Docs/Decisions/0033-m22-contract.md decision 6): pure CPU functions from a font
// chain's metrics (Asset/FontData.h) and a UTF-8 string to positioned glyphs, lines and a block size in pixels, with word
// wrapping, line spacing and alignment. The text-measurement layer of the UI (the Ui pass lays out UiText with it,
// Renderer/UiPass.h) and of every later text feature (W22.3's text input carets, fallback fonts, shaping and bidi). It sits
// beside the frozen TextLayout.h (Docs/Decisions/0013-m8-decisions.md decision 11), which keeps laying out TextComponent
// unchanged.
//
// Shaping and bidi are reserved for W22.3, behind these signatures: W22.1 maps each codepoint to its font's glyph, left to
// right, with the font's kerning (FontKerningPair) between two glyphs of one font; W22.3 runs SheenBidi's bidirectional
// algorithm (TextDirection) and HarfBuzz's shaping inside MeasureText, so a glyph may then stand for a cluster of
// codepoints (TextMeasuredGlyph::ByteOffset names the cluster's first byte). The font chain is the fallback chain W22.3
// fills: a codepoint takes the first font of the chain that has a glyph for it.
//
// Deterministic: identical input gives identical output in every configuration (only IEEE-exact operations on the fonts'
// floats; no transcendental function, no wall clock, no global state), so layouts and shaped text are byte-identical across
// configurations (Roadmap M22's acceptance). Thread-safe. Includes nothing beyond Core, Asset (through RenderSnapshot.h and
// the forward-declared FontData), glm and the standard library, so a later contract may list it among the headers Scene may
// include (Scripts/ModuleRules.json's SnapshotHeaders); W22.1's Scene does not include it (decision 6).

namespace Engine {

	struct FontData;

	// How a text block breaks into lines (besides at line feeds, which always break).
	enum class TextWrap : uint8_t
	{
		None, // lines break only at line feeds
		Word  // also before a word that would end beyond MaxWidth; a word wider than MaxWidth breaks between glyphs
	};

	// The base direction of a text block (W22.3's bidirectional algorithm). In W22.1 every value lays out left to right.
	enum class TextDirection : uint8_t
	{
		Auto, // the direction of the text's first strong character (left to right without one)
		LeftToRight,
		RightToLeft
	};

	struct TextMeasureOptions
	{
		float Size = 32.0f; // pixels per em, finite and > 0
		// Multiplies the line height (Ascent - Descent + LineGap of the chain's first font, in em: times Size in pixels), finite
		// and > 0.
		float LineSpacing = 1.0f;
		TextWrap Wrap = TextWrap::None;
		// The block's width in pixels when > 0: Word wraps lines to it, and every line aligns within it; <= 0 (or not finite):
		// no wrapping, and lines align within the widest line.
		float MaxWidth = 0.0f;
		RenderTextAlignment Alignment = RenderTextAlignment::Left;
		TextDirection Direction = TextDirection::Auto;
	};

	// One positioned glyph, in pixels relative to the block's top-left corner (+x right, +y down).
	struct TextMeasuredGlyph
	{
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Max = glm::vec2(0.0f);
		// Its rectangle in its font's atlas (normalized, origin top left; FontGlyph::AtlasMin and AtlasMax).
		glm::vec2 AtlasMin = glm::vec2(0.0f);
		glm::vec2 AtlasMax = glm::vec2(0.0f);
		uint32_t Font = 0;       // the index of the font that draws it in the measured chain
		uint32_t ByteOffset = 0; // the UTF-8 byte offset in the text of the (first) codepoint it draws

		bool operator==(const TextMeasuredGlyph&) const = default;
	};

	// One line of a measured block.
	struct TextMeasuredLine
	{
		uint32_t FirstGlyph = 0; // its glyphs: TextMeasurement::Glyphs[FirstGlyph, FirstGlyph + GlyphCount)
		uint32_t GlyphCount = 0;
		uint32_t ByteBegin = 0; // its text: [ByteBegin, ByteEnd) of the input, without the line feed or the space it broke at
		uint32_t ByteEnd = 0;
		float Left = 0.0f;     // the x of its pen start after alignment, pixels
		float Width = 0.0f;    // its advance width (glyph advances and kerning), pixels
		float Top = 0.0f;      // its top, pixels: lineIndex * (Ascent - Descent + LineGap) * Size * LineSpacing
		float Baseline = 0.0f; // its baseline, pixels: Top + Ascent * Size

		bool operator==(const TextMeasuredLine&) const = default;
	};

	struct TextMeasurement
	{
		std::vector<TextMeasuredGlyph> Glyphs{}; // glyphs with an outline, in text order (spaces advance but have no glyph)
		std::vector<TextMeasuredLine> Lines{};   // one per line ('\n' separates them, wrapping adds more); empty text: none
		// The block in pixels: its width (MaxWidth when > 0, otherwise the widest line's) and its height, every line's pitch but
		// the last's, then the last line without its gap: (LineCount - 1) * (Ascent - Descent + LineGap) * Size * LineSpacing
		// + (Ascent - Descent) * Size (the metrics are the chain's first font's, in em).
		glm::vec2 Size = glm::vec2(0.0f);
		// Codepoints no font of the chain has a glyph for, plus invalid UTF-8 sequences: each skipped without advancing the pen.
		uint32_t SkippedCodepoints = 0;

		bool operator==(const TextMeasurement&) const = default;
	};

	// Measures and lays out `text` with the font chain `fonts` (at least one, none null; asserted) by `options` (asserted
	// finite and positive where documented). '\r' is ignored. Pure.
	[[nodiscard]] TextMeasurement MeasureText(std::span<const FontData* const> fonts, std::string_view text, const TextMeasureOptions& options);

	// MeasureText's Size alone. Pure.
	[[nodiscard]] glm::vec2 MeasureTextSize(std::span<const FontData* const> fonts, std::string_view text, const TextMeasureOptions& options);

}
