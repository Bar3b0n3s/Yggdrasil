#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The Taa pass's push constants (M26, C26.1, R-TAA; Docs/Decisions/0037-m26-contract.md decision 17; Renderer/TaaPass.h,
	// Passes/Taa.slang). TaaFlagReset: the history is not used (a history reset, the view's first Taa pass, the history images
	// created again), so the resolved image is the current one, exactly.
#if defined(ENGINE_SHADER)
	static const uint TaaFlagReset = 1;
#else
inline constexpr uint32_t TaaFlagReset = 1;
#endif

	struct TaaConstants
	{
		UInt2 ImageSize;         // SceneColor's size in pixels
		Float2 InverseImageSize; // 1 / ImageSize: the history's texture coordinates
		float Feedback = 0.0f;   // the clamped history's weight in the resolved image, in [0, 1) (TaaPass.h's TaaFeedback)
		uint32_t Flags = 0;      // TaaFlag bits
		uint32_t Padding0 = 0;
		uint32_t Padding1 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(TaaConstants) == 32, "TaaConstants must match its 32-byte push-constant layout");
	static_assert(offsetof(TaaConstants, Feedback) == 16 && offsetof(TaaConstants, Flags) == 20,
		"TaaConstants members must sit where the push-constant layout puts them");

}
#endif
