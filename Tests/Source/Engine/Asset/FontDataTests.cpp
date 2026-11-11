#include "TestsPCH.h"

#include "Engine/Asset/FontData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/AssetPipeline/Importers/FontImporter.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Random.h"
#include "Engine/Renderer/TextMeasurement.h"
#include "Support/Fuzz.h"
#include "Support/TestData.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		FontData MakeFont()
		{
			FontData font;
			font.Ascent = 0.9f;
			font.Descent = -0.25f;
			font.LineGap = 0.05f;
			font.AtlasWidth = 4;
			font.AtlasHeight = 2;
			font.AtlasPixels = Buffer(8, std::byte{ 128 });
			font.Glyphs = {
				{ .Codepoint = 'A', .Advance = 0.6f, .PlaneMin = { 0.0f, 0.0f }, .PlaneMax = { 0.5f, 0.7f }, .AtlasMin = { 0.0f, 0.0f }, .AtlasMax = { 0.5f, 1.0f } },
				{ .Codepoint = 'V', .Advance = 0.6f, .PlaneMin = { 0.0f, 0.0f }, .PlaneMax = { 0.5f, 0.7f }, .AtlasMin = { 0.5f, 0.0f }, .AtlasMax = { 1.0f, 1.0f } },
			};
			font.Kerning = { { .First = 'A', .Second = 'V', .Advance = -0.08f } };
			return font;
		}

		// MakeFont with format 2's members (C22.3): glyph indices, an indexed ligature glyph only shaping reaches, a shaping
		// source and two fallbacks.
		FontData MakeShapedFont()
		{
			FontData font = MakeFont();
			font.Glyphs[0].GlyphIndex = 36;
			font.Glyphs[1].GlyphIndex = 57;
			font.UnitsPerEm = 2048;
			font.IndexedGlyphs = { font.Glyphs[0],
				FontGlyph{ .Codepoint = 0, .Advance = 1.1f, .PlaneMax = { 1.0f, 0.7f }, .AtlasMin = { 0.0f, 0.0f }, .AtlasMax = { 1.0f, 1.0f }, .GlyphIndex = 40 },
				font.Glyphs[1] };
			font.ShapingSource = Buffer(64, std::byte{ 7 });
			font.Fallbacks = { AssetHandle(0xFA11), AssetHandle(0xBAC4) };
			return font;
		}

		// The bytes of format 2's members of a font with `glyphCount` glyphs and no shaping data: each glyph's index, then
		// UnitsPerEm, IndexedGlyphCount, ShapingSourceSize and FallbackCount, all uint32.
		size_t GetPlainFormat2TailSize(size_t glyphCount)
		{
			return glyphCount * 4 + 4 * 4;
		}

		uint32_t ReadPayloadU32(std::span<const std::byte> payload, size_t offset)
		{
			uint32_t value = 0;
			for (size_t index = 0; index < 4; ++index)
				value |= std::to_integer<uint32_t>(payload[offset + index]) << (8 * index);
			return value;
		}

		template<typename T>
		std::optional<ErrorCode> GetErrorCode(const Result<T>& result)
		{
			if (result.has_value())
				return std::nullopt;
			return result.error().GetCode();
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("FontData: the payload round-trips")
		{
			const FontData font = MakeFont();
			REQUIRE(ValidateFontData(font).has_value());
			const Buffer payload = SerializeFontPayload(font);
			Result<FontData> read = DeserializeFontPayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Glyphs == font.Glyphs);
			CHECK(read->Kerning == font.Kerning);
			CHECK(read->AtlasPixels == font.AtlasPixels);
			CHECK(read->PixelSize == 48.0f);
			CHECK(read->Spread == 8.0f);
			CHECK(read->Ascent == font.Ascent);
			CHECK(read->Descent == font.Descent);
			CHECK(read->LineGap == font.LineGap);
			Result<AssetRef<FontData>> loaded = LoadCookedFont(CookFont(font, 1));
			REQUIRE(loaded.has_value());
			CHECK(SerializeFontPayload(**loaded) == payload);
		}

		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 44; stream S): format 2's members round-trip, and an indexed glyph
		// is found by its glyph index.
		TEST_CASE("FontData: a format 2 payload round-trips glyph indices, indexed glyphs, the shaping source and fallbacks")
		{
			FontData font = MakeFont();
			font.Glyphs[0].GlyphIndex = 36;
			font.Glyphs[1].GlyphIndex = 57;
			font.UnitsPerEm = 2048;
			// A ligature glyph only shaping reaches (Codepoint 0) between the two codepoint glyphs by index.
			font.IndexedGlyphs = { font.Glyphs[0],
				FontGlyph{ .Codepoint = 0, .Advance = 1.1f, .PlaneMax = { 1.0f, 0.7f }, .AtlasMin = { 0.0f, 0.0f }, .AtlasMax = { 1.0f, 1.0f }, .GlyphIndex = 40 },
				font.Glyphs[1] };
			font.ShapingSource = Buffer(64, std::byte{ 7 });
			font.Fallbacks = { AssetHandle(0xFA11), AssetHandle(0xBAC4) };
			REQUIRE(ValidateFontData(font).has_value());
			CHECK(FontData::FormatVersion == 2u);
			Result<FontData> read = DeserializeFontPayload(SerializeFontPayload(font));
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Glyphs == font.Glyphs);
			CHECK(read->IndexedGlyphs == font.IndexedGlyphs);
			CHECK(read->UnitsPerEm == 2048u);
			CHECK(read->ShapingSource == font.ShapingSource);
			CHECK(read->Fallbacks == font.Fallbacks);

			// FindGlyphByIndex binary-searches IndexedGlyphs; validation refuses an unsorted list, a codepoint glyph missing from
			// it and more than MaxFontFallbacks fallbacks.
			REQUIRE(FindGlyphByIndex(*read, 40) != nullptr);
			CHECK(FindGlyphByIndex(*read, 40)->Advance == 1.1f);
			CHECK(FindGlyphByIndex(*read, 57)->Codepoint == static_cast<uint32_t>('V'));
			CHECK(FindGlyphByIndex(*read, 41) == nullptr);
			FontData unsorted = font;
			std::swap(unsorted.IndexedGlyphs[0], unsorted.IndexedGlyphs[2]);
			CHECK(GetErrorCode(ValidateFontData(unsorted)) == ErrorCode::Validation);
			FontData missing = font;
			missing.IndexedGlyphs.erase(missing.IndexedGlyphs.begin());
			CHECK(GetErrorCode(ValidateFontData(missing)) == ErrorCode::Validation);
			FontData many = font;
			many.Fallbacks.assign(MaxFontFallbacks + 1, AssetHandle(0x1));
			CHECK(GetErrorCode(ValidateFontData(many)) == ErrorCode::Validation);
		}

		TEST_CASE("FontData: the payload layout is the documented one")
		{
			const FontData font = MakeFont();
			const Buffer payload = SerializeFontPayload(font);
			// Five f32, four uint32, two 40-byte glyphs, one 12-byte kerning pair, then the 4 x 2 atlas; then format 2's two glyph
			// indices and four counts (C22.3).
			CHECK(payload.size() == 5 * 4 + 4 * 4 + 2 * 40 + 12 + 8 + 2 * 4 + 4 * 4);
			CHECK(std::to_integer<uint8_t>(payload[20]) == 4); // AtlasWidth
			CHECK(std::to_integer<uint8_t>(payload[28]) == 2); // GlyphCount
			CHECK(std::to_integer<uint8_t>(payload[36]) == 'A');

			// Format 2 after the atlas (offset 136): the glyph indices, UnitsPerEm, the indexed glyphs (44 bytes each: a glyph
			// record and its index), the shaping source's size and bytes, and the fallbacks' count and handles.
			const Buffer shaped = SerializeFontPayload(MakeShapedFont());
			CHECK(shaped.size() == 136 + 2 * 4 + 4 + 4 + 3 * 44 + 4 + 64 + 4 + 2 * 8);
			CHECK(ReadPayloadU32(shaped, 136) == 36u);
			CHECK(ReadPayloadU32(shaped, 140) == 57u);
			CHECK(ReadPayloadU32(shaped, 144) == 2048u); // UnitsPerEm
			CHECK(ReadPayloadU32(shaped, 148) == 3u);    // IndexedGlyphCount
			CHECK(ReadPayloadU32(shaped, 152) == static_cast<uint32_t>('A'));
			CHECK(ReadPayloadU32(shaped, 152 + 40) == 36u);             // A's GlyphIndex
			CHECK(ReadPayloadU32(shaped, 152 + 44 + 40) == 40u);        // the ligature's
			CHECK(ReadPayloadU32(shaped, 152 + 3 * 44) == 64u);         // ShapingSourceSize
			CHECK(ReadPayloadU32(shaped, 152 + 3 * 44 + 4 + 64) == 2u); // FallbackCount
			CHECK(ReadPayloadU32(shaped, 152 + 3 * 44 + 4 + 64 + 4) == 0xFA11u);
		}

		// C22.3 (decision 44): a cooked font of format 1, cached or packed before FontImporter version 5, still loads, without
		// shaping data or fallbacks; other format versions are refused.
		TEST_CASE("FontData: a format 1 payload still loads, without shaping data or fallbacks")
		{
			const FontData font = MakeFont();
			const Buffer format2 = SerializeFontPayload(font);
			// Format 1's payload is format 2's without its members after the atlas.
			const Buffer format1(format2.begin(), format2.end() - static_cast<std::ptrdiff_t>(GetPlainFormat2TailSize(font.Glyphs.size())));
			Result<FontData> read = DeserializeFontPayload(format1, 1);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Glyphs == font.Glyphs);
			CHECK(read->Kerning == font.Kerning);
			CHECK(read->AtlasPixels == font.AtlasPixels);
			CHECK(read->IndexedGlyphs.empty());
			CHECK(read->ShapingSource.empty());
			CHECK(read->UnitsPerEm == 0u);
			CHECK(read->Fallbacks.empty());
			// Each format reads only its own layout.
			CHECK(GetErrorCode(DeserializeFontPayload(format2, 1)) == ErrorCode::Parse);
			CHECK(GetErrorCode(DeserializeFontPayload(format1, 2)) == ErrorCode::Parse);
			CHECK(GetErrorCode(DeserializeFontPayload(format2, 3)) == ErrorCode::UnsupportedVersion);

			// A cooked artifact of format 1 loads; one of another version, or of another type, is refused.
			Result<AssetRef<FontData>> loaded = LoadCookedFont(WriteCookedArtifact(AssetType::Font, 1, 4, format1));
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			CHECK((*loaded)->Glyphs == font.Glyphs);
			CHECK(GetErrorCode(LoadCookedFont(WriteCookedArtifact(AssetType::Font, 3, 5, format2))) == ErrorCode::UnsupportedVersion);
			CHECK(GetErrorCode(LoadCookedFont(WriteCookedArtifact(AssetType::Texture, 2, 5, format2))) == ErrorCode::Validation);
		}

		// C22.3: format 2's shaping data is whole or absent, its units per em in range, its shaping source bounded, and an
		// indexed glyph that names a codepoint is that codepoint's glyph.
		TEST_CASE("FontData: format 2 shaping data is whole, in range and consistent with the codepoint glyphs")
		{
			const FontData shaped = MakeShapedFont();
			REQUIRE(ValidateFontData(shaped).has_value());
			FontData noSource = shaped;
			noSource.ShapingSource.clear();
			CHECK(GetErrorCode(ValidateFontData(noSource)) == ErrorCode::Validation);
			FontData noUnits = shaped;
			noUnits.UnitsPerEm = 0;
			CHECK(GetErrorCode(ValidateFontData(noUnits)) == ErrorCode::Validation);
			FontData noIndexed = shaped;
			noIndexed.IndexedGlyphs.clear();
			CHECK(GetErrorCode(ValidateFontData(noIndexed)) == ErrorCode::Validation);
			for (const uint32_t unitsPerEm : { MinFontUnitsPerEm - 1, MaxFontUnitsPerEm + 1 })
			{
				CAPTURE(unitsPerEm);
				FontData outside = shaped;
				outside.UnitsPerEm = unitsPerEm;
				CHECK(GetErrorCode(ValidateFontData(outside)) == ErrorCode::Validation);
			}
			FontData large = shaped;
			large.ShapingSource.resize(MaxFontShapingSourceSize + 1);
			CHECK(GetErrorCode(ValidateFontData(large)) == ErrorCode::Validation);
			// An indexed glyph naming a codepoint whose glyph has another index, and a codepoint glyph whose indexed glyph's
			// metrics differ.
			FontData wrongCodepoint = shaped;
			wrongCodepoint.IndexedGlyphs[1].Codepoint = 'V';
			CHECK(GetErrorCode(ValidateFontData(wrongCodepoint)) == ErrorCode::Validation);
			FontData wrongMetrics = shaped;
			wrongMetrics.IndexedGlyphs[2].Advance = 0.7f;
			CHECK(GetErrorCode(ValidateFontData(wrongMetrics)) == ErrorCode::Validation);
			// The reader refuses a count beyond its limit before allocating: a shaping source beyond the limit, nine
			// fallbacks.
			const Buffer payload = SerializeFontPayload(shaped);
			constexpr size_t SourceSizeOffset = 152 + 3 * 44;
			Buffer hugeSource = payload;
			hugeSource[SourceSizeOffset + 3] = std::byte{ 0x7F };
			CHECK(GetErrorCode(DeserializeFontPayload(hugeSource)) == ErrorCode::Parse);
			Buffer nineFallbacks = payload;
			nineFallbacks[SourceSizeOffset + 4 + 64] = std::byte{ 9 };
			CHECK(GetErrorCode(DeserializeFontPayload(nineFallbacks)) == ErrorCode::Parse);
		}

		TEST_CASE("FontData: glyphs are found by codepoint and invalid fonts fail validation")
		{
			const FontData font = MakeFont();
			REQUIRE(FindGlyph(font, 'V') != nullptr);
			CHECK(FindGlyph(font, 'V')->AtlasMin.x == 0.5f);
			CHECK(FindGlyph(font, 'B') == nullptr);
			CHECK(FindGlyph(FontData(), 'A') == nullptr);

			FontData unsorted = font;
			std::swap(unsorted.Glyphs[0], unsorted.Glyphs[1]);
			CHECK_FALSE(ValidateFontData(unsorted).has_value());
			FontData orphanKerning = font;
			orphanKerning.Kerning.push_back({ .First = 'A', .Second = 'Z', .Advance = 0.0f });
			CHECK_FALSE(ValidateFontData(orphanKerning).has_value());
			FontData shortAtlas = font;
			shortAtlas.AtlasPixels.pop_back();
			CHECK_FALSE(ValidateFontData(shortAtlas).has_value());

			FontData outside = font;
			outside.Glyphs[1].AtlasMax.x = 1.5f;
			CHECK(GetErrorCode(ValidateFontData(outside)) == ErrorCode::Validation);
			FontData nonFinite = font;
			nonFinite.Glyphs[0].Advance = std::numeric_limits<float>::infinity();
			CHECK(GetErrorCode(ValidateFontData(nonFinite)) == ErrorCode::Validation);
			FontData inverted = font;
			inverted.Descent = 0.1f;
			CHECK(GetErrorCode(ValidateFontData(inverted)) == ErrorCode::Validation);
			FontData noSpread = font;
			noSpread.Spread = 0.0f;
			CHECK(GetErrorCode(ValidateFontData(noSpread)) == ErrorCode::Validation);
			FontData repeatedPair = font;
			repeatedPair.Kerning.push_back(repeatedPair.Kerning.front());
			CHECK(GetErrorCode(ValidateFontData(repeatedPair)) == ErrorCode::Validation);
		}

		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 42; stream F): a font's chain as FontImporter cooks it, at most
		// MaxFontFallbacks fallbacks (a name may repeat: the chain takes each font once), none null.
		TEST_CASE("FontData: validation accepts at most MaxFontFallbacks fallbacks and refuses a null one")
		{
			FontData font = MakeFont();
			font.Fallbacks.assign(MaxFontFallbacks, AssetHandle(0xFA11));
			CHECK(ValidateFontData(font).has_value());
			FontData many = font;
			many.Fallbacks.push_back(AssetHandle(0xBAC4));
			CHECK(GetErrorCode(ValidateFontData(many)) == ErrorCode::Validation);
			FontData null = MakeFont();
			null.Fallbacks = { AssetHandle(0xFA11), AssetHandle{} };
			CHECK(GetErrorCode(ValidateFontData(null)) == ErrorCode::Validation);
		}

		TEST_CASE("FontData: truncation and trailing bytes fail with Parse")
		{
			const Buffer payload = SerializeFontPayload(MakeFont());
			const Buffer truncated(payload.begin(), payload.end() - 1);
			CHECK(GetErrorCode(DeserializeFontPayload(truncated)) == ErrorCode::Parse);
			Buffer trailing = payload;
			trailing.push_back(std::byte{ 0 });
			CHECK(GetErrorCode(DeserializeFontPayload(trailing)) == ErrorCode::Parse);
			// A glyph count far beyond the bytes present is rejected before anything is allocated.
			Buffer hugeCount = payload;
			hugeCount[28] = std::byte{ 0xFF };
			hugeCount[29] = std::byte{ 0xFF };
			hugeCount[30] = std::byte{ 0xFF };
			hugeCount[31] = std::byte{ 0xFF };
			CHECK(GetErrorCode(DeserializeFontPayload(hugeCount)) == ErrorCode::Parse);
		}

		TEST_CASE("FontData: 10,000 seeded mutations of a payload never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// The seeds, by index: a format 2 payload without shaping data; a real one with it (C22.3, W22.3's review): the
			// Hebrew fixture's letters and points baked by FontImporter version 5 (glyph indices, units per em and a shaping
			// subset that keeps its GSUB and its GPOS, which places the points), with two fallbacks; and a format 1 payload,
			// read as format 1.
			const FontData font = MakeFont();
			const Buffer plain = SerializeFontPayload(font);
			const Buffer format1(plain.begin(), plain.end() - static_cast<std::ptrdiff_t>(GetPlainFormat2TailSize(font.Glyphs.size())));
			const Result<Buffer> hebrewSource = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Fonts") / "NotoSansHebrew-Regular.ttf");
			REQUIRE_MESSAGE(hebrewSource.has_value(), hebrewSource.error().ToString());
			FontBakeReport report;
			Result<FontData> hebrew = FontImporter::Bake(*hebrewSource,
				FontImportSettings{ .PixelSize = 16.0f, .Spread = 2.0f, .Ranges = { FontCharacterRange{ .First = 0x0591, .Last = 0x05F4 } } },
				"NotoSansHebrew-Regular.ttf", report);
			REQUIRE_MESSAGE(hebrew.has_value(), hebrew.error().ToString());
			REQUIRE_FALSE(hebrew->ShapingSource.empty());
			REQUIRE_FALSE(hebrew->IndexedGlyphs.empty());
			hebrew->Fallbacks = { AssetHandle(0xFA11), AssetHandle(0xBAC4) };
			const std::array<Buffer, 3> seeds = { plain, SerializeFontPayload(*hebrew), format1 };
			// Every font the reader accepts measures a mutated text (MeasureText and FindTextCaret, shaped through HarfBuzz and
			// SheenBidi when the font kept its shaping data, so they see an accepted but damaged subset, as from a damaged
			// pak): "AV", pointed Hebrew, digits and mirrored parentheses, in each base direction and alignment, wrapped.
			const std::string text = "AV \xD7\xA9\xD7\x81\xD6\xB8\xD7\x9C\xD7\x95\xD6\xB9\xD7\x9D 12 (\xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D)";
			const Test::FuzzRun run = Test::RunFuzzCase(0xF047, 10000, [&seeds, &text](Random& random, uint64_t index)
			{
				const size_t seed = index % seeds.size();
				Buffer mutated = seeds[seed];
				Test::MutateBytes(mutated, random);
				// A payload the reader accepts is a valid font.
				const Result<FontData> read = DeserializeFontPayload(mutated, seed == 2 ? uint16_t{ 1 } : FontData::FormatVersion);
				if (!read.has_value())
					return false;
				CHECK(ValidateFontData(*read).has_value());
				std::string measuredText = text;
				Test::MutateText(measuredText, random);
				const std::array<const FontData*, 1> chain = { &*read };
				constexpr std::array<RenderTextAlignment, 3> Alignments = { RenderTextAlignment::Left, RenderTextAlignment::Center, RenderTextAlignment::Right };
				constexpr std::array<TextDirection, 3> Directions = { TextDirection::Auto, TextDirection::LeftToRight, TextDirection::RightToLeft };
				const TextMeasurement measured = MeasureText(chain, measuredText,
					{ .Size = 16.0f,
						.LineSpacing = 1.25f,
						.Wrap = TextWrap::Word,
						.MaxWidth = 60.0f,
						.Alignment = Alignments[(index / 3) % Alignments.size()],
						.Direction = Directions[(index / 9) % Directions.size()] });
				const auto size = static_cast<uint32_t>(measuredText.size());
				const bool glyphsInText = std::ranges::all_of(measured.Glyphs, [size](const TextMeasuredGlyph& glyph)
				{
					return glyph.Font == 0 && glyph.ByteOffset < size;
				});
				const bool clustersInText = std::ranges::all_of(measured.Clusters, [size, &measured](const TextMeasuredCluster& cluster)
				{
					return cluster.ByteBegin < cluster.ByteEnd && cluster.ByteEnd <= size && cluster.Line < measured.Lines.size();
				});
				const TextCaret caret = FindTextCaret(measured, size / 2);
				CHECK((glyphsInText && clustersInText && caret.Line < std::max<size_t>(1, measured.Lines.size())));
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
				CHECK(run.Counts.Rejected > 0);
		}
	}

}
