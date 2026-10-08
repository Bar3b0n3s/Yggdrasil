#include "EnginePCH.h"
#include "Engine/Scene/RenderExtraction.h"

#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

namespace Engine {

	ConstEntity FindPrimaryCamera(const Scene& /*scene*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	glm::mat4 ComputeRenderedWorldMatrix(ConstEntity entity, float /*alpha*/)
	{
		ENGINE_CONTRACT_STUB();
		return TransformSystem::ComputeWorldMatrix(entity);
	}

	Result<RenderSnapshot> ExtractRenderSnapshot(const Scene& /*scene*/, const RenderExtractionRequest& /*request*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "render extraction is not implemented yet (M7 stream B)");
	}

}
