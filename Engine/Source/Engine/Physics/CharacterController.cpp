#include "EnginePCH.h"
#include "Engine/Physics/CharacterController.h"

#include "Engine/Physics/PhysicsWorld.h"

// M11 contract stubs (Docs/Decisions/0014-m11-decisions.md): stream C implements the controller over the world's Jolt
// objects (Physics/Private/PhysicsWorldState.h).

namespace Engine {

	struct CharacterController::State
	{
	};

	CharacterController::CharacterController(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	CharacterController::~CharacterController() = default;

	Result<Scope<CharacterController>> CharacterController::Create(PhysicsWorld& /*world*/, const CharacterControllerDescription& /*description*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the character controller is not implemented yet (M11 stream C)");
	}

	void CharacterController::Update(float /*deltaTime*/, const glm::vec3& /*desiredVelocity*/, const glm::vec3& /*gravity*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	PhysicsPose CharacterController::GetPose() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void CharacterController::SetPose(const PhysicsPose& /*pose*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	glm::vec3 CharacterController::GetVelocity() const
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

	void CharacterController::SetVelocity(const glm::vec3& /*velocity*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	CharacterGroundState CharacterController::GetGroundState() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	BodyHandle CharacterController::GetInnerBody() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	uint64_t CharacterController::GetUserData() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
