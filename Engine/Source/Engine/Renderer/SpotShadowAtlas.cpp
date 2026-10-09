#include "EnginePCH.h"
#include "Engine/Renderer/SpotShadowAtlas.h"

namespace Engine {

	Result<SpotShadowAtlas> AllocateSpotShadowAtlas(std::span<const LightData>, std::span<const uint32_t>, const CameraData&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

}
