#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <cstdint>
#include <string_view>

namespace Engine {

	class AssetManager;
	class PhysicsSystem;
	class Scene;

	// Asset-dependent view overlays (§8.13), after ExtractRenderSnapshot/PlaySession::ExtractView. Main thread.
	// Uses snapshot.Flags, Annotations, SelectedEntities and Alpha; does not alter scene, editor selection or options.
	// Appends label text (name plus first six UUID hex digits), transformed mesh AABBs and unit RGB world axes to
	// DebugDraw, plus procedural icon records. Colliders use BuildColliderDebugDraw with options.Alpha = snapshot.Alpha,
	// physics for play (null in edit), then AppendColliderDebugDraw. No camera -> no world overlays.
	// Canonical entity order; labels skip disabled/pending-destruction entities and positions behind the camera.
	// Selected uses SelectedEntities; Explicit uses LabelEntities; unknown UUIDs are ignored. All labels eligible entities.
	// Label mode does not filter bounds/axes/colliders; those flags independently apply to eligible entities.
	// InvalidArgument: invalid snapshot alpha/annotation values. Build first, then append, so failure is atomic.
	// Repeated calls intentionally append again: the host calls once for each freshly extracted snapshot.
	[[nodiscard]] Status AppendRenderAnnotations(const Scene& scene, AssetManager& assets, const PhysicsLayerTable& layers,
		const PhysicsSystem* physics, RenderSnapshot& snapshot);

	inline constexpr std::string_view RenderNoLightingCode = "RENDER_NO_LIGHTING";

	struct RenderSceneValidation
	{
		bool NoLighting = false;
		uint32_t ShadowedSpotLights = 0; // global scene count, not view-dependent atlas allocation
	};

	// Main-thread read-only scene/CPU asset scan. Uses the existing synchronous CPU AssetManager lookup, which may
	// load, cook or wait for CPU assets; never waits for GPU or steps physics. NoLighting only when a visible enabled
	// non-null mesh has at least one material without potential emission and there is neither an enabled light with positive finite radiance
	// (local lights also positive range) nor ambient environment illumination. Effective environment uses the first
	// enabled Environment or default RenderEnvironment: positive intensity and either a loaded map with a positive SH
	// DC term or, when no usable map exists, the effective positive fallback color. Missing maps use the renderer fallback.
	// Empty/text-only scenes and scenes whose every visible submesh has potential emission do not warn. Potential
	// emission means max(Emissive * EmissiveStrength)>0; conservatively do not inspect texture texels. Emission never
	// illuminates another nonemissive material. All current materials are PBR; no new Unlit field is assumed. Resolve missing
	// meshes/materials via the renderer's CPU placeholders. ShadowedSpotLights counts enabled finite contributing spots
	// with CastShadows, regardless of camera visibility. Parent ProjectValidator maps NoLighting and count>8 to
	// RENDER_NO_LIGHTING/RENDER_SPOT_SHADOW_BUDGET: Warning, AutoFixable=false, one stable scene-file diagnostic each.
	// Run identically for the open scene and scratch-loaded project scenes. Separate asset errors remain unchanged.
	// InvalidArgument for nonfinite material/environment values that prevent illumination classification; malformed
	// light values do not contribute, matching CullLights. Missing assets use placeholders, not a helper failure.
	[[nodiscard]] Result<RenderSceneValidation> EvaluateRenderSceneValidation(const Scene& scene, AssetManager& assets);

}
