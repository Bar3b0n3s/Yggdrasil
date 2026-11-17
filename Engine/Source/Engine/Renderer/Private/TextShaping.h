#pragma once

#include "Engine/Core/Base.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The text-measurement layer's bidirectional algorithm and shaping (Renderer/TextMeasurement.h's shaping and bidi rules;
// Docs/Decisions/0033-m22-contract.md decision 43): SheenBidi's UAX #9 levels and runs and its UAX #24 script runs, and
// HarfBuzz's shaping of a run with a font's shaping data (FontData::ShapingSource). TextShaping.cpp is the only Renderer
// translation unit that includes either library, and this header exposes none of their types (Architecture §3 rule 3).
// Every object lives within one MeasureText call: nothing is cached across calls, so measurement stays pure.
// Deterministic: SheenBidi works on integers; HarfBuzz shapes at a scale of the font's units per em, so its positions are
// integers in font units, and the language is set explicitly ("und"), never read from the process.

namespace Engine {

	struct FontData;

	// One run of a line at one embedding level (the bidirectional algorithm's rules L1 and L2 applied): the codepoints
	// [Offset, Offset + Length) of the paragraph.
	struct TextBidiRun
	{
		uint32_t Offset = 0;
		uint32_t Length = 0;
		uint8_t Level = 0;
	};

	// The bidirectional algorithm (UAX #9, SheenBidi) on one paragraph of codepoints. The paragraph is never split: a
	// codepoint of bidirectional type B (a paragraph separator other than the line feed MeasureText splits at) is read as a
	// space. When SheenBidi cannot allocate its objects, every codepoint is at the base level (0 for Auto and LeftToRight, 1
	// for RightToLeft) and a line is one run. Not copyable.
	class TextBidiParagraph
	{
	public:
		// The direction of the paragraph: Auto takes its first strong character's (left to right without one).
		enum class Direction : uint8_t
		{
			Auto,
			LeftToRight,
			RightToLeft
		};

		TextBidiParagraph(std::span<const uint32_t> codepoints, Direction direction);
		~TextBidiParagraph();

		TextBidiParagraph(const TextBidiParagraph&) = delete;
		TextBidiParagraph& operator=(const TextBidiParagraph&) = delete;

		// The resolved embedding level (rules X1 to I2) of the codepoint at `index`, below the paragraph's size.
		[[nodiscard]] uint8_t GetLevel(size_t index) const;
		// M38 (Docs/Decisions/0055-m38-runtime-locale.md decision 11): the paragraph's base level (rules P2 and P3 for Auto, the
		// first strong character's; 0 without one), odd for a right-to-left paragraph; the direction's level when SheenBidi
		// could not allocate its objects or the paragraph is empty.
		[[nodiscard]] uint8_t GetBaseLevel() const;
		// The runs of the line [offset, offset + length) of the paragraph (length > 0) in visual order, left to right, after
		// rules L1 (trailing whitespace and segment separators reset to the paragraph's level) and L2.
		[[nodiscard]] std::vector<TextBidiRun> GetLineRuns(uint32_t offset, uint32_t length) const;
	private:
		struct State; // the SheenBidi algorithm and paragraph (TextShaping.cpp)
	private:
		Scope<State> m_State;
	};

	// One run of codepoints of one script (UAX #24, SheenBidi's script locator: common and inherited codepoints take their
	// neighbours' script): [Offset, Offset + Length), Script an ISO 15924 tag in big-endian byte order ('Arab' is
	// 0x41726162), or 'Zyyy' (Common) for a run of common codepoints alone.
	struct TextScriptRun
	{
		uint32_t Offset = 0;
		uint32_t Length = 0;
		uint32_t Script = 0;
	};

	// One glyph of a shaped run, in font units (the font's scale is its units per em): the glyph index, the index of the
	// first codepoint of its cluster in the shaped codepoints, its advance and its offset (y up).
	struct TextShapedGlyph
	{
		uint32_t GlyphIndex = 0;
		uint32_t Cluster = 0;
		int32_t XAdvance = 0;
		int32_t XOffset = 0;
		int32_t YOffset = 0;
	};

	// HarfBuzz faces and fonts of a font chain's shaping sources, created when the shaper is: the fonts (and their
	// ShapingSource bytes, which the faces read in place) must outlive it, so it lives within one MeasureText call. Not
	// copyable.
	class TextShaper
	{
	public:
		explicit TextShaper(std::span<const FontData* const> fonts);
		~TextShaper();

		TextShaper(const TextShaper&) = delete;
		TextShaper& operator=(const TextShaper&) = delete;

		// True when the chain's font `font` has shaping data (FontData::ShapingSource with its UnitsPerEm).
		[[nodiscard]] bool CanShape(uint32_t font) const;
		// Shapes the codepoints [offset, offset + length) of `codepoints` (the others are context, as for a run's
		// neighbours) with the chain's font `font` (CanShape) at the script `script` (an ISO 15924 tag, TextScriptRun) in
		// the direction `rightToLeft` gives: the language "und", the shaper's default features, monotone character
		// clusters (each glyph's Cluster the index in `codepoints` of its cluster's first codepoint) and no dotted circle
		// for a broken cluster. Returns the glyphs in visual order, left to right (a right-to-left run's clusters
		// descending), or none when HarfBuzz failed to allocate them.
		[[nodiscard]] std::vector<TextShapedGlyph> Shape(uint32_t font, std::span<const uint32_t> codepoints, uint32_t offset, uint32_t length,
			uint32_t script, bool rightToLeft) const;
	private:
		struct State; // the HarfBuzz objects (TextShaping.cpp)
	private:
		Scope<State> m_State;
	};

	namespace Utils {

		// The script runs of `codepoints`, in order, covering them.
		[[nodiscard]] std::vector<TextScriptRun> FindTextScriptRuns(std::span<const uint32_t> codepoints);

		// True when `codepoint` is a combining mark (general category Mn, Mc or Me) or a zero-width joiner or non-joiner
		// (U+200D, U+200C): a codepoint that takes the font of the codepoint before it (TextMeasurement.h's itemization).
		[[nodiscard]] bool IsTextMarkOrJoiner(uint32_t codepoint);

	}

}
