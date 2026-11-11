#include "EnginePCH.h"
#include "Engine/Asset/FontData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace Engine {

	namespace Utils {

		// The largest atlas width or height (the texture limit, TextureData.h).
		static constexpr uint32_t MaxFontAtlasDimension = 16384;
		// The bytes of one glyph record in the payload: uint32 Codepoint, f32 Advance, four f32x2.
		static constexpr size_t FontGlyphRecordSize = 40;
		// The bytes of one kerning record: uint32 First, uint32 Second, f32 Advance.
		static constexpr size_t FontKerningRecordSize = 12;
		// Format 2 (C22.3): an indexed glyph's record (a glyph record and its uint32 GlyphIndex), a fallback's (uint64), and
		// the fixed fields after the glyph indices (UnitsPerEm, IndexedGlyphCount, ShapingSourceSize and FallbackCount).
		static constexpr size_t FontIndexedGlyphRecordSize = FontGlyphRecordSize + 4;
		static constexpr size_t FontFallbackRecordSize = 8;
		static constexpr size_t FontShapingFieldsSize = 4 * 4;
		// The most indexed glyphs a payload holds: one per glyph id of a font.
		static constexpr uint32_t MaxFontIndexedGlyphs = 65536;
		// The payload format of FormatVersion 1, which still loads.
		static constexpr uint16_t FontFormatVersion1 = 1;

		[[nodiscard]] static bool IsFinite(glm::vec2 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
		}

		[[nodiscard]] static bool IsInUnitSquare(glm::vec2 value)
		{
			return value.x >= 0.0f && value.x <= 1.0f && value.y >= 0.0f && value.y <= 1.0f;
		}

		[[nodiscard]] static Status ValidateGlyph(const FontGlyph& glyph)
		{
			if (!std::isfinite(glyph.Advance) || !IsFinite(glyph.PlaneMin) || !IsFinite(glyph.PlaneMax) || !IsFinite(glyph.AtlasMin)
				|| !IsFinite(glyph.AtlasMax))
			{
				return MakeError(ErrorCode::Validation, "glyph U+{:04X} has a non-finite metric", glyph.Codepoint);
			}
			if (glyph.PlaneMin.x > glyph.PlaneMax.x || glyph.PlaneMin.y > glyph.PlaneMax.y)
				return MakeError(ErrorCode::Validation, "glyph U+{:04X} has a plane rectangle whose minimum exceeds its maximum", glyph.Codepoint);
			if (!IsInUnitSquare(glyph.AtlasMin) || !IsInUnitSquare(glyph.AtlasMax) || glyph.AtlasMin.x > glyph.AtlasMax.x
				|| glyph.AtlasMin.y > glyph.AtlasMax.y)
			{
				return MakeError(ErrorCode::Validation, "glyph U+{:04X} has an atlas rectangle outside [0, 1] or inverted", glyph.Codepoint);
			}
			return {};
		}

		static void WriteVec2(BinaryWriter& writer, glm::vec2 value)
		{
			writer.WriteF32(value.x);
			writer.WriteF32(value.y);
		}

		[[nodiscard]] static Result<glm::vec2> ReadVec2(BinaryReader& reader)
		{
			ENGINE_TRY_ASSIGN(const float x, reader.ReadF32());
			ENGINE_TRY_ASSIGN(const float y, reader.ReadF32());
			return glm::vec2(x, y);
		}

		// A glyph record's fields (FontGlyphRecordSize bytes), GlyphIndex aside.
		static void WriteGlyphRecord(BinaryWriter& writer, const FontGlyph& glyph)
		{
			writer.WriteU32(glyph.Codepoint);
			writer.WriteF32(glyph.Advance);
			WriteVec2(writer, glyph.PlaneMin);
			WriteVec2(writer, glyph.PlaneMax);
			WriteVec2(writer, glyph.AtlasMin);
			WriteVec2(writer, glyph.AtlasMax);
		}

		[[nodiscard]] static Result<FontGlyph> ReadGlyphRecord(BinaryReader& reader)
		{
			FontGlyph glyph;
			ENGINE_TRY_ASSIGN(glyph.Codepoint, reader.ReadU32());
			ENGINE_TRY_ASSIGN(glyph.Advance, reader.ReadF32());
			ENGINE_TRY_ASSIGN(glyph.PlaneMin, ReadVec2(reader));
			ENGINE_TRY_ASSIGN(glyph.PlaneMax, ReadVec2(reader));
			ENGINE_TRY_ASSIGN(glyph.AtlasMin, ReadVec2(reader));
			ENGINE_TRY_ASSIGN(glyph.AtlasMax, ReadVec2(reader));
			return glyph;
		}

		// A count of `what` just read: at most `limit`, and its records of `recordSize` bytes within the bytes left (checked
		// before anything is allocated).
		[[nodiscard]] static Status CheckPayloadCount(const BinaryReader& reader, uint64_t count, uint64_t limit, uint64_t recordSize, std::string_view what)
		{
			if (count > limit)
				return MakeError(ErrorCode::Parse, "font payload: {} {} at offset {}, more than the {} a font may have", count, what, reader.GetPosition(), limit);
			if (count * recordSize > reader.GetRemaining())
			{
				return MakeError(ErrorCode::Parse, "font payload: {} {} need {} bytes, but {} follow at offset {}", count, what, count * recordSize,
					reader.GetRemaining(), reader.GetPosition());
			}
			return {};
		}

		// Format 2's checks of ValidateFontData (FontData.h).
		[[nodiscard]] static Status ValidateShapingData(const FontData& font)
		{
			const bool hasSource = !font.ShapingSource.empty();
			if (font.ShapingSource.size() > MaxFontShapingSourceSize)
			{
				return MakeError(ErrorCode::Validation, "the shaping source holds {} bytes, more than the {} a font may hold", font.ShapingSource.size(),
					MaxFontShapingSourceSize);
			}
			if (hasSource != (font.UnitsPerEm != 0) || hasSource != !font.IndexedGlyphs.empty())
			{
				return MakeError(ErrorCode::Validation,
					"the shaping data is incomplete: a shaping source of {} bytes, {} units per em and {} indexed glyphs (all three are set, or none)",
					font.ShapingSource.size(), font.UnitsPerEm, font.IndexedGlyphs.size());
			}
			if (hasSource && (font.UnitsPerEm < MinFontUnitsPerEm || font.UnitsPerEm > MaxFontUnitsPerEm))
				return MakeError(ErrorCode::Validation, "{} units per em is outside {} to {}", font.UnitsPerEm, MinFontUnitsPerEm, MaxFontUnitsPerEm);
			for (size_t index = 0; index < font.IndexedGlyphs.size(); ++index)
			{
				const FontGlyph& glyph = font.IndexedGlyphs[index];
				if (index > 0 && !(font.IndexedGlyphs[index - 1].GlyphIndex < glyph.GlyphIndex))
					return MakeError(ErrorCode::Validation, "indexed glyph {} at index {} is not sorted by glyph index or repeated", glyph.GlyphIndex, index);
				ENGINE_TRY(ValidateGlyph(glyph));
				if (glyph.Codepoint != 0)
				{
					const FontGlyph* codepointGlyph = FindGlyph(font, glyph.Codepoint);
					if (codepointGlyph == nullptr || codepointGlyph->GlyphIndex != glyph.GlyphIndex)
					{
						return MakeError(ErrorCode::Validation, "indexed glyph {} names U+{:04X}, which is not a glyph of the font with that index", glyph.GlyphIndex,
							glyph.Codepoint);
					}
				}
			}
			if (hasSource)
			{
				for (const FontGlyph& glyph : font.Glyphs)
				{
					const FontGlyph* indexed = FindGlyphByIndex(font, glyph.GlyphIndex);
					if (indexed == nullptr || indexed->Advance != glyph.Advance || indexed->PlaneMin != glyph.PlaneMin || indexed->PlaneMax != glyph.PlaneMax)
					{
						return MakeError(ErrorCode::Validation, "glyph U+{:04X} (glyph index {}) is not among the indexed glyphs with its metrics", glyph.Codepoint,
							glyph.GlyphIndex);
					}
				}
			}
			if (font.Fallbacks.size() > MaxFontFallbacks)
				return MakeError(ErrorCode::Validation, "{} fallbacks, more than the {} a font may name", font.Fallbacks.size(), MaxFontFallbacks);
			return {};
		}

	}

	const FontGlyph* FindGlyph(const FontData& font, uint32_t codepoint)
	{
		const auto found = std::ranges::lower_bound(font.Glyphs, codepoint, {}, &FontGlyph::Codepoint);
		if (found == font.Glyphs.end() || found->Codepoint != codepoint)
			return nullptr;
		return &*found;
	}

	const FontGlyph* FindGlyphByIndex(const FontData& font, uint32_t glyphIndex)
	{
		const auto found = std::ranges::lower_bound(font.IndexedGlyphs, glyphIndex, {}, &FontGlyph::GlyphIndex);
		if (found == font.IndexedGlyphs.end() || found->GlyphIndex != glyphIndex)
			return nullptr;
		return &*found;
	}

	Status ValidateFontData(const FontData& font)
	{
		if (!std::isfinite(font.PixelSize) || !(font.PixelSize > 0.0f))
			return MakeError(ErrorCode::Validation, "the pixel size must be positive and finite (got {})", font.PixelSize);
		if (!std::isfinite(font.Spread) || !(font.Spread > 0.0f))
			return MakeError(ErrorCode::Validation, "the distance field spread must be positive and finite (got {})", font.Spread);
		if (!std::isfinite(font.Ascent) || !std::isfinite(font.Descent) || !std::isfinite(font.LineGap))
			return MakeError(ErrorCode::Validation, "the vertical metrics must be finite");
		if (!(font.Ascent >= 0.0f) || !(font.Descent <= 0.0f))
			return MakeError(ErrorCode::Validation, "the ascent must be >= 0 and the descent <= 0 (got {} and {})", font.Ascent, font.Descent);
		if (font.AtlasWidth == 0 || font.AtlasHeight == 0 || font.AtlasWidth > Utils::MaxFontAtlasDimension
			|| font.AtlasHeight > Utils::MaxFontAtlasDimension)
		{
			return MakeError(ErrorCode::Validation, "an atlas of {}x{} pixels is outside 1 to {} pixels in each dimension", font.AtlasWidth,
				font.AtlasHeight, Utils::MaxFontAtlasDimension);
		}
		const uint64_t atlasSize = static_cast<uint64_t>(font.AtlasWidth) * font.AtlasHeight;
		if (font.AtlasPixels.size() != atlasSize)
		{
			return MakeError(ErrorCode::Validation, "the {}x{} atlas holds {} bytes instead of {}", font.AtlasWidth, font.AtlasHeight,
				font.AtlasPixels.size(), atlasSize);
		}

		for (size_t index = 0; index < font.Glyphs.size(); ++index)
		{
			const FontGlyph& glyph = font.Glyphs[index];
			if (index > 0 && !(font.Glyphs[index - 1].Codepoint < glyph.Codepoint))
				return MakeError(ErrorCode::Validation, "glyph U+{:04X} at index {} is not sorted by codepoint or repeated", glyph.Codepoint, index);
			ENGINE_TRY(Utils::ValidateGlyph(glyph));
		}

		for (size_t index = 0; index < font.Kerning.size(); ++index)
		{
			const FontKerningPair& pair = font.Kerning[index];
			if (index > 0)
			{
				const FontKerningPair& previous = font.Kerning[index - 1];
				if (!(std::pair(previous.First, previous.Second) < std::pair(pair.First, pair.Second)))
				{
					return MakeError(ErrorCode::Validation, "the kerning pair (U+{:04X}, U+{:04X}) is not sorted or repeated", pair.First,
						pair.Second);
				}
			}
			if (!std::isfinite(pair.Advance))
				return MakeError(ErrorCode::Validation, "the kerning pair (U+{:04X}, U+{:04X}) has a non-finite advance", pair.First, pair.Second);
			if (FindGlyph(font, pair.First) == nullptr || FindGlyph(font, pair.Second) == nullptr)
			{
				return MakeError(ErrorCode::Validation, "the kerning pair (U+{:04X}, U+{:04X}) names a glyph the font does not have", pair.First,
					pair.Second);
			}
		}
		ENGINE_TRY(Utils::ValidateShapingData(font));

		// The fonts of its chain (C22.3, Docs/Decisions/0033-m22-contract.md decision 42): what FontImporter cooks. ValidateShapingData
		// checks the count.
		if (const auto null = std::ranges::find(font.Fallbacks, AssetHandle{}); null != font.Fallbacks.end())
			return MakeError(ErrorCode::Validation, "fallback {} is the null handle", null - font.Fallbacks.begin());
		return {};
	}

	Buffer SerializeFontPayload(const FontData& font)
	{
		ENGINE_CORE_ASSERT(ValidateFontData(font).has_value(), "SerializeFontPayload needs a valid font");
		BinaryWriter writer;
		writer.WriteF32(font.PixelSize);
		writer.WriteF32(font.Spread);
		writer.WriteF32(font.Ascent);
		writer.WriteF32(font.Descent);
		writer.WriteF32(font.LineGap);
		writer.WriteU32(font.AtlasWidth);
		writer.WriteU32(font.AtlasHeight);
		writer.WriteU32(static_cast<uint32_t>(font.Glyphs.size()));
		writer.WriteU32(static_cast<uint32_t>(font.Kerning.size()));
		for (const FontGlyph& glyph : font.Glyphs)
			Utils::WriteGlyphRecord(writer, glyph);
		for (const FontKerningPair& pair : font.Kerning)
		{
			writer.WriteU32(pair.First);
			writer.WriteU32(pair.Second);
			writer.WriteF32(pair.Advance);
		}
		writer.WriteBytes(font.AtlasPixels);

		// Format 2 (C22.3).
		for (const FontGlyph& glyph : font.Glyphs)
			writer.WriteU32(glyph.GlyphIndex);
		writer.WriteU32(font.UnitsPerEm);
		writer.WriteU32(static_cast<uint32_t>(font.IndexedGlyphs.size()));
		for (const FontGlyph& glyph : font.IndexedGlyphs)
		{
			Utils::WriteGlyphRecord(writer, glyph);
			writer.WriteU32(glyph.GlyphIndex);
		}
		writer.WriteU32(static_cast<uint32_t>(font.ShapingSource.size()));
		writer.WriteBytes(font.ShapingSource);
		writer.WriteU32(static_cast<uint32_t>(font.Fallbacks.size()));
		for (const AssetHandle fallback : font.Fallbacks)
			writer.WriteU64(fallback.GetValue());
		return writer.TakeBuffer();
	}

	Result<FontData> DeserializeFontPayload(std::span<const std::byte> payload, uint16_t formatVersion)
	{
		if (formatVersion != Utils::FontFormatVersion1 && formatVersion != FontData::FormatVersion)
		{
			return MakeError(ErrorCode::UnsupportedVersion, "font payload format version {}; this build reads versions {} and {}", formatVersion,
				Utils::FontFormatVersion1, FontData::FormatVersion);
		}
		const bool hasFormat2 = formatVersion == FontData::FormatVersion;
		BinaryReader reader(payload);
		FontData font;
		ENGINE_TRY_ASSIGN(font.PixelSize, reader.ReadF32());
		ENGINE_TRY_ASSIGN(font.Spread, reader.ReadF32());
		ENGINE_TRY_ASSIGN(font.Ascent, reader.ReadF32());
		ENGINE_TRY_ASSIGN(font.Descent, reader.ReadF32());
		ENGINE_TRY_ASSIGN(font.LineGap, reader.ReadF32());
		ENGINE_TRY_ASSIGN(font.AtlasWidth, reader.ReadU32());
		ENGINE_TRY_ASSIGN(font.AtlasHeight, reader.ReadU32());
		ENGINE_TRY_ASSIGN(const uint32_t glyphCount, reader.ReadU32());
		ENGINE_TRY_ASSIGN(const uint32_t kerningCount, reader.ReadU32());

		// Every count is checked against the bytes left before anything is allocated (§6.8): format 1's records and atlas end
		// its payload, and format 2's fields follow them, the glyph indices and four counts at least.
		const uint64_t atlasSize = static_cast<uint64_t>(font.AtlasWidth) * font.AtlasHeight;
		const uint64_t recordBytes = static_cast<uint64_t>(glyphCount) * Utils::FontGlyphRecordSize
			+ static_cast<uint64_t>(kerningCount) * Utils::FontKerningRecordSize;
		const uint64_t format2Bytes = hasFormat2 ? static_cast<uint64_t>(glyphCount) * 4 + Utils::FontShapingFieldsSize : 0;
		const uint64_t remaining = reader.GetRemaining();
		if (recordBytes > remaining || atlasSize > remaining - recordBytes || format2Bytes > remaining - recordBytes - atlasSize
			|| (!hasFormat2 && atlasSize != remaining - recordBytes))
		{
			return MakeError(ErrorCode::Parse, "font payload: {} glyphs, {} kerning pairs and a {}x{} atlas need {} bytes, but {} follow at offset {}",
				glyphCount, kerningCount, font.AtlasWidth, font.AtlasHeight, recordBytes + atlasSize + format2Bytes, remaining, reader.GetPosition());
		}

		font.Glyphs.reserve(glyphCount);
		for (uint32_t index = 0; index < glyphCount; ++index)
		{
			ENGINE_TRY_ASSIGN(const FontGlyph glyph, Utils::ReadGlyphRecord(reader));
			font.Glyphs.push_back(glyph);
		}
		font.Kerning.reserve(kerningCount);
		for (uint32_t index = 0; index < kerningCount; ++index)
		{
			FontKerningPair pair;
			ENGINE_TRY_ASSIGN(pair.First, reader.ReadU32());
			ENGINE_TRY_ASSIGN(pair.Second, reader.ReadU32());
			ENGINE_TRY_ASSIGN(pair.Advance, reader.ReadF32());
			font.Kerning.push_back(pair);
		}
		ENGINE_TRY_ASSIGN(const std::span<const std::byte> atlas, reader.ReadBytes(static_cast<size_t>(atlasSize)));
		font.AtlasPixels.assign(atlas.begin(), atlas.end());

		if (hasFormat2)
		{
			for (FontGlyph& glyph : font.Glyphs)
			{
				ENGINE_TRY_ASSIGN(glyph.GlyphIndex, reader.ReadU32());
			}
			ENGINE_TRY_ASSIGN(font.UnitsPerEm, reader.ReadU32());
			ENGINE_TRY_ASSIGN(const uint32_t indexedCount, reader.ReadU32());
			ENGINE_TRY(Utils::CheckPayloadCount(reader, indexedCount, Utils::MaxFontIndexedGlyphs, Utils::FontIndexedGlyphRecordSize, "indexed glyphs"));
			font.IndexedGlyphs.reserve(indexedCount);
			for (uint32_t index = 0; index < indexedCount; ++index)
			{
				ENGINE_TRY_ASSIGN(FontGlyph glyph, Utils::ReadGlyphRecord(reader));
				ENGINE_TRY_ASSIGN(glyph.GlyphIndex, reader.ReadU32());
				font.IndexedGlyphs.push_back(glyph);
			}
			ENGINE_TRY_ASSIGN(const uint32_t sourceSize, reader.ReadU32());
			ENGINE_TRY(Utils::CheckPayloadCount(reader, sourceSize, MaxFontShapingSourceSize, 1, "shaping source bytes"));
			ENGINE_TRY_ASSIGN(const std::span<const std::byte> source, reader.ReadBytes(sourceSize));
			font.ShapingSource.assign(source.begin(), source.end());
			ENGINE_TRY_ASSIGN(const uint32_t fallbackCount, reader.ReadU32());
			ENGINE_TRY(Utils::CheckPayloadCount(reader, fallbackCount, MaxFontFallbacks, Utils::FontFallbackRecordSize, "fallbacks"));
			font.Fallbacks.reserve(fallbackCount);
			for (uint32_t index = 0; index < fallbackCount; ++index)
			{
				ENGINE_TRY_ASSIGN(const uint64_t fallback, reader.ReadU64());
				font.Fallbacks.push_back(AssetHandle(fallback));
			}
			if (reader.GetRemaining() != 0)
				return MakeError(ErrorCode::Parse, "font payload: {} trailing bytes at offset {}", reader.GetRemaining(), reader.GetPosition());
		}
		ENGINE_TRY(ValidateFontData(font));
		return font;
	}

	Buffer CookFont(const FontData& font, uint32_t importerVersion)
	{
		return WriteCookedArtifact(AssetType::Font, FontData::FormatVersion, importerVersion, SerializeFontPayload(font));
	}

	Result<AssetRef<FontData>> LoadCookedFont(std::span<const std::byte> cooked)
	{
		// Format 1 payloads still load (C22.3): the header's version, if it is format 1's, else this build's, which
		// ReadCookedArtifact refuses with UnsupportedVersion when it is neither.
		ENGINE_TRY_ASSIGN(const CookedArtifactView header, ReadCookedArtifact(cooked));
		const uint16_t formatVersion = header.Header.FormatVersion == Utils::FontFormatVersion1 ? Utils::FontFormatVersion1 : FontData::FormatVersion;
		ENGINE_TRY_ASSIGN(const CookedArtifactView view, ReadCookedArtifact(cooked, AssetType::Font, formatVersion));
		ENGINE_TRY_ASSIGN(FontData font, DeserializeFontPayload(view.Payload, formatVersion));
		return AssetRef<FontData>(CreateRef<FontData>(std::move(font)));
	}

}
