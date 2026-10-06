#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

// M3 contract stub (Roadmap rule 3): stream D (components) registers, in this order (Docs/Decisions/0006-m3-decisions.md,
// decision 9, has the flags, relations and field metadata):
//   enums BodyType, MotionQuality;
//   RigidBody (Requires Transform, Excludes CharacterController; Mass >= 0.001 kg; EnhancedInternalEdgeRemoval),
//   BoxCollider, SphereCollider, CapsuleCollider (Requires Transform; HalfExtents, Radius, HalfHeight >=
//   MinColliderDimension), MeshCollider (Requires Transform), CharacterController (Requires Transform, Excludes
//   RigidBody).

namespace Engine {

	void RegisterPhysicsComponents(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
