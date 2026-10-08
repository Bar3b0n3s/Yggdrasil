#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The push constants of the Bloom program (Passes/Bloom.slang, §8.3 pass 10): one dispatch's source and destination, set
	// by BloomPass::Record for each downsample and upsample step.
	struct BloomConstants
	{
		Float2 SourceTexelSize;      // 1 / the size of the level read (SceneColor, or a mip of the chain)
		Float2 DestinationTexelSize; // 1 / the size of the mip written
		UInt2 DestinationSize;       // the size of the mip written, in texels
		float Scale = 1.0f;          // CSUpsample: the factor of the sum (1 / the chain's mip count for the step into mip 0, else 1)
		uint32_t Padding0 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(BloomConstants) == 32, "BloomConstants must match its 32-byte push-constant layout");
	static_assert(offsetof(BloomConstants, DestinationTexelSize) == 8 && offsetof(BloomConstants, DestinationSize) == 16
			&& offsetof(BloomConstants, Scale) == 24,
		"BloomConstants members must sit where the push-constant layout puts them");

}
#endif
