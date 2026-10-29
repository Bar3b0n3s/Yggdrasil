#include "EnginePCH.h"
#include "Engine/Scene/SpritePicking.h"

namespace Engine {

	Result<std::optional<SceneRaycastHit>> PickSprite(const Scene& /*scene*/, AssetManager& /*assets*/, const SpritePickRequest& /*request*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "2D picking (Docs/Decisions/0032-m21-contract.md decision 14, stream A)");
	}

}
