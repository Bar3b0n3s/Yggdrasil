#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <array>
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The Exposure pass's shared numbers and structs (M26, C26.1, R-AEX; Docs/Decisions/0037-m26-contract.md decision 15;
	// Renderer/ExposurePass.h, Passes/Exposure.slang): what Renderer/AutoExposure.h's CPU statement and the GPU share. The
	// floats are written as their IEEE binary32 bit patterns, which both sides reinterpret (std::bit_cast in C++, asfloat in
	// Slang), so no decimal literal can be read differently by the C++ compiler and slangc:
	//   - ExposureHistogramEdgeBits: the lower edge of each of the ExposureHistogramBinCount bins, 2^(-12 + i / 2) rounded to
	//     the nearest float (the even entries are powers of two, the odd ones sqrt(2)'s nearest float times one), over the
	//     log2 range [-12, 20) of AutoExposure.h. A luminance falls in the last bin whose edge is at most it, found by
	//     comparisons only (never a log2): bin 0 holds everything below edge 1 (black and negative values), and every
	//     non-finite luminance is put in bin 0 before any comparison.
	//   - ExposureLuminanceWeightBits: Rec. 709's 0.2126, 0.7152 and 0.0722 (the nearest floats). The luminance of (r, g, b)
	//     is (0.2126 r + 0.7152 g) + 0.0722 b, each product and sum rounded on its own (NoContraction on the GPU, /fp:precise
	//     and -ffp-contract=off on the CPU).
	//   - ExposureMiddleGreyLog2Bits: log2(0.18) (AutoExposure.h's ExposureMiddleGrey), the target's middle grey.
	//   - The metered window of a histogram of N pixels: the ranks [N / ExposureLowRankDivisor, N - N / ExposureHighRankDivisor)
	//     of its pixels sorted from the darkest, in integer arithmetic: floor(N / 2) and ceil(0.95 N), AutoExposure.h's
	//     ExposureLowPercentile and ExposureHighPercentile (AutoExposure.cpp checks the correspondence). The window is never
	//     empty for N >= 1.
	// The tables' values are written once for both languages: a static const array in Slang, a std::array in C++.
#if defined(ENGINE_SHADER)
	static const uint ExposureHistogramBinCount = 64;
	static const uint ExposureLowRankDivisor = 2;
	static const uint ExposureHighRankDivisor = 20;
	static const uint ExposureMiddleGreyLog2Bits = 0xC01E54E3u;
	static const uint ExposureLuminanceWeightBits[3] = { 0x3E59B3D0u, 0x3F371759u, 0x3D93DD98u };
	static const uint ExposureHistogramEdgeBits[ExposureHistogramBinCount] = {
#else
inline constexpr uint32_t ExposureHistogramBinCount = 64;
inline constexpr uint32_t ExposureLowRankDivisor = 2;
inline constexpr uint32_t ExposureHighRankDivisor = 20;
inline constexpr uint32_t ExposureMiddleGreyLog2Bits = 0xC01E54E3u;
inline constexpr std::array<uint32_t, 3> ExposureLuminanceWeightBits = { 0x3E59B3D0u, 0x3F371759u, 0x3D93DD98u };
inline constexpr std::array<uint32_t, ExposureHistogramBinCount> ExposureHistogramEdgeBits = {
#endif
		0x39800000u,
		0x39B504F3u,
		0x3A000000u,
		0x3A3504F3u,
		0x3A800000u,
		0x3AB504F3u,
		0x3B000000u,
		0x3B3504F3u,
		0x3B800000u,
		0x3BB504F3u,
		0x3C000000u,
		0x3C3504F3u,
		0x3C800000u,
		0x3CB504F3u,
		0x3D000000u,
		0x3D3504F3u,
		0x3D800000u,
		0x3DB504F3u,
		0x3E000000u,
		0x3E3504F3u,
		0x3E800000u,
		0x3EB504F3u,
		0x3F000000u,
		0x3F3504F3u,
		0x3F800000u,
		0x3FB504F3u,
		0x40000000u,
		0x403504F3u,
		0x40800000u,
		0x40B504F3u,
		0x41000000u,
		0x413504F3u,
		0x41800000u,
		0x41B504F3u,
		0x42000000u,
		0x423504F3u,
		0x42800000u,
		0x42B504F3u,
		0x43000000u,
		0x433504F3u,
		0x43800000u,
		0x43B504F3u,
		0x44000000u,
		0x443504F3u,
		0x44800000u,
		0x44B504F3u,
		0x45000000u,
		0x453504F3u,
		0x45800000u,
		0x45B504F3u,
		0x46000000u,
		0x463504F3u,
		0x46800000u,
		0x46B504F3u,
		0x47000000u,
		0x473504F3u,
		0x47800000u,
		0x47B504F3u,
		0x48000000u,
		0x483504F3u,
		0x48800000u,
		0x48B504F3u,
		0x49000000u,
		0x493504F3u,
	};

	// ExposureConstants::Flags bits. Reset: the adapted exposure is the target at once (a history reset, a view's first
	// Exposure pass, a buffer created again). Bloom: CSHistogram composites the bloom chain's mip 0 (t1) as Tonemap does.
#if defined(ENGINE_SHADER)
	static const uint ExposureFlagReset = 1;
	static const uint ExposureFlagBloom = 2;
#else
inline constexpr uint32_t ExposureFlagReset = 1;
inline constexpr uint32_t ExposureFlagBloom = 2;
#endif

	// The push constants of the Exposure program's two entries (ExposurePass::Record): CSHistogram reads the image's size,
	// the bloom flag and its share; CSAdapt the adapted exposure's range, its speeds, the frame's DeltaTime and the reset flag.
	struct ExposureConstants
	{
		UInt2 ImageSize;             // SceneColor's size in pixels
		Float2 InverseImageSize;     // 1 / ImageSize: the bloom's texture coordinates, as Tonemap's InverseViewportSize
		uint32_t Flags = 0;          // ExposureFlag bits
		float BloomIntensity = 0.0f; // the bloom composite's lerp factor, in [0, 1], as Tonemap's
		float MinEV = -6.0f;         // PostProcessSettings::AutoExposureMinEV
		float MaxEV = 12.0f;         // PostProcessSettings::AutoExposureMaxEV
		float SpeedUp = 3.0f;        // EV per second towards a brighter (larger) target, >= 0
		float SpeedDown = 1.0f;      // EV per second towards a darker (smaller) target, >= 0
		float DeltaTime = 0.0f;      // CameraData::DeltaTime, seconds, >= 0
		uint32_t Padding0 = 0;
	};

	// The adapted exposure, the one element of the renderer's per-view exposure buffer: written by CSAdapt, read back by the
	// next CSAdapt (the previous frame's AdaptedEV) and by the Tonemap pass (t8, TonemapFlagAutoExposure), which multiplies
	// ViewConstants::Exposure (2^ExposureEV, the compensation) by Multiplier.
	struct ExposureState
	{
		float AdaptedEV = 0.0f;  // the adapted exposure in EV (AutoExposure.h's AdaptExposure)
		float Multiplier = 1.0f; // 2^AdaptedEV
		float TargetEV = 0.0f;   // this frame's target (ComputeExposureTarget)
		uint32_t Padding0 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(ExposureConstants) == 48, "ExposureConstants must match its 48-byte push-constant layout");
	static_assert(offsetof(ExposureConstants, InverseImageSize) == 8 && offsetof(ExposureConstants, Flags) == 16
			&& offsetof(ExposureConstants, BloomIntensity) == 20 && offsetof(ExposureConstants, MinEV) == 24
			&& offsetof(ExposureConstants, DeltaTime) == 40,
		"ExposureConstants members must sit where the push-constant layout puts them");
	static_assert(sizeof(ExposureState) == 16, "ExposureState must match its 16-byte structured-buffer element");

}
#endif
