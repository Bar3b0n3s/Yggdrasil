#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string_view>
#include <vector>

// The blue-noise texture of the tonemap's dither (Architecture §8.4 t7, §8.9): generated in code by a seeded void-and-cluster
// implementation (Ulichney 1993), so no third-party texture or license is involved. `Editor --bake-engine-assets` runs it
// as the EngineAssetGenerator "BlueNoise" (EditorCore/EngineAssetGenerators.h) into the engine cooked cache, from where it
// ships in Engine.pak as the built-in texture engine://Textures/BlueNoise (BuiltinAssetHandles::BlueNoiseTexture, a
// Generated entry of Resources/EngineAssets.json). "BlueNoise: output hash matches the committed value" compares the XXH64
// of GenerateBlueNoise() with a constant committed in the test.
//
// Deterministic on every compiler, host and configuration: the Gaussian energy uses Core/DetMath (never <cmath>'s exp),
// candidates are scanned in row-major order and ties go to the lowest index, and the initial pattern comes from a seeded
// Core/Random. Pure and thread-safe. Frozen by the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 8).

namespace Engine {

	inline constexpr uint32_t BlueNoiseSize = 64;
	inline constexpr uint64_t BlueNoiseSeed = 0x0B1E0015E5EEDull;
	// The Gaussian's sigma in texels (Ulichney's 1.5).
	inline constexpr double BlueNoiseSigma = 1.5;
	// The EngineAssetGenerator of the built-in (EngineAssetBaker.h): bump the version whenever the output changes.
	inline constexpr std::string_view BlueNoiseGeneratorId = "BlueNoise";
	inline constexpr uint32_t BlueNoiseGeneratorVersion = 1;

	// A `size` x `size` blue-noise threshold map (size a power of two from 4 to 256, asserted): the void-and-cluster rank of
	// each texel, toroidally wrapped, scaled to 0-255 (rank * 256 / size²), rows top first. Every value occurs size² / 256
	// times for size >= 16.
	[[nodiscard]] std::vector<uint8_t> GenerateBlueNoise(uint32_t size = BlueNoiseSize, uint64_t seed = BlueNoiseSeed);

	// The complete cooked artifact (CookedHeader + TextureData payload, Asset/TextureData.h) of GenerateBlueNoise() as a
	// linear R8 texture of BlueNoiseSize² with one mip: the generator of the "BlueNoise" built-in. Errors: those of the
	// texture codec (none for valid data).
	[[nodiscard]] Result<Buffer> GenerateBlueNoiseTexture();

}
