#pragma once

#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Relit probes (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3, "Relit probes"; Architecture §8.6): the CPU
// half of relighting an irradiance probe by the sky. A probe baked under a Sky keeps its relight block
// (Asset/IrradianceVolumeData.h's IrradianceProbeRelight): Local, the irradiance of the scene's own lights, and Transfer, the
// symmetric 9 x 9 matrix T that turns the sky's radiance SH9 S into the radiance SH9 of the sky the probe sees. Its irradiance
// under any sky is then
//     E = Local + conv(T S)
// conv the clamped cosine's convolution over pi and the Hanning window per band (§8.6 step 4, EnvironmentData's convention),
// which ProbeCapture's ProjectCubeToIrradianceSH9 applies to a capture. The bake writes the probe's coefficients at the bake's
// sky (what a renderer that does not relight shows); the renderer's ProbeRelight row evaluates the same sum on the GPU each
// time the sky's light changes (Passes/Sky.slang's CSProbeRelight), so a cave darkens at night with no bake between. The light
// that the sun and the sky bounce off surfaces is not in it (decision 1's accepted limit).
//
// Pure functions in double precision with IEEE-exact operations and sqrt only, in a fixed order, so a bake's values are
// identical in every configuration.

namespace Engine {

	// The largest scale a reflection probe inside a relit volume takes (ComputeReflectionRelightScale).
	inline constexpr float MaxReflectionRelightScale = 4.0f;

	// The index of T_ij (i and j below 9, in either order) in IrradianceProbeRelight::Transfer: the upper triangle, row by row.
	[[nodiscard]] size_t GetSkyTransferIndex(size_t i, size_t j);

	// The SH band of coefficient `index` (below 9): 0, then 1 for 1 to 3, then 2.
	[[nodiscard]] uint32_t GetShBand(size_t index);

	// Per band: the clamped cosine's convolution over pi (1, 2/3, 1/4) times the Hanning window (1 + cos(pi l / w)) / 2 of
	// EnvironmentData::IrradianceShWindow (w 4, whose cosines 1, sqrt(1/2) and 0 need no transcendental): what turns a radiance
	// SH9 into the irradiance SH9 of EnvironmentData's convention.
	[[nodiscard]] std::array<double, 3> GetIrradianceBandFactors();

	// A probe's irradiance SH9 under the sky whose radiance SH9 is `skyRadiance`: `local` + conv(T skyRadiance), T the symmetric
	// matrix whose upper triangle `transfer` holds (see the file comment). Pure.
	[[nodiscard]] std::array<glm::vec3, 9> RelightIrradianceSH9(const std::array<glm::vec3, 9>& local, std::span<const float, IrradianceTransferCount> transfer,
		const std::array<glm::vec3, 9>& skyRadiance);

	// The sky's radiance SH9 from its lighting's reduced totals (SceneRenderer::GetSkyRadianceTotals: the nine coefficients' sums
	// in xyz, then the solid angle in the tenth's x), normalized as CSSkyFinalize normalizes them (times 4 pi over the solid
	// angle, 0 for a solid angle that is not positive and finite) and times `intensity` (the Sky's Intensity, clamped to
	// [0, 65504], 0 when not finite). Pure.
	[[nodiscard]] std::array<glm::vec3, 9> MakeSkyRadianceSH9(std::span<const glm::vec4, 10> totals, float intensity);

	// The scale of a reflection probe inside a relit volume (decision 3): the luminance (Rec. 709) of the relit constant term
	// `relit` of the volume's probe nearest its centre over that of its baked one `baked`, clamped to [0,
	// MaxReflectionRelightScale]; 1 when the baked luminance is below 1e-6 (a probe baked in the dark keeps its bake). What
	// CSProbeRelight writes per reflection probe, times its Intensity. Pure.
	[[nodiscard]] float ComputeReflectionRelightScale(const glm::vec3& relit, const glm::vec3& baked);

}
