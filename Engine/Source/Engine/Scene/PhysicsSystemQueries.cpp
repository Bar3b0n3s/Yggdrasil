#include "EnginePCH.h"
#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Scene/Private/PhysicsSystemState.h"

// The scene-level queries and bounds (Architecture §9.5): M11 contract stubs (Docs/Decisions/0014-m11-decisions.md);
// stream C implements them over the system's PhysicsWorld, resolving hits to collider and body entities.

namespace Engine {

	namespace Utils {

		static std::unexpected<Error> MakeQueryStubError()
		{
			return MakeError(ErrorCode::Unsupported, "physics queries are not implemented yet (M11 stream C)");
		}

	}

	Result<std::optional<PhysicsRaycastHit>> PhysicsSystem::Raycast(const glm::vec3& /*origin*/, const glm::vec3& /*direction*/, float /*maxDistance*/,
		PhysicsLayerMask /*layers*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeQueryStubError();
	}

	Result<std::vector<PhysicsRaycastHit>> PhysicsSystem::RaycastAll(const glm::vec3& /*origin*/, const glm::vec3& /*direction*/, float /*maxDistance*/,
		PhysicsLayerMask /*layers*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeQueryStubError();
	}

	Result<std::optional<PhysicsRaycastHit>> PhysicsSystem::SphereCast(const glm::vec3& /*origin*/, float /*radius*/, const glm::vec3& /*direction*/,
		float /*maxDistance*/, PhysicsLayerMask /*layers*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeQueryStubError();
	}

	Result<std::vector<PhysicsOverlapHit>> PhysicsSystem::OverlapSphere(const glm::vec3& /*center*/, float /*radius*/, PhysicsLayerMask /*layers*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeQueryStubError();
	}

	Result<std::vector<PhysicsOverlapHit>> PhysicsSystem::OverlapBox(const glm::vec3& /*center*/, const glm::vec3& /*halfExtents*/,
		const glm::quat& /*rotation*/, PhysicsLayerMask /*layers*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeQueryStubError();
	}

	std::optional<Aabb> PhysicsSystem::GetBodyBounds(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::optional<Aabb> PhysicsSystem::GetColliderBounds(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	Result<PhysicsLayerMask> PhysicsSystem::MakeLayerMask(std::span<const std::string_view> /*names*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeQueryStubError();
	}

}
