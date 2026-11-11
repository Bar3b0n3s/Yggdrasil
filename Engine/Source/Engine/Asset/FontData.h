#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Font CPU data and its cooked payload (Architecture §6.8 "font: R8 SDF atlas and glyph metrics", §7.4 FontImporter, §8.10
// text). Produced by FontImporter (TTF, OTF: ASCII + Latin-1 at 48 px with spread 8) and drawn by the TextRenderer (M8).
//
// Format 2 (M22, C22.3, R-UI-L10N and R-UI-L10N-RTL; Docs/Decisions/0033-m22-contract.md decision 44; FontImporter version
// 5): what shaping and the fallback chain read. Each glyph records its glyph index in the font; IndexedGlyphs holds every
// baked glyph by index, those that only shaping reaches included (an Arabic letter's contextual forms, ligatures: the
// importer bakes the glyphs the font's substitutions can produce from its baked codepoints); ShapingSource is the sfnt
// HarfBuzz shapes with; Fallbacks are the fonts that follow this one in a text's chain (FontImportSettings::Fallbacks).
// The members are frozen by the contract and filled by stream S (the glyph indices, IndexedGlyphs, UnitsPerEm and
// ShapingSource, the payload's version 2) and stream F (Fallbacks); a FontData without ShapingSource (format 1, or built
// by hand) measures without shaping (Renderer/TextMeasurement.h). Shaping data is whole or absent: ShapingSource,
// UnitsPerEm and IndexedGlyphs are all set (FontImporter version 5 sets them for every font it bakes) or all empty
// (ValidateFontData).

namespace Engine {

	// One glyph's metrics and atlas rectangle. Plane coordinates are in em units relative to the pen position on the
	// baseline, x to the right and y up; atlas coordinates are normalized texture coordinates with the origin at the top left.
	// A glyph without an outline (space) has an empty rectangle (PlaneMin == PlaneMax, AtlasMin == AtlasMax).
	struct FontGlyph
	{
		uint32_t Codepoint = 0;
		float Advance = 0.0f; // em units
		glm::vec2 PlaneMin{ 0.0f };
		glm::vec2 PlaneMax{ 0.0f };
		glm::vec2 AtlasMin{ 0.0f };
		glm::vec2 AtlasMax{ 0.0f };
		// Format 2 (C22.3): the glyph's index in the font (its glyph id, the one HarfBuzz shapes to); 0 in format 1.
		uint32_t GlyphIndex = 0;

		bool operator==(const FontGlyph&) const = default;
	};

	// The kerning adjustment between two glyphs, added to First's advance when Second follows it.
	struct FontKerningPair
	{
		uint32_t First = 0;
		uint32_t Second = 0;
		float Advance = 0.0f; // em units, usually negative

		bool operator==(const FontKerningPair&) const = default;
	};

	// A loaded SDF font (AssetType::Font). Plain data; immutable once loaded (AssetRef<FontData>).
	struct FontData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Font;
		// The cooked payload layout below (CookedHeader::FormatVersion) that SerializeFontPayload writes; format 1 payloads
		// still load (LoadCookedFont).
		static constexpr uint16_t FormatVersion = 2;

		FontData()
			: Asset(StaticType)
		{
		}

		float PixelSize = 48.0f; // atlas pixels per em the glyphs were rasterized at (§7.4: 48 px)
		float Spread = 8.0f;     // the distance field's range in atlas pixels on each side of an edge (§7.4: spread 8)
		float Ascent = 0.0f;     // em units above the baseline (positive)
		float Descent = 0.0f;    // em units below the baseline (negative)
		float LineGap = 0.0f;    // em units; line height = Ascent - Descent + LineGap
		uint32_t AtlasWidth = 0;
		uint32_t AtlasHeight = 0;
		// R8 signed distance field, rows top first, tightly packed: 128 is the outline, larger values inside.
		Buffer AtlasPixels{};
		std::vector<FontGlyph> Glyphs{};        // sorted by Codepoint, unique
		std::vector<FontKerningPair> Kerning{}; // sorted by (First, Second), unique, both glyphs present

		// --- Format 2 (C22.3, decision 44) ------------------------------------------------------------------------------
		// The font's units per em (head.unitsPerEm, 16 to 16,384); 0 without shaping data.
		uint32_t UnitsPerEm = 0;
		// Every baked glyph once, sorted by GlyphIndex, unique: the glyphs of Glyphs (each with the smallest baked codepoint
		// that maps to it as its Codepoint, and the same metrics and atlas rectangle) and the glyphs only shaping reaches
		// (Codepoint 0). Empty without shaping data.
		std::vector<FontGlyph> IndexedGlyphs{};
		// The sfnt HarfBuzz shapes with: the source font subset by HarfBuzz's subsetter to the baked glyphs, glyph ids
		// retained, its layout (GDEF, GSUB, GPOS, kern) and metrics tables kept; empty: the font has no shaping data.
		Buffer ShapingSource{};
		// The fonts that follow this one in a text's font chain, in order (FontImportSettings::Fallbacks), at most
		// MaxFontFallbacks, none null (ValidateFontData checks both); the importer records them as its dependencies.
		std::vector<AssetHandle> Fallbacks{};
	};

	// C22.3: the most fallbacks one font names.
	inline constexpr size_t MaxFontFallbacks = 8;
	// C22.3 (decision 44; stream S): FontData::UnitsPerEm's range when the font has shaping data (head.unitsPerEm's, as
	// FontImporter reads it), and the largest ShapingSource a font holds, the import's limit on the subset it embeds.
	inline constexpr uint32_t MinFontUnitsPerEm = 16;
	inline constexpr uint32_t MaxFontUnitsPerEm = 16384;
	inline constexpr size_t MaxFontShapingSourceSize = size_t{ 64 } * 1024 * 1024;

	// The glyph for `codepoint`, or nullptr (binary search).
	[[nodiscard]] const FontGlyph* FindGlyph(const FontData& font, uint32_t codepoint);

	// C22.3 (decision 44; stream S): the glyph whose GlyphIndex is `glyphIndex` in IndexedGlyphs, or nullptr (binary search).
	[[nodiscard]] const FontGlyph* FindGlyphByIndex(const FontData& font, uint32_t glyphIndex);

	// Checks: PixelSize and Spread positive; Ascent >= 0 >= Descent; every float finite; an atlas of 1 to 16384 pixels in
	// each dimension holding exactly AtlasWidth * AtlasHeight bytes; glyphs and kerning sorted and unique, kerning glyphs
	// present, atlas rectangles inside [0, 1]. Format 2 (C22.3): shaping data whole or absent (ShapingSource of at most
	// MaxFontShapingSourceSize bytes, UnitsPerEm from MinFontUnitsPerEm to MaxFontUnitsPerEm and IndexedGlyphs all set, or
	// all empty); IndexedGlyphs sorted by GlyphIndex and unique, each glyph valid, each one with a Codepoint other than 0 the
	// glyph of that codepoint (FindGlyph, the same GlyphIndex); with shaping data, every codepoint glyph present in
	// IndexedGlyphs by its GlyphIndex with equal metrics (Advance, PlaneMin and PlaneMax); at most MaxFontFallbacks
	// Fallbacks, none null. Pure. Errors: Validation naming the first violation.
	[[nodiscard]] Status ValidateFontData(const FontData& font);

	// The cooked font payload (FormatVersion 2), little-endian. Format 1's:
	//     f32 PixelSize; f32 Spread; f32 Ascent; f32 Descent; f32 LineGap;
	//     uint32 AtlasWidth; uint32 AtlasHeight; uint32 GlyphCount; uint32 KerningCount;
	//     GlyphCount x { uint32 Codepoint; f32 Advance; f32x2 PlaneMin; f32x2 PlaneMax; f32x2 AtlasMin; f32x2 AtlasMax; }
	//     KerningCount x { uint32 First; uint32 Second; f32 Advance; }
	//     AtlasWidth * AtlasHeight atlas bytes
	// then format 2's (C22.3, decision 44):
	//     GlyphCount x uint32 GlyphIndex (Glyphs' own, in their order)
	//     uint32 UnitsPerEm; uint32 IndexedGlyphCount;
	//     IndexedGlyphCount x { uint32 Codepoint; f32 Advance; f32x2 PlaneMin; f32x2 PlaneMax; f32x2 AtlasMin; f32x2 AtlasMax;
	//                           uint32 GlyphIndex; }
	//     uint32 ShapingSourceSize; ShapingSourceSize bytes
	//     uint32 FallbackCount; FallbackCount x uint64 Fallback (an AssetHandle)
	// Asserts ValidateFontData. Pure; identical fonts give identical bytes.
	[[nodiscard]] Buffer SerializeFontPayload(const FontData& font);

	// Reads a payload of `formatVersion` (1 or 2: format 1's ends after the atlas and leaves the format 2 members empty and
	// every GlyphIndex 0), then ValidateFontData. Every count is checked against its limit and the bytes left before
	// anything is allocated. Never asserts on data. Errors: Parse for truncation, a count beyond its limit or trailing
	// bytes; UnsupportedVersion for another version; Validation from ValidateFontData.
	[[nodiscard]] Result<FontData> DeserializeFontPayload(std::span<const std::byte> payload, uint16_t formatVersion = FontData::FormatVersion);

	// The complete cooked artifact (CookedHeader + payload) of `font`, at FormatVersion.
	[[nodiscard]] Buffer CookFont(const FontData& font, uint32_t importerVersion);

	// The font of a cooked artifact of payload format 1 or 2. Errors: as ReadCookedArtifact (Validation for another type,
	// UnsupportedVersion for another format version) and DeserializeFontPayload.
	[[nodiscard]] Result<AssetRef<FontData>> LoadCookedFont(std::span<const std::byte> cooked);

}
