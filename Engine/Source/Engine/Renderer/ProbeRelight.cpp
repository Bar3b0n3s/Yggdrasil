#include "EnginePCH.h"
#include "Engine/Renderer/ProbeRelight.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

// Relit probes (ProbeRelight.h; Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3).

namespace Engine {

	size_t GetSkyTransferIndex(size_t i, size_t j)
	{
		ENGINE_CORE_ASSERT(i < 9 && j < 9, "Sky transfer entry ({}, {}) of a 9 x 9 matrix", i, j);
		const size_t row = std::min(i, j);
		const size_t column = std::max(i, j);
		// Rows 0 to row - 1 hold 9, 8 ... entries: 9 row - row (row - 1) / 2 = row (19 - row) / 2 in all.
		return row * (19 - row) / 2 + (column - row);
	}

	uint32_t GetShBand(size_t index)
	{
		ENGINE_CORE_ASSERT(index < 9, "SH coefficient {} of nine", index);
		return index == 0 ? 0U : (index < 4 ? 1U : 2U);
	}

	std::array<double, 3> GetIrradianceBandFactors()
	{
		static_assert(EnvironmentData::IrradianceShWindow == 4.0f, "the window's cosines below assume w = 4");
		return { 1.0, (2.0 / 3.0) * ((1.0 + std::sqrt(0.5)) * 0.5), 0.25 * 0.5 };
	}

	std::array<glm::vec3, 9> RelightIrradianceSH9(const std::array<glm::vec3, 9>& local, std::span<const float, IrradianceTransferCount> transfer,
		const std::array<glm::vec3, 9>& skyRadiance)
	{
		const std::array<double, 3> factors = GetIrradianceBandFactors();
		std::array<glm::vec3, 9> irradiance{};
		for (size_t i = 0; i < 9; ++i)
		{
			glm::dvec3 seen(0.0);
			for (size_t j = 0; j < 9; ++j)
				seen += glm::dvec3(skyRadiance[j]) * static_cast<double>(transfer[GetSkyTransferIndex(i, j)]);
			irradiance[i] = glm::vec3(glm::dvec3(local[i]) + seen * factors[GetShBand(i)]);
		}
		return irradiance;
	}

	std::array<glm::vec3, 9> MakeSkyRadianceSH9(std::span<const glm::vec4, 10> totals, float intensity)
	{
		const double solidAngle = static_cast<double>(totals[9].x);
		const double normalization = std::isfinite(solidAngle) && solidAngle > 0.0 ? 4.0 * std::numbers::pi / solidAngle : 0.0;
		const double scale = std::isfinite(intensity) ? std::clamp(static_cast<double>(intensity), 0.0, 65504.0) : 0.0;
		std::array<glm::vec3, 9> radiance{};
		for (size_t index = 0; index < radiance.size(); ++index)
			radiance[index] = glm::vec3(glm::dvec3(glm::vec3(totals[index])) * (normalization * scale));
		return radiance;
	}

	float ComputeReflectionRelightScale(const glm::vec3& relit, const glm::vec3& baked)
	{
		const glm::dvec3 weights(0.2126, 0.7152, 0.0722);
		const double bakedLuminance = glm::dot(glm::dvec3(baked), weights);
		if (!(bakedLuminance >= 1e-6))
			return 1.0f;
		const double ratio = glm::dot(glm::dvec3(relit), weights) / bakedLuminance;
		return std::isfinite(ratio) ? static_cast<float>(std::clamp(ratio, 0.0, static_cast<double>(MaxReflectionRelightScale))) : 1.0f;
	}

}
