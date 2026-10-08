#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/DebugDrawList.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
// extracts. The M8 contract added the texts, the debug draw list and the debug view (Docs/Decisions/0013-m8-decisions.md
// decision 5); M9 adds the pick table and the view flags. Additions are frozen-header changes reviewed by their contract
// owner; no member here changes meaning. A plain value type: copyable, movable, thread-compatible.

namespace Engine {

	// How a camera projects (CameraComponent::Projection, without including Scene).
	enum class RenderProjection : uint8_t
	{
		Perspective,
		Orthographic
	};

	// The tonemapper of PostProcessComponent::Tonemap (§8.9): AgX (Base look, the default), ACES (Hill's RRT+ODT fit), Khronos
	// PBR Neutral and Linear (clamp; debugging). TonemapPass applies it (M8); the walking skeleton applied Linear whatever the
	// value.
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

	// What the forward passes output (§8.5 debug views; viewport.screenshot "debugView"). Lit is the image; the others show
	// one material or geometry quantity per pixel through a Vulkan specialization constant of the forward pipelines (the
	// enumerator's value is the constant's value). With a view other than Lit the post chain is fixed (SceneRenderer.h):
	// no skybox, bloom, FXAA or dither, exposure 1, the Linear tonemapper, and the sRGB OETF only for the colour views
	// (Albedo, Emissive), so a data view's value v is stored as round(255 v). M9 appends AO, ShadowCascades and Overdraw after
	// Emissive (Docs/Decisions/0013-m8-decisions.md decision 12).
	enum class RenderDebugView : uint8_t
	{
		Lit,       // the shaded image
		Albedo,    // base colour (factor times map, linear), alpha ignored
		Normals,   // the shading normal (normal-mapped, world space) as N * 0.5 + 0.5
		Roughness, // perceptual roughness (after the 0.045 clamp, §8.5) as grey
		Metallic,  // metallic as grey
		Emissive   // emitted radiance (Emissive times map times EmissiveStrength), clamped to [0, 1]
	};

	// Where a TextItem is laid out (TextComponent::Space, §5.3, §8.10).
	enum class RenderTextSpace : uint8_t
	{
		Screen, // laid out on the viewport from Anchor, Pivot and Offset
		World   // laid out in the entity's local XY plane at its rendered world matrix
	};

	// TextComponent::Alignment: how the lines of a multi-line text align within the text block.
	enum class RenderTextAlignment : uint8_t
	{
		Left,
		Center,
		Right
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
		// CameraComponent::ClearColor, linear. SceneColor is cleared to it; with ClearToSkybox and an environment map the
		// skybox pass draws over every pixel the scene left at depth 0 (M8).
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

	// One light (§8.2: directional, point, spot). The renderer culls them on the CPU and lights with at most
	// MaxVisibleLights of them (RenderPrepare.h, RENDER_LIGHT_LIMIT_EXCEEDED); the walking skeleton used the first
	// directional light only.
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

	// The scene's EnvironmentComponent (§5.3, §8.6), or its defaults when the scene has none. With an environment map the
	// forward pass lights with its SH9 irradiance and prefiltered specular cube and the skybox pass draws its skybox cube
	// (M8); without one, or when the map fails to load, FallbackColor * Intensity is a constant ambient and the camera's
	// ClearColor shows where the sky would be (Shared/EnvironmentConstants.h).
	struct RenderEnvironment
	{
		AssetHandle Environment{}; // null: no map, FallbackColor is the ambient
		float Intensity = 1.0f;
		float Rotation = 0.0f; // degrees about +Y
		bool ShowSkybox = true;
		float SkyboxBlur = 0.0f;
		glm::vec3 FallbackColor = glm::vec3(0.2f, 0.22f, 0.25f); // linear
	};

	// The scene's PostProcessComponent (§5.3), or its defaults when the scene has none. M8 applies the exposure, the
	// tonemapper (with the blue-noise dither), bloom and FXAA; the Ssao members take effect with GTAO (M9).
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

	// World text pixels per metre: a world TextItem of Size s has an em of s / WorldTextPixelsPerMetre metres before the
	// entity's scale (the default Size 32 is 0.32 m).
	inline constexpr float WorldTextPixelsPerMetre = 100.0f;

	// One TextComponent to draw (§5.3, §8.10), from render extraction (M8). TextRenderer lays it out with TextLayout.h and
	// draws it as signed-distance-field glyph quads (smoothstep over fwidth).
	struct TextItem
	{
		std::string Text{}; // UTF-8; a line feed starts a new line; glyphs the font lacks are skipped
		AssetHandle Font{}; // null: the Default font (BuiltinAssetHandles::DefaultFont); a missing font draws with its placeholder
		// Pixels per em at the 1080p reference height. Screen text is scaled by viewport height / 1080; world text has an em of
		// Size / WorldTextPixelsPerMetre metres in the entity's local space (so the entity's scale applies).
		float Size = 32.0f;
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB and straight alpha
		RenderTextSpace Space = RenderTextSpace::Screen;
		// Screen: the viewport point the text block is placed at, normalized ((0, 0) top left, (1, 1) bottom right).
		glm::vec2 Anchor = glm::vec2(0.5f);
		// The point of the text block placed at the anchor (Screen) or at the entity's origin (World), normalized to the block
		// ((0, 0) its top left, (1, 1) its bottom right).
		glm::vec2 Pivot = glm::vec2(0.5f);
		// Screen: pixels at the 1080p reference added to the anchor, +x right and +y down (scaled like Size).
		glm::vec2 Offset = glm::vec2(0.0f);
		RenderTextAlignment Alignment = RenderTextAlignment::Center;
		// World: the block faces the camera (its plane parallel to the view plane, the camera's up as its up) instead of the
		// entity's local +Z, keeping the entity's position and scale.
		bool Billboard = false;
		glm::mat4 World = glm::mat4(1.0f); // World: the entity's rendered world matrix (§5.2); unused for Screen
		UUID Entity{};
	};

	// One view's render snapshot (§8.2). Vectors are in canonical entity order (§5.1), so a snapshot of a deterministic scene
	// is deterministic.
	struct RenderSnapshot
	{
		// False when the view has no camera (a game view of a scene without a primary camera): the renderer then clears to
		// the default ClearColor and draws nothing but screen texts (M8, TextRenderInputs::HasCamera).
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
		// M8: every effectively enabled TextComponent with non-empty Text, in canonical entity order (screen and world texts
		// together; the text pass draws the world ones, depth-tested, then the screen ones, §8.3 pass 14).
		std::vector<TextItem> Texts{};
		// M8: the debug primitives of this view (DebugDrawList.h). Extraction leaves it empty; its producers append to it (M11's
		// collider visualization, M13's scripts through the session's persistent list, the editor's gizmos).
		DebugDrawList DebugDraw{};
		// M8: what the forward passes output; Lit for every view except a viewport.screenshot with "debugView".
		RenderDebugView DebugView = RenderDebugView::Lit;
	};

	// The name of `view` as viewport.screenshot spells it ("Lit", "Albedo", "Normals", "Roughness", "Metallic", "Emissive").
	[[nodiscard]] std::string_view RenderDebugViewToString(RenderDebugView view);

	// The debug view named `name`, ignoring ASCII case; nullopt for any other name (M9's AO, ShadowCascades and Overdraw
	// included).
	[[nodiscard]] std::optional<RenderDebugView> ParseRenderDebugView(std::string_view name);

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
