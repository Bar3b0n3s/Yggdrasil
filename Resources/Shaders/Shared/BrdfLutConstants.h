#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstdint>

namespace Engine {

#endif

	// The DFG LUT's size and sample count (Renderer/BrdfLut.h, which asserts that its Size and LutSampleCount equal these):
	// the generator (Passes/BrdfLut.slang) integrates BrdfLutSampleCount GGX importance samples per texel of the
	// BrdfLutSize x BrdfLutSize RG16_FLOAT texture.
#if defined(ENGINE_SHADER)
	static const uint BrdfLutSize = 128;
	static const uint BrdfLutSampleCount = 1024;
#else
inline constexpr uint32_t BrdfLutSize = 128;
inline constexpr uint32_t BrdfLutSampleCount = 1024;
}
#endif
