#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>
#include <string>

namespace Engine {

	// Registry name "SpriteAnimator" (Architecture §5.3; M21, Docs/Decisions/0032-m21-contract.md decision 5): plays a clip of
	// its entity's SpriteRenderer sheet (Asset/SpriteSheetData.h: a clip is a named list of {Frame, Ticks} steps with Loop),
	// driven by fixed-step ticks in the SpriteAnimation phase (Scene/SpriteAnimationSystem.h), never by the wall clock.
	// Requires SpriteRenderer. The playback state is component data (Step, StepTicks, Playing), so the scene's serialization,
	// and with it the state hash, covers it and a replay reproduces every frame.
	//   - Clip: the clip's Name; empty plays nothing. A name the sheet's clips lack is SPRITE_UNKNOWN_CLIP and plays nothing.
	//   - Playing: whether the phase advances it. A clip without Loop that shows its last step for that step's Ticks clears
	//     Playing and holds its last frame.
	//   - Step: the index of the clip step shown; StepTicks: the ticks that step has been shown before the current one.
	struct SpriteAnimatorComponent
	{
		std::string Clip{};
		bool Playing = true;
		uint32_t Step = 0;
		uint32_t StepTicks = 0;
	};

}
