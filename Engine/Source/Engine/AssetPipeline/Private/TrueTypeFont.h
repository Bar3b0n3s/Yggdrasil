#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// FontImporter's interface to stb_truetype (stb_truetype.h v1.26 of the vendored stb, Vendor/stb). The one translation
// unit that compiles it, AssetPipeline/ThirdParty/TrueTypeRasterizer.cpp, does so with STBTT_STATIC (Architecture §7.4,
// Appendix C), so its symbols never clash with Dear ImGui's own copy (imstb_truetype.h, for its font atlas) and no stb
// type appears outside it.
// Its transcendental functions are routed to DetMath, so a glyph's distance field does not depend on the C runtime, the
// CPU or the configuration. Kerning is read by TrueTypeTables.h, not by stb_truetype.

namespace Engine {

	// The font's vertical metrics (hhea table), in font units, y up.
	struct TrueTypeVerticalMetrics
	{
		int32_t Ascent = 0;  // above the baseline, usually positive
		int32_t Descent = 0; // below the baseline, usually negative
		int32_t LineGap = 0;
	};

	// One glyph's horizontal metrics (hmtx table), in font units.
	struct TrueTypeGlyphMetrics
	{
		int32_t Advance = 0;
		int32_t LeftSideBearing = 0;
	};

	// The pixels one glyph's distance field covers (TrueTypeFont::GetGlyphSdfBox): Width x Height pixels whose top-left
	// corner lies at (OffsetX, OffsetY) pixels from the pen position on the baseline, with y pointing down. Every member is
	// zero for a glyph without a field (no outline, or one too small to cover a pixel).
	struct TrueTypeGlyphBox
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		int32_t OffsetX = 0;
		int32_t OffsetY = 0;
	};

	// One glyph's signed distance field: R8 values, rows top first, tightly packed. The bitmap's top-left corner lies at
	// (OffsetX, OffsetY) pixels from the pen position on the baseline, with y pointing down. Every member is zero or empty
	// for a glyph without an outline (a space).
	struct TrueTypeGlyphSdf
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		int32_t OffsetX = 0;
		int32_t OffsetY = 0;
		Buffer Pixels{};
	};

	// One TrueType or OpenType (CFF) font opened from memory. Not copyable; movable. Thread-compatible: one importer job
	// uses one instance.
	class TrueTypeFont
	{
	public:
		~TrueTypeFont();

		TrueTypeFont(TrueTypeFont&& other) noexcept;
		TrueTypeFont& operator=(TrueTypeFont&& other) noexcept;
		TrueTypeFont(const TrueTypeFont&) = delete;
		TrueTypeFont& operator=(const TrueTypeFont&) = delete;

		// The number of font units per em (head table).
		[[nodiscard]] int32_t GetUnitsPerEm() const;
		[[nodiscard]] TrueTypeVerticalMetrics GetVerticalMetrics() const;

		// The glyph index the font maps `codepoint` to, or nullopt when it maps it to none (glyph 0, .notdef).
		[[nodiscard]] std::optional<int32_t> FindGlyph(uint32_t codepoint) const;
		[[nodiscard]] TrueTypeGlyphMetrics GetGlyphMetrics(int32_t glyph) const;
		// True when `glyph` has no outline.
		[[nodiscard]] bool IsGlyphEmpty(int32_t glyph) const;

		// The largest distance-field box GetGlyphSdfBox gives (so the largest field RenderGlyphSdf makes), in pixels on a
		// side: more than the importer's largest glyph (128 pixels per em plus a 32-pixel spread on each side) needs for any
		// glyph less than 15 em across.
		static constexpr int32_t MaxGlyphBitmapSide = 2048;

		// The box the distance field of `glyph` at `pixelsPerEm` (> 0) covers: the outline's bounding box in whole pixels
		// with `padding` (>= 0) pixels on each side. stb_truetype takes a TrueType glyph's box from its glyf header and a CFF
		// glyph's from running its charstring for the bounds alone; no outline is kept and no field is computed, so a caller
		// can measure every glyph before it rasterizes any. Empty for a glyph without an outline, or with one too small to
		// cover a pixel. Errors: ImportFailed when the box is inverted, or wider or taller than MaxGlyphBitmapSide pixels.
		[[nodiscard]] Result<TrueTypeGlyphBox> GetGlyphSdfBox(int32_t glyph, float pixelsPerEm, int32_t padding) const;

		// The distance field of `glyph` rasterized at `pixelsPerEm` (> 0) over `box`, the non-empty box GetGlyphSdfBox gave
		// for the same glyph and size: 128 on the outline, changing by `valuePerPixel` (> 0) per pixel of distance, larger
		// inside, clamped to 0..255; its size and offset are the box's. Errors: ImportFailed when stb_truetype cannot read
		// the outline.
		[[nodiscard]] Result<TrueTypeGlyphSdf> RenderGlyphSdf(int32_t glyph, const TrueTypeGlyphBox& box, float pixelsPerEm, float valuePerPixel) const;

		// Checks everything the methods of this class make stb_truetype read without bounds checks or assert on
		// (ValidateTrueTypeTables: the table directory, the tables it reads at fixed offsets, the selected cmap subtable, and
		// every TrueType glyph outline or the CFF table and every glyph's charstring), then opens the font, which keeps
		// pointing into `data`: `data` must outlive it. Errors: ImportFailed naming the first problem (not a font, a
		// truncated or inconsistent table, a font collection, no Unicode cmap, a glyph outline or charstring stb_truetype
		// cannot read safely).
		[[nodiscard]] static Result<TrueTypeFont> Open(std::span<const std::byte> data);
	private:
		struct State; // the stbtt_fontinfo (TrueTypeRasterizer.cpp)

		explicit TrueTypeFont(Scope<State> state);
	private:
		Scope<State> m_State;
	};

}
