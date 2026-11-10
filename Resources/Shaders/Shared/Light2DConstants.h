#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// Light2DConstants::Type: the light's type (RenderLight2DType's values; Lights2DPass.cpp asserts the correspondence).
#if defined(ENGINE_SHADER)
	static const uint Light2DTypePoint = 0;
	static const uint Light2DTypeGlobal = 1;
#else
inline constexpr uint32_t Light2DTypePoint = 0;
inline constexpr uint32_t Light2DTypeGlobal = 1;
#endif

	// The push constants of the Lights2D program (M21, C21.2, R-2D-LIGHT; Renderer/Lights2DPass.h, Passes/Lights2D.slang;
	// Docs/Decisions/0032-m21-contract.md decision 22): one light's draw, declared in Slang as
	// [[vk::push_constant]] ConstantBuffer<Light2DConstants> Light. A Point light draws its square
	// [x - Radius, x + Radius] x [y - Radius, y + Radius] in its plane z, projected by ViewProjection with its depth held
	// inside the clip range; a Global light draws one triangle over the target and reads only Radiance.
	struct Light2DConstants
	{
		Float4x4 ViewProjection; // the view's projection times its view (reverse-Z clip space, §8.3)
		Float4 PositionRadius;   // Point: the light's world position (xyz) and its Radius (w)
		Float4 RadianceFalloff;  // Color x Intensity (rgb, linear) and, Point, the Falloff (w)
		float Height = 1.0f;     // Point: the light's height above its plane, for normal-mapped pixels
		uint32_t Type = 0;       // Light2DTypePoint or Light2DTypeGlobal
		uint32_t Padding0 = 0;
		uint32_t Padding1 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(Light2DConstants) == 112, "Light2DConstants must match its 112-byte push-constant layout");
	static_assert(offsetof(Light2DConstants, PositionRadius) == 64 && offsetof(Light2DConstants, RadianceFalloff) == 80
			&& offsetof(Light2DConstants, Height) == 96 && offsetof(Light2DConstants, Type) == 100,
		"Light2DConstants members must sit where the push-constant layout puts them");

}
#endif
