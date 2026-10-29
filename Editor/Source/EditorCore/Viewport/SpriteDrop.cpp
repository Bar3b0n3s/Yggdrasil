#include "EditorPCH.h"
#include "EditorCore/Viewport/SpriteDrop.h"

#include <nlohmann/json.hpp>

namespace Engine {

	std::optional<glm::vec3> IntersectSpritePlane(const RenderRay& /*ray*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	Result<Json> MakeSpriteDropParams(const SpriteDropRequest& /*request*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "dropping sprites into the scene view (Docs/Decisions/0032-m21-contract.md decision 14, stream D)");
	}

}
