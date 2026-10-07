#include "TestsPCH.h"

#include "Engine/Asset/FontData.h"

#include "Engine/Core/Random.h"

#include <limits>
#include <optional>

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

		TEST_CASE("FontData: the payload layout is the documented one")
		{
			const FontData font = MakeFont();
			const Buffer payload = SerializeFontPayload(font);
			// Five f32, four uint32, two 40-byte glyphs, one 12-byte kerning pair, then the 4 x 2 atlas.
			CHECK(payload.size() == 5 * 4 + 4 * 4 + 2 * 40 + 12 + 8);
			CHECK(std::to_integer<uint8_t>(payload[20]) == 4); // AtlasWidth
			CHECK(std::to_integer<uint8_t>(payload[28]) == 2); // GlyphCount
			CHECK(std::to_integer<uint8_t>(payload[36]) == 'A');
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

		TEST_CASE("FontData: 10,000 seeded mutations of a payload never crash")
		{
			const Buffer payload = SerializeFontPayload(MakeFont());
			Random random(0xF047);
			for (int iteration = 0; iteration < 10000; ++iteration)
			{
				Buffer mutated = payload;
				const size_t index = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()) - 1));
				mutated[index] = static_cast<std::byte>(random.NextU32() & 0xFF);
				if (random.NextBool(0.2))
					mutated.resize(static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()))));
				const Result<FontData> read = DeserializeFontPayload(mutated);
				if (read.has_value())
					CHECK(ValidateFontData(*read).has_value());
			}
		}
	}

}
