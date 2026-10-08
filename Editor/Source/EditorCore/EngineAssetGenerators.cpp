#include "EditorPCH.h"
#include "EditorCore/EngineAssetGenerators.h"

#include "Engine/Renderer/BlueNoise.h"

#include <array>

namespace Engine {

	std::span<const EngineAssetGenerator> GetEngineAssetGenerators()
	{
		static constexpr std::array<EngineAssetGenerator, 1> Generators = { {
			{ .Id = BlueNoiseGeneratorId, .Version = BlueNoiseGeneratorVersion, .Generate = &GenerateBlueNoiseTexture },
		} };
		return Generators;
	}

}
