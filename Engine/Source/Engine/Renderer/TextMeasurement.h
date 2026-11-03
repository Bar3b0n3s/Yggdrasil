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
//
// Shaping and bidi (C22.3, R-UI-L10N-RTL; Docs/Decisions/0033-m22-contract.md decision 43; stream S). MeasureText:
//   1. splits the text into paragraphs at line feeds ('\r' ignored, ill-formed bytes skipped as before);
//   2. runs SheenBidi's bidirectional algorithm (UAX #9) on each paragraph with the base level of Direction (Auto: the
//      first strong character's, left to right without one; LeftToRight 0; RightToLeft 1);
//   3. itemizes each paragraph into runs of one embedding level, one script (SheenBidi's script locator) and one font
//      (each codepoint the first font of the chain whose baked glyphs hold it, FindGlyph; a mark or joiner the font of
//      the codepoint before it when that font has it; a codepoint no font has is skipped as before);
//   4. shapes each run whose font carries shaping data (FontData::ShapingSource, format 2) with HarfBuzz: a face and
//      font per chain font from its ShapingSource, the font's scale its UnitsPerEm (so positions come out as integer
//      font units), the run's direction and script, the language set explicitly to "und" (never the process's locale),
//      the shaper's default features, monotone character clusters;
//      advances and offsets in pixels are value / UnitsPerEm * Size, and each glyph's atlas rectangle is its font's
//      FindGlyphByIndex (a glyph the font did not bake is skipped and counted in SkippedCodepoints); a run whose font has
//      none is laid out codepoint by codepoint with the font's kerning pairs, as W22.1 lays out every text;
//   5. breaks lines as W22.1 does, in logical order, at the spaces between clusters, with the shaped advances;
//   6. reorders each line into visual order (UAX #9 rules L1 and L2) and aligns it by Alignment within the block.
// Kerning in a shaped run is the font's own (GPOS or kern, through HarfBuzz; FontKerningPair is not read). Left-to-right
// text in fonts without shaping data (a FontData built by hand, every test of W22.1) measures exactly as before: its
// bidirectional runs are one left-to-right run per line, and its runs are not shaped. Every font FontImporter version 5
// bakes carries shaping data, the built-in default font (Inter) included, so its text is shaped with the default features
// and may measure differently from W22.1's (the lead's decision 2 of the review of C22.3: the goldens that move are
// regenerated at W22.3's integration; text measurement reaches no state hash). Positions are integers before the
// one conversion to pixels, which is IEEE-exact, so shaped text is byte-identical across configurations (Roadmap M22's
// acceptance). HarfBuzz and SheenBidi stay private to Renderer (no public header exposes
// their types; Scripts/ModuleRules.json).

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

	// C22.3 (decision 43): one cluster of a measured block, the smallest part of the text the shaper does not split (one
	// codepoint without shaping data; a ligature's codepoints, or a base and its marks, with it), with its place on its
	// line: what a caret is placed by (FindTextCaret).
	struct TextMeasuredCluster
	{
		uint32_t ByteBegin = 0; // its text: [ByteBegin, ByteEnd) of the input
		uint32_t ByteEnd = 0;
		uint32_t Line = 0; // the index of its line in TextMeasurement::Lines
		// Its horizontal extent from the block's left edge in pixels, Left <= Right (a space's advance included).
		float Left = 0.0f;
		float Right = 0.0f;
		bool RightToLeft = false; // its embedding level is odd

		bool operator==(const TextMeasuredCluster&) const = default;
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
		// C22.3 (decision 43; stream S fills it on both paths): every cluster of every line, line by line, each line's in
		// visual order (left to right on the page), spaces included, line feeds and skipped bytes left out.
		std::vector<TextMeasuredCluster> Clusters{};
		// C22.3 (stream S): the height of a line's glyph box, (Ascent - Descent) * Size of the chain's first font (a line's
		// pitch adds its gap and multiplies by LineSpacing); a caret spans it from its line's Top.
		float LineHeight = 0.0f;

		bool operator==(const TextMeasurement&) const = default;
	};

	// C22.3 (decision 48): where a caret is drawn in a measured block, in pixels from the block's top-left corner.
	struct TextCaret
	{
		glm::vec2 Top = glm::vec2(0.0f); // the caret's x and its line's top; it spans TextMeasurement::LineHeight down
		uint32_t Line = 0;

		bool operator==(const TextCaret&) const = default;
	};

	// Measures and lays out `text` with the font chain `fonts` (at least one, none null; asserted) by `options` (asserted
	// finite and positive where documented). '\r' is ignored. Pure.
	[[nodiscard]] TextMeasurement MeasureText(std::span<const FontData* const> fonts, std::string_view text, const TextMeasureOptions& options);

	// MeasureText's Size alone. Pure.
	[[nodiscard]] glm::vec2 MeasureTextSize(std::span<const FontData* const> fonts, std::string_view text, const TextMeasureOptions& options);

	// C22.3 (decision 48; stream S): the caret before the byte `byteOffset` of the text `measurement` measured. The cluster
	// that begins at it gives its leading edge (Left for a left-to-right cluster, Right for a right-to-left one); else the
	// cluster that ends at it, its trailing edge (the caret after the text's or a line's last cluster); else the cluster
	// that holds it, its leading edge (a caret never splits a cluster); a byte of a line without clusters (an empty line after
	// a line feed) gives that line's start (its Left, the alignment's); a byte of a line's [ByteBegin, ByteEnd) that no
	// cluster begins at, ends at or holds (a skipped ill-formed byte or a carriage return) gives the trailing edge of that
	// line's cluster that ends last before it in logical order, or, when none does, that line's start (the leading edge of
	// its first cluster in logical order); a text without lines (empty) gives line 0 at Top 0 and the alignment's start of
	// the block (0 for Left, half its width for Center, its width for Right). A byte past the text is its end. Pure.
	[[nodiscard]] TextCaret FindTextCaret(const TextMeasurement& measurement, uint32_t byteOffset);

}
