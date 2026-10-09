#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>

namespace Engine {

#endif

	// Shadow caster set 0 b0. Bias.x is the slope factor; Bias.y converts a world texel to reverse depth.
	// Bias.z enables near-plane pancaking for directional maps; Bias.w is padding. Receiver normal bias is separate.
	struct ShadowCasterConstants
	{
		Float4x4 ViewProjection;
		Float4 Bias;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(ShadowCasterConstants) == 80);
	static_assert(offsetof(ShadowCasterConstants, Bias) == 64);

}
#endif
