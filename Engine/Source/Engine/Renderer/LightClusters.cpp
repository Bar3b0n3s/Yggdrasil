#include "EnginePCH.h"
#include "Engine/Renderer/LightClusters.h"

// The CPU side of the cluster grid (LightClusters.h). Contract stubs of M23's C23.1, implemented by stream C
// (Docs/Decisions/0034-m23-contract.md decision 5): until then the grid is empty, every pixel is cluster 0 and every count
// draws black.

namespace Engine {

	LightClusterGrid ComputeLightClusterGrid(const CameraData& /*camera*/, uint32_t /*width*/, uint32_t /*height*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	uint32_t GetLightClusterIndex(const LightClusterGrid& /*grid*/, uint32_t /*x*/, uint32_t /*y*/, float /*viewDepth*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	glm::vec3 GetLightCountDebugColor(uint32_t /*count*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

}
