#include "EnginePCH.h"
#include "Engine/Scene/TransformSystem.h"

#include "Engine/Core/DetMath.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

// M3 contract stub (Roadmap rule 3): stream B (Scene core) implements world transforms with IEEE-exact operations and
// DetMath only (simulation path, §4.12).

namespace Engine {

	void TransformSystem::Update(Scene& /*scene*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	glm::mat4 TransformSystem::ComputeLocalMatrix(const TransformComponent& /*transform*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::mat4(1.0f);
	}

	glm::mat4 TransformSystem::ComputeWorldMatrix(ConstEntity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::mat4(1.0f);
	}

	Result<TransformDecomposition> TransformSystem::DecomposeMatrix(const glm::mat4& /*matrix*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "TransformSystem::DecomposeMatrix is an M3 contract stub");
	}

	glm::vec3 TransformSystem::GetWorldPosition(ConstEntity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

	void TransformSystem::SetWorldPosition(Entity /*entity*/, const glm::vec3& /*position*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	glm::quat TransformSystem::GetWorldRotation(ConstEntity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	}

	void TransformSystem::SetWorldRotation(Entity /*entity*/, const glm::quat& /*rotation*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	glm::vec3 TransformSystem::GetWorldScale(ConstEntity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(1.0f);
	}

	glm::vec3 TransformSystem::GetRenderPosition(ConstEntity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

	glm::quat TransformSystem::GetRenderRotation(ConstEntity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	}

	glm::quat TransformSystem::QuaternionFromEulerDegrees(const glm::vec3& /*degrees*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	}

	glm::vec3 TransformSystem::EulerDegreesFromQuaternion(const glm::quat& /*rotation*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

}
