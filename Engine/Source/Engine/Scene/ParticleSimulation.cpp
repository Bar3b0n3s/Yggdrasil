#include "EnginePCH.h"
#include "Engine/Scene/ParticleSimulation.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Core/Hash.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>

// The particle simulation of one emitter (ParticleSimulation.h; M26, stream P-A, Docs/Decisions/0037-m26-contract.md decision
// 5). On the simulation path: IEEE-exact operations (+, -, *, /, sqrt, floor), DetMath and the emitter's seeded Random only, so
// a step gives the same bits in every configuration.

namespace Engine {

	namespace Utils {

		constexpr float TwoPi = 6.28318530717958647692f;
		constexpr float RadiansPerDegree = 0.0174532925199432957692f;
		constexpr float OneThird = 1.0f / 3.0f;

		// The draws a new particle's Shape takes, whatever the shape (unused ones are drawn all the same), so every particle
		// takes ShapeDraws + 3 draws and one emitter's stream repeats exactly.
		constexpr size_t ShapeDraws = 4;

		// Where a new particle starts and which way it moves, in the emitter's local space.
		struct ParticleSpawn
		{
			glm::vec3 Position{ 0.0f };
			glm::vec3 Direction{ 0.0f, 0.0f, -1.0f };
		};

		// A uniformly random unit vector from two uniform draws in [0, 1): z uniform in (-1, 1], the azimuth uniform.
		static glm::vec3 UniformDirection(float zDraw, float azimuthDraw)
		{
			const float z = 1.0f - 2.0f * zDraw;
			const float radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
			const SinCosResult<float> azimuth = DetMath::SinCos(TwoPi * azimuthDraw);
			return glm::vec3(radius * azimuth.Cos, radius * azimuth.Sin, z);
		}

		// The shape's start and direction from its ShapeDraws draws (ParticleEmitterShape's comments).
		static ParticleSpawn SpawnInShape(const ParticleEmitterComponent& emitter, const std::array<float, ShapeDraws>& draws)
		{
			ParticleSpawn spawn;
			switch (emitter.Shape)
			{
				case ParticleEmitterShape::Point:
					spawn.Direction = UniformDirection(draws[0], draws[1]);
					break;
				case ParticleEmitterShape::Sphere:
				{
					// Uniform in the ball: the radius goes with the cube root of a uniform draw.
					spawn.Direction = UniformDirection(draws[0], draws[1]);
					const float radius = emitter.ShapeRadius * DetMath::Pow(draws[2], OneThird);
					spawn.Position = spawn.Direction * radius;
					break;
				}
				case ParticleEmitterShape::Cone:
				{
					// Uniform on the base disc in the local XY plane, and uniform over the cap of directions within ConeAngle of -Z.
					const float discRadius = emitter.ShapeRadius * std::sqrt(draws[0]);
					const SinCosResult<float> around = DetMath::SinCos(TwoPi * draws[1]);
					spawn.Position = glm::vec3(discRadius * around.Cos, discRadius * around.Sin, 0.0f);
					const float cosAngle = DetMath::Cos(emitter.ConeAngle * RadiansPerDegree);
					const float cosTilt = 1.0f - draws[2] * (1.0f - cosAngle);
					const float sinTilt = std::sqrt(std::max(0.0f, 1.0f - cosTilt * cosTilt));
					const SinCosResult<float> azimuth = DetMath::SinCos(TwoPi * draws[3]);
					spawn.Direction = glm::vec3(sinTilt * azimuth.Cos, sinTilt * azimuth.Sin, -cosTilt);
					break;
				}
				case ParticleEmitterShape::Box:
					spawn.Position = glm::vec3((2.0f * draws[0] - 1.0f) * emitter.BoxHalfExtents.x, (2.0f * draws[1] - 1.0f) * emitter.BoxHalfExtents.y,
						(2.0f * draws[2] - 1.0f) * emitter.BoxHalfExtents.z);
					break;
			}
			return spawn;
		}

		// Uniform in [range.x, range.y] (range.x when the two are equal) from one draw in [0, 1).
		static float DrawInRange(const glm::vec2& range, float draw)
		{
			return range.x + (range.y - range.x) * draw;
		}

		// A new particle (the header's draw order: shape, speed, lifetime, size), posed by `emitterWorld` in World space.
		static Particle SpawnParticle(const ParticleEmitterComponent& emitter, const glm::mat4& emitterWorld, Random& generator)
		{
			std::array<float, ShapeDraws> draws{};
			for (float& draw : draws)
				draw = generator.NextFloat();
			const ParticleSpawn spawn = SpawnInShape(emitter, draws);
			const float speed = DrawInRange(emitter.Speed, generator.NextFloat());
			const float lifetime = DrawInRange(emitter.Lifetime, generator.NextFloat());
			const float size = DrawInRange(emitter.Size, generator.NextFloat());

			Particle particle;
			particle.Position = spawn.Position;
			particle.Velocity = spawn.Direction * speed;
			if (emitter.SimulationSpace == ParticleSimulationSpace::World)
			{
				particle.Position = glm::vec3(emitterWorld * glm::vec4(particle.Position, 1.0f));
				particle.Velocity = glm::vec3(emitterWorld * glm::vec4(particle.Velocity, 0.0f));
			}
			particle.PreviousPosition = particle.Position;
			particle.Age = 0.0f;
			particle.Lifetime = lifetime;
			particle.StartSize = size;
			return particle;
		}

		static uint32_t SaturatingAdd(uint32_t value, uint64_t increment)
		{
			const uint64_t sum = static_cast<uint64_t>(value) + std::min<uint64_t>(increment, std::numeric_limits<uint32_t>::max());
			return static_cast<uint32_t>(std::min<uint64_t>(sum, std::numeric_limits<uint32_t>::max()));
		}

		// Emits up to `count` particles while fewer than `cap` live; the rest are dropped and counted in Capped (drawing nothing).
		static void Emit(const ParticleEmitterComponent& emitter, const glm::mat4& emitterWorld, uint64_t count, size_t cap, ParticleEmitterState& state)
		{
			for (; count > 0 && state.Particles.size() < cap; --count)
				state.Particles.push_back(SpawnParticle(emitter, emitterWorld, state.Generator));
			state.Capped = SaturatingAdd(state.Capped, count);
		}

		// The whole particles of a fractional count: at most 2^32 (a count only fields outside their ranges reach), none for a
		// count that is not a number.
		static uint64_t WholeParticles(float count)
		{
			constexpr float Limit = 4294967296.0f;
			if (!(count > 0.0f))
				return 0;
			return static_cast<uint64_t>(std::min(count, Limit));
		}

		// The 4 little-endian bytes of `value`.
		static void HashU32(XXH64Hasher& hasher, uint32_t value)
		{
			const std::array<std::byte, 4> bytes{ static_cast<std::byte>(value & 0xffU), static_cast<std::byte>((value >> 8) & 0xffU),
				static_cast<std::byte>((value >> 16) & 0xffU), static_cast<std::byte>((value >> 24) & 0xffU) };
			hasher.Update(bytes);
		}

		// The little-endian bits of `value`.
		static void HashFloat(XXH64Hasher& hasher, float value)
		{
			HashU32(hasher, std::bit_cast<uint32_t>(value));
		}

		static void HashVector(XXH64Hasher& hasher, const glm::vec3& value)
		{
			HashFloat(hasher, value.x);
			HashFloat(hasher, value.y);
			HashFloat(hasher, value.z);
		}

		// One over-life curve at `normalizedAge`: the first key's value at or before its Time, the last key's after its Time,
		// linear between keys; `fallback` without keys. A NaN age reads as 0.
		template<typename Key, typename T, typename Read>
		static T EvaluateCurve(const std::vector<Key>& keys, float normalizedAge, const T& fallback, Read read)
		{
			if (keys.empty())
				return fallback;
			const float age = normalizedAge >= 0.0f ? std::min(normalizedAge, 1.0f) : 0.0f;
			if (age <= keys.front().Time)
				return read(keys.front());
			for (size_t index = 0; index + 1 < keys.size(); ++index)
			{
				// Here age >= keys[index].Time, so the span below is positive.
				const Key& next = keys[index + 1];
				if (age < next.Time)
				{
					const Key& key = keys[index];
					const float fraction = (age - key.Time) / (next.Time - key.Time);
					const T from = read(key);
					return from + (read(next) - from) * fraction;
				}
			}
			return read(keys.back());
		}

	}

	uint64_t MakeParticleEmitterSeed(UUID entity, uint32_t seed)
	{
		XXH64Hasher hasher;
		hasher.UpdateU64(entity.GetValue());
		hasher.UpdateU64(seed);
		return hasher.Digest();
	}

	void StepParticleEmitter(const ParticleEmitterComponent& emitter, const glm::mat4& emitterWorld, float deltaTime, ParticleEmitterState& state)
	{
		ENGINE_CORE_ASSERT(std::isfinite(deltaTime) && deltaTime > 0.0f, "A particle step of {} s", deltaTime);

		// 1. Integrate and age every live particle; those that reach their lifetime die, the survivors keeping their order.
		const float drag = std::max(0.0f, 1.0f - emitter.Drag * deltaTime);
		const glm::vec3 acceleration = emitter.Acceleration * deltaTime;
		for (Particle& particle : state.Particles)
		{
			particle.PreviousPosition = particle.Position;
			particle.Velocity += acceleration;
			particle.Velocity *= drag;
			particle.Position += particle.Velocity * deltaTime;
			particle.Age += deltaTime;
		}
		std::erase_if(state.Particles, [](const Particle& particle)
		{
			return particle.Age >= particle.Lifetime;
		});

		// 2. Emission: the cycle's burst and rate while the cycle runs, then the queued emits whatever the cycle says.
		const size_t cap = std::min(emitter.MaxParticles, MaxParticlesPerEmitter);
		const bool cycling = emitter.Playing && (emitter.Loop || state.Cycles == 0);
		if (cycling)
		{
			if (state.CycleTime == 0.0f)
				Utils::Emit(emitter, emitterWorld, emitter.Burst, cap, state);
			if (std::isfinite(emitter.Rate) && emitter.Rate > 0.0f)
			{
				state.EmitAccumulator += emitter.Rate * deltaTime;
				const float whole = std::floor(state.EmitAccumulator);
				state.EmitAccumulator -= whole;
				Utils::Emit(emitter, emitterWorld, Utils::WholeParticles(whole), cap, state);
			}
		}
		const uint32_t pending = state.PendingEmits;
		state.PendingEmits = 0;
		Utils::Emit(emitter, emitterWorld, pending, cap, state);

		// 3. The cycle: a looping emitter restarts at exactly 0 (so every cycle bursts), a one-shot one stays at Duration.
		if (cycling)
		{
			state.CycleTime += deltaTime;
			if (state.CycleTime >= emitter.Duration)
			{
				state.Cycles = Utils::SaturatingAdd(state.Cycles, 1);
				state.CycleTime = emitter.Loop ? 0.0f : emitter.Duration;
			}
		}
		++state.Steps;
	}

	glm::vec4 EvaluateParticleColor(const ParticleEmitterComponent& emitter, float normalizedAge)
	{
		return Utils::EvaluateCurve(emitter.ColorOverLife, normalizedAge, glm::vec4(1.0f), [](const ParticleColorKey& key)
		{
			return key.Color;
		});
	}

	float EvaluateParticleSize(const ParticleEmitterComponent& emitter, float normalizedAge)
	{
		return Utils::EvaluateCurve(emitter.SizeOverLife, normalizedAge, 1.0f, [](const ParticleSizeKey& key)
		{
			return key.Size;
		});
	}

	uint64_t ComputeParticleDigest(const ParticleEmitterState& state)
	{
		XXH64Hasher hasher;
		hasher.UpdateU64(state.Particles.size());
		for (const Particle& particle : state.Particles)
		{
			Utils::HashVector(hasher, particle.Position);
			Utils::HashVector(hasher, particle.PreviousPosition);
			Utils::HashVector(hasher, particle.Velocity);
			Utils::HashFloat(hasher, particle.Age);
			Utils::HashFloat(hasher, particle.Lifetime);
			Utils::HashFloat(hasher, particle.StartSize);
		}
		for (const uint64_t word : state.Generator.GetState())
			hasher.UpdateU64(word);
		Utils::HashFloat(hasher, state.CycleTime);
		Utils::HashU32(hasher, state.Cycles);
		Utils::HashFloat(hasher, state.EmitAccumulator);
		Utils::HashU32(hasher, state.PendingEmits);
		hasher.UpdateU64(state.Steps);
		return hasher.Digest();
	}

}
