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
		// and every table inside the file, the head, hhea, maxp and hmtx tables with the fields it reads at fixed offsets, a
		// Unicode cmap subtable of a format it reads (0, 4, 6, 12 or 13) with every range its lookup touches inside the cmap
		// table, and glyph locations (loca) inside the glyf table for TrueType outlines, or a CFF table otherwise (whose
		// contents stb_truetype parses itself). Errors: ImportFailed naming the first problem.
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
