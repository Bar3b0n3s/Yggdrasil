#include "EnginePCH.h"
#include "Engine/Renderer/RenderSnapshot.h"

namespace Engine {

	glm::mat4 ComputeReverseZProjection(RenderProjection /*projection*/, float /*verticalFovDegrees*/, float /*orthographicSize*/,
		float /*nearClip*/, float /*farClip*/, uint32_t /*width*/, uint32_t /*height*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::mat4(1.0f);
	}

}
