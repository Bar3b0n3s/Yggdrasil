#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>

namespace Engine {

#endif

	// The lighting of the walking skeleton's forward pass (Roadmap M7; Docs/Decisions/0012-m7-decisions.md decision 7): b2
	// of descriptor set 0, the register §8.4 gives the environment constants. One directional light (the snapshot's first)
	// plus a constant ambient term; the PBR renderer (M8) replaces it with the environment constants and the light list.
	// Radiances are linear (Color * Intensity, §8.5 artist units); a zero LightRadiance means no directional light.
	struct LightingConstants
	{
		Float3 LightDirection; // world space, unit: where the light shines (its -Z axis)
		float Padding0 = 0.0f;
		Float3 LightRadiance;
		float Padding1 = 0.0f;
		Float3 AmbientRadiance; // RenderEnvironment::FallbackColor * Intensity
		float Padding2 = 0.0f;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(LightingConstants) == 48, "LightingConstants must match its 48-byte constant-buffer layout");
	static_assert(offsetof(LightingConstants, LightRadiance) == 16 && offsetof(LightingConstants, AmbientRadiance) == 32,
		"LightingConstants members must sit where the constant-buffer layout puts them");

}
#endif
