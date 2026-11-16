#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// ShaderLight::Type values (RenderLightType's enumerator values).
#if defined(ENGINE_SHADER)
	static const uint ShaderLightTypeDirectional = 0;
	static const uint ShaderLightTypePoint = 1;
	static const uint ShaderLightTypeSpot = 2;
#else
inline constexpr uint32_t ShaderLightTypeDirectional = 0;
inline constexpr uint32_t ShaderLightTypePoint = 1;
inline constexpr uint32_t ShaderLightTypeSpot = 2;
#endif

	// ShaderLight::ShadowSlot of a light without a spot or point shadow (M29, Docs/Decisions/0044-m29-light-limits.md
	// decision 2).
#if defined(ENGINE_SHADER)
	static const uint ShaderLightNoShadowSlot = 0xFFFFFFFF;
#else
inline constexpr uint32_t ShaderLightNoShadowSlot = 0xFFFFFFFF;
#endif

	// One light of the view's light list: t0 of descriptor set 0 (§8.4 "t0 Lights (StructuredBuffer)"), at most
	// RenderQualitySettings::MaxShadedLights entries in CullLights' order (Renderer/RenderPrepare.h; directional lights first), of which
	// ViewConstants::LightCount are valid. Written once per view by the scene renderer from the snapshot's LightData (§8.5
	// artist units):
	//   - Position (world space; point and spot) and Direction (world space, unit: where the light shines, its local -Z);
	//   - Radiance = Color * Intensity;
	//   - InverseRangeSquared = 1 / Range² of the windowed inverse-square falloff (point and spot; 0 for directional lights);
	//   - SourceRadius widens the specular lobe (representative point, point and spot);
	//   - SpotCosOuter and SpotCosInner: the cosines of the outer and inner cone half-angles of the smoothstep cone
	//     attenuation (spot; -1 and 1 otherwise, which never attenuates);
	//   - ShadowSlot (M29, decision 2): a shadowed spot light's index in ShadowConstants::Spots, a shadowed point light's in
	//     ShadowConstants::Points, ShaderLightNoShadowSlot otherwise (directional lights keep ShadowConstants::Counts.z), so the
	//     shadow lookup reads its entry without a search.
	// The members are ordered so that every Float3 starts a 16-byte row, which makes the C++ layout equal both the std430
	// layout of a Vulkan structured buffer and the constant-buffer layout.
	struct ShaderLight
	{
		Float3 Position;
		uint32_t Type = ShaderLightTypeDirectional;
		Float3 Direction;
		float InverseRangeSquared = 0.0f;
		Float3 Radiance;
		float SourceRadius = 0.0f;
		float SpotCosOuter = -1.0f;
		float SpotCosInner = 1.0f;
		uint32_t ShadowSlot = ShaderLightNoShadowSlot;
		float Padding1 = 0.0f;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(ShaderLight) == 64, "ShaderLight must match its 64-byte structured-buffer layout");
	static_assert(offsetof(ShaderLight, Direction) == 16 && offsetof(ShaderLight, Radiance) == 32 && offsetof(ShaderLight, SpotCosOuter) == 48
			&& offsetof(ShaderLight, ShadowSlot) == 56,
		"ShaderLight members must sit where the structured-buffer layout puts them");

}
#endif
