#include "EnginePCH.h"
#include "Engine/Renderer/BlueNoise.h"

#include "Engine/Core/Error.h"

namespace Engine {

	std::vector<uint8_t> GenerateBlueNoise(uint32_t /*size*/, uint64_t /*seed*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<Buffer> GenerateBlueNoiseTexture()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the blue-noise generator is not implemented yet (M8 stream C)");
	}

}
