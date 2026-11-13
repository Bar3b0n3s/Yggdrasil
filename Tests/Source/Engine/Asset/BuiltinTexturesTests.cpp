#include "TestsPCH.h"

#include "Engine/Asset/BuiltinTextures.h"

#include "Engine/Core/DetMath.h"
#include "Engine/Core/Hash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <span>
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
			// M21 (Docs/Decisions/0032-m21-contract.md decision 7): engine://Textures/Sprite<Shape>.
			CHECK(BuiltinTextureToString(BuiltinTexture::SpriteSquare) == "SpriteSquare");
			CHECK(BuiltinTextureToString(BuiltinTexture::SpriteRounded) == "SpriteRounded");
			CHECK(BuiltinTextureToString(BuiltinTexture::SpriteBevel) == "SpriteBevel");
			CHECK(BuiltinTextureToString(BuiltinTexture::SpriteCircle) == "SpriteCircle");
			// M26 (Docs/Decisions/0037-m26-contract.md decision 7): engine://Textures/Particle.
			CHECK(BuiltinTextureToString(BuiltinTexture::Particle) == "Particle");
		}

		// Docs/Decisions/0032-m21-contract.md decision 7 (stream B): the procedural sprite textures, white shapes a sprite tints.
		TEST_CASE("BuiltinTextures: the sprite textures are 16x16 white shapes of one level, identical on every run")
		{
			const auto texel = [](const TextureData& texture, uint32_t x, uint32_t y)
			{
				const std::span<const std::byte> pixels = GetMipPixels(texture, 0);
				const size_t offset = (static_cast<size_t>(y) * texture.Width + x) * 4;
				return std::array<uint8_t, 4>{ static_cast<uint8_t>(pixels[offset]), static_cast<uint8_t>(pixels[offset + 1]), static_cast<uint8_t>(pixels[offset + 2]),
					static_cast<uint8_t>(pixels[offset + 3]) };
			};
			constexpr std::array<uint8_t, 4> Opaque = { 255, 255, 255, 255 };
			constexpr std::array<uint8_t, 4> Clear = { 255, 255, 255, 0 };
			for (const BuiltinTexture sprite : { BuiltinTexture::SpriteSquare, BuiltinTexture::SpriteRounded, BuiltinTexture::SpriteBevel, BuiltinTexture::SpriteCircle })
			{
				CAPTURE(BuiltinTextureToString(sprite));
				const TextureData texture = GenerateBuiltinSpriteTexture(sprite);
				REQUIRE(ValidateTextureData(texture).has_value());
				CHECK(texture.Format == TextureFormat::RGBA8Srgb);
				CHECK(texture.Width == 16);
				CHECK(texture.Height == 16);
				CHECK(texture.Mips.size() == 1);
				const TextureData again = GenerateBuiltinSpriteTexture(sprite);
				CHECK(std::ranges::equal(GetMipPixels(texture, 0), GetMipPixels(again, 0)));
			}
			const TextureData square = GenerateBuiltinSpriteTexture(BuiltinTexture::SpriteSquare);
			CHECK(texel(square, 0, 0) == Opaque);
			CHECK(texel(square, 15, 15) == Opaque);
			const TextureData rounded = GenerateBuiltinSpriteTexture(BuiltinTexture::SpriteRounded);
			CHECK(texel(rounded, 0, 0) == Clear);
			CHECK(texel(rounded, 8, 0) == Opaque);
			CHECK(texel(rounded, 15, 15) == Clear);
			const TextureData bevel = GenerateBuiltinSpriteTexture(BuiltinTexture::SpriteBevel);
			CHECK(texel(bevel, 0, 8) == Opaque);
			CHECK(texel(bevel, 15, 8) == std::array<uint8_t, 4>{ 128, 128, 128, 255 });
			CHECK(texel(bevel, 8, 8) == std::array<uint8_t, 4>{ 204, 204, 204, 255 });
			const TextureData circle = GenerateBuiltinSpriteTexture(BuiltinTexture::SpriteCircle);
			CHECK(texel(circle, 0, 0) == Clear);
			CHECK(texel(circle, 8, 8) == Opaque);
			CHECK(texel(circle, 0, 8) == Opaque);
		}

		// M26 (C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 7, stream P-B): the particles' default texture, a white
		// soft disc whose alpha falls as (1 - d^2)^2 from the centre to the rim, where d is the texel centre's distance from the
		// texture's centre over its half side.
		TEST_CASE("BuiltinTextures: the particle texture is a 32x32 white soft disc of one level, identical on every run")
		{
			const TextureData texture = GenerateBuiltinParticleTexture();
			REQUIRE(ValidateTextureData(texture).has_value());
			CHECK(texture.Format == TextureFormat::RGBA8Srgb);
			CHECK(texture.Width == 32);
			CHECK(texture.Height == 32);
			REQUIRE(texture.Mips.size() == 1);
			CHECK(std::ranges::equal(texture.Pixels, GenerateBuiltinParticleTexture().Pixels));
			CHECK(std::ranges::equal(texture.Pixels, GenerateBuiltinTexture(BuiltinTexture::Particle).Pixels));
			const std::span<const std::byte> pixels = GetMipPixels(texture, 0);
			REQUIRE(pixels.size() == 32 * 32 * 4);
			const auto alpha = [&pixels](uint32_t x, uint32_t y)
			{
				return std::to_integer<int>(pixels[(static_cast<size_t>(y) * 32 + x) * 4 + 3]);
			};
			for (uint32_t y = 0; y < 32; ++y)
			{
				for (uint32_t x = 0; x < 32; ++x)
				{
					CAPTURE(x);
					CAPTURE(y);
					const size_t offset = (static_cast<size_t>(y) * 32 + x) * 4;
					CHECK(pixels[offset] == std::byte{ 255 });
					CHECK(pixels[offset + 1] == std::byte{ 255 });
					CHECK(pixels[offset + 2] == std::byte{ 255 });
					// The definition in doubles: (1 - d^2) is a multiple of 2^-10 and its square of 2^-20, both exact, and no
					// value lies halfway between two bytes, so rounding the double gives the generator's byte.
					const double dx = (static_cast<double>(x) + 0.5 - 16.0) / 16.0;
					const double dy = (static_cast<double>(y) + 0.5 - 16.0) / 16.0;
					const double falloff = std::max(0.0, 1.0 - (dx * dx + dy * dy));
					CHECK(alpha(x, y) == static_cast<int>(std::round(255.0 * falloff * falloff)));
					// Symmetric about both axes.
					CHECK(alpha(x, y) == alpha(31 - x, y));
					CHECK(alpha(x, y) == alpha(x, 31 - y));
				}
			}
			// The four centre texels are nearly opaque, the edge midpoints barely visible and the corners clear.
			CHECK(alpha(15, 15) == 254);
			CHECK(alpha(16, 16) == 254);
			CHECK(alpha(0, 15) == 1);
			CHECK(alpha(0, 0) == 0);
			CHECK(alpha(31, 31) == 0);
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
			// M21's sprite textures (Docs/Decisions/0032-m21-contract.md decision 7) follow the M6 textures, and M26's particle
			// texture (Docs/Decisions/0037-m26-contract.md decision 7) follows them.
			constexpr std::array<BuiltinTexture, 10> Textures = { BuiltinTexture::White, BuiltinTexture::Black, BuiltinTexture::FlatNormal,
				BuiltinTexture::Checker, BuiltinTexture::Missing, BuiltinTexture::SpriteSquare, BuiltinTexture::SpriteRounded, BuiltinTexture::SpriteBevel,
				BuiltinTexture::SpriteCircle, BuiltinTexture::Particle };
			constexpr std::array<uint64_t, 10> RecordedHashes = { 0x2e54fae2959dbb84ull, 0x088910b435c92dc1ull, 0xee5a85739c397f1cull,
				0xc4e010a786d84db6ull, 0x959a5dac370947e5ull, 0x9ea922e57a320e17ull, 0xc0165963ae845894ull, 0xbb5ce8d25334fb6dull, 0x3737bbbc5466cf4eull,
				0x6b032256171593caull };
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
