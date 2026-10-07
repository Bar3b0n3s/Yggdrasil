#include "TestsPCH.h"

#include "Engine/Asset/TextureData.h"

#include "Engine/Core/Random.h"

namespace Engine {

	namespace {

		// A width x height RGBA8 texture with the full mip chain, texel values a function of position and level.
		TextureData MakeTexture(uint32_t width, uint32_t height, TextureFormat format = TextureFormat::RGBA8Srgb)
		{
			TextureData texture;
			texture.Format = format;
			texture.Width = width;
			texture.Height = height;
			const uint32_t bytesPerPixel = GetTextureBytesPerPixel(format);
			uint64_t offset = 0;
			for (uint32_t level = 0; level < ComputeFullMipCount(width, height); ++level)
			{
				const uint32_t mipWidth = std::max(1u, width >> level);
				const uint32_t mipHeight = std::max(1u, height >> level);
				const uint64_t size = static_cast<uint64_t>(mipWidth) * mipHeight * bytesPerPixel;
				texture.Mips.push_back({ .Width = mipWidth, .Height = mipHeight, .Offset = offset, .Size = size });
				for (uint64_t index = 0; index < size; ++index)
					texture.Pixels.push_back(static_cast<std::byte>((index * 13 + level * 71) & 0xFF));
				offset += size;
			}
			return texture;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("TextureData: mip counts and bytes per pixel follow the base size and format")
		{
			CHECK(ComputeFullMipCount(1, 1) == 1);
			CHECK(ComputeFullMipCount(8, 8) == 4);
			CHECK(ComputeFullMipCount(16, 3) == 5);
			CHECK(ComputeFullMipCount(16384, 1) == 15);
			CHECK(ComputeFullMipCount(0, 7) == 3);
			CHECK(ComputeFullMipCount(0, 0) == 0);
			CHECK(GetTextureBytesPerPixel(TextureFormat::RGBA8Unorm) == 4);
			CHECK(GetTextureBytesPerPixel(TextureFormat::RGBA8Srgb) == 4);
			CHECK(GetTextureBytesPerPixel(TextureFormat::R8Unorm) == 1);
			CHECK(TextureFormatToString(TextureFormat::RGBA8Srgb) == "RGBA8Srgb");
			CHECK(TextureFormatToString(static_cast<TextureFormat>(0)) == "Unknown");
		}

		TEST_CASE("TextureData: the payload round-trips with a full mip chain" * doctest::skip(true))
		{
			const TextureData texture = MakeTexture(16, 4);
			REQUIRE(ValidateTextureData(texture).has_value());
			const Buffer payload = SerializeTexturePayload(texture);
			Result<TextureData> read = DeserializeTexturePayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Format == texture.Format);
			CHECK(read->Width == 16);
			CHECK(read->Height == 4);
			CHECK(read->Mips == texture.Mips);
			CHECK(read->Pixels == texture.Pixels);
			CHECK(GetMipPixels(*read, 2).size() == 4 * 1 * 4);

			const Buffer cooked = CookTexture(texture, 1);
			Result<AssetRef<TextureData>> loaded = LoadCookedTexture(cooked);
			REQUIRE(loaded.has_value());
			CHECK(SerializeTexturePayload(**loaded) == payload);
		}

		TEST_CASE("TextureData: invalid textures fail validation" * doctest::skip(true))
		{
			TextureData empty;
			CHECK_FALSE(ValidateTextureData(empty).has_value());
			TextureData tooLarge = MakeTexture(1, 1);
			tooLarge.Width = MaxTextureDimension + 1;
			CHECK_FALSE(ValidateTextureData(tooLarge).has_value());
			TextureData partialChain = MakeTexture(8, 8);
			partialChain.Mips.pop_back();
			CHECK_FALSE(ValidateTextureData(partialChain).has_value());
			TextureData shortPixels = MakeTexture(8, 8);
			shortPixels.Pixels.pop_back();
			CHECK_FALSE(ValidateTextureData(shortPixels).has_value());
			TextureData unknownFormat = MakeTexture(2, 2);
			unknownFormat.Format = static_cast<TextureFormat>(9);
			CHECK_FALSE(ValidateTextureData(unknownFormat).has_value());
		}

		TEST_CASE("TextureData: 10,000 seeded mutations of a payload never crash" * doctest::skip(true))
		{
			const Buffer payload = SerializeTexturePayload(MakeTexture(8, 8, TextureFormat::R8Unorm));
			Random random(0x7E47);
			for (int iteration = 0; iteration < 10000; ++iteration)
			{
				Buffer mutated = payload;
				const size_t index = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()) - 1));
				mutated[index] = static_cast<std::byte>(random.NextU32() & 0xFF);
				if (random.NextBool(0.2))
					mutated.resize(static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()))));
				const Result<TextureData> read = DeserializeTexturePayload(mutated);
				if (read.has_value())
					CHECK(ValidateTextureData(*read).has_value());
			}
		}
	}

}
