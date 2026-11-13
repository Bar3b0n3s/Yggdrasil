#include "EnginePCH.h"
#include "Engine/Renderer/AutoExposure.h"

#include "Shared/ExposureConstants.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

// Histogram auto-exposure's CPU statement (AutoExposure.h; M26, stream X-A, Docs/Decisions/0037-m26-contract.md decision
// 15). Passes/Exposure.slang computes the same steps on the GPU from the same shared numbers (Shared/ExposureConstants.h):
// the bins exactly (comparisons and integer counts), the target and the adapted exposure with the same IEEE operations in
// the same order, NoContraction on the GPU, where only the mean's division and exp2 may round differently from the CPU's.
//
// The target's mean: each bin's pixels inside the metered window count at the bin's centre, -12 + (i + 0.5) / 2 in log2
// luminance (exact in float), summed in bin order as float(count) * centre (one rounding each), then divided once by the
// window's pixel count.

namespace Engine {

	static_assert(ExposureHistogramBinCount == ExposureHistogramBins, "Shared/ExposureConstants.h's table has a lower edge per bin");
	static_assert(ExposureHistogramMaxLog2 - ExposureHistogramMinLog2 == 0.5f * static_cast<float>(ExposureHistogramBins),
		"the bins are half a stop wide, so their edges are 2^(min + i / 2) and their centres exact in float");
	static_assert(ExposureHistogramMinLog2 == -12.0f,
		"Shared/ExposureConstants.h's edges and Passes/Exposure.slang's bin centres (-12 + (i + 0.5) / 2) start at 2^-12");
	static_assert(1.0f / static_cast<float>(ExposureLowRankDivisor) == ExposureLowPercentile,
		"the metered window starts at floor(N / ExposureLowRankDivisor): ExposureLowPercentile of the pixels");
	static_assert(1.0f - 1.0f / static_cast<float>(ExposureHighRankDivisor) == ExposureHighPercentile,
		"the metered window ends at N - floor(N / ExposureHighRankDivisor): ceil(ExposureHighPercentile N)");

	namespace Utils {

		// The width of a bin in log2 luminance: half a stop.
		constexpr float ExposureBinWidthLog2 = (ExposureHistogramMaxLog2 - ExposureHistogramMinLog2) / static_cast<float>(ExposureHistogramBins);

		// The centre of bin `bin` in log2 luminance (exact: a multiple of a quarter).
		static float GetBinCentreLog2(uint32_t bin)
		{
			return ExposureHistogramMinLog2 + (static_cast<float>(bin) + 0.5f) * ExposureBinWidthLog2;
		}

		// A speed of the adaptation, in EV per second: a negative or non-finite one adapts nothing.
		static float SanitizeSpeed(float speed)
		{
			return std::isfinite(speed) && speed > 0.0f ? speed : 0.0f;
		}

	}

	uint32_t GetLuminanceHistogramBin(const glm::vec3& radiance)
	{
		// Each product and sum rounded on its own: the build never contracts them into fused multiply-adds (/fp:precise,
		// -ffp-contract=off), and the shader decorates them NoContraction.
		const float red = std::bit_cast<float>(ExposureLuminanceWeightBits[0]) * radiance.r;
		const float green = std::bit_cast<float>(ExposureLuminanceWeightBits[1]) * radiance.g;
		const float blue = std::bit_cast<float>(ExposureLuminanceWeightBits[2]) * radiance.b;
		const float luminance = (red + green) + blue;
		if (!std::isfinite(luminance))
			return 0;
		// The last bin whose lower edge is at most the luminance: a binary search over the 63 edges above bin 0's.
		uint32_t bin = 0;
		for (uint32_t step = ExposureHistogramBins / 2; step > 0; step /= 2)
		{
			if (luminance >= std::bit_cast<float>(ExposureHistogramEdgeBits[bin + step]))
				bin += step;
		}
		return bin;
	}

	ExposureHistogram ComputeLuminanceHistogram(std::span<const glm::vec4> pixels)
	{
		ExposureHistogram histogram{};
		for (const glm::vec4& pixel : pixels)
			++histogram[GetLuminanceHistogramBin(glm::vec3(pixel))];
		return histogram;
	}

	float ComputeExposureTarget(const ExposureHistogram& histogram, const PostProcessSettings& post)
	{
		uint64_t total = 0;
		for (const uint32_t count : histogram)
			total += count;
		if (total == 0)
			return 0.0f;
		const uint64_t low = total / ExposureLowRankDivisor;
		const uint64_t high = total - total / ExposureHighRankDivisor;
		uint64_t below = 0;
		uint64_t metered = 0;
		float sum = 0.0f;
		for (uint32_t bin = 0; bin < ExposureHistogramBins; ++bin)
		{
			const uint64_t start = std::max(below, low);
			const uint64_t end = std::min(below + histogram[bin], high);
			if (end > start)
			{
				const float weighted = static_cast<float>(end - start) * Utils::GetBinCentreLog2(bin);
				sum = sum + weighted;
				metered += end - start;
			}
			below += histogram[bin];
		}
		const float mean = sum / static_cast<float>(metered);
		const float target = std::bit_cast<float>(ExposureMiddleGreyLog2Bits) - mean;
		// min(max(.)), not std::clamp: an inverted range (refused by the PostProcess validator) still gives a value.
		return std::min(std::max(target, post.AutoExposureMinEV), post.AutoExposureMaxEV);
	}

	float AdaptExposure(float current, float target, float deltaTime, bool reset, const PostProcessSettings& post)
	{
		// A reset, or a previous exposure that is not a number (a buffer never written), converges at once.
		if (reset || !std::isfinite(current))
			return target;
		const float seconds = std::isfinite(deltaTime) && deltaTime > 0.0f ? deltaTime : 0.0f;
		if (target > current)
		{
			const float step = Utils::SanitizeSpeed(post.AutoExposureSpeedUp) * seconds;
			return target - current <= step ? target : current + step;
		}
		if (target < current)
		{
			const float step = Utils::SanitizeSpeed(post.AutoExposureSpeedDown) * seconds;
			return current - target <= step ? target : current - step;
		}
		return target;
	}

}
