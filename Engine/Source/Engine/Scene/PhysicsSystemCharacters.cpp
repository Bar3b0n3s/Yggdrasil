#include "EnginePCH.h"
#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Scene/Private/PhysicsSystemState.h"

// The characters of the scene (Architecture §9.6): M11 contract stubs (Docs/Decisions/0014-m11-decisions.md); stream C
// implements them: their creation in canonical order among the bodies, their PreStep update and write-back (called from
// stream B's step through helpers declared in Scene/Private/), and the script-facing functions below.

namespace Engine {

	Status PhysicsSystem::MoveCharacter(UUID /*entity*/, const glm::vec3& /*velocity*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "characters are not implemented yet (M11 stream C)");
	}

	Result<PhysicsCharacterState> PhysicsSystem::GetCharacterState(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "characters are not implemented yet (M11 stream C)");
	}

}
