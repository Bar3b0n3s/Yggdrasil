#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>
namespace Engine {

#endif

	struct GtaoConstants
	{
		UInt2 FullSize;
		UInt2 OutputSize;
		Float2 PositionScale; // inverse projection diagonal (perspective), ortho half extents otherwise
		float Radius = 0.5f;
		float Intensity = 1.0f;
		uint32_t ProjectionKind = 0;
		uint32_t SliceCount = 2;
		uint32_t MipCount = 1;
		uint32_t Axis = 0;
		float RadiusScale = 1.0f; // full-resolution pixel radius per world unit, before perspective depth division
		uint32_t Padding0 = 0;
		uint32_t Padding1 = 0;
		uint32_t Padding2 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(GtaoConstants) == 64);
	static_assert(offsetof(GtaoConstants, FullSize) == 0 && offsetof(GtaoConstants, OutputSize) == 8
		&& offsetof(GtaoConstants, PositionScale) == 16 && offsetof(GtaoConstants, Radius) == 24
		&& offsetof(GtaoConstants, Intensity) == 28 && offsetof(GtaoConstants, ProjectionKind) == 32
		&& offsetof(GtaoConstants, SliceCount) == 36 && offsetof(GtaoConstants, MipCount) == 40
		&& offsetof(GtaoConstants, Axis) == 44 && offsetof(GtaoConstants, RadiusScale) == 48
		&& offsetof(GtaoConstants, Padding0) == 52 && offsetof(GtaoConstants, Padding1) == 56
		&& offsetof(GtaoConstants, Padding2) == 60);

}
#endif
