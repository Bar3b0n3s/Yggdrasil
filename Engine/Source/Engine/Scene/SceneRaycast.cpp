#include "EnginePCH.h"
#include "Engine/Scene/SceneRaycast.h"

namespace Engine {

	Result<std::optional<SceneRaycastHit>> RaycastScene(const Scene&, AssetManager&, const PhysicsLayerTable&, const SceneRaycastRequest&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

}
