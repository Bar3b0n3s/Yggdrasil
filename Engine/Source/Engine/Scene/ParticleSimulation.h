#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/Components/ParticleEmitterComponent.h"
#include "Engine/Scene/Components/ParticleLookComponent.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// The particle simulation of one emitter (M26, C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 5), ECS-agnostic:
// pure functions over a ParticleEmitterComponent's fields, the emitter's world matrix and its state. The Particles frame
// phase (Scene/ParticleSystem.h) calls StepParticleEmitter once per fixed step the session ran, so an emitter's particles
// are a function of its fields, its seed, its poses at the frames that stepped it and the tick count, never of the frame
// rate within a frame or of the configuration. On the simulation path (Scene), so DetMath only: no CRT transcendental
// function, no glm function built on one; directions are drawn from the seeded Random and DetMath's Sin, Cos and Pow.
//
// One step of dt seconds (FixedDelta), in this order:
//   1. every live particle: PreviousPosition = Position; Velocity += Acceleration * dt; Velocity *= max(0, 1 - Drag * dt);
//      Position += Velocity * dt; Age += dt; (M33) PreviousRotation = Rotation; Rotation += Spin * dt; a particle whose Age
//      reaches its Lifetime dies (removed, the survivors keeping their order);
//   2. emission, in this order, each particle while fewer than MaxParticles live (the rest of a step's emits dropped,
//      counted in Capped): while Playing and (Loop or Cycles == 0), the cycle's Burst when CycleTime is 0 (the cycle's
//      first step: the state's first step and every step after a wrap, which restarts CycleTime at exactly 0), then Rate *
//      dt added to EmitAccumulator and its whole part emitted; then, always, the PendingEmits a script queued
//      (ParticleEmitter:Emit), whatever Playing, Loop and the cycle say, so Emit works on a stopped or finished emitter;
//   3. while Playing, CycleTime advances by dt; when it reaches Duration a cycle is complete (Cycles + 1): a looping
//      emitter's CycleTime restarts at exactly 0 (the remainder dropped, so a cycle lasts a whole number of steps, at least
//      one, and every cycle's first step emits its Burst), and a one-shot emitter's stays at Duration (it emits nothing
//      more but queued emits).
// A new particle draws, in this order, from the emitter's Random: its position and direction (Shape: four NextFloat draws
// whatever the shape, those it does not use included), its speed, its lifetime and its size (one draw each, uniform in its
// range), so every particle takes seven draws and a dropped emit takes none; it starts with Age 0 and PreviousPosition =
// Position. With SimulationSpace World its position and velocity are transformed by the emitter's world matrix at the step
// (a point and a direction); with Local they stay in the emitter's space.
//
// M33 (Docs/Decisions/0048-m33-particle-look.md decision 2): with the entity's ParticleLook, a new particle then takes three
// draws from the state's second stream, LookGenerator, in this order: its start angle (uniform in StartRotation, in
// radians), its spin (uniform in Spin, radians per second) and its FrameDraw (uniform in [0, 1)); without a look all three
// are 0 and LookGenerator is not drawn, so the first stream, and with it every particle's motion, is the same with a look or
// without. Every new particle takes Serial = Emitted, which then increases. The first step taken with a look sets HasLook.

namespace Engine {

	// M33 (decision 2): the seed of a state's second stream, LookGenerator, from the state's seed (MakeParticleEmitterSeed's
	// result): XXH64 (seed 0) over `seed` and the tag 0x6b6f6f4c ("Look"), both as little-endian 64-bit values. Pure.
	[[nodiscard]] uint64_t MakeParticleLookSeed(uint64_t seed);

	// One live particle. Positions and velocities are world-space (SimulationSpace World) or the emitter's local space
	// (Local).
	struct Particle
	{
		glm::vec3 Position{ 0.0f };
		glm::vec3 PreviousPosition{ 0.0f }; // the position one step before, for the drawing's interpolation (alpha, §5.2)
		glm::vec3 Velocity{ 0.0f };
		float Age = 0.0f;       // seconds
		float Lifetime = 1.0f;  // seconds
		float StartSize = 0.1f; // metres
		// M33 (decision 2): the angle at this step and at the one before (radians, counter-clockwise as seen), the spin
		// (radians per second) and the Random frame mode's pick, all 0 for a particle born without a ParticleLook; and the
		// particle's ordinal among its emitter's emissions, which matches it across the view's renders for its motion
		// vectors (Renderer/TemporalHistory.h).
		float Rotation = 0.0f;
		float PreviousRotation = 0.0f;
		float Spin = 0.0f;
		float FrameDraw = 0.0f;
		uint64_t Serial = 0;

		bool operator==(const Particle&) const = default;
	};

	// One emitter's live particles and its random stream: the runtime state ParticleStateComponent holds.
	struct ParticleEmitterState
	{
		explicit ParticleEmitterState(uint64_t seed)
			: Generator(seed), LookGenerator(MakeParticleLookSeed(seed))
		{
		}

		Random Generator;
		std::vector<Particle> Particles{}; // in emission order
		float CycleTime = 0.0f;            // seconds into the current cycle: exactly 0 at its first step (step 3)
		uint32_t Cycles = 0;               // completed cycles
		float EmitAccumulator = 0.0f;      // the fractional particle Rate has carried
		uint32_t PendingEmits = 0;         // queued by ParticleEmitter:Emit, spawned at the next step
		uint64_t Steps = 0;                // the steps this state has taken
		uint32_t Capped = 0;               // emits dropped at MaxParticles since the state was made
		// M33 (decision 2): the second stream a ParticleLook draws a new particle's angle, spin and frame from (seeded from
		// the first stream's seed, so it exists for every state), whether a step has run with a look, and the emissions so far
		// (the next particle's Serial).
		Random LookGenerator;
		bool HasLook = false;
		uint64_t Emitted = 0;
	};

	// The emitter's random seed: XXH64 (seed 0) over the entity's UUID and `seed` (ParticleEmitter.Seed), both as
	// little-endian 64-bit values, so two emitters with one Seed differ and an emitter's particles are the same in every
	// configuration and run. Pure.
	[[nodiscard]] uint64_t MakeParticleEmitterSeed(UUID entity, uint32_t seed);

	// Advances `state` by one step of `deltaTime` seconds (> 0, finite) for `emitter` posed at `emitterWorld` (the entity's
	// world matrix at the step), with the entity's ParticleLook `look` or null (M33), as the file comment states. Pure apart
	// from `state`; deterministic in every configuration.
	void StepParticleEmitter(const ParticleEmitterComponent& emitter, const glm::mat4& emitterWorld, float deltaTime, ParticleEmitterState& state,
		const ParticleLookComponent* look = nullptr);

	// ColorOverLife at `normalizedAge` (Age / Lifetime, clamped to [0, 1]): white and opaque without keys, held before the
	// first key and after the last, linear between keys. Pure.
	[[nodiscard]] glm::vec4 EvaluateParticleColor(const ParticleEmitterComponent& emitter, float normalizedAge);

	// SizeOverLife at `normalizedAge`, as EvaluateParticleColor: 1 without keys. Pure.
	[[nodiscard]] float EvaluateParticleSize(const ParticleEmitterComponent& emitter, float normalizedAge);

	// The seeded particle hash (the acceptance "the seeded particle hash is equal across configurations"): XXH64 (seed 0) over
	// the particle count (8 bytes) and every live particle's Position, PreviousPosition, Velocity, Age, Lifetime and StartSize
	// as the little-endian bits of their floats (4 bytes each), in order, then the Random state (its four 64-bit words),
	// CycleTime, Cycles, EmitAccumulator, PendingEmits (4 bytes each) and Steps (8 bytes), every value little-endian; then,
	// only when HasLook (M33, decision 2), every live particle's Rotation, PreviousRotation, Spin and FrameDraw as the bits of
	// their floats, in order, and LookGenerator's state (its four 64-bit words), so an emitter that never had a ParticleLook
	// hashes exactly as before M33. Serial and Emitted are not hashed. Test.GetParticleDigest returns it as 16 hexadecimal
	// digits. Pure.
	[[nodiscard]] uint64_t ComputeParticleDigest(const ParticleEmitterState& state);

}
