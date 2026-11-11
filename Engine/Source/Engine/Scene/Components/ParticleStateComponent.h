#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Scene/ParticleSimulation.h"

#include <cstdint>
#include <optional>

namespace Engine {

	// A runtime-only component (Components/RuntimeComponents.h's rules: never registered, serialized or hashed, invisible to
	// scripts and automation, and adding or patching it changes neither the scene's revision nor the change tracker): a
	// ParticleEmitter's live particles (M26, C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decisions 5 and 6). The
	// Particles frame phase creates it on every effectively enabled ParticleEmitter entity that has none, and so does
	// ParticleEmitter:Emit (QueueParticleEmit), each seeded with MakeParticleEmitterSeed of the entity's UUID and the
	// emitter's Seed; the phase anchors and steps it (Scene/ParticleSystem.h); Emit and Clear write it; a session's serializer
	// copy never carries one, and edit scenes never hold one. Because nothing that is hashed reads it, particles never change
	// the state hash (ADR 0028's phase-3 ledger).
	//
	// Readers: render extraction (the snapshot's ParticleEmitters and Particles, at the frame's interpolation alpha between
	// each particle's PreviousPosition and Position, Scene/RenderExtraction.h), Test.GetParticleDigest and the session's
	// particle stats.
	struct ParticleStateComponent
	{
		ParticleEmitterState State{ 0 };
		// The session tick the state was last stepped to; empty until a Particles phase anchors it (a state Emit made before
		// its emitter's first phase has none, whatever tick it was made at, so it never catches up the ticks before it).
		std::optional<uint64_t> LastTick{};
	};

}
