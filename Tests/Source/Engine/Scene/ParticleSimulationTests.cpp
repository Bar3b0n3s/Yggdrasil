#include "TestsPCH.h"

#include "Engine/Scene/ParticleSimulation.h"

#include "Engine/Core/UUID.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <format>
#include <string>

// The particle simulation of one emitter (M26, C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 5, stream P-A):
// skipped until stream P-A implements it.

namespace Engine {

	namespace {

		constexpr float FixedDelta = 1.0f / 60.0f;

		// A busy emitter: a cone with a burst, a rate, gravity, drag and both curves.
		ParticleEmitterComponent MakeFountain()
		{
			ParticleEmitterComponent emitter;
			emitter.Rate = 120.0f;
			emitter.Burst = 16;
			emitter.MaxParticles = 300;
			emitter.Seed = 9;
			emitter.Acceleration = glm::vec3(0.0f, -9.81f, 0.0f);
			emitter.Drag = 0.25f;
			emitter.ColorOverLife = { { .Time = 0.0f, .Color = glm::vec4(1.0f, 0.5f, 0.0f, 1.0f) }, { .Time = 1.0f, .Color = glm::vec4(0.2f, 0.2f, 0.2f, 0.0f) } };
			emitter.SizeOverLife = { { .Time = 0.0f, .Size = 0.5f }, { .Time = 1.0f, .Size = 2.0f } };
			return emitter;
		}

		// `steps` steps of `emitter`, which moves along +X by 0.1 m a step.
		ParticleEmitterState Run(const ParticleEmitterComponent& emitter, UUID entity, uint32_t steps)
		{
			ParticleEmitterState state(MakeParticleEmitterSeed(entity, emitter.Seed));
			for (uint32_t step = 0; step < steps; ++step)
			{
				const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(0.1f * static_cast<float>(step), 1.0f, 0.0f));
				StepParticleEmitter(emitter, world, FixedDelta, state);
			}
			return state;
		}

	}

	TEST_SUITE("Scene")
	{
		// The acceptance "the seeded particle hash is equal across configurations": stream P-A commits the digest from Debug and
		// confirms it in Release; FeatureTest's Particles suite checks Test.GetParticleDigest in the Editor, Release and Dist runs
		// (decision 25), since the Tests executable does not exist in Dist.
		TEST_CASE("ParticleSimulation: a seeded emitter's particles hash to their committed digest in every configuration" * doctest::skip(true))
		{
			const ParticleEmitterState state = Run(MakeFountain(), UUID(0x7a1c3e5f20b4d608), 240);
			CHECK_FALSE(state.Particles.empty());
			CHECK(std::format("{:016x}", ComputeParticleDigest(state)) == std::string("0000000000000000"));
		}

		TEST_CASE("ParticleSimulation: one emitter repeats exactly, and two emitters with one Seed differ" * doctest::skip(true))
		{
			const ParticleEmitterComponent emitter = MakeFountain();
			const ParticleEmitterState first = Run(emitter, UUID(0x1111), 90);
			const ParticleEmitterState again = Run(emitter, UUID(0x1111), 90);
			const ParticleEmitterState other = Run(emitter, UUID(0x2222), 90);
			CHECK(first.Particles == again.Particles);
			CHECK(ComputeParticleDigest(first) == ComputeParticleDigest(again));
			CHECK(ComputeParticleDigest(first) != ComputeParticleDigest(other));
			CHECK(MakeParticleEmitterSeed(UUID(0x1111), 9) != MakeParticleEmitterSeed(UUID(0x2222), 9));
			CHECK(MakeParticleEmitterSeed(UUID(0x1111), 9) != MakeParticleEmitterSeed(UUID(0x1111), 10));
		}

		TEST_CASE("ParticleSimulation: a step ages, moves and kills particles, and emits its burst and rate up to its cap" * doctest::skip(true))
		{
			// The header's order: integrate, age and kill, then the burst at the cycle's first step, then the rate (60 * (1/60.0f)
			// is exactly 1.0f, so one particle a step), each while fewer than MaxParticles live.
			ParticleEmitterComponent emitter;
			emitter.Rate = 60.0f; // one particle a step at 60 Hz
			emitter.Burst = 5;
			emitter.MaxParticles = 7;
			emitter.Lifetime = glm::vec2(0.05f, 0.05f); // three steps: an age of three steps' sum rounds to above 0.05f
			emitter.Speed = glm::vec2(0.0f, 0.0f);
			emitter.Acceleration = glm::vec3(0.0f, 60.0f, 0.0f);
			ParticleEmitterState state(MakeParticleEmitterSeed(UUID(0x33), 0));
			StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state);
			// The burst and the step's one rate particle.
			REQUIRE(state.Particles.size() == 6);
			StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state);
			// The six move: velocity 1 m/s after one acceleration step, so 1/60 m up; a seventh is emitted, reaching the cap.
			CHECK(state.Particles.front().Velocity.y == doctest::Approx(1.0f));
			CHECK(state.Particles.front().Position.y == doctest::Approx(1.0f / 60.0f));
			CHECK(state.Particles.size() == 7);
			CHECK(state.Capped == 0);
			StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state);
			// Nothing dies yet, so the cap drops the step's rate particle.
			CHECK(state.Particles.size() == 7);
			CHECK(state.Capped == 1);
			StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state);
			// The first six reached their lifetime and died; the survivor and the step's rate particle remain, in emission order.
			CHECK(state.Particles.size() == 2);
			CHECK(state.Capped == 1);
			for (const Particle& particle : state.Particles)
				CHECK(particle.Age < particle.Lifetime);
			CHECK(state.Particles.back().Age == 0.0f);
			CHECK(state.Steps == 4);
		}

		TEST_CASE("ParticleSimulation: over-life curves hold before the first key and after the last, and are linear between" * doctest::skip(true))
		{
			const ParticleEmitterComponent emitter = MakeFountain();
			CHECK(EvaluateParticleColor(ParticleEmitterComponent{}, 0.5f) == glm::vec4(1.0f));
			CHECK(EvaluateParticleSize(ParticleEmitterComponent{}, 0.5f) == 1.0f);
			CHECK(EvaluateParticleColor(emitter, 0.0f) == glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));
			CHECK(EvaluateParticleColor(emitter, 1.0f) == glm::vec4(0.2f, 0.2f, 0.2f, 0.0f));
			CHECK(EvaluateParticleSize(emitter, 0.5f) == doctest::Approx(1.25f));
			CHECK(EvaluateParticleSize(emitter, -1.0f) == 0.5f);
			CHECK(EvaluateParticleSize(emitter, 2.0f) == 2.0f);
		}
	}

}
