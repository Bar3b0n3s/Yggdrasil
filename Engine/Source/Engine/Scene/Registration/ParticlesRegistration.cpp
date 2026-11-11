#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

// The Particles category (M26, C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 4; ADR 0028's phase-3 ledger:
// M26's components go in categories of their own after Animation, Particles, Probes and Terrain in that order): the
// ParticleEmitter, whose presentation-only simulation the Particles frame phase advances (Scene/ParticleSystem.h).
// Registered by the contract, so every W26.1 and W26.2 stream builds on the final entries; no scene of FeatureTest or of
// the audited projects holds it, so no canonical file or state hash changes.

namespace Engine {

	namespace Utils {

		// A [min, max] range field reads as (x, y): x may not exceed y.
		static void CheckRange(const glm::vec2& range, std::string_view field, ValidationContext& context)
		{
			if (!(range.x <= range.y))
				context.Error(field, std::format("its minimum {} must not exceed its maximum {}", range.x, range.y));
		}

		static void OrderRange(glm::vec2& range)
		{
			if (range.y < range.x)
				std::swap(range.x, range.y);
		}

		// A curve holds at most MaxParticleCurveKeys keys in ascending Time.
		template<typename Key>
		static void CheckCurve(const std::vector<Key>& keys, std::string_view field, ValidationContext& context)
		{
			if (keys.size() > MaxParticleCurveKeys)
				context.Error(field, std::format("must hold at most {} keys, not {}", MaxParticleCurveKeys, keys.size()));
			for (size_t index = 1; index < keys.size(); ++index)
			{
				if (!(keys[index - 1].Time <= keys[index].Time))
					context.Error(field, std::format("key {}'s Time must not precede key {}'s", index, index - 1));
			}
		}

		template<typename Key>
		static void MakeCurveValid(std::vector<Key>& keys)
		{
			if (keys.size() > MaxParticleCurveKeys)
				keys.resize(MaxParticleCurveKeys);
			std::ranges::stable_sort(keys, {}, &Key::Time);
		}

		static void ValidateParticleEmitter(const ParticleEmitterComponent& emitter, ValidationContext& context)
		{
			CheckRange(emitter.Lifetime, "Lifetime", context);
			CheckRange(emitter.Speed, "Speed", context);
			CheckRange(emitter.Size, "Size", context);
			if (emitter.Burst > emitter.MaxParticles)
				context.Error("Burst", std::format("must not exceed MaxParticles ({}), not {}", emitter.MaxParticles, emitter.Burst));
			CheckCurve(emitter.ColorOverLife, "ColorOverLife", context);
			CheckCurve(emitter.SizeOverLife, "SizeOverLife", context);
		}

		static void MakeParticleEmitterValid(ParticleEmitterComponent& emitter, Random& /*random*/)
		{
			OrderRange(emitter.Lifetime);
			OrderRange(emitter.Speed);
			OrderRange(emitter.Size);
			emitter.Burst = std::min(emitter.Burst, emitter.MaxParticles);
			MakeCurveValid(emitter.ColorOverLife);
			MakeCurveValid(emitter.SizeOverLife);
		}

	}

	void RegisterParticlesComponents(TypeRegistry& registry)
	{
		registry.Enum<ParticleEmitterShape>("ParticleEmitterShape", "Where a ParticleEmitter's new particles start and which way they move.")
			.Entry(ParticleEmitterShape::Point, "Point", "At the emitter's origin, moving in a uniformly random direction.")
			.Entry(ParticleEmitterShape::Sphere, "Sphere", "Uniformly inside a sphere of ShapeRadius, moving away from its centre.")
			.Entry(ParticleEmitterShape::Cone, "Cone", "On a disc of ShapeRadius in the local XY plane, moving within ConeAngle of the local -Z axis.")
			.Entry(ParticleEmitterShape::Box, "Box", "Uniformly inside a box of BoxHalfExtents, moving down the local -Z axis.");

		registry.Enum<ParticleSimulationSpace>("ParticleSimulationSpace", "Which space a ParticleEmitter's live particles move in.")
			.Entry(ParticleSimulationSpace::World, "World", "Spawned at the emitter's pose, then independent of it: a moving emitter leaves a trail.")
			.Entry(ParticleSimulationSpace::Local, "Local", "Simulated in the emitter's local space, so they move with the emitter.");

		registry.Enum<ParticleBlend>("ParticleBlend", "How a particle's colour reaches the image.")
			.Entry(ParticleBlend::Alpha, "Alpha", "Straight-alpha blending, the emitter's particles drawn back to front.")
			.Entry(ParticleBlend::Additive, "Additive", "Added to the image (colour times alpha): glows, sparks and fire, in any order.");

		registry.Struct<ParticleColorKey>("ParticleColorKey", "One key of a particle colour curve over the particle's life.")
			.Field("Time", &ParticleColorKey::Time, "The particle's age over its lifetime at this key, from 0 (born) to 1 (dying).", { .Min = 0.0, .Max = 1.0 })
			.ColorField("Color", &ParticleColorKey::Color, "The linear colour with opacity in the fourth component at this key.", { .Min = 0.0, .Max = 1.0 });

		registry.Struct<ParticleSizeKey>("ParticleSizeKey", "One key of a particle size curve over the particle's life.")
			.Field("Time", &ParticleSizeKey::Time, "The particle's age over its lifetime at this key, from 0 (born) to 1 (dying).", { .Min = 0.0, .Max = 1.0 })
			.Field("Size", &ParticleSizeKey::Size, "The factor of the particle's start size at this key.", { .Min = 0.0, .Max = 1000.0 });

		RegisterComponent<ParticleEmitterComponent>(registry, "ParticleEmitter",
			"Emits camera-facing textured particles from the entity: a presentation-only, seeded simulation that never changes the state hash and "
			"has no gameplay collision.")
			.Category("Particles")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Playing", &ParticleEmitterComponent::Playing, "Whether the emitter emits; live particles finish their lives either way.")
			.Field("Loop", &ParticleEmitterComponent::Loop, "Whether emission repeats every Duration seconds; otherwise it stops after one cycle.")
			.Field("Duration", &ParticleEmitterComponent::Duration, "The length of one emission cycle, in seconds; Burst is emitted at each cycle's start.",
				{ .Min = 0.01, .Max = static_cast<double>(MaxParticleEmitterDuration), .Unit = "s" })
			.Field("Rate", &ParticleEmitterComponent::Rate, "Particles emitted per second while playing.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxParticleRate), .Unit = "1/s" })
			.Field("Burst", &ParticleEmitterComponent::Burst, "Particles emitted at once at the start of each cycle, at most MaxParticles.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxParticlesPerEmitter) })
			.Field("MaxParticles", &ParticleEmitterComponent::MaxParticles,
				"The most particles alive at once; beyond it no new particle is emitted until one dies.",
				{ .Min = 1.0, .Max = static_cast<double>(MaxParticlesPerEmitter) })
			.Field("Seed", &ParticleEmitterComponent::Seed,
				"Seeds the emitter's random stream together with the entity's id, so a replay shows the same particles in every build.")
			.Field("Lifetime", &ParticleEmitterComponent::Lifetime, "The range a new particle's lifetime is drawn from, in seconds: (minimum, maximum).",
				{ .Min = static_cast<double>(MinParticleLifetime), .Max = static_cast<double>(MaxParticleLifetime), .Unit = "s" })
			.Field("Speed", &ParticleEmitterComponent::Speed, "The range a new particle's speed is drawn from, in metres per second: (minimum, maximum).",
				{ .Min = 0.0, .Max = 1000.0, .Unit = "m/s" })
			.Field("Size", &ParticleEmitterComponent::Size, "The range a new particle's size is drawn from, in metres: (minimum, maximum).",
				{ .Min = 0.0001, .Max = 1000.0, .Unit = "m" })
			.Field("Shape", &ParticleEmitterComponent::Shape, "Where new particles start and which way they move.")
			.Field("ShapeRadius", &ParticleEmitterComponent::ShapeRadius, "The radius of the Sphere shape and of the Cone shape's base disc, in metres.",
				{ .Min = 0.0, .Max = 1000.0, .Unit = "m" })
			.Field("ConeAngle", &ParticleEmitterComponent::ConeAngle, "The Cone shape's half angle from the local -Z axis, in degrees.",
				{ .Min = 0.0, .Max = 180.0, .Unit = "deg" })
			.Field("BoxHalfExtents", &ParticleEmitterComponent::BoxHalfExtents, "The Box shape's half extents, in metres.",
				{ .Min = 0.0, .Max = 1000.0, .Unit = "m" })
			.Field("Acceleration", &ParticleEmitterComponent::Acceleration,
				"The acceleration of every live particle, in metres per second squared, in the SimulationSpace (set (0, -9.81, 0) for gravity).",
				{ .Unit = "m/s^2" })
			.Field("Drag", &ParticleEmitterComponent::Drag, "How quickly particles slow down, per second.", { .Min = 0.0, .Max = 100.0, .Unit = "1/s" })
			.Field("SimulationSpace", &ParticleEmitterComponent::SimulationSpace, "Whether live particles stay where they were emitted or move with the emitter.")
			.Field("Texture", &ParticleEmitterComponent::Texture, "The particle's texture; null uses the built-in soft disc (engine://Textures/Particle).")
			.Field("Blend", &ParticleEmitterComponent::Blend, "How particles are blended into the image.")
			.Field("ColorOverLife", &ParticleEmitterComponent::ColorOverLife,
				"At most eight colour keys over a particle's life, in ascending Time, interpolated linearly; empty keeps particles white and opaque.")
			.Field("SizeOverLife", &ParticleEmitterComponent::SizeOverLife,
				"At most eight size factors over a particle's life, in ascending Time, interpolated linearly; empty keeps the start size.")
			.Validate(&Utils::ValidateParticleEmitter)
			.Generate(&Utils::MakeParticleEmitterValid);
	}

}
