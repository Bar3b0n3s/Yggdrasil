#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// The render snapshot (Architecture §8.2): everything the renderer needs to draw one view, as plain data. Scene's render
// extraction (Scene/RenderExtraction.h) builds it from the ECS, so the renderer never sees the scene and is tested with
// synthetic snapshots. This header is one of the two Renderer headers Scene may include (§3 rule 3): it includes nothing
// beyond Core, Asset, glm and the standard library, and in particular no NVRHI (the SnapshotHeaders rule of
// Scripts/ModuleRules.json, checked by Lint.py).
//
// Conventions (§5.2, §8.3): right-handed, +Y up, metres; cameras and lights look down their local -Z; colours are linear;
// angles are degrees; depth is reverse-Z (perspective projections have an infinite far plane, orthographic ones the finite
// range of §8.3) and clip-space +Y is up (NVRHI's viewport performs the Vulkan Y flip, ADR 0009 decision 23).
//
// Frozen by the M7 contract (Docs/Decisions/0012-m7-decisions.md decision 6) with the members the walking skeleton
// extracts. Later milestones add theirs (texts and the debug draw list in M8, the pick table and the view flags in M9) as
// frozen-header additions reviewed by their contract owner; no member here changes meaning. A plain value type: copyable,
// movable, thread-compatible.

namespace Engine {

	// How a camera projects (CameraComponent::Projection, without including Scene).
	enum class RenderProjection : uint8_t
	{
		Perspective,
		Orthographic
	};

	// The tonemapper of PostProcessComponent::Tonemap (§8.9). The M7 renderer applies Linear whatever the value (Roadmap M7:
	// "Linear tonemap"); the others arrive with M8.
	enum class RenderTonemapper : uint8_t
	{
		AgX,
		Aces,
		PbrNeutral,
		Linear
	};

	// PostProcessComponent::SsaoQuality (§8.8; used from M9).
	enum class RenderSsaoQuality : uint8_t
	{
		Low,
		Medium,
		High
	};

	enum class RenderLightType : uint8_t
	{
		Directional,
		Point,
		Spot
	};

	// The view's camera (§8.2 "view, projection (reverse-Z), position, viewport, exposure").
	struct CameraData
	{
		glm::mat4 View = glm::mat4(1.0f);       // world -> view: the inverse of the camera's rendered world matrix, scale removed
		glm::mat4 Projection = glm::mat4(1.0f); // view -> clip, reverse-Z (§8.3); ComputeReverseZProjection
		glm::vec3 Position = glm::vec3(0.0f);   // world space
		RenderProjection ProjectionKind = RenderProjection::Perspective;
		float VerticalFov = 60.0f;      // degrees, perspective
		float OrthographicSize = 10.0f; // half the view height in metres, orthographic
		float NearClip = 0.1f;
		float FarClip = 1000.0f; // the orthographic depth range and the culling distance of both projections
		uint32_t ViewportWidth = 1;
		uint32_t ViewportHeight = 1;
		// CameraComponent::ClearColor, linear. The M7 renderer clears to it for both clear modes; the Skybox mode draws the
		// environment's skybox over it from M8.
		glm::vec3 ClearColor = glm::vec3(0.05f, 0.05f, 0.06f);
		bool ClearToSkybox = true; // CameraComponent::Clear == ClearMode::Skybox
		UUID Entity{};             // the camera entity; the invalid UUID for an explicit camera (the editor's scene view)
	};

	// One MeshRenderer to draw (§8.2 "world matrix, mesh handle+version, material per submesh, flags"). The renderer resolves
	// the asset versions, the mesh's default materials and placeholders through the AssetManager and its GpuResourceCache,
	// so extraction never touches assets.
	struct MeshDrawItem
	{
		glm::mat4 World = glm::mat4(1.0f); // the rendered (interpolated, §5.2) world matrix
		AssetHandle Mesh{};                // a null handle draws nothing
		// MeshRendererComponent::Materials: one slot per submesh; a missing or null slot uses the mesh's default material.
		std::vector<AssetHandle> Materials{};
		bool CastShadows = true;
		bool ReceiveShadows = true;
		UUID Entity{};
	};

	// One light (§8.2: directional, point, spot). The M7 renderer lights with the first directional light in snapshot
	// order (canonical entity order, §5.1) and ignores the others; M8 culls and packs them all (at most 256 visible).
	struct LightData
	{
		RenderLightType Type = RenderLightType::Directional;
		glm::vec3 Color = glm::vec3(1.0f); // linear; radiance scale = Color * Intensity (§8.5 artist units)
		float Intensity = 1.0f;
		glm::vec3 Position = glm::vec3(0.0f);               // world space (point, spot)
		glm::vec3 Direction = glm::vec3(0.0f, 0.0f, -1.0f); // world space, unit: where the light shines (its local -Z)
		float Range = 10.0f;                                // metres (point, spot)
		float InnerConeAngle = 20.0f;                       // degrees (spot)
		float OuterConeAngle = 30.0f;                       // degrees (spot)
		float SourceRadius = 0.05f;                         // metres (point, spot)
		float LightAngle = 1.0f;                            // degrees (directional; PCSS from M9)
		bool CastShadows = false;
		UUID Entity{};
	};

	// The scene's EnvironmentComponent (§5.3, §8.6), or its defaults when the scene has none. The M7 renderer lights with
	// the constant ambient FallbackColor * Intensity; the IBL maps and the skybox arrive with M8.
	struct RenderEnvironment
	{
		AssetHandle Environment{}; // null: no map, FallbackColor is the ambient
		float Intensity = 1.0f;
		float Rotation = 0.0f; // degrees about +Y
		bool ShowSkybox = true;
		float SkyboxBlur = 0.0f;
		glm::vec3 FallbackColor = glm::vec3(0.2f, 0.22f, 0.25f); // linear
	};

	// The scene's PostProcessComponent (§5.3), or its defaults when the scene has none. M7 uses ExposureEV and renders
	// RenderTonemapper::Linear; the rest takes effect with M8 (bloom, FXAA, the tonemappers) and M9 (GTAO).
	struct PostProcessSettings
	{
		float ExposureEV = 0.0f; // multiplier 2^ExposureEV
		RenderTonemapper Tonemap = RenderTonemapper::AgX;
		bool SsaoEnabled = true;
		float SsaoRadius = 0.5f;
		float SsaoIntensity = 1.0f;
		RenderSsaoQuality SsaoQuality = RenderSsaoQuality::Medium;
		bool BloomEnabled = true;
		float BloomIntensity = 0.04f;
		bool FxaaEnabled = true;
	};

	// One view's render snapshot (§8.2). Vectors are in canonical entity order (§5.1), so a snapshot of a deterministic scene
	// is deterministic.
	struct RenderSnapshot
	{
		// False when the view has no camera (a game view of a scene without a primary camera): the renderer then clears to
		// the default ClearColor and draws nothing.
		bool HasCamera = false;
		CameraData Camera{};
		std::vector<MeshDrawItem> Meshes{};
		std::vector<LightData> Lights{};
		RenderEnvironment Environment{};
		PostProcessSettings Post{};
		// The interpolation alpha the poses were extracted at (§5.2): the frame's Alpha for the game view (1 for ManualClock
		// frames and lockstep ticks); PlaySession::GetViewAlpha for a view of the play scene between ticks (screenshots: 1
		// while paused, in lockstep or after a ManualClock frame, otherwise the last frame's Alpha); 1 for an edit scene. Read
		// by the FeatureTest Timing suite (Test.GetLastExtraction, M13).
		float Alpha = 1.0f;
	};

	// The reverse-Z projection of §8.3 for a `width` x `height` viewport (both > 0, asserted by callers):
	//   perspective:  m[0][0] = f / aspect, m[1][1] = f, m[2][3] = -1, m[3][2] = NearClip, every other entry 0, with
	//                 f = 1 / tan(VerticalFov / 2) and an infinite far plane;
	//   orthographic: x and y scaled by 1 / (OrthographicSize * aspect) and 1 / OrthographicSize, and
	//                 d = (FarClip + zView) / (FarClip - NearClip), so d = 1 at the near plane and 0 at the far plane.
	// Clip-space +Y is up (no Y flip). Implemented in Renderer/RenderSnapshot.cpp with Core/DetMath (Tan), not <cmath>, so
	// Scene's render extraction, which is on the simulation path's lint scope (§4.12), may call it and the result is
	// identical in every configuration.
	[[nodiscard]] glm::mat4 ComputeReverseZProjection(RenderProjection projection, float verticalFovDegrees, float orthographicSize,
		float nearClip, float farClip, uint32_t width, uint32_t height);

}
