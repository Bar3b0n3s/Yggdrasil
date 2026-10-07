// The one translation unit that compiles stb_truetype (imstb_truetype.h of the vendored Dear ImGui, stb_truetype v1.26),
// with STBTT_STATIC so its functions have internal linkage and never clash with Dear ImGui's copy (Architecture §7.4,
// Appendix C). Compiled without the precompiled header (the ThirdParty filter of Engine/premake5.lua), because the
// configuration macros below must precede the library. The cube root, cosine and arc cosine of its distance-field solver
// come from DetMath (§4.12), so a glyph's field does not depend on the C runtime, the CPU or the configuration; sqrt,
// fabs, fmod, floor and ceil are IEEE-exact already. stb_truetype reads fonts without bounds checks, so Open validates
// the structure it trusts first (TrueTypeTables.h).

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_sqrt(x) std::sqrt(x)
#define STBTT_pow(x, y) ::Engine::DetMath::Pow(x, y)
#define STBTT_cos(x) ::Engine::DetMath::Cos(x)
#define STBTT_acos(x) ::Engine::DetMath::ACos(x)

#include "Engine/AssetPipeline/Private/TrueTypeFont.h"

#include "Engine/AssetPipeline/Private/TrueTypeTables.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"

#include <imstb_truetype.h>

#include <climits>
#include <cmath>
#include <memory>
#include <utility>

namespace Engine {

	struct TrueTypeFont::State
	{
		stbtt_fontinfo Info{}; // points into the caller's font bytes, which outlive the font
		int32_t UnitsPerEm = 0;
		int32_t GlyphCount = 0;
	};

	namespace {

		// Releases a bitmap stb_truetype allocated.
		struct StbBitmapDeleter
		{
			void operator()(unsigned char* bitmap) const { stbtt_FreeSDF(bitmap, nullptr); }
		};

	}

	namespace Utils {

		[[nodiscard]] static const unsigned char* AsFontBytes(std::span<const std::byte> data)
		{
			return reinterpret_cast<const unsigned char*>(data.data());
		}

		// head.unitsPerEm, a big-endian uint16 at offset 18 of the head table (present and checked by
		// ValidateTrueTypeTables).
		[[nodiscard]] static int32_t ReadUnitsPerEm(const stbtt_fontinfo& info)
		{
			const unsigned char* head = info.data + info.head;
			return (static_cast<int32_t>(head[18]) << 8) | static_cast<int32_t>(head[19]);
		}

	}

	TrueTypeFont::TrueTypeFont(Scope<State> state)
		: m_State(std::move(state))
	{
	}

	TrueTypeFont::~TrueTypeFont() = default;

	TrueTypeFont::TrueTypeFont(TrueTypeFont&& other) noexcept = default;

	TrueTypeFont& TrueTypeFont::operator=(TrueTypeFont&& other) noexcept = default;

	int32_t TrueTypeFont::GetUnitsPerEm() const
	{
		return m_State->UnitsPerEm;
	}

	TrueTypeVerticalMetrics TrueTypeFont::GetVerticalMetrics() const
	{
		int ascent = 0;
		int descent = 0;
		int lineGap = 0;
		stbtt_GetFontVMetrics(&m_State->Info, &ascent, &descent, &lineGap);
		return { .Ascent = ascent, .Descent = descent, .LineGap = lineGap };
	}

	std::optional<int32_t> TrueTypeFont::FindGlyph(uint32_t codepoint) const
	{
		if (codepoint > static_cast<uint32_t>(INT_MAX))
			return std::nullopt;
		const int glyph = stbtt_FindGlyphIndex(&m_State->Info, static_cast<int>(codepoint));
		// A glyph index beyond maxp.numGlyphs would make stb_truetype read past the metrics; treat it as missing.
		if (glyph <= 0 || glyph >= m_State->GlyphCount)
			return std::nullopt;
		return glyph;
	}

	TrueTypeGlyphMetrics TrueTypeFont::GetGlyphMetrics(int32_t glyph) const
	{
		ENGINE_CORE_ASSERT(glyph >= 0 && glyph < m_State->GlyphCount, "glyph {} of a font with {} glyphs", glyph, m_State->GlyphCount);
		int advance = 0;
		int leftSideBearing = 0;
		stbtt_GetGlyphHMetrics(&m_State->Info, glyph, &advance, &leftSideBearing);
		return { .Advance = advance, .LeftSideBearing = leftSideBearing };
	}

	bool TrueTypeFont::IsGlyphEmpty(int32_t glyph) const
	{
		ENGINE_CORE_ASSERT(glyph >= 0 && glyph < m_State->GlyphCount, "glyph {} of a font with {} glyphs", glyph, m_State->GlyphCount);
		return stbtt_IsGlyphEmpty(&m_State->Info, glyph) != 0;
	}

	Result<TrueTypeGlyphSdf> TrueTypeFont::RenderGlyphSdf(int32_t glyph, float pixelsPerEm, int32_t padding, float valuePerPixel) const
	{
		ENGINE_CORE_ASSERT(glyph >= 0 && glyph < m_State->GlyphCount, "glyph {} of a font with {} glyphs", glyph, m_State->GlyphCount);
		ENGINE_CORE_ASSERT(pixelsPerEm > 0.0f && padding >= 0 && valuePerPixel > 0.0f, "invalid distance field parameters");
		if (IsGlyphEmpty(glyph))
			return TrueTypeGlyphSdf{};

		constexpr unsigned char OnEdgeValue = 128;
		const float scale = stbtt_ScaleForMappingEmToPixels(&m_State->Info, pixelsPerEm);
		int width = 0;
		int height = 0;
		int offsetX = 0;
		int offsetY = 0;
		const std::unique_ptr<unsigned char, StbBitmapDeleter> bitmap(
			stbtt_GetGlyphSDF(&m_State->Info, scale, glyph, padding, OnEdgeValue, valuePerPixel, &width, &height, &offsetX, &offsetY));
		if (bitmap == nullptr)
		{
			// An outline too small to cover a pixel at this size has an empty bounding box; it draws nothing.
			int x0 = 0;
			int y0 = 0;
			int x1 = 0;
			int y1 = 0;
			stbtt_GetGlyphBitmapBox(&m_State->Info, glyph, scale, scale, &x0, &y0, &x1, &y1);
			if (x0 == x1 || y0 == y1)
				return TrueTypeGlyphSdf{};
			return MakeError(ErrorCode::ImportFailed, "stb_truetype could not rasterize the distance field of glyph {}", glyph);
		}
		if (width <= 0 || height <= 0)
			return MakeError(ErrorCode::ImportFailed, "stb_truetype returned a {}x{} distance field for glyph {}", width, height, glyph);

		TrueTypeGlyphSdf sdf;
		sdf.Width = static_cast<uint32_t>(width);
		sdf.Height = static_cast<uint32_t>(height);
		sdf.OffsetX = offsetX;
		sdf.OffsetY = offsetY;
		const auto* pixels = reinterpret_cast<const std::byte*>(bitmap.get());
		sdf.Pixels.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height));
		return sdf;
	}

	Result<TrueTypeFont> TrueTypeFont::Open(std::span<const std::byte> data)
	{
		ENGINE_TRY(Utils::ValidateTrueTypeTables(data));
		Scope<State> state = CreateScope<State>();
		if (stbtt_InitFont(&state->Info, Utils::AsFontBytes(data), 0) == 0)
			return MakeError(ErrorCode::ImportFailed, "stb_truetype could not read the font (unsupported outlines or tables)");
		state->UnitsPerEm = Utils::ReadUnitsPerEm(state->Info);
		state->GlyphCount = state->Info.numGlyphs;
		return TrueTypeFont(std::move(state));
	}

}
