#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
namespace Engine {

#endif

	struct SelectionMaskConstants
	{
		Float4x4 World;
		Float4x4 ViewProjection;
	};

	struct SelectionFilterConstants
	{
		Float4 Color;
		uint32_t Radius = 2;
		uint32_t Vertical = 0;
		UInt2 Padding;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(SelectionMaskConstants) == 128 && offsetof(SelectionMaskConstants, ViewProjection) == 64);
	static_assert(sizeof(SelectionFilterConstants) == 32 && offsetof(SelectionFilterConstants, Radius) == 16);

}
#endif
