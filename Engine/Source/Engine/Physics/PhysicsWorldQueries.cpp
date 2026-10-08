#include "EnginePCH.h"
#include "Engine/Physics/PhysicsWorld.h"

#include "Engine/Physics/Private/PhysicsWorldState.h"

// The world's queries (Architecture §9.5): M11 contract stubs (Docs/Decisions/0014-m11-decisions.md); stream C implements
// them through the world's JPH::PhysicsSystem (its NarrowPhaseQuery).

namespace Engine {

	std::optional<PhysicsQueryHit> PhysicsWorld::Raycast(const PhysicsRay& /*ray*/, const PhysicsQueryFilter& /*filter*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::vector<PhysicsQueryHit> PhysicsWorld::RaycastAll(const PhysicsRay& /*ray*/, const PhysicsQueryFilter& /*filter*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::optional<PhysicsQueryHit> PhysicsWorld::ShapeCast(const PhysicsShapeGeometry& /*shape*/, const PhysicsPose& /*start*/,
		const glm::vec3& /*direction*/, float /*maxDistance*/, const PhysicsQueryFilter& /*filter*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::vector<PhysicsOverlap> PhysicsWorld::Overlap(const PhysicsShapeGeometry& /*shape*/, const PhysicsPose& /*pose*/,
		const PhysicsQueryFilter& /*filter*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::optional<Aabb> PhysicsWorld::GetBodyBounds(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::optional<Aabb> PhysicsWorld::GetSubShapeBounds(BodyHandle /*body*/, uint32_t /*collider*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

}
