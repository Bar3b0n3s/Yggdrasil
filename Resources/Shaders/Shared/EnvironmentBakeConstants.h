#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The largest finite binary16 value: the bake's RGBA16F cubes clamp every value to it (a radiance beyond binary16's range
	// would otherwise become an infinity, which ValidateEnvironmentData rejects).
#if defined(ENGINE_SHADER)
	static const float EnvironmentBakeMaxHalf = 65504.0;
#else
inline constexpr float EnvironmentBakeMaxHalf = 65504.0f;
#endif

	// The per-dispatch constants of the environment bake (Renderer/EnvironmentBaker.h, Passes/EnvironmentBake.slang), the
	// push constants of its five compute pipelines; each entry point reads the members its comment names.
	struct EnvironmentBakeConstants
	{
		uint32_t FaceSize = 0;       // every entry: the face size of the level the dispatch writes (or reads, for the SH projection)
		uint32_t SourceFaceSize = 0; // CSDownsampleCube, CSPrefilterSpecular: the face size of the source level
		uint32_t SourceWidth = 0;    // CSEquirectToCube: the equirectangular image's width (its height is half of it)
		uint32_t SampleCount = 0;    // CSPrefilterSpecular: importance samples per texel; 0 resamples the source level (mip 0)
		float Alpha = 0.0f;          // CSPrefilterSpecular: GGX alpha (perceptual roughness squared)
		// CSPrefilterSpecular: the solid angle of one texel of the skybox's level 0, 4 pi / (6 * size0²), for filtered
		// importance sampling (Krivanek and Colbert 2008).
		float SourceTexelSolidAngle = 0.0f;
		float SourceMaxLod = 0.0f; // CSPrefilterSpecular: the skybox's last mip level
		uint32_t GroupCount = 0;   // CSReduceIrradiance: the partial sums CSProjectIrradiance wrote, one per thread group
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(EnvironmentBakeConstants) == 32, "EnvironmentBakeConstants must match its 32-byte push-constant layout");
	static_assert(offsetof(EnvironmentBakeConstants, Alpha) == 16 && offsetof(EnvironmentBakeConstants, GroupCount) == 28,
		"EnvironmentBakeConstants members must sit where the push-constant layout puts them");

}
#endif
