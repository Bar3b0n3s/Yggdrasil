#include "EnginePCH.h"
#include "Engine/Scene/PhysicsValidation.h"

// M11 contract stub (Docs/Decisions/0014-m11-decisions.md): stream D implements the checks. It reports nothing until then,
// so project.validate keeps its M6 behaviour.

namespace Engine {

	std::vector<PhysicsDiagnostic> ValidateScenePhysics(const Scene& /*scene*/, const PhysicsLayerTable& /*layers*/, AssetManager* /*assets*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
