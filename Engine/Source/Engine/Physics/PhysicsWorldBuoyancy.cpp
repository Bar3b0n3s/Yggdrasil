#include "EnginePCH.h"
#include "Engine/Physics/PhysicsWorld.h"

// PhysicsWorld's buoyancy (M47, Docs/Decisions/0065-m47-water.md decision 3; Physics/PhysicsBuoyancy.h). C47 freezes the
// interface; W47.2's physics stream implements it.

namespace Engine {

	Result<PhysicsBuoyancyResult> PhysicsWorld::ApplyBuoyancy(BodyHandle /*body*/, std::span<const PhysicsFluidPlane> /*planes*/,
		const PhysicsBuoyancySettings& /*settings*/, float /*deltaTime*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PhysicsWorld::ApplyBuoyancy is not implemented yet (M47's contract C47)");
	}

}
