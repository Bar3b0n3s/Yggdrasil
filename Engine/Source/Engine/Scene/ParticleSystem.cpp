#include "EnginePCH.h"
#include "Engine/Scene/ParticleSystem.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scene/Entity.h"

#include <cmath>

// The Particles phase's system (ParticleSystem.h). The construction and the accessors are the contract's; the step and the
// script entries are contract stubs of M26's C26.1, implemented by stream P-A (Docs/Decisions/0037-m26-contract.md decision
// 5). No session creates the system until then.

namespace Engine {

	ParticleSystem::ParticleSystem(float fixedDelta)
		: m_FixedDelta(fixedDelta)
	{
		ENGINE_CORE_ASSERT(std::isfinite(fixedDelta) && fixedDelta > 0.0f, "A particle step of {} s", fixedDelta);
	}

	void ParticleSystem::Step(Scene& /*scene*/, uint64_t /*tick*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status QueueParticleEmit(Entity /*entity*/, uint32_t /*count*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 particles contract (Docs/Decisions/0037-m26-contract.md decision 5, stream P-A)");
	}

	Status ClearParticles(Entity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 particles contract (Docs/Decisions/0037-m26-contract.md decision 5, stream P-A)");
	}

	std::optional<uint64_t> FindParticleDigest(const Entity& /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

}
