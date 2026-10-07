#pragma once

#include "Engine/Asset/Asset.h"
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
		// The cooked payload layout below (CookedHeader::FormatVersion).
		static constexpr uint16_t FormatVersion = 1;

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
	};

	// The glyph for `codepoint`, or nullptr (binary search).
	[[nodiscard]] const FontGlyph* FindGlyph(const FontData& font, uint32_t codepoint);

	// Checks: PixelSize and Spread positive; Ascent >= 0 >= Descent; every float finite; an atlas of 1 to 16384 pixels in
	// each dimension holding exactly AtlasWidth * AtlasHeight bytes; glyphs and kerning sorted and unique, kerning glyphs
	// present, atlas rectangles inside [0, 1]. Pure. Errors: Validation naming the first violation.
	[[nodiscard]] Status ValidateFontData(const FontData& font);

	// The cooked font payload (FormatVersion 1), little-endian:
	//     f32 PixelSize; f32 Spread; f32 Ascent; f32 Descent; f32 LineGap;
	//     uint32 AtlasWidth; uint32 AtlasHeight; uint32 GlyphCount; uint32 KerningCount;
	//     GlyphCount x { uint32 Codepoint; f32 Advance; f32x2 PlaneMin; f32x2 PlaneMax; f32x2 AtlasMin; f32x2 AtlasMax; }
	//     KerningCount x { uint32 First; uint32 Second; f32 Advance; }
	//     AtlasWidth * AtlasHeight atlas bytes
	// Asserts ValidateFontData. Pure; identical fonts give identical bytes.
	[[nodiscard]] Buffer SerializeFontPayload(const FontData& font);

	// Reads a payload written by SerializeFontPayload, then ValidateFontData. Never asserts on data. Errors: Parse for
	// truncation or trailing bytes; Validation from ValidateFontData.
	[[nodiscard]] Result<FontData> DeserializeFontPayload(std::span<const std::byte> payload);

	// The complete cooked artifact (CookedHeader + payload) of `font`.
	[[nodiscard]] Buffer CookFont(const FontData& font, uint32_t importerVersion);

	// The font of a cooked artifact. Errors: as ReadCookedArtifact and DeserializeFontPayload.
	[[nodiscard]] Result<AssetRef<FontData>> LoadCookedFont(std::span<const std::byte> cooked);

}
