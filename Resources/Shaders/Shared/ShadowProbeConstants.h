#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
namespace Engine {

#endif

	// GPU test oracle controls: mode/layer/output width/output height; distance step/offset/receiver distance/padding.
	struct ShadowProbeConstants
	{
		UInt4 Options;
		Float4 Parameters;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(ShadowProbeConstants) == 32);

}
#endif
