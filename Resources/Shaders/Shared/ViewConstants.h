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
	// have an infinite far plane, orthographic ones the finite range d = (Far + zView) / (Far - Near). Clip-space +Y is up:
	// NVRHI's Vulkan backend gives every viewport a negative height, which is the Vulkan Y flip, so projections do not
	// flip Y. Linear depth is Near / d for perspective and Far - d * (Far - Near) for orthographic; the view ray is
	// normalize(ndc.xy * TanHalfFovY * (AspectRatio, 1), -1) for perspective, while orthographic views use the direction
	// (0, 0, -1) from the origin (ndc.xy * OrthoHalfExtents, 0).
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
		// M8 (Docs/Decisions/0013-m8-decisions.md decision 7: appended, the members above are frozen): the number of valid
		// entries of the Lights structured buffer (t0, Shared/ShaderLight.h), at most RenderQualitySettings::MaxShadedLights; 0 for passes that
		// bind no light list.
		uint32_t LightCount = 0;
		uint32_t Padding1 = 0;
		uint32_t Padding2 = 0;
		uint32_t Padding3 = 0;
		// M26 (C26.1, R-TAA-VEL, R-TAA; Docs/Decisions/0037-m26-contract.md decisions 10, 11, 14 and 17; stream X-C: appended,
		// the members above are frozen): the view's temporal inputs (RenderSnapshot.h's CameraData). With TAA (a Lit view
		// outside the wireframe and pixel-art mode whose PostProcessSettings::AntiAliasing is Taa) the scene passes' Projection
		// and ViewProjection, and their inverses, include Jitter: the clip-space translation J that moves every rasterized
		// point by Jitter scene pixels (+x right, +y down), ViewProjection = J * (Projection * View); otherwise Jitter is
		// (0, 0) and the matrices are the camera's. PreviousViewProjection: J times the previous rendered frame's unjittered
		// Projection * View (CameraData::PreviousViewProjection), or ViewProjection itself after a history reset (no camera
		// motion), so the prepass's motion vectors (SceneVelocity, Common/Motion.slang), the difference of the current and
		// previous clip positions both moved by J, are unjittered, and exactly zero for what did not move.
		Float4x4 PreviousViewProjection;
		Float2 Jitter;
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3; its ledger: ReflectionView in Padding4, ClipPlane and
		// CascadeDepthPlane appended, the members above frozen): the planar reflections' view (Renderer/PlanarReflections.h).
		// ReflectionView: 1 in a mirror view (the camera mirrored in a planar reflection's plane), 0 in every other view.
		// ClipPlane: the world plane (xyz the unit normal, w the distance, dot(ClipPlane, (p, 1)) >= 0 on the kept side) the
		// vertex shaders a mirror view runs write as SV_ClipDistance0 (Passes/Scene.slang, Terrain.slang and
		// Particles.slang), the mirror's plane moved PlanarReflectionClipBias towards its reflecting side; (0, 0, 0, 1),
		// a distance of 1 at every vertex, clips nothing, which every other view sets. The fog of a mirror view starts at
		// the view ray's crossing of it (Common/Fog.slang). CascadeDepthPlane: in a mirror view, the main view's depth
		// plane (dot(CascadeDepthPlane, (p, 1)) is the main camera's view depth of the world point p), by which a mirror view
		// chooses the main view's directional shadow cascade (Common/Shadows.slang); unread in every other view.
		uint32_t ReflectionView = 0;
		uint32_t Padding5 = 0;
		Float4 ClipPlane = Float4(0.0f, 0.0f, 0.0f, 1.0f);
		Float4 CascadeDepthPlane = Float4(0.0f, 0.0f, 0.0f, 0.0f);
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(ViewConstants) == 512, "ViewConstants must match its 512-byte constant-buffer layout");
	static_assert(offsetof(ViewConstants, CameraPosition) == 320 && offsetof(ViewConstants, OrthoHalfExtents) == 336
			&& offsetof(ViewConstants, ViewportSize) == 352 && offsetof(ViewConstants, TanHalfFovY) == 368
			&& offsetof(ViewConstants, LightCount) == 384 && offsetof(ViewConstants, PreviousViewProjection) == 400
			&& offsetof(ViewConstants, Jitter) == 464 && offsetof(ViewConstants, ReflectionView) == 472
			&& offsetof(ViewConstants, ClipPlane) == 480 && offsetof(ViewConstants, CascadeDepthPlane) == 496,
		"ViewConstants members must sit where the constant-buffer layout puts them");

}
#endif
