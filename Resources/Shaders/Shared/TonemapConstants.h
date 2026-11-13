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

	// TonemapConstants::Flags bits (Renderer/TonemapPass.h's steps 1, 4 and 5). M26 (C26.1, Docs/Decisions/0037-m26-contract.md
	// decision 15): AutoExposure multiplies the exposure by the adapted exposure the Exposure pass wrote (t8, stream X-A), and
	// Lut grades the tonemapped value through the 3D LUT (t9, sampled with s0, LutIntensity its share, stream X-B), in the
	// order exposure, curve, LUT, encode, dither. TonemapPass::Record sets AutoExposure when its inputs carry an adapted
	// exposure and Lut when they carry a LUT (SceneRenderer's Tonemap stage passes them for ExposureMode Auto and a
	// PostProcessSettings::Lut, outside pixel-art mode); PostProcess version 2's defaults (Manual, no LUT) set neither, so
	// they render as version 1 did.
#if defined(ENGINE_SHADER)
	static const uint TonemapFlagBloom = 1;        // composite the bloom chain's mip 0 (t1)
	static const uint TonemapFlagEncodeSrgb = 2;   // apply the sRGB OETF
	static const uint TonemapFlagDither = 4;       // blue-noise triangular dither from t7
	static const uint TonemapFlagAutoExposure = 8; // M26: times the adapted exposure (t8)
	static const uint TonemapFlagLut = 16;         // M26: grade through the LUT (t9)
#else
inline constexpr uint32_t TonemapFlagBloom = 1;
inline constexpr uint32_t TonemapFlagEncodeSrgb = 2;
inline constexpr uint32_t TonemapFlagDither = 4;
inline constexpr uint32_t TonemapFlagAutoExposure = 8;
inline constexpr uint32_t TonemapFlagLut = 16;
#endif

	// The push constants of the Tonemap pass (Passes/Tonemap.slang, §8.3 pass 11) and of the TonemapCurves test program
	// (Passes/TonemapCurves.slang, which reads Tonemapper only): what TonemapPass applies besides the view's exposure.
	// M26 (C26.1, R-LUT; Docs/Decisions/0037-m26-contract.md decision 15): LutIntensity in place of the padding, and LutScale
	// and LutOffset appended (48 bytes): the LUT's 3D texture coordinate of the display-encoded value e is
	// e * LutScale.xyz + LutOffset.xyz, sampled trilinearly with a clamping sampler, the CPU folding the LUT's domain and its
	// texel centres into them (TonemapPass, stream X-B, from TonemapPassInputs' LutDomainMin, LutDomainMax and the LUT's Size
	// N): LutScale = (N - 1) / (N (LutDomainMax - LutDomainMin)) and LutOffset = 0.5 / N - LutDomainMin * LutScale, so the
	// domain's ends land on the first and last entries' centres; w is 0 in both. Zero, and unread, without TonemapFlagLut.
	struct TonemapConstants
	{
		uint32_t Tonemapper = TonemapperAgx; // one of the Tonemapper values above
		uint32_t Flags = 0;                  // TonemapFlag bits
		float BloomIntensity = 0.0f;         // the bloom composite's lerp factor, in [0, 1]
		float LutIntensity = 0.0f;           // M26: the graded value's share with TonemapFlagLut, in [0, 1] (the former padding)
		Float4 LutScale;                     // M26: xyz the LUT coordinate's scale (above)
		Float4 LutOffset;                    // M26: xyz the LUT coordinate's offset (above)
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(TonemapConstants) == 48, "TonemapConstants must match its 48-byte push-constant layout");
	static_assert(offsetof(TonemapConstants, Flags) == 4 && offsetof(TonemapConstants, BloomIntensity) == 8
			&& offsetof(TonemapConstants, LutIntensity) == 12 && offsetof(TonemapConstants, LutScale) == 16
			&& offsetof(TonemapConstants, LutOffset) == 32,
		"TonemapConstants members must sit where the push-constant layout puts them");

}
#endif
