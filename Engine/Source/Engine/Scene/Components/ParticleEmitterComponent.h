#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Engine {

	// M26 (C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decisions 4 and 5): the bounds of a ParticleEmitter's
	// registered fields. MaxParticlesPerEmitter caps the live particles of one emitter (the plan's "capped per emitter");
	// MaxParticleCurveKeys the keys of each over-life curve.
	inline constexpr uint32_t MaxParticlesPerEmitter = 4096;
	inline constexpr uint32_t MaxParticleCurveKeys = 8;
	inline constexpr float MaxParticleEmitterDuration = 3600.0f;
	inline constexpr float MaxParticleRate = 10000.0f;
	inline constexpr float MaxParticleLifetime = 1000.0f;
	inline constexpr float MinParticleLifetime = 0.001f;

	// Registry enum "ParticleEmitterShape": where a new particle starts, in the emitter's local space, and which way it moves
	// (its speed along the direction given).
	enum class ParticleEmitterShape : uint8_t
	{
		Point,  // at the origin, in a uniformly random direction
		Sphere, // uniformly inside a sphere of ShapeRadius, moving away from the centre
		Cone,   // on a disc of ShapeRadius in the local XY plane, moving within ConeAngle of local -Z (the forward axis)
		Box     // uniformly inside a box of BoxHalfExtents, moving down local -Z
	};

	// Registry enum "ParticleSimulationSpace": which space live particles move in.
	enum class ParticleSimulationSpace : uint8_t
	{
		World, // spawned at the emitter's pose, then independent of it: a moving emitter leaves a trail
		Local  // simulated in the emitter's local space, so they move with it
	};

	// Registry enum "ParticleBlend": how a particle's colour reaches the image.
	enum class ParticleBlend : uint8_t
	{
		Alpha,   // straight-alpha blending, drawn back to front within the emitter
		Additive // added (colour times alpha): glows, sparks, fire; order-independent
	};

	// Registry struct "ParticleColorKey": one key of ColorOverLife. Time is the particle's age over its lifetime, in [0, 1].
	struct ParticleColorKey
	{
		float Time = 0.0f;
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB, straight alpha
	};

	// Registry struct "ParticleSizeKey": one key of SizeOverLife, a factor of the particle's start size at Time in [0, 1].
	struct ParticleSizeKey
	{
		float Time = 0.0f;
		float Size = 1.0f;
	};

	// Registry name "ParticleEmitter", version 1, category Particles (M26, C26.1, C-PART; decisions 4 and 5): emits camera-
	// facing textured particles from the entity. Requires Transform. Particles are presentation only (plan §6 "M26"): a CPU
	// simulation that the Particles frame phase advances one fixed step per tick the session ran since the last frame
	// (Scene/ParticleSystem.h), seeded per emitter (Seed with the entity's UUID), capped at MaxParticles, with no gameplay
	// collision (a kept limit, Backlog §6). Its live particles are runtime state (Components/ParticleStateComponent.h),
	// never serialized or hashed, so particles never change the state hash; these fields are ordinary component data, which
	// the scene's serialization covers. Edit scenes simulate no particles (a kept limit, Backlog §6): play or simulate the
	// scene to see them.
	//   - Emission: while Playing, Rate particles per second (a fractional remainder carried to the next step) and Burst
	//     particles at the first step of each cycle of Duration seconds (rounded up to whole fixed steps, so every cycle of a
	//     looping emitter bursts; Scene/ParticleSimulation.h); without Loop, one cycle, after which Playing stays true but
	//     nothing more is emitted. A script's ParticleEmitter:Emit(count) adds count particles at the next Particles phase,
	//     whether the emitter is Playing, looping or finished. Beyond MaxParticles live particles, new ones are not emitted (the
	//     oldest live on).
	//   - A particle's start: its position and direction from Shape, a speed uniform in [Speed.x, Speed.y], a lifetime uniform
	//     in [Lifetime.x, Lifetime.y] seconds and a size uniform in [Size.x, Size.y] metres; then each step it accelerates by
	//     Acceleration (world space with World, local with Local), slows by Drag (velocity scaled by max(0, 1 - Drag * dt)),
	//     moves, ages and dies at its lifetime. Its colour is ColorOverLife (white without keys) and its size its start size
	//     times SizeOverLife (1 without keys), each keyed by age over lifetime and interpolated linearly between keys, held
	//     before the first and after the last.
	//   - Drawing: a square quad of the particle's size facing the camera, textured by Texture (null: the built-in particle
	//     texture, engine://Textures/Particle, a soft white disc) times the particle's colour, blended by Blend, depth-tested
	//     against the opaque scene without writing depth, in the Sprites stage after the sprites and the 2D lights, before the
	//     HDR post chain (ADR 0028's phase-3 ledger).
	struct ParticleEmitterComponent
	{
		bool Playing = true;
		bool Loop = true;
		float Duration = 5.0f;            // seconds per cycle, [0.01, MaxParticleEmitterDuration]
		float Rate = 10.0f;               // particles per second, [0, MaxParticleRate]
		uint32_t Burst = 0;               // particles at each cycle's start, [0, MaxParticlesPerEmitter]
		uint32_t MaxParticles = 256;      // [1, MaxParticlesPerEmitter]
		uint32_t Seed = 0;                // with the entity's UUID, the emitter's random stream
		glm::vec2 Lifetime{ 1.0f, 2.0f }; // seconds, [MinParticleLifetime, MaxParticleLifetime], x <= y
		glm::vec2 Speed{ 1.0f, 2.0f };    // metres per second, [0, 1000], x <= y
		glm::vec2 Size{ 0.1f, 0.2f };     // metres, [0.0001, 1000], x <= y
		ParticleEmitterShape Shape = ParticleEmitterShape::Cone;
		float ShapeRadius = 0.25f;        // metres (Sphere, Cone), [0, 1000]
		float ConeAngle = 25.0f;          // degrees from -Z (Cone), [0, 180]
		glm::vec3 BoxHalfExtents{ 0.5f }; // metres (Box), each [0, 1000]
		glm::vec3 Acceleration{ 0.0f };   // metres per second squared
		float Drag = 0.0f;                // per second, [0, 100]
		ParticleSimulationSpace SimulationSpace = ParticleSimulationSpace::World;
		TypedAssetHandle<AssetType::Texture> Texture{}; // null: the built-in particle texture
		ParticleBlend Blend = ParticleBlend::Alpha;
		std::vector<ParticleColorKey> ColorOverLife{}; // at most MaxParticleCurveKeys, Times in [0, 1] ascending
		std::vector<ParticleSizeKey> SizeOverLife{};   // at most MaxParticleCurveKeys, Times in [0, 1] ascending
	};

}
