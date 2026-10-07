#include "EnginePCH.h"
#include "Engine/Asset/FontData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Engine {

	namespace Utils {

		// The largest atlas width or height (the texture limit, TextureData.h).
		static constexpr uint32_t MaxFontAtlasDimension = 16384;
		// The bytes of one glyph record in the payload: uint32 Codepoint, f32 Advance, four f32x2.
		static constexpr size_t FontGlyphRecordSize = 40;
		// The bytes of one kerning record: uint32 First, uint32 Second, f32 Advance.
		static constexpr size_t FontKerningRecordSize = 12;

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

	}

	const FontGlyph* FindGlyph(const FontData& font, uint32_t codepoint)
	{
		const auto found = std::ranges::lower_bound(font.Glyphs, codepoint, {}, &FontGlyph::Codepoint);
		if (found == font.Glyphs.end() || found->Codepoint != codepoint)
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
		{
			writer.WriteU32(glyph.Codepoint);
			writer.WriteF32(glyph.Advance);
			Utils::WriteVec2(writer, glyph.PlaneMin);
			Utils::WriteVec2(writer, glyph.PlaneMax);
			Utils::WriteVec2(writer, glyph.AtlasMin);
			Utils::WriteVec2(writer, glyph.AtlasMax);
		}
		for (const FontKerningPair& pair : font.Kerning)
		{
			writer.WriteU32(pair.First);
			writer.WriteU32(pair.Second);
			writer.WriteF32(pair.Advance);
		}
		writer.WriteBytes(font.AtlasPixels);
		return writer.TakeBuffer();
	}

	Result<FontData> DeserializeFontPayload(std::span<const std::byte> payload)
	{
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

		// Every count is checked against the bytes left before anything is allocated (§6.8).
		const uint64_t atlasSize = static_cast<uint64_t>(font.AtlasWidth) * font.AtlasHeight;
		const uint64_t recordBytes = static_cast<uint64_t>(glyphCount) * Utils::FontGlyphRecordSize
			+ static_cast<uint64_t>(kerningCount) * Utils::FontKerningRecordSize;
		if (recordBytes > reader.GetRemaining() || atlasSize != reader.GetRemaining() - recordBytes)
		{
			return MakeError(ErrorCode::Parse, "font payload: {} glyphs, {} kerning pairs and a {}x{} atlas need {} bytes, but {} follow at offset {}",
				glyphCount, kerningCount, font.AtlasWidth, font.AtlasHeight, recordBytes + atlasSize, reader.GetRemaining(), reader.GetPosition());
		}

		font.Glyphs.reserve(glyphCount);
		for (uint32_t index = 0; index < glyphCount; ++index)
		{
			FontGlyph glyph;
			ENGINE_TRY_ASSIGN(glyph.Codepoint, reader.ReadU32());
			ENGINE_TRY_ASSIGN(glyph.Advance, reader.ReadF32());
			ENGINE_TRY_ASSIGN(glyph.PlaneMin, Utils::ReadVec2(reader));
			ENGINE_TRY_ASSIGN(glyph.PlaneMax, Utils::ReadVec2(reader));
			ENGINE_TRY_ASSIGN(glyph.AtlasMin, Utils::ReadVec2(reader));
			ENGINE_TRY_ASSIGN(glyph.AtlasMax, Utils::ReadVec2(reader));
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
		ENGINE_TRY(ValidateFontData(font));
		return font;
	}

	Buffer CookFont(const FontData& font, uint32_t importerVersion)
	{
		return WriteCookedArtifact(AssetType::Font, FontData::FormatVersion, importerVersion, SerializeFontPayload(font));
	}

	Result<AssetRef<FontData>> LoadCookedFont(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView view, ReadCookedArtifact(cooked, AssetType::Font, FontData::FormatVersion));
		ENGINE_TRY_ASSIGN(FontData font, DeserializeFontPayload(view.Payload));
		return AssetRef<FontData>(CreateRef<FontData>(std::move(font)));
	}

}
