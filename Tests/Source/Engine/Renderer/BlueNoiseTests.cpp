#include "TestsPCH.h"

#include "Engine/Renderer/BlueNoise.h"

#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Hash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

// The blue-noise generator (Architecture §8.4, §8.9; Renderer/BlueNoise.h). Skeletons of the M8 contract
// (Docs/Decisions/0013-m8-decisions.md decision 8); stream C implements the generator, commits the hash below (the one value
// the implementation produces, identical in Debug and Release and on every compiler) and removes the skips.

namespace Engine {

	namespace {

		// The XXH64 of GenerateBlueNoise() with the default size and seed. Stream C sets it from the first implementation's
		// output and never changes it without bumping BlueNoiseGeneratorVersion (the cooked built-in's key).
		constexpr uint64_t CommittedBlueNoiseHash = 0;

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("BlueNoise: output hash matches the committed value" * doctest::skip(true))
		{
			const std::vector<uint8_t> noise = GenerateBlueNoise();
			REQUIRE(noise.size() == static_cast<size_t>(BlueNoiseSize) * BlueNoiseSize);
			CHECK(XXH64(std::as_bytes(std::span(noise))) == CommittedBlueNoiseHash);
		}

		TEST_CASE("BlueNoise: every value occurs equally often and the generator is deterministic" * doctest::skip(true))
		{
			// The ranks of 4,096 texels scaled to 256 levels: each level 16 times.
			const std::vector<uint8_t> noise = GenerateBlueNoise();
			std::array<uint32_t, 256> histogram{};
			for (const uint8_t value : noise)
				++histogram[value];
			for (const uint32_t count : histogram)
				CHECK(count == 16);
			CHECK(GenerateBlueNoise() == noise);
			CHECK(GenerateBlueNoise(BlueNoiseSize, BlueNoiseSeed + 1) != noise);
			CHECK(GenerateBlueNoise(16).size() == 256);
		}

		TEST_CASE("BlueNoise: the spectrum has little low-frequency energy" * doctest::skip(true))
		{
			// Blue noise: neighbouring texels are anti-correlated. The mean absolute difference of horizontal neighbours is
			// well above white noise's (about 85 for uniform 8-bit values) and the 4x4 block means stay near the middle.
			const std::vector<uint8_t> noise = GenerateBlueNoise();
			double difference = 0.0;
			for (uint32_t row = 0; row < BlueNoiseSize; ++row)
			{
				for (uint32_t column = 0; column < BlueNoiseSize; ++column)
				{
					const int left = noise[row * BlueNoiseSize + column];
					const int right = noise[row * BlueNoiseSize + (column + 1) % BlueNoiseSize];
					difference += std::abs(left - right);
				}
			}
			CHECK(difference / (BlueNoiseSize * BlueNoiseSize) > 95.0);
			for (uint32_t blockRow = 0; blockRow < BlueNoiseSize; blockRow += 4)
			{
				for (uint32_t blockColumn = 0; blockColumn < BlueNoiseSize; blockColumn += 4)
				{
					double sum = 0.0;
					for (uint32_t row = 0; row < 4; ++row)
					{
						for (uint32_t column = 0; column < 4; ++column)
							sum += noise[(blockRow + row) * BlueNoiseSize + blockColumn + column];
					}
					CHECK(std::abs(sum / 16.0 - 127.5) < 48.0);
				}
			}
		}

		TEST_CASE("BlueNoise: the generator cooks a linear R8 texture of the noise" * doctest::skip(true))
		{
			const Result<Buffer> cooked = GenerateBlueNoiseTexture();
			REQUIRE_MESSAGE(cooked.has_value(), cooked.error().ToString());
			const Result<AssetRef<TextureData>> texture = LoadCookedTexture(*cooked);
			REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());
			CHECK((*texture)->Format == TextureFormat::R8Unorm);
			CHECK((*texture)->Width == BlueNoiseSize);
			CHECK((*texture)->Height == BlueNoiseSize);
			REQUIRE((*texture)->Mips.size() == 1);
			const std::vector<uint8_t> noise = GenerateBlueNoise();
			const std::span<const std::byte> pixels = GetMipPixels(**texture, 0);
			CHECK(std::ranges::equal(pixels, std::as_bytes(std::span(noise))));
			// Deterministic bytes, so the engine cache's key never sees a different artifact for the same version.
			const Result<Buffer> again = GenerateBlueNoiseTexture();
			REQUIRE(again.has_value());
			CHECK(*again == *cooked);
		}
	}

}
