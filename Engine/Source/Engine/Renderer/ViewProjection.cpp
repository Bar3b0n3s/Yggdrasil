#include "EnginePCH.h"
#include "Engine/Renderer/RenderSnapshot.h"

namespace Engine {

	Result<RenderRay> ComputeViewPixelRay(const CameraData&, uint32_t, uint32_t)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 view pixel ray is not implemented"));
	}

	Result<RenderRayInterval> ComputeViewPixelRayInterval(const CameraData&, uint32_t, uint32_t)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 clipped pixel ray is not implemented"));
	}

	glm::vec3 GetOverdrawDebugColor(uint32_t)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

	glm::vec3 GetShadowCascadeDebugColor(uint32_t)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

}
