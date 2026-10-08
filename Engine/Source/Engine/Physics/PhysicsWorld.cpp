#include "EnginePCH.h"
#include "Engine/Physics/PhysicsWorld.h"

#include "Engine/Physics/Private/PhysicsWorldState.h"

// M11 contract stubs (Docs/Decisions/0014-m11-decisions.md): stream A implements the world; its queries are stream C's
// (PhysicsWorldQueries.cpp).

namespace Engine {

	namespace Utils {

		static std::unexpected<Error> MakeWorldStubError()
		{
			return MakeError(ErrorCode::Unsupported, "the physics world is not implemented yet (M11 stream A)");
		}

	}

	PhysicsWorld::PhysicsWorld(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	PhysicsWorld::~PhysicsWorld() = default;

	Result<Scope<PhysicsWorld>> PhysicsWorld::Create(const PhysicsWorldSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeWorldStubError();
	}

	Result<BodyHandle> PhysicsWorld::CreateBody(const BodyDescription& /*description*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeWorldStubError();
	}

	void PhysicsWorld::DestroyBody(BodyHandle /*body*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool PhysicsWorld::IsBodyValid(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	uint64_t PhysicsWorld::GetUserData(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	PhysicsMotionType PhysicsWorld::GetMotionType(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return PhysicsMotionType::Static;
	}

	bool PhysicsWorld::IsSensor(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	uint32_t PhysicsWorld::GetLayer(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Ref<const PhysicsShape> PhysicsWorld::GetShape(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	Status PhysicsWorld::SetShape(BodyHandle /*body*/, const Ref<const PhysicsShape>& /*shape*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeWorldStubError();
	}

	PhysicsPose PhysicsWorld::GetPose(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void PhysicsWorld::SetPose(BodyHandle /*body*/, const PhysicsPose& /*pose*/, bool /*activate*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PhysicsWorld::MoveKinematic(BodyHandle /*body*/, const PhysicsPose& /*target*/, float /*deltaTime*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PhysicsWorld::AddForce(BodyHandle /*body*/, const glm::vec3& /*force*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PhysicsWorld::AddForceAtPosition(BodyHandle /*body*/, const glm::vec3& /*force*/, const glm::vec3& /*position*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PhysicsWorld::AddTorque(BodyHandle /*body*/, const glm::vec3& /*torque*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PhysicsWorld::AddImpulse(BodyHandle /*body*/, const glm::vec3& /*impulse*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PhysicsWorld::AddAngularImpulse(BodyHandle /*body*/, const glm::vec3& /*angularImpulse*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	glm::vec3 PhysicsWorld::GetLinearVelocity(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

	void PhysicsWorld::SetLinearVelocity(BodyHandle /*body*/, const glm::vec3& /*velocity*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	glm::vec3 PhysicsWorld::GetAngularVelocity(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

	void PhysicsWorld::SetAngularVelocity(BodyHandle /*body*/, const glm::vec3& /*velocity*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool PhysicsWorld::IsSleeping(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return true;
	}

	void PhysicsWorld::WakeUp(BodyHandle /*body*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status PhysicsWorld::Step(float /*deltaTime*/, uint32_t /*collisionSteps*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeWorldStubError();
	}

	glm::vec3 PhysicsWorld::GetGravity() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Specification.Gravity;
	}

	Status PhysicsWorld::SetGravity(const glm::vec3& /*gravity*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeWorldStubError();
	}

	std::vector<ContactEvent> PhysicsWorld::DrainContactEvents()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	ContactBuffer& PhysicsWorld::GetContactBuffer()
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Contacts;
	}

	const PhysicsLayerTable& PhysicsWorld::GetLayers() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Specification.Layers;
	}

	const PhysicsWorldLimits& PhysicsWorld::GetLimits() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Specification.Limits;
	}

	PhysicsWorldStats PhysicsWorld::GetStats() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
