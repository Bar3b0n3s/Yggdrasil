#include "TestsPCH.h"

#include "Engine/Scene/ParticleSimulation.h"

#include "Engine/Core/DetMath.h"
#include "Engine/Core/UUID.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

// The particle simulation of one emitter (M26, C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 5, stream P-A), and
// M33's look: the second stream, the angles and the serials (Docs/Decisions/0048-m33-particle-look.md decision 2).

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

		// `steps` steps of `emitter` with `look` (or none), the emitter moving along +X by 0.1 m a step.
		ParticleEmitterState Run(const ParticleEmitterComponent& emitter, UUID entity, uint32_t steps, const ParticleLookComponent* look = nullptr)
		{
			ParticleEmitterState state(MakeParticleEmitterSeed(entity, emitter.Seed));
			for (uint32_t step = 0; step < steps; ++step)
			{
				const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(0.1f * static_cast<float>(step), 1.0f, 0.0f));
				StepParticleEmitter(emitter, world, FixedDelta, state, look);
			}
			return state;
		}

		// A look that turns, spins and picks frames, with other fields off their defaults too (they do not reach the step).
		ParticleLookComponent MakeSpinningLook()
		{
			ParticleLookComponent look;
			look.FrameMode = ParticleFrameMode::Random;
			look.StartRotation = glm::vec2(-90.0f, 90.0f);
			look.Spin = glm::vec2(-180.0f, 360.0f);
			look.Alignment = ParticleAlignment::Flat;
			look.SoftDistance = 0.5f;
			look.Intensity = 4.0f;
			return look;
		}

		constexpr float RadiansPerDegree = 0.0174532925199432957692f;

		// An emitter of `burst` particles at its first step that live long, move at `speed` and emit nothing else.
		ParticleEmitterComponent MakeBurst(ParticleEmitterShape shape, uint32_t burst, float speed)
		{
			ParticleEmitterComponent emitter;
			emitter.Shape = shape;
			emitter.Rate = 0.0f;
			emitter.Burst = burst;
			emitter.MaxParticles = burst;
			emitter.Lifetime = glm::vec2(10.0f, 10.0f);
			emitter.Speed = glm::vec2(speed, speed);
			emitter.SimulationSpace = ParticleSimulationSpace::Local;
			return emitter;
		}

		float Length(const glm::vec3& value)
		{
			return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
		}

	}

	TEST_SUITE("Scene")
	{
		// The acceptance "the seeded particle hash is equal across configurations": the digest committed from Debug and confirmed
		// in Release (Test.py --config Release on this case); FeatureTest's Particles suite checks Test.GetParticleDigest in the
		// Editor, Release and Dist runs (decision 25), since the Tests executable does not exist in Dist.
		TEST_CASE("ParticleSimulation: a seeded emitter's particles hash to their committed digest in every configuration")
		{
			const ParticleEmitterState state = Run(MakeFountain(), UUID(0x7a1c3e5f20b4d608), 240);
			CHECK_FALSE(state.Particles.empty());
			CHECK(std::format("{:016x}", ComputeParticleDigest(state)) == std::string("6963470cc388a75b"));
		}

		TEST_CASE("ParticleSimulation: one emitter repeats exactly, and two emitters with one Seed differ")
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

		TEST_CASE("ParticleSimulation: a step ages, moves and kills particles, and emits its burst and rate up to its cap")
		{
			// The header's order: integrate, age and kill, then the burst at the cycle's first step, then the rate (60 * (1/60.0f)
			// is exactly 1.0f, so one particle a step), each while fewer than MaxParticles live.
			ParticleEmitterComponent emitter;
			emitter.Shape = ParticleEmitterShape::Point; // at the origin, so a particle's height is how far it rose
			emitter.Rate = 60.0f;                        // one particle a step at 60 Hz
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

		TEST_CASE("ParticleSimulation: over-life curves hold before the first key and after the last, and are linear between")
		{
			const ParticleEmitterComponent emitter = MakeFountain();
			CHECK(EvaluateParticleColor(ParticleEmitterComponent{}, 0.5f) == glm::vec4(1.0f));
			CHECK(EvaluateParticleSize(ParticleEmitterComponent{}, 0.5f) == 1.0f);
			CHECK(EvaluateParticleColor(emitter, 0.0f) == glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));
			CHECK(EvaluateParticleColor(emitter, 1.0f) == glm::vec4(0.2f, 0.2f, 0.2f, 0.0f));
			CHECK(EvaluateParticleSize(emitter, 0.5f) == doctest::Approx(1.25f));
			CHECK(EvaluateParticleSize(emitter, -1.0f) == 0.5f);
			CHECK(EvaluateParticleSize(emitter, 2.0f) == 2.0f);
			// Keys inside [0, 1]: held before the first and after the last; three keys interpolate within their segment.
			ParticleEmitterComponent keyed;
			keyed.SizeOverLife = { { .Time = 0.25f, .Size = 1.0f }, { .Time = 0.5f, .Size = 3.0f }, { .Time = 0.75f, .Size = 2.0f } };
			CHECK(EvaluateParticleSize(keyed, 0.0f) == 1.0f);
			CHECK(EvaluateParticleSize(keyed, 0.375f) == 2.0f);
			CHECK(EvaluateParticleSize(keyed, 0.625f) == 2.5f);
			CHECK(EvaluateParticleSize(keyed, 1.0f) == 2.0f);
			CHECK(EvaluateParticleSize(keyed, std::nanf("")) == 1.0f);
		}

		// The shapes' rules (ParticleEmitterShape's comments), in Local space at the origin so positions are the shape's own.
		TEST_CASE("ParticleSimulation: each shape starts its particles inside it and moves them its way")
		{
			constexpr uint32_t Count = 200;
			constexpr float Speed = 2.0f;
			SUBCASE("Point")
			{
				const ParticleEmitterComponent emitter = MakeBurst(ParticleEmitterShape::Point, Count, Speed);
				ParticleEmitterState state(MakeParticleEmitterSeed(UUID(0x44), 0));
				StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state);
				REQUIRE(state.Particles.size() == Count);
				glm::vec3 sum(0.0f);
				for (const Particle& particle : state.Particles)
				{
					CHECK(particle.Position == glm::vec3(0.0f));
					CHECK(Length(particle.Velocity) == doctest::Approx(Speed).epsilon(1e-5));
					sum += particle.Velocity;
				}
				// Uniform directions: their mean is far from any one direction.
				CHECK(Length(sum) / static_cast<float>(Count) < 0.5f * Speed);
			}
			SUBCASE("Sphere")
			{
				ParticleEmitterComponent emitter = MakeBurst(ParticleEmitterShape::Sphere, Count, Speed);
				emitter.ShapeRadius = 1.5f;
				ParticleEmitterState state(MakeParticleEmitterSeed(UUID(0x45), 0));
				StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state);
				REQUIRE(state.Particles.size() == Count);
				for (const Particle& particle : state.Particles)
				{
					const float distance = Length(particle.Position);
					CHECK(distance <= 1.5f + 1e-5f);
					CHECK(Length(particle.Velocity) == doctest::Approx(Speed).epsilon(1e-5));
					// Away from the centre: the velocity points along the position.
					if (distance > 1e-3f)
						CHECK(glm::dot(particle.Position, particle.Velocity) == doctest::Approx(distance * Speed).epsilon(1e-4));
				}
			}
			SUBCASE("Cone")
			{
				ParticleEmitterComponent emitter = MakeBurst(ParticleEmitterShape::Cone, Count, Speed);
				emitter.ShapeRadius = 0.5f;
				emitter.ConeAngle = 30.0f;
				ParticleEmitterState state(MakeParticleEmitterSeed(UUID(0x46), 0));
				StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state);
				REQUIRE(state.Particles.size() == Count);
				const float cosAngle = DetMath::Cos(30.0f * 0.0174532925199432957692f);
				for (const Particle& particle : state.Particles)
				{
					CHECK(particle.Position.z == 0.0f);
					CHECK(Length(particle.Position) <= 0.5f + 1e-5f);
					CHECK(Length(particle.Velocity) == doctest::Approx(Speed).epsilon(1e-5));
					// Within ConeAngle of -Z.
					CHECK(-particle.Velocity.z / Speed >= cosAngle - 1e-5f);
				}
			}
			SUBCASE("Box")
			{
				ParticleEmitterComponent emitter = MakeBurst(ParticleEmitterShape::Box, Count, Speed);
				emitter.BoxHalfExtents = glm::vec3(1.0f, 2.0f, 0.5f);
				ParticleEmitterState state(MakeParticleEmitterSeed(UUID(0x47), 0));
				StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state);
				REQUIRE(state.Particles.size() == Count);
				for (const Particle& particle : state.Particles)
				{
					CHECK(std::abs(particle.Position.x) <= 1.0f);
					CHECK(std::abs(particle.Position.y) <= 2.0f);
					CHECK(std::abs(particle.Position.z) <= 0.5f);
					CHECK(particle.Velocity == glm::vec3(0.0f, 0.0f, -Speed));
				}
			}
		}

		// SimulationSpace (ParticleSimulationSpace's comments): World particles start at the emitter's pose and then ignore it;
		// Local ones stay in the emitter's space.
		TEST_CASE("ParticleSimulation: World particles start at the emitter's pose and stay behind, Local ones live in its space")
		{
			ParticleEmitterComponent emitter = MakeBurst(ParticleEmitterShape::Box, 1, 1.0f);
			emitter.BoxHalfExtents = glm::vec3(0.0f);
			const glm::mat4 placed = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f));
			const glm::mat4 moved = glm::translate(glm::mat4(1.0f), glm::vec3(9.0f, 0.0f, 0.0f));

			emitter.SimulationSpace = ParticleSimulationSpace::World;
			ParticleEmitterState world(MakeParticleEmitterSeed(UUID(0x48), 0));
			StepParticleEmitter(emitter, placed, FixedDelta, world);
			REQUIRE(world.Particles.size() == 1);
			CHECK(world.Particles[0].Position == glm::vec3(5.0f, 0.0f, 0.0f));
			CHECK(world.Particles[0].Velocity == glm::vec3(0.0f, 0.0f, -1.0f));
			StepParticleEmitter(emitter, moved, FixedDelta, world);
			CHECK(world.Particles[0].PreviousPosition == glm::vec3(5.0f, 0.0f, 0.0f));
			CHECK(world.Particles[0].Position.x == 5.0f);

			emitter.SimulationSpace = ParticleSimulationSpace::Local;
			ParticleEmitterState local(MakeParticleEmitterSeed(UUID(0x48), 0));
			StepParticleEmitter(emitter, placed, FixedDelta, local);
			REQUIRE(local.Particles.size() == 1);
			CHECK(local.Particles[0].Position == glm::vec3(0.0f));
			StepParticleEmitter(emitter, moved, FixedDelta, local);
			CHECK(local.Particles[0].Position.x == 0.0f);
		}

		TEST_CASE("ParticleSimulation: drag slows particles each step and never reverses them")
		{
			ParticleEmitterComponent emitter = MakeBurst(ParticleEmitterShape::Box, 1, 6.0f);
			emitter.BoxHalfExtents = glm::vec3(0.0f);
			emitter.Drag = 30.0f; // 1 - 30 / 60: half the velocity a step
			ParticleEmitterState state(MakeParticleEmitterSeed(UUID(0x49), 0));
			StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state);
			StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state);
			REQUIRE(state.Particles.size() == 1);
			CHECK(state.Particles[0].Velocity.z == doctest::Approx(-3.0f));
			emitter.Drag = 100.0f; // past a full stop in one step: held at rest
			StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state);
			CHECK(state.Particles[0].Velocity == glm::vec3(0.0f));
		}

		// Decision 5 (the review's finding 10): a looping wrap restarts CycleTime at exactly 0, so every cycle bursts; a one-shot
		// emitter bursts once and stays finished; queued emits spawn whatever Playing, Loop and the cycle say, up to the cap.
		TEST_CASE("ParticleSimulation: a looping emitter bursts every cycle, a one-shot one once, and queued emits spawn either way")
		{
			ParticleEmitterComponent emitter = MakeBurst(ParticleEmitterShape::Point, 3, 0.0f);
			emitter.MaxParticles = 64;
			emitter.Duration = 0.03f; // two steps at 60 Hz
			std::vector<size_t> counts;

			ParticleEmitterState looping(MakeParticleEmitterSeed(UUID(0x50), 0));
			for (int step = 0; step < 5; ++step)
			{
				StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, looping);
				counts.push_back(looping.Particles.size());
			}
			CHECK(counts == std::vector<size_t>{ 3, 3, 6, 6, 9 });
			CHECK(looping.Cycles == 2);
			CHECK(looping.CycleTime == FixedDelta);

			emitter.Loop = false;
			ParticleEmitterState once(MakeParticleEmitterSeed(UUID(0x51), 0));
			counts.clear();
			for (int step = 0; step < 4; ++step)
			{
				StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, once);
				counts.push_back(once.Particles.size());
			}
			CHECK(counts == std::vector<size_t>{ 3, 3, 3, 3 });
			CHECK(once.Cycles == 1);
			CHECK(once.CycleTime == emitter.Duration);
			once.PendingEmits = 2;
			StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, once);
			CHECK(once.Particles.size() == 5);
			CHECK(once.PendingEmits == 0);

			// A stopped emitter neither bursts nor advances its cycle, but spawns what was queued, up to the cap.
			emitter.Playing = false;
			emitter.MaxParticles = 4;
			ParticleEmitterState stopped(MakeParticleEmitterSeed(UUID(0x52), 0));
			stopped.PendingEmits = 10;
			StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, stopped);
			CHECK(stopped.Particles.size() == 4);
			CHECK(stopped.Capped == 6);
			CHECK(stopped.PendingEmits == 0);
			CHECK(stopped.CycleTime == 0.0f);
			CHECK(stopped.Cycles == 0);
		}

		// M33 (decision 2): the look's draws come from a stream of their own, so the first stream's particles, and the digest of
		// an emitter that never had a look, are those of M26.
		TEST_CASE("ParticleSimulation: a ParticleLook draws angle, spin and frame from a second stream, so the first stream's particles are unchanged")
		{
			const ParticleEmitterComponent emitter = MakeFountain();
			const ParticleLookComponent look = MakeSpinningLook();
			const ParticleEmitterState plain = Run(emitter, UUID(0x4401), 120);
			const ParticleEmitterState looked = Run(emitter, UUID(0x4401), 120, &look);
			REQUIRE(plain.Particles.size() == looked.Particles.size());
			REQUIRE_FALSE(plain.Particles.empty());
			CHECK(plain.Generator.GetState() == looked.Generator.GetState());
			CHECK_FALSE(plain.HasLook);
			CHECK(looked.HasLook);
			// The look's stream was drawn; the plain state's was not.
			const ParticleEmitterState fresh(MakeParticleEmitterSeed(UUID(0x4401), emitter.Seed));
			CHECK(plain.LookGenerator.GetState() == fresh.LookGenerator.GetState());
			CHECK(looked.LookGenerator.GetState() != plain.LookGenerator.GetState());
			for (size_t index = 0; index < plain.Particles.size(); ++index)
			{
				const Particle& before = plain.Particles[index];
				const Particle& after = looked.Particles[index];
				CAPTURE(index);
				CHECK(before.Position == after.Position);
				CHECK(before.PreviousPosition == after.PreviousPosition);
				CHECK(before.Velocity == after.Velocity);
				CHECK(before.Age == after.Age);
				CHECK(before.Lifetime == after.Lifetime);
				CHECK(before.StartSize == after.StartSize);
				CHECK(before.Serial == after.Serial);
				CHECK(before.Rotation == 0.0f);
				CHECK(before.Spin == 0.0f);
				CHECK(before.FrameDraw == 0.0f);
				CHECK(after.Spin >= -180.0f * RadiansPerDegree);
				CHECK(after.Spin <= 360.0f * RadiansPerDegree);
				CHECK(after.FrameDraw >= 0.0f);
				CHECK(after.FrameDraw < 1.0f);
			}
			// The look's part of the digest is there only for the looked state.
			CHECK(ComputeParticleDigest(plain) != ComputeParticleDigest(looked));
			ParticleEmitterState stripped = looked;
			for (Particle& particle : stripped.Particles)
			{
				particle.Rotation = 0.0f;
				particle.PreviousRotation = 0.0f;
				particle.Spin = 0.0f;
				particle.FrameDraw = 0.0f;
			}
			stripped.LookGenerator = plain.LookGenerator;
			stripped.HasLook = false;
			CHECK(ComputeParticleDigest(stripped) == ComputeParticleDigest(plain));
			CHECK(MakeParticleLookSeed(1) != MakeParticleLookSeed(2));
			CHECK(MakeParticleLookSeed(1) != 1);
		}

		// The digest of an emitter with a look, committed from Debug and confirmed in Release; FeatureTest's ParticleLook suite
		// checks one in the Editor, Release and Dist runs (decision 8).
		TEST_CASE("ParticleSimulation: a seeded emitter with a ParticleLook hashes to its committed digest in every configuration")
		{
			const ParticleLookComponent look = MakeSpinningLook();
			const ParticleEmitterState state = Run(MakeFountain(), UUID(0x7a1c3e5f20b4d608), 240, &look);
			CHECK_FALSE(state.Particles.empty());
			CHECK(std::format("{:016x}", ComputeParticleDigest(state)) == std::string("965089b021ea1ccd"));
		}

		TEST_CASE("ParticleSimulation: a particle turns by its spin each step, keeping its previous angle for the drawing")
		{
			const ParticleEmitterComponent emitter = MakeBurst(ParticleEmitterShape::Point, 1, 0.0f);
			ParticleLookComponent look;
			look.StartRotation = glm::vec2(30.0f, 30.0f);
			look.Spin = glm::vec2(90.0f, 90.0f);
			ParticleEmitterState state(MakeParticleEmitterSeed(UUID(0x4402), 0));
			StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state, &look);
			REQUIRE(state.Particles.size() == 1);
			// Born at its start angle, which it held the step it was born.
			CHECK(state.Particles[0].Rotation == 30.0f * RadiansPerDegree);
			CHECK(state.Particles[0].PreviousRotation == 30.0f * RadiansPerDegree);
			CHECK(state.Particles[0].Spin == 90.0f * RadiansPerDegree);
			float expected = 30.0f * RadiansPerDegree;
			for (int step = 0; step < 3; ++step)
			{
				// A step without the look still turns the particles the look gave a spin.
				StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state, step == 1 ? nullptr : &look);
				const float previous = expected;
				expected += 90.0f * RadiansPerDegree * FixedDelta;
				CHECK(state.Particles[0].PreviousRotation == previous);
				CHECK(state.Particles[0].Rotation == expected);
			}
		}

		TEST_CASE("ParticleSimulation: every new particle takes the next serial, with or without a look")
		{
			ParticleEmitterComponent emitter = MakeBurst(ParticleEmitterShape::Point, 3, 0.0f);
			emitter.Lifetime = glm::vec2(1.5f * FixedDelta, 1.5f * FixedDelta); // dies at its second step
			emitter.Duration = 2.0f * FixedDelta;                               // a burst every other step
			const ParticleLookComponent look = MakeSpinningLook();
			ParticleEmitterState state(MakeParticleEmitterSeed(UUID(0x4403), 0));
			StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state);
			REQUIRE(state.Particles.size() == 3);
			CHECK(state.Particles[0].Serial == 0);
			CHECK(state.Particles[2].Serial == 2);
			StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state, &look);
			StepParticleEmitter(emitter, glm::mat4(1.0f), FixedDelta, state, &look);
			// The first burst died at the third step, whose cycle's burst took serials 3 to 5.
			REQUIRE(state.Particles.size() == 3);
			CHECK(state.Particles[0].Serial == 3);
			CHECK(state.Particles[2].Serial == 5);
			CHECK(state.Emitted == 6);
		}
	}

}
