#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// FontImporter's interface to stb_truetype (imstb_truetype.h from the vendored Dear ImGui, stb_truetype v1.26). The one
// translation unit that compiles it, AssetPipeline/ThirdParty/TrueTypeRasterizer.cpp, does so with STBTT_STATIC
// (Architecture §7.4, Appendix C), so its symbols never clash with Dear ImGui's copy and no stb type appears outside it.
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

		// The distance field of `glyph` rasterized at `pixelsPerEm` (> 0) with `padding` (>= 0) pixels around the outline's
		// bounding box: 128 on the outline, changing by `valuePerPixel` (> 0) per pixel of distance, larger inside, clamped
		// to 0..255. Empty for a glyph without an outline. Errors: ImportFailed when a glyph with an outline yields no
		// bitmap.
		[[nodiscard]] Result<TrueTypeGlyphSdf> RenderGlyphSdf(int32_t glyph, float pixelsPerEm, int32_t padding, float valuePerPixel) const;

		// Checks the sfnt structure stb_truetype trusts without bounds checks (ValidateTrueTypeTables: the table directory,
		// the tables it reads at fixed offsets, the selected cmap subtable and the glyph locations), then opens the font,
		// which keeps pointing into `data`: `data` must outlive it. stb_truetype itself is not hardened against hostile
		// input beyond these checks (the same policy as stb_image, Vendor/stb/VENDOR.md): import only trusted font files.
		// Errors: ImportFailed naming the first problem (not a font, a truncated or inconsistent table, a font collection,
		// no Unicode cmap).
		[[nodiscard]] static Result<TrueTypeFont> Open(std::span<const std::byte> data);
	private:
		struct State; // the stbtt_fontinfo (TrueTypeRasterizer.cpp)

		explicit TrueTypeFont(Scope<State> state);
	private:
		Scope<State> m_State;
	};

}
