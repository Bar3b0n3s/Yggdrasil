#include "TestsPCH.h"

#include "Engine/Asset/BuiltinTextures.h"

#include "Engine/Core/DetMath.h"
#include "Engine/Core/Hash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("BuiltinTextures: each texture is named after its engine path and other values are Unknown")
		{
			CHECK(BuiltinTextureToString(BuiltinTexture::White) == "White");
			CHECK(BuiltinTextureToString(BuiltinTexture::FlatNormal) == "FlatNormal");
			CHECK(BuiltinTextureToString(BuiltinTexture::Missing) == "Missing");
			CHECK(BuiltinTextureToString(static_cast<BuiltinTexture>(99)) == "Unknown");
		}

		TEST_CASE("BuiltinTextures: textures have their documented sizes, formats and texels")
		{
			const TextureData white = GenerateBuiltinTexture(BuiltinTexture::White);
			REQUIRE(ValidateTextureData(white).has_value());
			CHECK(white.Format == TextureFormat::RGBA8Srgb);
			CHECK(white.Width == 4);
			CHECK(std::ranges::all_of(GetMipPixels(white, 0), [](std::byte value)
			{
				return value == std::byte{ 255 };
			}));

			const TextureData flat = GenerateBuiltinTexture(BuiltinTexture::FlatNormal);
			REQUIRE(ValidateTextureData(flat).has_value());
			CHECK(flat.Format == TextureFormat::RGBA8Unorm);
			const std::span<const std::byte> texel = GetMipPixels(flat, 0).first(4);
			CHECK(texel[0] == std::byte{ 128 });
			CHECK(texel[1] == std::byte{ 128 });
			CHECK(texel[2] == std::byte{ 255 });
			CHECK(texel[3] == std::byte{ 255 });

			for (const BuiltinTexture checker : { BuiltinTexture::Checker, BuiltinTexture::Missing })
			{
				CAPTURE(std::string(BuiltinTextureToString(checker)));
				const TextureData texture = GenerateBuiltinTexture(checker);
				REQUIRE(ValidateTextureData(texture).has_value());
				CHECK(texture.Width == 64);
				CHECK(texture.Height == 64);
				CHECK(texture.Mips.size() == ComputeFullMipCount(64, 64));
				CHECK(SerializeTexturePayload(texture) == SerializeTexturePayload(GenerateBuiltinTexture(checker)));
			}
			// Missing alternates magenta and black squares of 8x8 texels.
			const TextureData missing = GenerateBuiltinTexture(BuiltinTexture::Missing);
			const std::span<const std::byte> base = GetMipPixels(missing, 0);
			CHECK(base[0] == std::byte{ 255 });
			CHECK(base[1] == std::byte{ 0 });
			CHECK(base[2] == std::byte{ 255 });
			CHECK(base[8 * 4] == std::byte{ 0 });
		}

		TEST_CASE("BuiltinTextures: the generated texels match the hashes recorded in every configuration")
		{
			// Integer arithmetic and a fixed table only, so the recorded hashes hold in every configuration and on every platform.
			constexpr std::array<BuiltinTexture, 5> Textures = { BuiltinTexture::White, BuiltinTexture::Black, BuiltinTexture::FlatNormal,
				BuiltinTexture::Checker, BuiltinTexture::Missing };
			constexpr std::array<uint64_t, 5> RecordedHashes = { 0x2e54fae2959dbb84ull, 0x088910b435c92dc1ull, 0xee5a85739c397f1cull,
				0xc4e010a786d84db6ull, 0x959a5dac370947e5ull };
			for (size_t index = 0; index < Textures.size(); ++index)
			{
				CAPTURE(std::string(BuiltinTextureToString(Textures[index])));
				const uint64_t hash = XXH64(GenerateBuiltinTexture(Textures[index]).Pixels);
				CAPTURE(std::format("{:016x}", hash));
				CHECK(hash == RecordedHashes[index]);
			}
		}

		TEST_CASE("BuiltinTextures: the solid textures are one opaque colour and one level")
		{
			for (const BuiltinTexture solid : { BuiltinTexture::White, BuiltinTexture::Black, BuiltinTexture::FlatNormal })
			{
				CAPTURE(std::string(BuiltinTextureToString(solid)));
				const TextureData texture = GenerateBuiltinTexture(solid);
				REQUIRE(ValidateTextureData(texture).has_value());
				CHECK(texture.Width == 4);
				CHECK(texture.Height == 4);
				REQUIRE(texture.Mips.size() == 1);
				const std::span<const std::byte> pixels = GetMipPixels(texture, 0);
				REQUIRE(pixels.size() == 4 * 4 * 4);
				for (size_t offset = 0; offset < pixels.size(); offset += 4)
				{
					CHECK(std::ranges::equal(pixels.subspan(offset, 4), pixels.first(4)));
					CHECK(pixels[offset + 3] == std::byte{ 255 });
				}
			}
			const TextureData black = GenerateBuiltinTexture(BuiltinTexture::Black);
			CHECK(black.Format == TextureFormat::RGBA8Srgb);
			CHECK(GetMipPixels(black, 0)[0] == std::byte{ 0 });
		}

		TEST_CASE("BuiltinTextures: checker mips are box filtered in linear light")
		{
			// The sRGB transfer function, here with DetMath (BuiltinTextures.cpp uses a fixed table of the same function).
			const auto decode = [](double value)
			{
				const double c = value / 255.0;
				return c <= 0.04045 ? c / 12.92 : DetMath::Pow((c + 0.055) / 1.055, 2.4);
			};
			const auto encode = [](double linear)
			{
				const double c = linear <= 0.0031308 ? linear * 12.92 : 1.055 * DetMath::Pow(linear, 1.0 / 2.4) - 0.055;
				return c * 255.0;
			};
			const auto texel = [](const TextureData& texture, uint32_t level, uint32_t x, uint32_t y)
			{
				return GetMipPixels(texture, level).subspan((static_cast<size_t>(y) * texture.Mips[level].Width + x) * 4, 4);
			};

			const TextureData checker = GenerateBuiltinTexture(BuiltinTexture::Checker);
			REQUIRE(checker.Mips.size() == 7);
			// Level 3 is 8x8: one texel per 8x8 square, so the squares' colours survive exactly.
			const std::byte light = texel(checker, 0, 0, 0)[0];
			const std::byte dark = texel(checker, 0, 8, 0)[0];
			CHECK(light != dark);
			CHECK(texel(checker, 3, 0, 0)[0] == light);
			CHECK(texel(checker, 3, 1, 0)[0] == dark);
			CHECK(texel(checker, 3, 1, 1)[0] == light);
			// From level 4 on, every texel covers two light and two dark squares: the mean of their linear values.
			const double mean = encode((decode(std::to_integer<int>(light)) + decode(std::to_integer<int>(dark))) / 2.0);
			for (uint32_t level = 4; level < 7; ++level)
			{
				CAPTURE(level);
				const std::span<const std::byte> value = texel(checker, level, 0, 0);
				CHECK(std::fabs(std::to_integer<int>(value[0]) - mean) <= 1.0);
				CHECK(value[0] == value[1]);
				CHECK(value[0] == value[2]);
				CHECK(value[3] == std::byte{ 255 });
			}

			const TextureData missing = GenerateBuiltinTexture(BuiltinTexture::Missing);
			const std::span<const std::byte> smallest = texel(missing, 6, 0, 0);
			CHECK(std::fabs(std::to_integer<int>(smallest[0]) - encode(0.5)) <= 1.0);
			CHECK(smallest[1] == std::byte{ 0 });
			CHECK(smallest[2] == smallest[0]);
			CHECK(smallest[3] == std::byte{ 255 });
		}
	}

}
