#include "EnginePCH.h"
#include "Engine/Renderer/AutoExposure.h"

// Histogram auto-exposure's CPU statement (AutoExposure.h): contract stubs of M26's C26.1, implemented by stream X-A
// (Docs/Decisions/0037-m26-contract.md decision 15).

namespace Engine {

	uint32_t GetLuminanceHistogramBin(const glm::vec3& /*radiance*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	ExposureHistogram ComputeLuminanceHistogram(std::span<const glm::vec4> /*pixels*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	float ComputeExposureTarget(const ExposureHistogram& /*histogram*/, const PostProcessSettings& /*post*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

	float AdaptExposure(float /*current*/, float /*target*/, float /*deltaTime*/, bool /*reset*/, const PostProcessSettings& /*post*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

}
