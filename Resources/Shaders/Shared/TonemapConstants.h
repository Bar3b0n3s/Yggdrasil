#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// TonemapConstants::Tonemapper values: RenderTonemapper's enumerators in declaration order (Renderer/RenderSnapshot.h;
	// TonemapPass.cpp checks the correspondence), so the snapshot's value is passed through unchanged.
#if defined(ENGINE_SHADER)
	static const uint TonemapperAgx = 0;
	static const uint TonemapperAces = 1;
	static const uint TonemapperPbrNeutral = 2;
	static const uint TonemapperLinear = 3;
#else
inline constexpr uint32_t TonemapperAgx = 0;
inline constexpr uint32_t TonemapperAces = 1;
inline constexpr uint32_t TonemapperPbrNeutral = 2;
inline constexpr uint32_t TonemapperLinear = 3;
#endif

	// TonemapConstants::Flags bits (Renderer/TonemapPass.h's steps 1, 4 and 5).
#if defined(ENGINE_SHADER)
	static const uint TonemapFlagBloom = 1;      // composite the bloom chain's mip 0 (t1)
	static const uint TonemapFlagEncodeSrgb = 2; // apply the sRGB OETF
	static const uint TonemapFlagDither = 4;     // blue-noise triangular dither from t7
#else
inline constexpr uint32_t TonemapFlagBloom = 1;
inline constexpr uint32_t TonemapFlagEncodeSrgb = 2;
inline constexpr uint32_t TonemapFlagDither = 4;
#endif

	// The push constants of the Tonemap pass (Passes/Tonemap.slang, §8.3 pass 11) and of the TonemapCurves test program
	// (Passes/TonemapCurves.slang, which reads Tonemapper only): what TonemapPass applies besides the view's exposure.
	struct TonemapConstants
	{
		uint32_t Tonemapper = TonemapperAgx; // one of the Tonemapper values above
		uint32_t Flags = 0;                  // TonemapFlag bits
		float BloomIntensity = 0.0f;         // the bloom composite's lerp factor, in [0, 1]
		uint32_t Padding0 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(TonemapConstants) == 16, "TonemapConstants must match its 16-byte push-constant layout");
	static_assert(offsetof(TonemapConstants, Flags) == 4 && offsetof(TonemapConstants, BloomIntensity) == 8,
		"TonemapConstants members must sit where the push-constant layout puts them");

}
#endif
