#include "EnginePCH.h"
#include "Engine/Physics/Physics2DWorld.h"

// The M21 contract's stubs (Docs/Decisions/0032-m21-contract.md decision 8): stream P1 implements the 2D world.

namespace Engine {

	namespace Utils {

		static std::unexpected<Error> MakePhysics2DWorldStubError()
		{
			return MakeError(ErrorCode::Unsupported, "the 2D physics world (Docs/Decisions/0032-m21-contract.md decision 8, stream P1)");
		}

	}

	struct Physics2DWorld::State
	{
		PhysicsLayerTable Layers{};
		Scope<PhysicsWorld> World{};
	};

	PhysicsPose ToPhysicsPose(const Physics2DPose& /*pose*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Physics2DPose ToPhysics2DPose(const PhysicsPose& /*pose*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	float GetPhysics2DHalfDepth(const Shape2DGeometry& /*geometry*/, const glm::vec2& /*scale*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

	Result<BodyShapeDescription> MakeBody2DShape(const Body2DShapeDescription& /*description*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DWorldStubError();
	}

	Physics2DWorld::Physics2DWorld(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	Physics2DWorld::~Physics2DWorld() = default;

	Result<Scope<Physics2DWorld>> Physics2DWorld::Create(const Physics2DWorldSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DWorldStubError();
	}

	Result<BodyHandle> Physics2DWorld::CreateBody(const Body2DDescription& /*description*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DWorldStubError();
	}

	void Physics2DWorld::DestroyBody(BodyHandle /*body*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool Physics2DWorld::IsBodyValid(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	uint64_t Physics2DWorld::GetUserData(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	PhysicsMotionType Physics2DWorld::GetMotionType(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return PhysicsMotionType::Static;
	}

	bool Physics2DWorld::IsSensor(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	uint32_t Physics2DWorld::GetLayer(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Ref<const PhysicsShape> Physics2DWorld::GetShape(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status Physics2DWorld::SetShape(BodyHandle /*body*/, const Ref<const PhysicsShape>& /*shape*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DWorldStubError();
	}

	uint32_t Physics2DWorld::GetSubShapeCollider(BodyHandle /*body*/, uint32_t /*subShape*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Physics2DPose Physics2DWorld::GetPose(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void Physics2DWorld::SetPose(BodyHandle /*body*/, const Physics2DPose& /*pose*/, bool /*activate*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Physics2DWorld::MoveKinematic(BodyHandle /*body*/, const Physics2DPose& /*target*/, float /*deltaTime*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Physics2DWorld::AddForce(BodyHandle /*body*/, const glm::vec2& /*force*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Physics2DWorld::AddForceAtPosition(BodyHandle /*body*/, const glm::vec2& /*force*/, const glm::vec2& /*position*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Physics2DWorld::AddTorque(BodyHandle /*body*/, float /*torque*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Physics2DWorld::AddImpulse(BodyHandle /*body*/, const glm::vec2& /*impulse*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Physics2DWorld::AddAngularImpulse(BodyHandle /*body*/, float /*angularImpulse*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	glm::vec2 Physics2DWorld::GetLinearVelocity(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec2(0.0f);
	}

	void Physics2DWorld::SetLinearVelocity(BodyHandle /*body*/, const glm::vec2& /*velocity*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	float Physics2DWorld::GetAngularVelocity(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

	void Physics2DWorld::SetAngularVelocity(BodyHandle /*body*/, float /*velocity*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool Physics2DWorld::IsSleeping(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void Physics2DWorld::WakeUp(BodyHandle /*body*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::optional<PhysicsQueryHit> Physics2DWorld::Raycast(const glm::vec2& /*origin*/, const glm::vec2& /*direction*/, float /*maxDistance*/,
		const PhysicsQueryFilter& /*filter*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::vector<PhysicsQueryHit> Physics2DWorld::RaycastAll(const glm::vec2& /*origin*/, const glm::vec2& /*direction*/, float /*maxDistance*/,
		const PhysicsQueryFilter& /*filter*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<PhysicsOverlap> Physics2DWorld::Overlap(const Overlap2DShape& /*shape*/, const Physics2DPose& /*pose*/, const PhysicsQueryFilter& /*filter*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::optional<Aabb> Physics2DWorld::GetBodyBounds(BodyHandle /*body*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::optional<Aabb> Physics2DWorld::GetSubShapeBounds(BodyHandle /*body*/, uint32_t /*collider*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	Status Physics2DWorld::Step(float /*deltaTime*/, uint32_t /*collisionSteps*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DWorldStubError();
	}

	glm::vec2 Physics2DWorld::GetGravity() const
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec2(0.0f);
	}

	Status Physics2DWorld::SetGravity(const glm::vec2& /*gravity*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DWorldStubError();
	}

	std::vector<ContactEvent> Physics2DWorld::DrainContactEvents()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	const PhysicsLayerTable& Physics2DWorld::GetLayers() const
	{
		return m_State->Layers;
	}

	PhysicsWorldStats Physics2DWorld::GetStats() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	const PhysicsWorld* Physics2DWorld::GetWorld() const
	{
		return m_State->World.get();
	}

}
