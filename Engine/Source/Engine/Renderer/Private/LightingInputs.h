#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/ShaderLight.h"

// The scene renderer's lighting inputs (SceneRenderer.h, §8.5, §8.6): the light list entries of the forward pass
// (Shared/ShaderLight.h) and the environment constants (Shared/EnvironmentConstants.h). Pure functions.

namespace Engine {

	struct GpuEnvironment;

	namespace Utils {

		// The ShaderLight of `light`, which CullLights kept (finite, non-zero radiance and range): its radiance Color *
		// Intensity, unit direction, 1 / Range², source radius and the cosines of its cone (the inner angle at most the
		// outer).
		[[nodiscard]] ShaderLight MakeShaderLight(const LightData& light);

		// The largest environment intensity the constants carry: the largest finite binary16 value, like the baked texels
		// and SH9 coefficients it scales (EnvironmentBakeMaxHalf), so the products the CPU and the forward shader form with
		// it (SH9 times intensity, the prefiltered texel times intensity, sums of nine SH terms) stay finite in float.
		inline constexpr float MaxEnvironmentIntensity = 65504.0f;

		// The environment constants of `environment` lit by `mirror` (its GpuEnvironment), or by the constant ambient
		// FallbackColor * Intensity when `mirror` is null. A non-finite or negative intensity is 0 and a larger one than
		// MaxEnvironmentIntensity is that, a non-finite rotation 0, the skybox blur is clamped to [0, 1]; a non-finite or
		// negative ambient colour channel is 0, and so is a scaled SH9 coefficient that is not finite.
		[[nodiscard]] EnvironmentConstants MakeEnvironmentConstants(const RenderEnvironment& environment, const GpuEnvironment* mirror);

	}

}
