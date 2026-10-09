#include "EditorPCH.h"
#include "EditorCore/Project/Basic3DTemplate.h"

#include <nlohmann/json.hpp>

namespace Engine {

	Result<Json> BuildBasic3DScene(const TypeRegistry& /*registry*/, UUIDGenerator& /*ids*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Result<CreatedProject> CreateBasic3DProject(const ProjectCreateSpecification& /*specification*/, const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
