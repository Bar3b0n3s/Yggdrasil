#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
namespace Engine {

#endif

	// The SelectionMask draw's push constants: the overlay camera's view projection, and the index of the draw's first
	// instance record in the view's Instances buffer (M23, R-INST: the world matrix is the record's, Shared/InstanceRecord.h).
	struct SelectionMaskConstants
	{
		Float4x4 ViewProjection;
		uint32_t FirstInstance = 0;
		uint32_t Padding0 = 0;
		UInt2 Padding1;
	};

	struct SelectionFilterConstants
	{
		Float4 Color;
		uint32_t Radius = 2;
		uint32_t Vertical = 0;
		UInt2 Padding;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(SelectionMaskConstants) == 80 && offsetof(SelectionMaskConstants, FirstInstance) == 64
		&& offsetof(SelectionMaskConstants, Padding1) == 72);
	static_assert(sizeof(SelectionFilterConstants) == 32 && offsetof(SelectionFilterConstants, Radius) == 16);

}
#endif
