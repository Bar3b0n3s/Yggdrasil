#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <entt/entity/fwd.hpp>

#include <cstdint>
#include <optional>
#include <vector>

// The Particles frame phase's system (M26, C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decisions 5 and 6; ADR 0028's
// phase-3 ledger: the presentation phase PlaySessionPhase::Particles, after AudioUpdate and before RenderExtraction).
//
// Step(scene, tick), once per frame phase, in Play and Simulate alike (a system, not a script), whether or not the session
// extracts a game view (so a particle digest never depends on rendering): for every entity holding a ParticleEmitter and
// effectively enabled, in canonical entity order, it
//   1. creates the entity's ParticleStateComponent when it has none (seeded with MakeParticleEmitterSeed);
//   2. anchors a state no phase has stepped yet (LastTick empty: one this phase just created, or one QueueParticleEmit made
//      since the last phase, at whatever tick) at `tick` - 1, or 0 at tick 0: a state starts at its first phase and never
//      catches up the ticks before it, so a new emitter steps once at its first phase (none at tick 0), whoever made its
//      state and whenever; M33: an anchored state takes the next of the system's incarnations (ParticleStateComponent);
//   3. steps the state once per tick between its LastTick and `tick` (none when `tick` is not past LastTick), at most
//      MaxParticleStepsPerPhase times (a longer gap is dropped: the state jumps to `tick`), each step StepParticleEmitter with
//      the session's FixedDelta, the entity's current world matrix (WorldTransform) and its ParticleLook, or null without one
//      (M33, Docs/Decisions/0048-m33-particle-look.md decision 2), and sets LastTick to `tick`.
// So a paused session (no tick) freezes its particles, a lockstep frame of one tick steps once, and a frame that ran N ticks
// steps N times (up to the cap); the frame's delta time is never read. An entity that lost its ParticleEmitter or became
// disabled loses its state (its particles end at once). Nothing it writes is hashed or serialized
// (ParticleStateComponent.h), and no simulation phase reads it, so the phase changes no state hash: "PlaySession: particles
// never change the state hash" pins it.
//
// Scripts reach the emitters through QueueParticleEmit and ClearParticles (ParticleEmitter:Emit and Clear), whose effects
// show at the next Particles phase; Test.GetParticleDigest reads FindParticleDigest. Main thread only (the ECS, §4.11).
//
// Frozen by the M26 contract C26.1; implemented by stream P-A.

namespace Engine {

	class Entity;
	class Scene;

	// The most steps one Particles phase takes for one emitter: one second at the default 60 Hz, twelve times the default
	// MaxStepsPerFrame (5), so no frame loop at the default settings reaches it. It bounds a phase's work when a frame follows
	// more fixed steps than that (a project's MaxStepsPerFrame above it, or fixed steps run without frames); the rest of such
	// a gap is dropped, not caught up later.
	inline constexpr uint64_t MaxParticleStepsPerPhase = 60;

	// What the last Step did (for the session's stats and the tests).
	struct ParticleSystemStats
	{
		uint32_t Emitters = 0;  // emitters stepped: the effectively enabled emitters the phase visited, whether a tick passed or not
		uint32_t Particles = 0; // live particles after the step
		uint32_t Steps = 0;     // emitter steps taken (emitters times ticks)
		uint32_t Capped = 0;    // emits dropped at MaxParticles during the step
	};

	class ParticleSystem
	{
	public:
		// `fixedDelta`: the session's fixed step in seconds (> 0, finite, asserted).
		explicit ParticleSystem(float fixedDelta);

		// The phase's work (see the file comment). `tick` is the session's tick after the frame's fixed steps.
		void Step(Scene& scene, uint64_t tick);

		[[nodiscard]] const ParticleSystemStats& GetStats() const { return m_Stats; }
		[[nodiscard]] float GetFixedDelta() const { return m_FixedDelta; }
	private:
		float m_FixedDelta = 0.0f;
		ParticleSystemStats m_Stats{};
		uint64_t m_Incarnations = 0;            // M33: the last incarnation an anchored state took
		std::vector<entt::entity> m_Emitters{}; // the step's emitters in canonical order (M43: the view), kept to reuse its capacity
	};

	// ParticleEmitter:Emit: queues `count` particles (1 to MaxParticlesPerEmitter) for the emitter's next Particles phase,
	// whether Playing or not (and whatever its Loop and cycle: ParticleSimulation.h's step 2). An emitter without a state gets
	// one, seeded and unanchored (LastTick empty), which its next phase anchors and steps once (Step's item 2), so particles
	// emitted from an emitter made mid-session spawn at that phase and none of the session's earlier ticks is caught up. Writes
	// only runtime state, so the scene's revision and state hash stay. Errors: InvalidArgument for a count outside its range;
	// NotFound when `entity` holds no ParticleEmitter.
	[[nodiscard]] Status QueueParticleEmit(Entity entity, uint32_t count);

	// ParticleEmitter:Clear: kills every live particle and drops the queued emits; the emitter's cycle and random stream go
	// on. An emitter without a state is left without one (nothing to clear). Errors: NotFound when `entity` holds no
	// ParticleEmitter.
	[[nodiscard]] Status ClearParticles(Entity entity);

	// The seeded particle hash of `entity`'s emitter (ComputeParticleDigest of its state), or nullopt before its first
	// Particles phase (no state, or a state Emit made that no phase has anchored yet) or without a ParticleEmitter. Pure read.
	[[nodiscard]] std::optional<uint64_t> FindParticleDigest(const Entity& entity);

}
