#include "EnginePCH.h"
#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Scene/Private/PhysicsSystemState.h"

#include <utility>

// M11 contract stubs (Docs/Decisions/0014-m11-decisions.md): stream B implements the system; its queries and characters are
// stream C's (PhysicsSystemQueries.cpp, PhysicsSystemCharacters.cpp). Until then Create returns an inert system without a
// world and the step hooks do nothing, so the play session, whose physics phases the contract wired, keeps its M7
// behaviour. The name function is complete.

namespace Engine {

	namespace Utils {

		static std::unexpected<Error> MakeSystemStubError()
		{
			return MakeError(ErrorCode::Unsupported, "the physics system is not implemented yet (M11 stream B)");
		}

	}

	PhysicsSystem::PhysicsSystem(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	PhysicsSystem::~PhysicsSystem() = default;

	Result<Scope<PhysicsSystem>> PhysicsSystem::Create(const PhysicsSystemSpecification& specification)
	{
		ENGINE_CONTRACT_STUB();
		Scope<PhysicsSystem> system = CreateScope<PhysicsSystem>(ConstructionKey());
		system->m_State->Specification = specification;
		return system;
	}

	void PhysicsSystem::PreStep(const SimStep& /*step*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PhysicsSystem::Step(const SimStep& /*step*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PhysicsSystem::PostStep(const SimStep& /*step*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PhysicsSystem::FlushDestroyed(uint64_t /*tick*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PhysicsSystem::SetEventListener(IPhysicsEventListener* listener)
	{
		ENGINE_CONTRACT_STUB();
		m_State->Listener = listener;
	}

	std::span<const PhysicsEvent> PhysicsSystem::GetLastEvents() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->LastEvents;
	}

	Status PhysicsSystem::AddForce(UUID /*entity*/, const glm::vec3& /*force*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	Status PhysicsSystem::AddForceAtPosition(UUID /*entity*/, const glm::vec3& /*force*/, const glm::vec3& /*position*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	Status PhysicsSystem::AddTorque(UUID /*entity*/, const glm::vec3& /*torque*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	Status PhysicsSystem::AddImpulse(UUID /*entity*/, const glm::vec3& /*impulse*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	Status PhysicsSystem::AddAngularImpulse(UUID /*entity*/, const glm::vec3& /*angularImpulse*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	Result<glm::vec3> PhysicsSystem::GetLinearVelocity(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	Status PhysicsSystem::SetLinearVelocity(UUID /*entity*/, const glm::vec3& /*velocity*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	Result<glm::vec3> PhysicsSystem::GetAngularVelocity(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	Status PhysicsSystem::SetAngularVelocity(UUID /*entity*/, const glm::vec3& /*velocity*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	Status PhysicsSystem::MoveKinematic(UUID /*entity*/, const glm::vec3& /*position*/, const glm::quat& /*rotation*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	Status PhysicsSystem::Teleport(UUID /*entity*/, const glm::vec3& /*position*/, std::optional<glm::quat> /*rotation*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	Result<bool> PhysicsSystem::IsSleeping(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	Status PhysicsSystem::WakeUp(UUID /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	const PhysicsLayerTable& PhysicsSystem::GetLayers() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Layers;
	}

	glm::vec3 PhysicsSystem::GetGravity() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Specification.Gravity;
	}

	Status PhysicsSystem::SetGravity(const glm::vec3& /*gravity*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeSystemStubError();
	}

	std::optional<PhysicsBodyReport> PhysicsSystem::GetBodyInfo(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::span<const PhysicsDiagnostic> PhysicsSystem::GetDiagnostics() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Diagnostics;
	}

	PhysicsSystemStats PhysicsSystem::GetStats() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void PhysicsSystem::AppendStateHash(XXH64Hasher& /*hasher*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

	std::string_view PhysicsEventTypeToString(PhysicsEventType type)
	{
		switch (type)
		{
			case PhysicsEventType::CollisionEnter: return "CollisionEnter";
			case PhysicsEventType::CollisionExit:  return "CollisionExit";
			case PhysicsEventType::TriggerEnter:   return "TriggerEnter";
			case PhysicsEventType::TriggerExit:    return "TriggerExit";
		}

		ENGINE_CORE_ASSERT(false, "Unknown PhysicsEventType {}", std::to_underlying(type));
		return "Unknown";
	}

}
