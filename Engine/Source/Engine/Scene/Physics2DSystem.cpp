#include "EnginePCH.h"
#include "Engine/Scene/Physics2DSystem.h"

// The M21 contract's stubs (Docs/Decisions/0032-m21-contract.md decisions 9 to 13): stream P2 implements the 2D system.

namespace Engine {

	namespace Utils {

		static std::unexpected<Error> MakePhysics2DSystemStubError()
		{
			return MakeError(ErrorCode::Unsupported, "the 2D physics system (Docs/Decisions/0032-m21-contract.md decision 9, stream P2)");
		}

	}

	struct Physics2DSystem::State
	{
		PhysicsLayerTable Layers{};
	};

	Physics2DSystem::Physics2DSystem(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	Physics2DSystem::~Physics2DSystem() = default;

	Result<Scope<Physics2DSystem>> Physics2DSystem::Create(const Physics2DSystemSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	void Physics2DSystem::PreStep(const SimStep& /*step*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Physics2DSystem::Step(const SimStep& /*step*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Physics2DSystem::PostStep(const SimStep& /*step*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Physics2DSystem::FlushDestroyed(uint64_t /*tick*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Physics2DSystem::SetEventListener(IPhysics2DEventListener* /*listener*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::span<const PhysicsEvent> Physics2DSystem::GetLastEvents() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status Physics2DSystem::AddForce(UUID /*entity*/, const glm::vec2& /*force*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Status Physics2DSystem::AddForceAtPosition(UUID /*entity*/, const glm::vec2& /*force*/, const glm::vec2& /*position*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Status Physics2DSystem::AddTorque(UUID /*entity*/, float /*torque*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Status Physics2DSystem::AddImpulse(UUID /*entity*/, const glm::vec2& /*impulse*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Status Physics2DSystem::AddAngularImpulse(UUID /*entity*/, float /*angularImpulse*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Result<glm::vec2> Physics2DSystem::GetLinearVelocity(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Status Physics2DSystem::SetLinearVelocity(UUID /*entity*/, const glm::vec2& /*velocity*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Result<float> Physics2DSystem::GetAngularVelocity(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Status Physics2DSystem::SetAngularVelocity(UUID /*entity*/, float /*velocity*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Status Physics2DSystem::MoveKinematic(UUID /*entity*/, const glm::vec2& /*position*/, float /*angle*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Status Physics2DSystem::Teleport(UUID /*entity*/, const glm::vec2& /*position*/, std::optional<float> /*angle*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Result<bool> Physics2DSystem::IsSleeping(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Status Physics2DSystem::WakeUp(UUID /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Result<std::optional<Physics2DRaycastHit>> Physics2DSystem::Raycast(const glm::vec2& /*origin*/, const glm::vec2& /*direction*/, float /*maxDistance*/,
		PhysicsLayerMask /*layers*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Result<std::vector<Physics2DRaycastHit>> Physics2DSystem::RaycastAll(const glm::vec2& /*origin*/, const glm::vec2& /*direction*/, float /*maxDistance*/,
		PhysicsLayerMask /*layers*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Result<std::vector<PhysicsOverlapHit>> Physics2DSystem::OverlapCircle(const glm::vec2& /*center*/, float /*radius*/, PhysicsLayerMask /*layers*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	Result<std::vector<PhysicsOverlapHit>> Physics2DSystem::OverlapBox(const glm::vec2& /*center*/, const glm::vec2& /*halfExtents*/, float /*angle*/,
		PhysicsLayerMask /*layers*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	std::optional<Aabb> Physics2DSystem::GetBodyBounds(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::optional<Aabb> Physics2DSystem::GetColliderBounds(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	const PhysicsLayerTable& Physics2DSystem::GetLayers() const
	{
		return m_State->Layers;
	}

	glm::vec2 Physics2DSystem::GetGravity() const
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec2(0.0f);
	}

	Status Physics2DSystem::SetGravity(const glm::vec2& /*gravity*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakePhysics2DSystemStubError();
	}

	std::span<const PhysicsDiagnostic> Physics2DSystem::GetDiagnostics() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Physics2DSystemStats Physics2DSystem::GetStats() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void Physics2DSystem::AppendStateHash(XXH64Hasher& /*hasher*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

}
