#include "EnginePCH.h"
#include "Engine/Scene/ColliderDebugDraw.h"

// M11 contract stub (Docs/Decisions/0014-m11-decisions.md): stream D implements the collider visualization.

namespace Engine {

	std::vector<ColliderDebugShape> BuildColliderDebugDraw(const Scene& /*scene*/, const PhysicsLayerTable& /*layers*/, const PhysicsSystem* /*physics*/,
		AssetManager* /*assets*/, const ColliderDebugDrawOptions& /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
