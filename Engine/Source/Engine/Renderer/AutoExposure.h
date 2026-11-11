#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>

// Histogram auto-exposure (M26, C26.1, R-AEX; Docs/Decisions/0037-m26-contract.md decision 15): the CPU statement of what the
// Exposure pass (the HdrPost stage, after Bloom; stream X-A's Renderer/ExposurePass.h) computes on the GPU, which its tests
// compare exactly ("ExposurePass: the luminance histogram equals the CPU bins"), and the temporal rule of the adapted
// exposure (the temporal-state section, decision 10).
//   1. The histogram: every pixel of the image Tonemap exposes (SceneColor with the bloom composite, before the exposure) at
//      the scene resolution adds one to the bin of its Rec. 709 luminance (GetLuminanceHistogramBin): ExposureHistogramBins
//      bins over log2 luminance in [ExposureHistogramMinLog2, ExposureHistogramMaxLog2), the first bin also counting every
//      luminance at or below 2^ExposureHistogramMinLog2 (black, and non-finite values) and the last every one at or above
//      the range's end. The GPU counts with one atomic add per pixel into its bin: integer sums, so the order of the adds
//      never changes a count (plan §7 item 9 forbids atomics only where they decide a float sum or an order).
//   2. The target: the mean log2 luminance of the pixels between the ExposureLowPercentile and ExposureHighPercentile of
//      the histogram's count (bins taken whole at their centres, from the darkest), then the exposure in EV that maps it to
//      middle grey (ExposureMiddleGrey): target = log2(ExposureMiddleGrey) - meanLog2, clamped to [AutoExposureMinEV,
//      AutoExposureMaxEV] (ComputeExposureTarget), with ExposureEV added as compensation by the tonemap pass.
//   3. Adaptation: the adapted exposure moves towards the target by at most AutoExposureSpeedUp EV per second when the
//      target is brighter (larger) and AutoExposureSpeedDown when darker, over CameraData::DeltaTime (AdaptExposure); after a
//      reset (CameraData::PreviousViewProjection empty: a view's first frame, every capture's first warm-up frame) it is the
//      target at once, which is what makes captures with auto-exposure byte-identical.
// Exact equality with the GPU: the luminance is the three products and two sums in that order, which the shader writes with
// NoContraction (as Scene.slang's MultiplyExact), so no fused multiply-add changes a bit; a luminance's bin is found by
// comparing it with the bins' lower edges, one fixed table of floats both sides share (2^(ExposureHistogramMinLog2 + i *
// width), written out in stream X-A's Shared/ExposureConstants.h), never by a log2. Every function is pure and uses
// IEEE-exact operations only (the target's mean log2 reads each bin's centre from the same table), so its results are
// identical in every configuration.
//
// Frozen by the M26 contract C26.1; stream X-A implements it.

namespace Engine {

	inline constexpr uint32_t ExposureHistogramBins = 64;
	inline constexpr float ExposureHistogramMinLog2 = -12.0f;
	inline constexpr float ExposureHistogramMaxLog2 = 20.0f;
	inline constexpr float ExposureLowPercentile = 0.5f;
	inline constexpr float ExposureHighPercentile = 0.95f;
	inline constexpr float ExposureMiddleGrey = 0.18f;

	using ExposureHistogram = std::array<uint32_t, ExposureHistogramBins>;

	// The bin of a pixel of linear radiance `radiance` (step 1): of its luminance 0.2126 r + 0.7152 g + 0.0722 b. Pure.
	[[nodiscard]] uint32_t GetLuminanceHistogramBin(const glm::vec3& radiance);

	// The histogram of `pixels` (step 1), each an RGBA radiance whose alpha is ignored. Pure.
	[[nodiscard]] ExposureHistogram ComputeLuminanceHistogram(std::span<const glm::vec4> pixels);

	// The target exposure in EV of `histogram` under `post`'s range (step 2); 0 for an empty histogram. Pure.
	[[nodiscard]] float ComputeExposureTarget(const ExposureHistogram& histogram, const PostProcessSettings& post);

	// The adapted exposure after `deltaTime` seconds from `current` towards `target` at `post`'s speeds (step 3); `target`
	// itself when `reset`. Pure.
	[[nodiscard]] float AdaptExposure(float current, float target, float deltaTime, bool reset, const PostProcessSettings& post);

}
