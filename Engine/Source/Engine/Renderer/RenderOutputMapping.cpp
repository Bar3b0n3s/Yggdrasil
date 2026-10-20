#include "EnginePCH.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <optional>

// The output stage's view-to-scene mapping (RenderSnapshot.h; Docs/Decisions/0029-m18-contract.md decisions 6 and 8): pure
// integer arithmetic for the scale and the offsets, IEEE-exact float operations for the UI's point mapping.

namespace Engine {

	RenderOutputMapping ComputeRenderOutputMapping(uint32_t /*viewWidth*/, uint32_t /*viewHeight*/, const RenderOutputSettings& /*settings*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::optional<glm::uvec2> MapViewPixelToScenePixel(const RenderOutputMapping& /*mapping*/, uint32_t /*x*/, uint32_t /*y*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	glm::vec2 MapViewPointToScenePoint(const RenderOutputMapping& /*mapping*/, glm::vec2 /*viewPoint*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec2(0.0f);
	}

	Result<std::optional<RenderRayInterval>> ComputeOutputPixelRayInterval(const CameraData& /*camera*/, const RenderOutputMapping& /*mapping*/,
		uint32_t /*x*/, uint32_t /*y*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 output stage contract (Docs/Decisions/0029-m18-contract.md decision 6, stream F)"));
	}

}
