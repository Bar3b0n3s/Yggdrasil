#include "EnginePCH.h"
#include "Engine/Physics/PhysicsShape.h"

#include "Engine/Physics/Private/PhysicsWorldState.h"

// M11 contract stubs (Docs/Decisions/0014-m11-decisions.md): stream A implements the shapes.

namespace Engine {

	PhysicsShape::PhysicsShape(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	PhysicsShape::~PhysicsShape() = default;

	Result<Ref<const PhysicsShape>> PhysicsShape::Create(const BodyShapeDescription& /*description*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "physics shapes are not implemented yet (M11 stream A)");
	}

	bool PhysicsShape::IsValidScale(const PhysicsShapeGeometry& /*geometry*/, const glm::vec3& /*scale*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	Aabb PhysicsShape::GetLocalBounds() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	uint32_t PhysicsShape::GetColliderCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::optional<Aabb> PhysicsShape::GetColliderLocalBounds(uint32_t /*userData*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

}
