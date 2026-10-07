#include "TestsPCH.h"

#include "Engine/Asset/TextureData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Random.h"

#include <optional>

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

		// The error code of a failed result; nullopt for a success.
		template<typename T>
		std::optional<ErrorCode> GetErrorCode(const Result<T>& result)
		{
			if (result.has_value())
				return std::nullopt;
			return result.error().GetCode();
		}

		// The same texture with its base level only.
		TextureData MakeBaseLevel(uint32_t width, uint32_t height, TextureFormat format = TextureFormat::RGBA8Unorm)
		{
			TextureData texture = MakeTexture(width, height, format);
			texture.Pixels.resize(static_cast<size_t>(texture.Mips.front().Size));
			texture.Mips.resize(1);
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

		TEST_CASE("TextureData: the payload round-trips with a full mip chain")
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

		TEST_CASE("TextureData: the payload layout is the documented one")
		{
			const TextureData texture = MakeBaseLevel(2, 1, TextureFormat::R8Unorm);
			const Buffer payload = SerializeTexturePayload(texture);
			// uint8 Format, 3 reserved bytes, Width, Height, MipCount, one { Width, Height, uint64 Size }, 2 texels.
			REQUIRE(payload.size() == 4 + 4 + 4 + 4 + 16 + 2);
			CHECK(std::to_integer<uint8_t>(payload[0]) == 3);
			CHECK(std::to_integer<uint8_t>(payload[1]) == 0);
			CHECK(std::to_integer<uint8_t>(payload[4]) == 2);  // width, little-endian
			CHECK(std::to_integer<uint8_t>(payload[8]) == 1);  // height
			CHECK(std::to_integer<uint8_t>(payload[12]) == 1); // one mip level
			CHECK(std::to_integer<uint8_t>(payload[24]) == 2); // its size
			CHECK(payload[32] == texture.Pixels[0]);
			CHECK(payload[33] == texture.Pixels[1]);
		}

		TEST_CASE("TextureData: every mip level views its own texels")
		{
			const TextureData texture = MakeTexture(8, 2, TextureFormat::RGBA8Unorm);
			REQUIRE(texture.Mips.size() == 4);
			size_t total = 0;
			for (uint32_t level = 0; level < texture.Mips.size(); ++level)
			{
				const std::span<const std::byte> pixels = GetMipPixels(texture, level);
				CHECK(pixels.size() == texture.Mips[level].Size);
				CHECK(pixels.data() == texture.Pixels.data() + texture.Mips[level].Offset);
				total += pixels.size();
			}
			CHECK(total == texture.Pixels.size());

			const TextureData single = MakeBaseLevel(3, 5);
			REQUIRE(ValidateTextureData(single).has_value());
			CHECK(GetMipPixels(single, 0).size() == 3 * 5 * 4);
		}

		TEST_CASE("TextureData: invalid textures fail validation")
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

			TextureData wrongLevelSize = MakeTexture(4, 4);
			wrongLevelSize.Mips[1].Width = 3;
			CHECK(GetErrorCode(ValidateTextureData(wrongLevelSize)) == ErrorCode::Validation);
			TextureData gap = MakeTexture(4, 4);
			gap.Mips[2].Offset += 1;
			CHECK(GetErrorCode(ValidateTextureData(gap)) == ErrorCode::Validation);
		}

		TEST_CASE("TextureData: reserved bytes, truncation and trailing bytes fail with Parse")
		{
			const Buffer payload = SerializeTexturePayload(MakeTexture(4, 2));
			Buffer reserved = payload;
			reserved[2] = std::byte{ 1 };
			CHECK(GetErrorCode(DeserializeTexturePayload(reserved)) == ErrorCode::Parse);
			const Buffer truncated(payload.begin(), payload.end() - 1);
			CHECK(GetErrorCode(DeserializeTexturePayload(truncated)) == ErrorCode::Parse);
			Buffer trailing = payload;
			trailing.push_back(std::byte{ 0 });
			CHECK(GetErrorCode(DeserializeTexturePayload(trailing)) == ErrorCode::Parse);
			// A mip count far beyond the bytes present is rejected before anything is allocated.
			Buffer hugeCount = payload;
			hugeCount[12] = std::byte{ 0xFF };
			hugeCount[13] = std::byte{ 0xFF };
			hugeCount[14] = std::byte{ 0xFF };
			hugeCount[15] = std::byte{ 0x7F };
			CHECK(GetErrorCode(DeserializeTexturePayload(hugeCount)) == ErrorCode::Parse);

			// A cooked artifact of another type is not a texture.
			const Buffer other = WriteCookedArtifact(AssetType::Font, TextureData::FormatVersion, 1, payload);
			CHECK_FALSE(LoadCookedTexture(other).has_value());
		}

		TEST_CASE("TextureData: 10,000 seeded mutations of a payload never crash")
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
