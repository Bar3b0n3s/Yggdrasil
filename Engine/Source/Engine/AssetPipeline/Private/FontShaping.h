#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// FontImporter's use of HarfBuzz (version 5, FontData format 2; Docs/Decisions/0033-m22-contract.md decision 44): the
// glyphs shaping can produce from a font's baked codepoints, and the font subset the text-measurement layer shapes with
// (FontData::ShapingSource). FontShaping.cpp is the only AssetPipeline translation unit that includes HarfBuzz, and this
// header exposes none of its types. HarfBuzz reads the font through its own bounds-checked sanitizer. Pure and
// thread-safe: every HarfBuzz object is created and destroyed by each call.

namespace Engine {

	// What shaping reaches from a font's baked codepoints (Utils::FindFontShapingClosure).
	struct FontShapingClosure
	{
		// Every glyph shaping can produce from the codepoints, sorted and unique: their own glyphs (the font's cmap, as
		// HarfBuzz reads it) and those the substitutions of the shaper's default features reach, each below the font's glyph
		// count.
		std::vector<uint32_t> Glyphs{};
		// The codepoints the shaping subset maps (Utils::SubsetFontForShaping), sorted and unique: the baked ones and the
		// mirrored form (Bidi_Mirroring_Glyph) of each, when the font maps it, which a right-to-left run draws instead.
		std::vector<uint32_t> Codepoints{};
	};

	namespace Utils {

		// The closure of the font `source` over the baked codepoints `codepoints` (sorted, unique, each mapped by the font).
		// The codepoints and their mirrored forms are grouped by script (HarfBuzz's Unicode data): one buffer per script,
		// holding that script's codepoints and the Common and Inherited ones (which take the script of the run around them),
		// and one of the Common and Inherited ones alone; each buffer's closure (hb_ot_shape_glyphs_closure: the language
		// "und", no user features, so the shaper's default features) is taken in both directions, since a run's embedding
		// level, not its script, gives its direction. Errors: ImportFailed when HarfBuzz cannot read the font or allocate.
		[[nodiscard]] Result<FontShapingClosure> FindFontShapingClosure(std::span<const std::byte> source, std::span<const uint32_t> codepoints);

		// The font `source` subset by HarfBuzz's subsetter to `glyphs` (sorted, unique; the baked glyphs) and `codepoints`
		// (sorted, unique: its cmap keeps only them), with glyph ids retained, hinting dropped, every layout feature and
		// script kept (the layout tables GDEF, GSUB and GPOS, and the legacy kern table whole, which the subsetter drops by
		// default and cannot subset), no layout or bidirectional closure (the glyphs are already the closure), and the
		// metrics tables kept.
		// Errors: ImportFailed when the subsetter fails or the subset exceeds MaxFontShapingSourceSize.
		[[nodiscard]] Result<Buffer> SubsetFontForShaping(std::span<const std::byte> source, std::span<const uint32_t> codepoints,
			std::span<const uint32_t> glyphs);

	}

}
