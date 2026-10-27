#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
namespace Engine {

#endif

	// The push constants of the Upscale program (Renderer/UpscalePass.h, Passes/Upscale.slang; M18, C18.1,
	// Docs/Decisions/0029-m18-contract.md decision 6): the output mapping's offset and scale. View pixel (x, y) of the scaled
	// scene rectangle shows scene texel ((x - OffsetX) / Scale, (y - OffsetY) / Scale), integer division of non-negative
	// values.
	struct UpscaleConstants
	{
		// x, y: RenderOutputMapping's OffsetX and OffsetY as the bits of 32-bit signed integers (the shader reads them with
		// asint: a cropped scene's offsets are negative); z: its Scale (>= 1); w: zero.
		UInt4 Mapping;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(UpscaleConstants) == 16, "UpscaleConstants must match its 16-byte push-constant layout");

}
#endif
