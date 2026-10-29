#include "EnginePCH.h"
#include "Engine/Asset/SpriteGeometry.h"

namespace Engine {

	std::optional<SpriteFrameGeometry> GetSpriteFrameGeometry(const SpriteSheetData* /*sheet*/, uint32_t /*frame*/, glm::uvec2 /*textureSize*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	SpriteLocalQuad ComputeSpriteLocalQuad(const SpriteFrameGeometry& /*frame*/, float /*pixelsPerUnit*/, bool /*flipX*/, bool /*flipY*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
