#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

	// M33 (Docs/Decisions/0048-m33-particle-look.md decision 1): the bounds of a ParticleLook's registered fields.
	inline constexpr float MaxParticleStartRotation = 360.0f; // degrees, either sign
	inline constexpr float MaxParticleSpin = 3600.0f;         // degrees per second, either sign
	inline constexpr float MinParticleFrameCycles = 0.01f;    // OverLife plays of the frame range over a life
	inline constexpr float MaxParticleFrameCycles = 1000.0f;
	inline constexpr float MaxParticleVelocityStretch = 100.0f; // seconds
	inline constexpr float MaxParticleSoftDistance = 1000.0f;   // metres
	inline constexpr float MaxParticleIntensity = 1000.0f;

	// Registry enum "ParticleFrameMode": which frame of a ParticleLook's flipbook range a particle shows.
	enum class ParticleFrameMode : uint8_t
	{
		OverLife, // the range played over the particle's life, FrameCycles times
		Random    // one frame of the range picked per particle at its birth and held
	};

	// Registry enum "ParticleAlignment": how a particle's quad is laid in the world.
	enum class ParticleAlignment : uint8_t
	{
		Camera,   // a square facing the camera, turned by the particle's angle
		Velocity, // stretched along the particle's velocity across the view; its angle is not used
		Flat      // a square in the emitter's local XZ plane (normal +Y), turned about that normal by the particle's angle
	};

	// Registry name "ParticleLook", version 1, category Particles (M33; Docs/Decisions/0048-m33-particle-look.md decisions 1 to
	// 6): how a ParticleEmitter's particles look, in a component of its own beside the emitter (ADR 0042's ledger, the
	// ParticleEmitter row), so the emitter stays version 1 and no committed hash or digest moves. Requires ParticleEmitter.
	// Presentation only, like the emitter: these fields are ordinary component data, which the scene's serialization covers,
	// while the angles and frames they give the particles are runtime state that is never hashed. Every default reproduces
	// the look of an emitter without a ParticleLook, so a default look draws byte-identically to none.
	//   - Flipbook: with a SpriteSheet, each particle shows a frame of the range [FirstFrame, FirstFrame + FrameCount) of the
	//     sheet (FrameCount 0: every frame from FirstFrame to the sheet's last), drawn square over the particle's quad from the
	//     sheet's texture (opaque white when the sheet has none), instead of the emitter's Texture. OverLife plays the range
	//     FrameCycles times over the particle's life (BlendFrames cross-fades each frame into the next); Random picks one frame
	//     per particle at its birth. A sheet that is missing or fails to load, or a range that is empty or ends past the
	//     sheet's frames, draws the emitter's particles not at all (project.validate's PARTICLE_FRAME_OUT_OF_RANGE names the
	//     range).
	//   - Rotation: a new particle's angle is drawn from StartRotation and its spin from Spin (degrees and degrees per second,
	//     positive counter-clockwise as the camera sees it), from the emitter's second random stream
	//     (Scene/ParticleSimulation.h), so the emitter's first stream, and so its particles' motion, stays as without a look.
	//   - Alignment: Camera (a square facing the camera), Velocity (the quad's long axis along the particle's velocity across
	//     the view, its length its size plus that speed times VelocityStretch; the camera-facing square when it is not moving
	//     across the view) or Flat (a square in the emitter's local XZ plane, its texture's top toward the emitter's -Z).
	//   - SoftDistance: above 0, a particle fades over that view-depth distance as it nears the surface behind it instead of
	//     being cut where it meets it.
	//   - Intensity: multiplies the colour's RGB (not its alpha), so a particle can be brighter than white and bloom.
	struct ParticleLookComponent
	{
		TypedAssetHandle<AssetType::SpriteSheet> SpriteSheet{}; // null: the emitter's Texture, drawn whole
		uint32_t FirstFrame = 0;                                // [0, MaxSpriteSheetFrames - 1]
		uint32_t FrameCount = 0;                                // [0, MaxSpriteSheetFrames]; 0: to the sheet's last frame
		ParticleFrameMode FrameMode = ParticleFrameMode::OverLife;
		float FrameCycles = 1.0f; // [MinParticleFrameCycles, MaxParticleFrameCycles]
		bool BlendFrames = false;
		glm::vec2 StartRotation{ 0.0f }; // degrees, each in [-MaxParticleStartRotation, MaxParticleStartRotation], x <= y
		glm::vec2 Spin{ 0.0f };          // degrees per second, each in [-MaxParticleSpin, MaxParticleSpin], x <= y
		ParticleAlignment Alignment = ParticleAlignment::Camera;
		float VelocityStretch = 0.1f; // seconds, [0, MaxParticleVelocityStretch]
		float SoftDistance = 0.0f;    // metres, [0, MaxParticleSoftDistance]; 0: a hard cut
		float Intensity = 1.0f;       // [0, MaxParticleIntensity]
	};

}
