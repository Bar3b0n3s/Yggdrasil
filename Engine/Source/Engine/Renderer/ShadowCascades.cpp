#include "EnginePCH.h"
#include "Engine/Renderer/ShadowCascades.h"

namespace Engine {

	Result<std::array<float, MaxShadowCascades>> ComputeCascadeSplits(float, float, uint32_t, float)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	Result<std::array<glm::vec3, 8>> ComputeFrustumSliceCorners(const CameraData&, float, float)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	Result<ShadowCascadeSet> BuildShadowCascades(const CameraData&, const LightData&, uint32_t, uint32_t)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	Result<float> ComputeStabilizedCascadeRadius(float, uint32_t)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 post-snap containment is not implemented"));
	}

}
