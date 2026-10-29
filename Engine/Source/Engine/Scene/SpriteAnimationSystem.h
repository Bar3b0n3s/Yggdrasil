#pragma once

#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/Time.h"
#include "Engine/Scene/Components/SpriteAnimatorComponent.h"
#include "Engine/Scene/SpriteValidation.h"

#include <cstdint>
#include <span>

// Sprite animation (M21, C21.1, R-2D-ANIM; Docs/Decisions/0032-m21-contract.md decision 5): the SpriteAnimation phase of the
// fixed step (PlaySessionPhase::SpriteAnimation, after NativeStep and before PreStepTransformUpdate; ADR 0028's phase-2
// execution order), in Play and Simulate alike. It runs after the tick's gameplay code (scripts, tasks, native systems),
// which starts and stops clips, so a clip started in OnFixedUpdate shows its first step at that tick; one started in a
// contact callback or OnDestroy (later in the tick) advances from the next tick. Driven by ticks only, never the wall
// clock: the playback state is SpriteAnimatorComponent data, so the scene's serialization and the state hash cover it.
//
// The step, for every effectively enabled entity with a SpriteAnimator and a SpriteRenderer, in canonical order: when
// Playing and Clip names a clip of the SpriteRenderer's sheet (loaded through the session's AssetManager), the
// SpriteRenderer's Frame becomes AdvanceSpriteAnimator's result; otherwise nothing changes (an unknown clip, or a sheet that
// is missing or fails to load, raises SPRITE_UNKNOWN_CLIP once per entity and clip name, logged at Warn and kept in
// GetDiagnostics). The writes go through the scene, so the change tracker sees them.
//
// Threading: main thread only. Not copyable or movable. Frozen by the M21 contract (C21.1); stream B implements it.

namespace Engine {

	class AssetManager;
	class Scene;

	// One tick of `animator` over `clip` (1 or more steps), pure: clamps Step to the clip's last step, takes the frame of step
	// Step as the result, then StepTicks + 1; when that reaches or passes the step's Ticks, StepTicks becomes 0 and Step + 1;
	// past the last step, Step becomes 0 with Loop, otherwise it stays on the last step and Playing becomes false (the last
	// frame holds). So each step shows its frame for exactly its Ticks ticks, and a StepTicks at or past the step's Ticks
	// (Step and StepTicks are fields that entity.update, a scene file or a Clip written without SpriteAnimator:Play can leave
	// from another clip) shows its step for one tick, then moves on. `animator.Playing` is not read: the caller advances only
	// playing animators.
	[[nodiscard]] uint32_t AdvanceSpriteAnimator(SpriteAnimatorComponent& animator, const SpriteClip& clip);

	struct SpriteAnimationSystemSpecification
	{
		// Required, outliving the system (documented back-references): the play session's runtime scene and asset manager.
		Scene* RuntimeScene = nullptr;
		AssetManager* Assets = nullptr;
	};

	class SpriteAnimationSystem
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class SpriteAnimationSystem;
		};

		// Use Create.
		explicit SpriteAnimationSystem(ConstructionKey key);
		~SpriteAnimationSystem();

		SpriteAnimationSystem(const SpriteAnimationSystem&) = delete;
		SpriteAnimationSystem& operator=(const SpriteAnimationSystem&) = delete;

		// Errors: InvalidArgument for a missing scene or asset manager.
		[[nodiscard]] static Result<Scope<SpriteAnimationSystem>> Create(const SpriteAnimationSystemSpecification& specification);

		// The SpriteAnimation phase (see the file comment).
		void Step(const SimStep& step);
		// The diagnostics raised so far, in the order raised.
		[[nodiscard]] std::span<const SpriteSceneIssue> GetDiagnostics() const;
	private:
		// The back-references and the diagnostics raised (Scene/SpriteAnimationSystem.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
