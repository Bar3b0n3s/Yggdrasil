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
			// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 6): engine://Textures/DissolveNoise.
			CHECK(BuiltinTextureToString(BuiltinTexture::DissolveNoise) == "DissolveNoise");
		}

		// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 6): the noise a MaterialParameters dissolve clips by. Its
		// values are equalized, so a dissolve's amount is the share of the surface it removes; they stay inside (0, 255), so a
		// linear sampler's blend of them never reaches 0 (an amount of 0 removes nothing) or 1 (an amount of 1 removes all);
		// it is smooth, so a dissolve's holes are blobs rather than salt and pepper; and it tiles, so a repeating surface
		// shows no seam.
		TEST_CASE("BuiltinTextures: the dissolve noise is a 128x128 single-channel smooth noise that tiles, its values 1 to 254 in equal shares")
		{
			const TextureData texture = GenerateBuiltinDissolveNoiseTexture();
			REQUIRE(ValidateTextureData(texture).has_value());
			CHECK(texture.Format == TextureFormat::R8Unorm);
			CHECK(texture.Width == 128);
			CHECK(texture.Height == 128);
			REQUIRE(texture.Mips.size() == 1);
			CHECK(std::ranges::equal(texture.Pixels, GenerateBuiltinDissolveNoiseTexture().Pixels));
			CHECK(std::ranges::equal(texture.Pixels, GenerateBuiltinTexture(BuiltinTexture::DissolveNoise).Pixels));
			const std::span<const std::byte> pixels = GetMipPixels(texture, 0);
			REQUIRE(pixels.size() == 128 * 128);
			const auto value = [&pixels](uint32_t x, uint32_t y)
			{
				return std::to_integer<int>(pixels[static_cast<size_t>(y % 128) * 128 + x % 128]);
			};

			// Every value from 1 to 254 holds 64 or 65 of the 16384 texels, and 0 and 255 none.
			std::array<int, 256> counts{};
			for (const std::byte texel : pixels)
				++counts[std::to_integer<size_t>(texel)];
			CHECK(counts[0] == 0);
			CHECK(counts[255] == 0);
			for (size_t level = 1; level <= 254; ++level)
			{
				CAPTURE(level);
				CHECK(counts[level] >= 64);
				CHECK(counts[level] <= 65);
			}

			// Smooth and seamless: neighbours differ by a small step, across the texture's wrapping edges as inside it (the
			// largest step, measured once, is far below the 253 a random texture would reach).
			int largestStep = 0;
			int wrapStep = 0;
			for (uint32_t y = 0; y < 128; ++y)
			{
				for (uint32_t x = 0; x < 128; ++x)
				{
					const int right = std::abs(value(x, y) - value(x + 1, y));
					const int down = std::abs(value(x, y) - value(x, y + 1));
					largestStep = std::max({ largestStep, right, down });
					if (x == 127)
						wrapStep = std::max(wrapStep, right);
					if (y == 127)
						wrapStep = std::max(wrapStep, down);
				}
			}
			CAPTURE(largestStep);
			CAPTURE(wrapStep);
			CHECK(largestStep <= 48);
			CHECK(wrapStep <= 48);
			// The noise has structure at every octave: not one smooth gradient but many values in any 32 x 32 tile.
			std::array<bool, 256> seen{};
			for (uint32_t y = 0; y < 32; ++y)
				for (uint32_t x = 0; x < 32; ++x)
					seen[static_cast<size_t>(value(x, y))] = true;
			CHECK(std::ranges::count(seen, true) > 64);
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

		// M47 (Docs/Decisions/0065-m47-water.md decision 3): the water's built-in maps, 128 x 128 with their full mip chains (each
		// level the exact 2 x 2 box mean of the one above), tiling seamlessly: the normal map's texels unit-length normals that
		// lean every way about +Z, and the foam map clear in places, dense in others.
		TEST_CASE("BuiltinTextures: the water's normal and foam maps are 128x128 tiling noise with full mip chains, identical on every run")
		{
			const TextureData normals = GenerateBuiltinWaterNormalTexture();
			const TextureData foam = GenerateBuiltinWaterFoamTexture();
			for (const TextureData* texture : { &normals, &foam })
			{
				REQUIRE(ValidateTextureData(*texture).has_value());
				CHECK(texture->Width == 128);
				CHECK(texture->Height == 128);
				REQUIRE(texture->Mips.size() == 8);
				for (uint32_t level = 0; level < 8; ++level)
				{
					CHECK(texture->Mips[level].Width == 128U >> level);
					CHECK(texture->Mips[level].Height == 128U >> level);
				}
			}
			CHECK(normals.Format == TextureFormat::RGBA8Unorm);
			CHECK(foam.Format == TextureFormat::R8Unorm);
			CHECK(std::ranges::equal(normals.Pixels, GenerateBuiltinWaterNormalTexture().Pixels));
			CHECK(std::ranges::equal(normals.Pixels, GenerateBuiltinTexture(BuiltinTexture::WaterNormal).Pixels));
			CHECK(std::ranges::equal(foam.Pixels, GenerateBuiltinWaterFoamTexture().Pixels));
			CHECK(std::ranges::equal(foam.Pixels, GenerateBuiltinTexture(BuiltinTexture::WaterFoam).Pixels));

			// The normals: each texel's decoded (x, y, z) within rounding of unit length, z positive, leaning every way.
			const std::span<const std::byte> base = GetMipPixels(normals, 0);
			REQUIRE(base.size() == 128 * 128 * 4);
			int64_t sumX = 0;
			int64_t sumY = 0;
			bool leansLeft = false;
			bool leansRight = false;
			for (size_t texel = 0; texel < 128 * 128; ++texel)
			{
				const double x = std::to_integer<int>(base[texel * 4]) - 128.0;
				const double y = std::to_integer<int>(base[texel * 4 + 1]) - 128.0;
				const double z = std::to_integer<int>(base[texel * 4 + 2]) - 128.0;
				CHECK(std::to_integer<int>(base[texel * 4 + 3]) == 255);
				CHECK(z > 0.0);
				CHECK(std::abs(std::sqrt(x * x + y * y + z * z) - 127.0) < 1.5);
				sumX += static_cast<int64_t>(x);
				sumY += static_cast<int64_t>(y);
				leansLeft = leansLeft || x < -40.0;
				leansRight = leansRight || x > 40.0;
			}
			// A sanity check, not the tiling's proof: the wrapping central differences of any height field sum to zero across
			// the period, so the mean lean is within rounding of none.
			CHECK(std::abs(static_cast<double>(sumX) / (128.0 * 128.0)) < 1.0);
			CHECK(std::abs(static_cast<double>(sumY) / (128.0 * 128.0)) < 1.0);
			CHECK(leansLeft);
			CHECK(leansRight);
			// Seamless (the W47.2 review's finding 5): neighbouring texels' decoded x and y differ by a small step, across the
			// wrapping edges (texel 127 against texel 0) as inside. The largest step, measured once, is 50; a height field that
			// stopped tiling leans 45 degrees along its seam, a step of about 90 beside it.
			const auto component = [&base](uint32_t x, uint32_t y, uint32_t channel)
			{
				return std::to_integer<int>(base[(static_cast<size_t>(y % 128) * 128 + x % 128) * 4 + channel]);
			};
			for (uint32_t channel = 0; channel < 2; ++channel)
			{
				CAPTURE(channel);
				int largestNormalStep = 0;
				for (uint32_t y = 0; y < 128; ++y)
				{
					for (uint32_t x = 0; x < 128; ++x)
					{
						const int right = std::abs(component(x, y, channel) - component(x + 1, y, channel));
						const int down = std::abs(component(x, y, channel) - component(x, y + 1, channel));
						largestNormalStep = std::max({ largestNormalStep, right, down });
					}
				}
				CAPTURE(largestNormalStep);
				CHECK(largestNormalStep <= 56);
			}

			// The foam: clear and dense texels both, and seamless across the wrapping edges as inside.
			const std::span<const std::byte> cover = GetMipPixels(foam, 0);
			REQUIRE(cover.size() == 128 * 128);
			const auto value = [&cover](uint32_t x, uint32_t y)
			{
				return std::to_integer<int>(cover[static_cast<size_t>(y % 128) * 128 + x % 128]);
			};
			// The steps between neighbours inside the texture (x < 127 to the right, y < 127 down), and across its wrapping
			// edges (texel 127 against texel 0): a seamless texture's wrap steps are no larger than its largest inside step
			// (measured once: 6 against 75), while a lattice that stopped wrapping steps up to 255 there.
			int clear = 0;
			int dense = 0;
			int interiorStep = 0;
			int wrapStep = 0;
			for (uint32_t y = 0; y < 128; ++y)
			{
				for (uint32_t x = 0; x < 128; ++x)
				{
					clear += value(x, y) == 0 ? 1 : 0;
					dense += value(x, y) == 255 ? 1 : 0;
					const int right = std::abs(value(x, y) - value(x + 1, y));
					const int down = std::abs(value(x, y) - value(x, y + 1));
					int& rightStep = x == 127 ? wrapStep : interiorStep;
					rightStep = std::max(rightStep, right);
					int& downStep = y == 127 ? wrapStep : interiorStep;
					downStep = std::max(downStep, down);
				}
			}
			CAPTURE(interiorStep);
			CAPTURE(wrapStep);
			CHECK(clear > 128 * 128 / 10);
			CHECK(dense > 0);
			CHECK(wrapStep <= interiorStep);
		}

		TEST_CASE("BuiltinTextures: the generated texels match the hashes recorded in every configuration")
		{
			// Integer arithmetic and a fixed table only, so the recorded hashes hold in every configuration and on every platform.
			// M21's sprite textures (Docs/Decisions/0032-m21-contract.md decision 7) follow the M6 textures, and M26's particle
			// texture (Docs/Decisions/0037-m26-contract.md decision 7) follows them, and M47's water normal and foam maps
			// (Docs/Decisions/0065-m47-water.md decision 3) follow it.
			constexpr std::array<BuiltinTexture, 12> Textures = { BuiltinTexture::White, BuiltinTexture::Black, BuiltinTexture::FlatNormal,
				BuiltinTexture::Checker, BuiltinTexture::Missing, BuiltinTexture::SpriteSquare, BuiltinTexture::SpriteRounded, BuiltinTexture::SpriteBevel,
				BuiltinTexture::SpriteCircle, BuiltinTexture::Particle, BuiltinTexture::WaterNormal, BuiltinTexture::WaterFoam };
			constexpr std::array<uint64_t, 12> RecordedHashes = { 0x2e54fae2959dbb84ull, 0x088910b435c92dc1ull, 0xee5a85739c397f1cull,
				0xc4e010a786d84db6ull, 0x959a5dac370947e5ull, 0x9ea922e57a320e17ull, 0xc0165963ae845894ull, 0xbb5ce8d25334fb6dull, 0x3737bbbc5466cf4eull,
				0x6b032256171593caull, 0x10e3bc3c39ac7b6aull, 0x6a4d21ad10ef4fa7ull };
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
