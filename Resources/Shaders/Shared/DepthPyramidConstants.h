#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>
namespace Engine {

#endif

	struct DepthPyramidConstants
	{
		UInt2 SourceSize;
		UInt2 DestinationSize;
		float Near = 0.1f;
		float Far = 1000.0f;
		uint32_t ProjectionKind = 0;
		uint32_t Padding0 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(DepthPyramidConstants) == 32);
	static_assert(offsetof(DepthPyramidConstants, SourceSize) == 0 && offsetof(DepthPyramidConstants, DestinationSize) == 8
		&& offsetof(DepthPyramidConstants, Near) == 16 && offsetof(DepthPyramidConstants, Far) == 20
		&& offsetof(DepthPyramidConstants, ProjectionKind) == 24 && offsetof(DepthPyramidConstants, Padding0) == 28);

}
#endif
