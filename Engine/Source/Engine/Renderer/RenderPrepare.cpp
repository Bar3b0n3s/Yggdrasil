#include "EnginePCH.h"
#include "Engine/Renderer/RenderPrepare.h"

namespace Engine {

	bool IsOutsideView(const Aabb& /*bounds*/, const glm::mat4& /*world*/, const CameraData& /*camera*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	LightCullResult CullLights(std::span<const LightData> /*lights*/, const CameraData& /*camera*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void SortOpaqueDraws(std::span<OpaqueSortKey> /*keys*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void SortTransparentDraws(std::span<TransparentSortKey> /*keys*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
