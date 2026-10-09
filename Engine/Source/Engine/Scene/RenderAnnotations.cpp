#include "EnginePCH.h"
#include "Engine/Scene/RenderAnnotations.h"

namespace Engine {

	Status AppendRenderAnnotations(const Scene&, AssetManager&, const PhysicsLayerTable&, const PhysicsSystem*, RenderSnapshot&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	Result<RenderSceneValidation> EvaluateRenderSceneValidation(const Scene&, AssetManager&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 render validation is not implemented"));
	}

}
