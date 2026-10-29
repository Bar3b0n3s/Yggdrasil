#include "TestsPCH.h"

#include "Engine/Asset/TextureCompression.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"
#include "Engine/Graphics/Image.h"
#include "Support/RenderReference.h"
#include "Support/TestData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The texture block codecs over CMP_Core (M23, C23.1; Docs/Decisions/0034-m23-contract.md decisions 13 to 15): the
// determinism check that CMP_Core's adoption waited for, and the codecs' size, padding, round-trip and refusal rules.

namespace Engine {

	namespace {

		// The decode bounds of the HDR sky through BC6H, per channel, relative to the source (measured at the contract: largest
		// 1.44 %, mean 0.31 %): twice the measurement, far below the error of a swapped channel (the sky's channels differ by a
		// factor of up to 4), a wrong stride or a wrong binary16 exponent.
		constexpr double MaxBc6hRelativeError = 0.03;
		constexpr double MeanBc6hRelativeError = 0.006;

		constexpr std::array<TextureBlockCodec, 4> LdrCodecs = { TextureBlockCodec::BC1, TextureBlockCodec::BC4, TextureBlockCodec::BC5,
			TextureBlockCodec::BC7 };

		// One level of the fixture set: RGBA8, or RGBA16F for BC6H.
		struct FixtureLevel
		{
			std::string Name{};
			uint32_t Width = 0;
			uint32_t Height = 0;
			Buffer Texels{};
		};

		void AppendRgba8(Buffer& texels, uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha)
		{
			texels.push_back(static_cast<std::byte>(red & 0xFFU));
			texels.push_back(static_cast<std::byte>(green & 0xFFU));
			texels.push_back(static_cast<std::byte>(blue & 0xFFU));
			texels.push_back(static_cast<std::byte>(alpha & 0xFFU));
		}

		// The binary16 bits of a finite, non-negative float below 65504, rounded half away from zero (std::lround; the tests'
		// values are exact halves or far from a tie, so the rounding rule never matters to the digest's stability).
		uint16_t FloatToHalf(float value)
		{
			if (value < std::ldexp(1.0f, -24))
				return 0;
			int exponent = 0;
			const float mantissa = std::frexp(value, &exponent); // value = mantissa * 2^exponent, mantissa in [0.5, 1)
			if (exponent < -13)
			{
				// Subnormal: value / 2^-24 as an integer.
				return static_cast<uint16_t>(std::lround(std::ldexp(value, 24)));
			}
			const auto significand = static_cast<uint32_t>(std::lround(std::ldexp(mantissa, 11))); // 1024 to 2048
			const uint32_t biased = static_cast<uint32_t>(exponent + 14);
			return static_cast<uint16_t>(significand == 2048 ? (biased + 1) << 10 : (biased << 10) | (significand - 1024));
		}

		// The binary16 value at byte `index` of RGBA16F texels.
		double ReadHalf(std::span<const std::byte> texels, size_t index)
		{
			return Test::HalfToDouble(static_cast<uint16_t>(std::to_integer<uint32_t>(texels[index]) | (std::to_integer<uint32_t>(texels[index + 1]) << 8)));
		}

		void AppendRgba16F(Buffer& texels, float red, float green, float blue)
		{
			for (const float value : { red, green, blue, 1.0f })
			{
				const uint16_t half = FloatToHalf(value);
				texels.push_back(static_cast<std::byte>(half & 0xFFU));
				texels.push_back(static_cast<std::byte>(half >> 8));
			}
		}

		// The committed PNG textures of Tests/Data/Assets/Textures, decoded to RGBA8.
		std::vector<FixtureLevel> LoadPngFixtures()
		{
			std::vector<FixtureLevel> levels;
			for (const std::string_view name : { "Rgba.png", "Rgb.png", "Normal.png", "Grey.png", "GreyAlpha.png", "Palette.png" })
			{
				const Result<Image> image = ReadPng(Test::GetTestDataPath(std::format("Assets/Textures/{}", name)));
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				levels.push_back({ .Name = std::string(name), .Width = image->Width, .Height = image->Height, .Texels = image->Pixels });
			}
			return levels;
		}

		// Procedural RGBA8 levels: smooth gradients with an alpha ramp, hard-edged checkers, seeded noise, and a 37 x 23
		// gradient whose size is not a multiple of 4 (the padding).
		std::vector<FixtureLevel> MakeProceduralLdrFixtures()
		{
			std::vector<FixtureLevel> levels;
			FixtureLevel gradient{ .Name = "Gradient", .Width = 64, .Height = 64 };
			for (uint32_t y = 0; y < gradient.Height; ++y)
			{
				for (uint32_t x = 0; x < gradient.Width; ++x)
					AppendRgba8(gradient.Texels, x * 4, y * 4, (x + y) * 2, 255 - y * 3);
			}
			levels.push_back(std::move(gradient));

			FixtureLevel checker{ .Name = "Checker", .Width = 64, .Height = 32 };
			for (uint32_t y = 0; y < checker.Height; ++y)
			{
				for (uint32_t x = 0; x < checker.Width; ++x)
				{
					const bool light = ((x / 3) + (y / 5)) % 2 == 0;
					AppendRgba8(checker.Texels, light ? 230 : 20, light ? 40 : 200, light ? 90 : 160, (x % 7) == 0 ? 0 : 255);
				}
			}
			levels.push_back(std::move(checker));

			FixtureLevel noise{ .Name = "Noise", .Width = 48, .Height = 48 };
			Random random(0x4D32335F42430000ULL);
			for (uint32_t texel = 0; texel < noise.Width * noise.Height; ++texel)
				AppendRgba8(noise.Texels, random.NextU32(), random.NextU32(), random.NextU32(), random.NextU32());
			levels.push_back(std::move(noise));

			FixtureLevel odd{ .Name = "Odd", .Width = 37, .Height = 23 };
			for (uint32_t y = 0; y < odd.Height; ++y)
			{
				for (uint32_t x = 0; x < odd.Width; ++x)
					AppendRgba8(odd.Texels, x * 7, 255 - y * 11, (x * y) % 256, 128 + x);
			}
			levels.push_back(std::move(odd));
			return levels;
		}

		// Procedural RGBA16F levels for BC6H: an HDR sky gradient with a bright sun, seeded noise across six orders of
		// magnitude, and a 13 x 9 ramp (the padding).
		std::vector<FixtureLevel> MakeProceduralHdrFixtures()
		{
			std::vector<FixtureLevel> levels;
			FixtureLevel sky{ .Name = "Sky", .Width = 32, .Height = 32 };
			for (uint32_t y = 0; y < sky.Height; ++y)
			{
				for (uint32_t x = 0; x < sky.Width; ++x)
				{
					const bool sun = x >= 20 && x < 24 && y >= 4 && y < 8;
					const float height = static_cast<float>(y) / 32.0f;
					AppendRgba16F(sky.Texels, sun ? 3000.0f : 0.25f + height, sun ? 2800.0f : 0.5f + height * 0.5f, sun ? 2500.0f : 1.0f + height * 0.125f);
				}
			}
			levels.push_back(std::move(sky));

			FixtureLevel noise{ .Name = "HdrNoise", .Width = 16, .Height = 16 };
			Random random(0x4D32335F42433648ULL);
			for (uint32_t texel = 0; texel < noise.Width * noise.Height; ++texel)
			{
				const auto scale = static_cast<float>(random.RangeInt(-4, 2));
				AppendRgba16F(noise.Texels, random.NextFloat() * std::ldexp(1.0f, static_cast<int>(scale) * 2),
					random.NextFloat() * std::ldexp(1.0f, static_cast<int>(scale) * 2), random.NextFloat() * std::ldexp(1.0f, static_cast<int>(scale) * 2));
			}
			levels.push_back(std::move(noise));

			FixtureLevel ramp{ .Name = "HdrRamp", .Width = 13, .Height = 9 };
			for (uint32_t y = 0; y < ramp.Height; ++y)
			{
				for (uint32_t x = 0; x < ramp.Width; ++x)
					AppendRgba16F(ramp.Texels, static_cast<float>(x) * 0.5f, static_cast<float>(y) * 8.0f, static_cast<float>(x + y) * 0.0625f);
			}
			levels.push_back(std::move(ramp));
			return levels;
		}

		// The mean absolute difference per channel byte of two RGBA8 levels over the channels `channels` keeps.
		double GetMeanAbsoluteError(std::span<const std::byte> left, std::span<const std::byte> right, uint32_t channels)
		{
			REQUIRE(left.size() == right.size());
			double sum = 0.0;
			size_t count = 0;
			for (size_t index = 0; index < left.size(); ++index)
			{
				if (index % 4 >= channels)
					continue;
				sum += std::abs(std::to_integer<int>(left[index]) - std::to_integer<int>(right[index]));
				++count;
			}
			return count == 0 ? 0.0 : sum / static_cast<double>(count);
		}

	}

	TEST_SUITE("Asset")
	{
		// Decision 13: CMP_Core is adopted only because this holds. Every codec compresses every level of the fixture set
		// (the committed PNG textures, procedural LDR and HDR levels, sizes that are not multiples of 4) to bytes whose XXH64
		// is the committed digest, in Debug and in Release (one compiler: the digest is never compared across compilers,
		// plan §7 item 12). A different digest in one configuration is a determinism failure, never a reason to change the
		// committed value; a deliberate codec change (its settings, a new CMP_Core) updates it with the importers' versions.
		TEST_CASE("TextureCompression: the fixture set compresses to the committed bytes in every configuration")
		{
			XXH64Hasher hasher;
			size_t blockBytes = 0;
			std::vector<FixtureLevel> ldr = LoadPngFixtures();
			for (FixtureLevel& level : MakeProceduralLdrFixtures())
				ldr.push_back(std::move(level));
			for (const TextureBlockCodec codec : LdrCodecs)
			{
				for (const FixtureLevel& level : ldr)
				{
					CAPTURE(TextureBlockCodecToString(codec));
					CAPTURE(level.Name);
					const Result<Buffer> blocks = CompressTextureBlocks(codec, level.Width, level.Height, level.Texels);
					REQUIRE_MESSAGE(blocks.has_value(), blocks.error().ToString());
					REQUIRE(blocks->size() == ComputeTextureBlockLevelSize(codec, level.Width, level.Height));
					hasher.Update(*blocks);
					blockBytes += blocks->size();
				}
			}
			for (const FixtureLevel& level : MakeProceduralHdrFixtures())
			{
				CAPTURE(level.Name);
				const Result<Buffer> blocks = CompressTextureBlocks(TextureBlockCodec::BC6H, level.Width, level.Height, level.Texels);
				REQUIRE_MESSAGE(blocks.has_value(), blocks.error().ToString());
				hasher.Update(*blocks);
				blockBytes += blocks->size();
			}
			CHECK(blockBytes == 30800);
			CHECK_MESSAGE(hasher.Digest() == 0x74F5A212DFCCACABULL, std::format("digest {:016x}", hasher.Digest()));
		}

		TEST_CASE("TextureCompression: block sizes follow the codec and round up partial blocks")
		{
			CHECK(GetTextureBlockBytes(TextureBlockCodec::BC1) == 8);
			CHECK(GetTextureBlockBytes(TextureBlockCodec::BC4) == 8);
			for (const TextureBlockCodec codec : { TextureBlockCodec::BC5, TextureBlockCodec::BC6H, TextureBlockCodec::BC7 })
				CHECK(GetTextureBlockBytes(codec) == 16);
			CHECK(GetTextureBlockTexelBytes(TextureBlockCodec::BC6H) == 8);
			CHECK(GetTextureBlockTexelBytes(TextureBlockCodec::BC7) == 4);
			CHECK(ComputeTextureBlockLevelSize(TextureBlockCodec::BC7, 1, 1) == 16);
			CHECK(ComputeTextureBlockLevelSize(TextureBlockCodec::BC7, 5, 4) == 32);
			CHECK(ComputeTextureBlockLevelSize(TextureBlockCodec::BC1, 37, 23) == 10 * 6 * 8);
			CHECK(ComputeTextureBlockLevelSize(TextureBlockCodec::BC5, 0, 4) == 0);
			CHECK(TextureBlockCodecToString(TextureBlockCodec::BC6H) == "BC6H");
		}

		// The blocks follow the GPU formats' channel order: a red RGBA8 block is a BC1 block whose endpoints are 0xF800 (red
		// in RGB565's high bits) and decodes red, and BC7 decodes it red.
		TEST_CASE("TextureCompression: compressed blocks keep the channel order of RGBA8 texels")
		{
			Buffer red;
			for (uint32_t texel = 0; texel < 16; ++texel)
				AppendRgba8(red, 255, 0, 0, 255);
			const Result<Buffer> bc1 = CompressTextureBlocks(TextureBlockCodec::BC1, 4, 4, red);
			REQUIRE(bc1.has_value());
			REQUIRE(bc1->size() == 8);
			CHECK((*bc1)[0] == std::byte{ 0x00 });
			CHECK((*bc1)[1] == std::byte{ 0xF8 });
			for (const TextureBlockCodec codec : { TextureBlockCodec::BC1, TextureBlockCodec::BC7 })
			{
				CAPTURE(TextureBlockCodecToString(codec));
				const Result<Buffer> blocks = CompressTextureBlocks(codec, 4, 4, red);
				REQUIRE(blocks.has_value());
				const Result<Buffer> texels = DecompressTextureBlocks(codec, 4, 4, *blocks);
				REQUIRE(texels.has_value());
				CHECK(std::to_integer<int>((*texels)[0]) >= 250);
				CHECK((*texels)[1] == std::byte{ 0 });
				CHECK((*texels)[2] == std::byte{ 0 });
				CHECK((*texels)[3] == std::byte{ 255 });
			}
		}

		// A partial block repeats the level's last column and row: a 5 x 5 level compresses its edge blocks exactly as an 8 x 8
		// level whose extra texels repeat them.
		TEST_CASE("TextureCompression: a level whose size is not a multiple of 4 is padded with its last column and row")
		{
			FixtureLevel small{ .Width = 5, .Height = 5 };
			for (uint32_t y = 0; y < 5; ++y)
			{
				for (uint32_t x = 0; x < 5; ++x)
					AppendRgba8(small.Texels, x * 50, y * 50, 100, 255);
			}
			FixtureLevel padded{ .Width = 8, .Height = 8 };
			for (uint32_t y = 0; y < 8; ++y)
			{
				for (uint32_t x = 0; x < 8; ++x)
					AppendRgba8(padded.Texels, std::min(x, 4U) * 50, std::min(y, 4U) * 50, 100, 255);
			}
			for (const TextureBlockCodec codec : LdrCodecs)
			{
				CAPTURE(TextureBlockCodecToString(codec));
				const Result<Buffer> smallBlocks = CompressTextureBlocks(codec, small.Width, small.Height, small.Texels);
				const Result<Buffer> paddedBlocks = CompressTextureBlocks(codec, padded.Width, padded.Height, padded.Texels);
				REQUIRE(smallBlocks.has_value());
				REQUIRE(paddedBlocks.has_value());
				CHECK(*smallBlocks == *paddedBlocks);
			}
		}

		// Decision 15's decode fallback: decompressing gives the level's size and form, close to its source (each codec's
		// channels within a mean error far below a visible difference on smooth content), and decompressing the same
		// blocks twice gives the same texels.
		TEST_CASE("TextureCompression: decompressed levels keep their size and stay close to their source")
		{
			const std::vector<FixtureLevel> ldr = MakeProceduralLdrFixtures();
			const FixtureLevel& gradient = ldr.front();
			const std::array<uint32_t, 4> channels = { 3, 1, 2, 4 };
			const std::array<double, 4> limits = { 4.0, 1.0, 1.0, 2.0 };
			for (size_t index = 0; index < LdrCodecs.size(); ++index)
			{
				const TextureBlockCodec codec = LdrCodecs[index];
				CAPTURE(TextureBlockCodecToString(codec));
				const Result<Buffer> blocks = CompressTextureBlocks(codec, gradient.Width, gradient.Height, gradient.Texels);
				REQUIRE(blocks.has_value());
				const Result<Buffer> texels = DecompressTextureBlocks(codec, gradient.Width, gradient.Height, *blocks);
				REQUIRE_MESSAGE(texels.has_value(), texels.error().ToString());
				REQUIRE(texels->size() == gradient.Texels.size());
				CHECK(GetMeanAbsoluteError(*texels, gradient.Texels, channels[index]) < limits[index]);
				const Result<Buffer> again = DecompressTextureBlocks(codec, gradient.Width, gradient.Height, *blocks);
				REQUIRE(again.has_value());
				CHECK(*again == *texels);
			}

			// BC4 and BC5 decode as the GPU's formats sample: (r, 0, 0, 255) and (r, g, 0, 255).
			const Result<Buffer> red = CompressTextureBlocks(TextureBlockCodec::BC4, gradient.Width, gradient.Height, gradient.Texels);
			REQUIRE(red.has_value());
			const Result<Buffer> redTexels = DecompressTextureBlocks(TextureBlockCodec::BC4, gradient.Width, gradient.Height, *red);
			REQUIRE(redTexels.has_value());
			CHECK((*redTexels)[1] == std::byte{ 0 });
			CHECK((*redTexels)[2] == std::byte{ 0 });
			CHECK((*redTexels)[3] == std::byte{ 255 });

			// BC6H: an odd-sized HDR ramp keeps its size; alpha decodes as binary16 1.0.
			const std::vector<FixtureLevel> hdr = MakeProceduralHdrFixtures();
			const FixtureLevel& ramp = hdr.back();
			const Result<Buffer> hdrBlocks = CompressTextureBlocks(TextureBlockCodec::BC6H, ramp.Width, ramp.Height, ramp.Texels);
			REQUIRE(hdrBlocks.has_value());
			const Result<Buffer> hdrTexels = DecompressTextureBlocks(TextureBlockCodec::BC6H, ramp.Width, ramp.Height, *hdrBlocks);
			REQUIRE(hdrTexels.has_value());
			REQUIRE(hdrTexels->size() == ramp.Texels.size());
			CHECK((*hdrTexels)[6] == std::byte{ 0x00 });
			CHECK((*hdrTexels)[7] == std::byte{ 0x3C });

			// BC6H decodes close to its source, channel by channel: the HDR sky (a smooth gradient whose three channels differ,
			// and a 3000-nit sun block whose channels differ by up to 17 %) within a small relative error, so a wrong channel
			// order, stride or binary16 encoding fails. The noise level spans six orders of magnitude within a block, which
			// BC6H's shared endpoints cannot hold, so it is only in the determinism check.
			const FixtureLevel& sky = hdr.front();
			const Result<Buffer> skyBlocks = CompressTextureBlocks(TextureBlockCodec::BC6H, sky.Width, sky.Height, sky.Texels);
			REQUIRE(skyBlocks.has_value());
			const Result<Buffer> skyTexels = DecompressTextureBlocks(TextureBlockCodec::BC6H, sky.Width, sky.Height, *skyBlocks);
			REQUIRE(skyTexels.has_value());
			REQUIRE(skyTexels->size() == sky.Texels.size());
			double largest = 0.0;
			double sum = 0.0;
			for (size_t index = 0; index < sky.Texels.size(); index += 2)
			{
				if ((index / 2) % 4 == 3)
					continue; // alpha
				const double source = ReadHalf(sky.Texels, index);
				const double decoded = ReadHalf(*skyTexels, index);
				const double relative = std::abs(decoded - source) / source;
				largest = std::max(largest, relative);
				sum += relative;
			}
			const double mean = sum / static_cast<double>(sky.Width * sky.Height * 3);
			CHECK_MESSAGE(largest < MaxBc6hRelativeError, std::format("largest relative error {}, mean {}", largest, mean));
			CHECK_MESSAGE(mean < MeanBc6hRelativeError, std::format("largest relative error {}, mean {}", largest, mean));
		}

		// The decode fallback reads blocks from cooked data, which a corrupt cache or pak may hold: CMP_Core's decoders turn any
		// bytes into texels of the level's form without a fault (every block mode and partition, reserved BC6H and BC7 modes
		// included).
		TEST_CASE("TextureCompression: any block bytes decompress without a fault")
		{
			Random random(0x424C4F434B42595AULL);
			for (const TextureBlockCodec codec : { TextureBlockCodec::BC1, TextureBlockCodec::BC4, TextureBlockCodec::BC5, TextureBlockCodec::BC6H,
					 TextureBlockCodec::BC7 })
			{
				CAPTURE(TextureBlockCodecToString(codec));
				for (int level = 0; level < 64; ++level)
				{
					Buffer blocks(ComputeTextureBlockLevelSize(codec, 16, 16));
					for (std::byte& value : blocks)
						value = static_cast<std::byte>(random.NextU32() >> 24);
					const Result<Buffer> texels = DecompressTextureBlocks(codec, 16, 16, blocks);
					REQUIRE_MESSAGE(texels.has_value(), texels.error().ToString());
					CHECK(texels->size() == size_t{ 16 } * 16 * GetTextureBlockTexelBytes(codec));
				}
			}
		}

		TEST_CASE("TextureCompression: wrong sizes and BC6H values a block cannot hold are refused")
		{
			const Buffer four(4 * 4 * 4, std::byte{ 0x80 });
			const auto refused = [](const Result<Buffer>& result)
			{
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().GetCode() == ErrorCode::InvalidArgument);
			};
			refused(CompressTextureBlocks(TextureBlockCodec::BC7, 0, 4, {}));
			refused(CompressTextureBlocks(TextureBlockCodec::BC7, 16385, 1, Buffer(16385 * 4)));
			refused(CompressTextureBlocks(TextureBlockCodec::BC7, 4, 4, std::span<const std::byte>(four).first(63)));
			refused(CompressTextureBlocks(static_cast<TextureBlockCodec>(99), 4, 4, four));
			refused(DecompressTextureBlocks(TextureBlockCodec::BC1, 4, 4, Buffer(16)));
			refused(DecompressTextureBlocks(TextureBlockCodec::BC1, 4, 0, Buffer(8)));

			// A negative and a non-finite (binary16 infinity, 0x7C00) value.
			for (const uint16_t half : { uint16_t{ 0xBC00 }, uint16_t{ 0x7C00 } })
			{
				Buffer texels;
				for (uint32_t texel = 0; texel < 16; ++texel)
					AppendRgba16F(texels, 1.0f, 1.0f, 1.0f);
				texels[8 * 5 + 2] = static_cast<std::byte>(half & 0xFFU);
				texels[8 * 5 + 3] = static_cast<std::byte>(half >> 8);
				refused(CompressTextureBlocks(TextureBlockCodec::BC6H, 4, 4, texels));
			}
		}
	}

}
