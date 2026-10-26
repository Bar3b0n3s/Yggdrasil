#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// FontImporter's first-party reading of sfnt tables (TrueType and OpenType/CFF fonts): the structural checks that guard
// stb_truetype, which reads a font without bounds checks (TrueTypeFont.h), and the font's horizontal kerning, read
// here with bounds-checked reads instead of stb_truetype's, which ignores extension lookups (Inter's kerning lives in
// one) and every lookup's feature. Big-endian reads of untrusted bytes; never asserts on data. Pure and thread-safe.

namespace Engine {

	// One glyph of the character set being imported: its codepoint and the glyph index the font maps it to.
	struct TrueTypeGlyphEntry
	{
		uint32_t Codepoint = 0;
		uint16_t Glyph = 0;
	};

	// A non-zero horizontal kerning adjustment between two glyphs of the set, in font units.
	struct TrueTypeKerningEntry
	{
		uint32_t First = 0;  // codepoint
		uint32_t Second = 0; // codepoint
		int32_t Adjustment = 0;
	};

	namespace Utils {

		// Checks what stb_truetype trusts: an sfnt signature (TrueType or CFF; collections are refused), the table directory
		// and every table inside the file, the head (units per em 16 to 16384), hhea, maxp and hmtx tables with the fields it
		// reads at fixed offsets, a Unicode cmap subtable of a format it reads (0, 4, 6, 12 or 13) with every range its lookup
		// touches inside the cmap table, and for TrueType outlines glyph locations (loca) inside the glyf table and every
		// glyph outline as stbtt_GetGlyphShape reads it (each simple glyph's contour ends, instructions, flags and
		// coordinates inside the glyph, and no final one-point off-curve contour; each composite glyph's components inside
		// it, placed by offsets, nested at most 8 deep without cycles, expanding to at most 65,536 points), or for CFF
		// outlines the CFF table as stbtt_InitFont and its charstring interpreter read it (at most 512 MiB; the header and
		// the Name, Top DICT, String and Global Subr INDEXes; the first Top DICT's CharStrings, Type 2 and at least one per
		// glyph, its Private DICT and local subroutines, and for a CID font every font dictionary's and an FDSelect naming
		// only font dictionaries the FDArray has; every structure inside the table and every DICT operand stb_truetype reads
		// an integer) and every glyph's charstring run as stb_truetype runs it, with no hint mask past the end of its code,
		// no line or curve before its first moveto, no local subroutine call from a CID glyph the FDSelect gives no font
		// dictionary, and at most 65,536 operators and operands per glyph (2^26 for the whole font). Errors: ImportFailed
		// naming the first problem.
		[[nodiscard]] Status ValidateTrueTypeTables(std::span<const std::byte> font);

		// The horizontal kerning between every ordered pair of `glyphs` (sorted by codepoint, unique): the pair adjustment
		// lookups (type 2, also inside extension lookups, type 9) of the GPOS 'kern' feature of the 'latn' script's default
		// language system (else 'DFLT', else every 'kern' feature), applied as OpenType specifies (the first subtable of a
		// lookup that matches a pair applies; lookups add up in lookup order), using each first glyph's XAdvance. A font
		// without such a feature uses its legacy kern table (horizontal format 0 subtables). The result holds only non-zero
		// adjustments, sorted by (First, Second). Errors: ImportFailed for a truncated or inconsistent GPOS or kern table.
		[[nodiscard]] Result<std::vector<TrueTypeKerningEntry>> ReadTrueTypeKerning(std::span<const std::byte> font,
			std::span<const TrueTypeGlyphEntry> glyphs);

	}

}
