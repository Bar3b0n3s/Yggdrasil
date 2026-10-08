#include "EnginePCH.h"
#include "Engine/Scene/PhysicsComposition.h"

#include "Engine/Core/Assert.h"

#include <utility>

// M11 contract stubs (Docs/Decisions/0014-m11-decisions.md): stream B implements the composition rules and the mesh shape
// cache. The name function is complete.

namespace Engine {

	// The cache's entries; stream B adds them (keyed by mesh handle, version, convex flag and the scale's bits).
	struct PhysicsMeshShapeCache::State
	{
	};

	PhysicsMeshShapeCache::PhysicsMeshShapeCache()
		: m_State(CreateScope<State>())
	{
	}

	PhysicsMeshShapeCache::~PhysicsMeshShapeCache() = default;

	Result<Ref<const PhysicsShape>> PhysicsMeshShapeCache::GetOrCreate(AssetManager& /*assets*/, AssetHandle /*mesh*/, bool /*convex*/,
		const glm::vec3& /*scale*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the mesh shape cache is not implemented yet (M11 stream B)");
	}

	void PhysicsMeshShapeCache::Prune()
	{
		ENGINE_CONTRACT_STUB();
	}

	void PhysicsMeshShapeCache::Clear()
	{
		ENGINE_CONTRACT_STUB();
	}

	size_t PhysicsMeshShapeCache::GetSize() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	PhysicsComposition ComposePhysicsBodies(const Scene& /*scene*/, const PhysicsLayerTable& /*layers*/, uint32_t /*maxBodies*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<BodyShapeDescription> DescribePhysicsBodyShape(const Scene& /*scene*/, const PhysicsBodyPlan& /*plan*/, AssetManager* /*assets*/,
		PhysicsMeshShapeCache* /*meshShapes*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the physics composition is not implemented yet (M11 stream B)");
	}

	std::string_view PhysicsBodyOriginToString(PhysicsBodyOrigin origin)
	{
		switch (origin)
		{
			case PhysicsBodyOrigin::RigidBody:      return "RigidBody";
			case PhysicsBodyOrigin::ImplicitStatic: return "ImplicitStatic";
			case PhysicsBodyOrigin::ImplicitSensor: return "ImplicitSensor";
			case PhysicsBodyOrigin::Character:      return "Character";
		}

		ENGINE_CORE_ASSERT(false, "Unknown PhysicsBodyOrigin {}", std::to_underlying(origin));
		return "Unknown";
	}

}
