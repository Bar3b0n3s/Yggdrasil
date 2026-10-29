#include "EnginePCH.h"
#include "Engine/Renderer/PointShadowMaps.h"

// The point-shadow plan (PointShadowMaps.h). Contract stubs of M23's C23.1, implemented by stream B
// (Docs/Decisions/0034-m23-contract.md decision 3): until then no point light is planned.

namespace Engine {

	Result<PointShadowSet> AllocatePointShadows(std::span<const LightData> /*lights*/, std::span<const uint32_t> /*visibleLightIndices*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M23 point-light shadows contract (Docs/Decisions/0034-m23-contract.md decision 3, stream B)");
	}

	glm::mat4 ComputePointShadowFaceViewProjection(const glm::vec3& /*position*/, uint32_t /*face*/, float /*nearClip*/, float /*farClip*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::mat4(1.0f);
	}

}
