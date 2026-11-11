#include "EnginePCH.h"
#include "Engine/Scene/ParticleSimulation.h"

// The particle simulation (ParticleSimulation.h): contract stubs of M26's C26.1, implemented by stream P-A
// (Docs/Decisions/0037-m26-contract.md decision 5).

namespace Engine {

	uint64_t MakeParticleEmitterSeed(UUID /*entity*/, uint32_t /*seed*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	void StepParticleEmitter(const ParticleEmitterComponent& /*emitter*/, const glm::mat4& /*emitterWorld*/, float /*deltaTime*/,
		ParticleEmitterState& /*state*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	glm::vec4 EvaluateParticleColor(const ParticleEmitterComponent& /*emitter*/, float /*normalizedAge*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec4(0.0f);
	}

	float EvaluateParticleSize(const ParticleEmitterComponent& /*emitter*/, float /*normalizedAge*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

	uint64_t ComputeParticleDigest(const ParticleEmitterState& /*state*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
