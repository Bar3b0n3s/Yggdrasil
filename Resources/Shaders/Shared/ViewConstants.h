#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// ViewConstants::ProjectionKind values (§8.3): every pass that reconstructs view-space data reads the projection kind
	// from here and never assumes perspective.
#if defined(ENGINE_SHADER)
	static const uint ProjectionKindPerspective = 0;
	static const uint ProjectionKindOrthographic = 1;
#else
inline constexpr uint32_t ProjectionKindPerspective = 0;
inline constexpr uint32_t ProjectionKindOrthographic = 1;
#endif

	// Per-view constants: b0 of descriptor set 0 (§8.4). Depth is reverse-Z everywhere (§8.3): perspective projections
	// have an infinite far plane, orthographic ones the finite range d = (Far + zView) / (Far - Near). Projection flips Y
	// for Vulkan clip space. Linear depth is Near / d for perspective and Far - d * (Far - Near) for orthographic; the view
	// ray is normalize(ndc.xy * TanHalfFovY * (AspectRatio, 1), -1) for perspective, while orthographic views use the
	// direction (0, 0, -1) from the origin (ndc.xy * OrthoHalfExtents, 0).
	struct ViewConstants
	{
		Float4x4 View;
		Float4x4 Projection;
		Float4x4 ViewProjection;
		Float4x4 InverseProjection;
		Float4x4 InverseViewProjection;
		Float3 CameraPosition;                               // world space
		uint32_t ProjectionKind = ProjectionKindPerspective; // or ProjectionKindOrthographic
		Float2 OrthoHalfExtents;                             // orthographic: half the view volume's width and height in view units; zero otherwise
		float Near = 0.1f;
		float Far = 0.0f;           // orthographic only; perspective projections have no far plane
		Float2 ViewportSize;        // pixels
		Float2 InverseViewportSize; // 1 / ViewportSize
		float TanHalfFovY = 0.0f;   // perspective only
		float AspectRatio = 1.0f;   // ViewportSize.x / ViewportSize.y
		float Exposure = 1.0f;      // 2^ExposureEV (§8.9); 1 when unused
		uint32_t Padding0 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(ViewConstants) == 384, "ViewConstants must match its 384-byte constant-buffer layout");
	static_assert(offsetof(ViewConstants, CameraPosition) == 320 && offsetof(ViewConstants, OrthoHalfExtents) == 336
			&& offsetof(ViewConstants, ViewportSize) == 352 && offsetof(ViewConstants, TanHalfFovY) == 368,
		"ViewConstants members must sit where the constant-buffer layout puts them");

}
#endif
