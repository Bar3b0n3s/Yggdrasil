#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Renderer/UiDrawList.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The render snapshot (Architecture §8.2): everything the renderer needs to draw one view, as plain data. Scene's render
// extraction (Scene/RenderExtraction.h) builds it from the ECS, so the renderer never sees the scene and is tested with
// synthetic snapshots. This header is one of the Renderer headers Scene may include (§3 rule 3, with DebugDrawList.h and
// M22's UiDrawList.h, both of which it includes): it includes nothing beyond Core, Asset, glm and the standard library, and
// in particular no NVRHI (the SnapshotHeaders rule of Scripts/ModuleRules.json, checked by Lint.py).
//
// Conventions (§5.2, §8.3): right-handed, +Y up, metres; cameras and lights look down their local -Z; colours are linear;
// angles are degrees; depth is reverse-Z (perspective projections have an infinite far plane, orthographic ones the finite
// range of §8.3) and clip-space +Y is up (NVRHI's viewport performs the Vulkan Y flip, ADR 0009 decision 23).
//
// Frozen by the M7 contract (Docs/Decisions/0012-m7-decisions.md decision 6) with the members the walking skeleton
// extracts. The M8 contract added the texts, the debug draw list and the debug view (Docs/Decisions/0013-m8-decisions.md
// decision 5); M9 added the pick table, the view flags, the selected entities, the annotations and their draw list, the
// quality settings, the icons and the frame and scene revision stamps (ADR 0017). The M18 contract (C18.1,
// Docs/Decisions/0029-m18-contract.md decisions 5 to 8) added the output stage (RenderOutputSettings, the view-to-scene
// mapping) and the memory resource of the containers (decision 9). The M23 contract (C23.1,
// Docs/Decisions/0034-m23-contract.md decisions 2 to 5) changed existing members only, as phase 2's ledger lets it: the mesh
// draw item's PreviousWorld for its instance record, the point lights' shadows and the quality settings' two reference
// paths; its C23.3 (decision 27) the mesh draw item's deformation (JointPalette, MorphWeights). The M21 contract (C21.1,
// Docs/Decisions/0032-m21-contract.md decision 4) added the sprites (SpriteItem, Sprites) after Output, as ADR 0028's
// phase-2 ledger places phase 2's members, and C21.2 (decisions 21, 22 and 24) the tilemaps and the 2D lights after them
// (TilemapItem, Tilemaps, Light2DItem, Lights2D) and a sprite's normal map. The M22 contract (C22.1,
// Docs/Decisions/0033-m22-contract.md decision 7) added the UI draw list (Ui, Renderer/UiDrawList.h) after M21's members, in
// landing order. The M26 contract (C26.1, Docs/Decisions/0037-m26-contract.md decisions 7, 10, 11, 12, 16 and 22) added,
// after M22's member and in ADR 0028's phase-3 ledger's order, the particles (ParticleEmitterItem, ParticleItem), the probes
// (IrradianceVolumeItem, ReflectionProbeItem) and the terrains (TerrainItem); the camera's temporal inputs (the previous
// view-projection, the jitter and the frame's delta time, the temporal-state section, decision 10) and the mesh item's
// PreviousWorld filled from the view's history (decision 11); and PostProcess version 2's settings, where the AntiAliasing
// enum replaces FxaaEnabled (decision 12). M32 (Docs/Decisions/0047-m32-fog-ssr.md decisions 4 and 12; ADR 0042's phase-5
// ledger) added the fog (RenderFog, Fog) after the terrains and the half-resolution SSR trace to the quality settings after
// SsaoHalfResolution. M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3; its ledger) added the planar
// reflections (PlanarReflectionItem, PlanarReflections) after the fog. Additions are frozen-header changes reviewed by their
// contract owner; no member here changes meaning.
// A plain value type: copyable, movable, thread-compatible.
//
// Containers (M18, R-ARENA): the snapshot's own containers that grow with the scene (Meshes, Lights, Texts, PickTable,
// Icons, M21's Sprites, Tilemaps and Lights2D, the commands of DebugDraw and AnnotationDraw, and M22's Ui items and
// annotations) are std::pmr containers. A snapshot built with RenderSnapshot(resource) allocates them from that memory
// resource, which must outlive the snapshot; a default-constructed one, and every copy (std::pmr's copy construction),
// allocates from std::pmr::get_default_resource(), the global heap unless a host changed it. Assignment keeps the target's
// resource. SelectedEntities (a handful of UUIDs, copied from the host's selection) and the nested containers (a mesh
// item's Materials, JointPalette and MorphWeights, a text's Text, the annotations' label list, a terrain item's Layers)
// keep the default allocator; M26's ParticleEmitters, Particles, IrradianceVolumes, ReflectionProbes and Terrains and M42's
// PlanarReflections are the snapshot's own containers and follow the rule.
//
// The output stage (M18, decisions 5 to 8): a view renders its scene at the scene resolution of RenderOutputSettings (the
// view's size by default, which renders exactly as before the output stage), then the output stage upscales the scene image
// by the largest integer factor that fits the view, with point sampling, centred in a letterbox. Picking, CPU rays,
// captures and UI hit testing map view pixels to scene pixels with RenderOutputMapping (ComputeRenderOutputMapping and the
// Map* functions below). UI is laid out and hit-tested in scene pixels (M22: the Ui member, drawn at the scene resolution
// before the upscale).

namespace Engine {

	// Per-view switches. None preserves a clean game/capture view. EditorOverlays gates Grid, Icons and Selection;
	// Colliders, Picking and Wireframe are independent. Flags affect this snapshot only, never the scene or UI state.
	// Wireframe replaces Lit mesh color with portable one-pixel triangle edges over camera ClearColor; normal solid
	// depth/EntityId prepass still runs, so picking/selection remain geometry-based. No fillModeNonSolid requirement.
	// DebugView != Lit takes precedence over Wireframe. Full wireframe/color/count rules are frozen in ADR0017.
	// M30 (Docs/Decisions/0045-m30-navigation.md decision 12): Navigation draws the scene's baked navigation mesh as polygon
	// edges in AnnotationDraw (Scene/NavigationDebugDraw.h), independent like Colliders.
	enum class RenderViewFlags : uint32_t
	{
		None = 0,
		EditorOverlays = 1U << 0,
		Grid = 1U << 1,
		Selection = 1U << 2,
		Colliders = 1U << 3,
		Icons = 1U << 4,
		Picking = 1U << 5,
		Wireframe = 1U << 6,
		Navigation = 1U << 7
	};

	template<>
	inline constexpr bool EnableFlagOperators<RenderViewFlags> = true;

	enum class RenderAnnotationLabels : uint8_t
	{
		None,
		All,
		Selected,
		Explicit
	};

	// Capture-only annotations. Collider annotations use Flags::Colliders; Selected refers to SelectedEntities.
	struct RenderAnnotations
	{
		RenderAnnotationLabels Labels = RenderAnnotationLabels::None;
		std::vector<UUID> LabelEntities{}; // Explicit only: owned, sorted/deduplicated; no editor-selection mutation
		bool Bounds = false;
		bool Axes = false;
	};

	enum class RenderIconKind : uint8_t
	{
		Camera,
		DirectionalLight,
		PointLight,
		SpotLight,
		AudioSource,
		AudioListener
	};

	// Renderer-owned world glyphs; Editor/Icons.cpp supplies UI glyphs separately. No UI font/ImGui dependency.
	struct RenderIcon
	{
		RenderIconKind Kind = RenderIconKind::Camera;
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec4 Color = glm::vec4(1.0f);
		float Size = 20.0f; // framebuffer pixels, constant apparent size in either projection
		UUID Entity{};
	};

	// M29 (Docs/Decisions/0044-m29-light-limits.md decision 1): the lights one view shades by default
	// (RenderQualitySettings::MaxShadedLights, ProjectSettings.Rendering.MaxShadedLights), and the largest cap a project may
	// set. More visible lights than the cap drop the least important (RenderPrepare.h's CullLights, RENDER_LIGHT_LIMIT_EXCEEDED).
	inline constexpr uint32_t DefaultMaxShadedLights = 256;
	inline constexpr uint32_t MaxShadedLightsCeiling = 2048;

	// Whether `value` is a shaded-light cap a view takes: 256, 512, 1024 or 2048 (the cluster masks hold a whole number of
	// 32-bit words per cluster, Shared/LightClusterConstants.h).
	[[nodiscard]] constexpr bool IsValidMaxShadedLights(uint32_t value)
	{
		return value == 256 || value == 512 || value == 1024 || value == 2048;
	}

	// Copied from ProjectSettings.Rendering by the host; no Project dependency in Renderer.
	struct RenderQualitySettings
	{
		uint32_t ShadowMapSize = 2048; // power of two, [256, 8192]
		bool SsaoHalfResolution = false;
		// M29 (decision 1): the most lights one view shades, IsValidMaxShadedLights (ExtractRenderSnapshot refuses others).
		uint32_t MaxShadedLights = DefaultMaxShadedLights;
		// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 11; ADR 0042's phase-5 ledger: the members copied from Rendering
		// settings after SsaoHalfResolution, in landing order): ProjectSettings.Rendering.SsrHalfResolution, the SSR pass's trace
		// at half resolution (Renderer/SsrPass.h).
		bool SsrHalfResolution = false;
		// M23 (C23.1, decisions 2 and 5): the reference paths the renderer's tests compare its fast paths with, on in every
		// host (no project setting copies them). Instancing false draws every mesh item's submesh with its own draw, which an
		// instanced render equals byte for byte (R-INST); ClusteredLights false shades every pixel with every light of the
		// light list, which a clustered render equals byte for byte (R-CLC).
		bool Instancing = true;
		bool ClusteredLights = true;
		// M26 (C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8): whether the forward passes add the probes'
		// indirect term (IrradianceVolumes and ReflectionProbes). True in every host; lighting.bake's captures turn it off, so a
		// bake sees the scene's direct light, its Environment and its emission, never an earlier bake (one bounce, and two
		// bakes in a row are byte-identical).
		bool Probes = true;
	};

	// A world-space ray. Camera pixel rays are normalized; SceneRaycast also accepts and normalizes non-unit directions.
	struct RenderRay
	{
		glm::vec3 Origin = glm::vec3(0.0f);
		glm::vec3 Direction = glm::vec3(0.0f, 0.0f, -1.0f);
	};

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
	// (Albedo, Emissive), so a data view's value v is stored as round(255 v). Overdraw bypasses the forward-depth test
	// and counts geometric coverage in its dedicated additive pass; it is not a forward shading specialization alone.
	// AO/ShadowCascades/Overdraw/LightCount suppress ordinary overlays and text; explicit capture annotations append
	// afterwards. M9 appends AO, ShadowCascades and Overdraw after Emissive (Docs/Decisions/0013-m8-decisions.md decision 12),
	// and M23 LightCount after Overdraw (Docs/Decisions/0034-m23-contract.md decision 5), a data view like them.
	enum class RenderDebugView : uint8_t
	{
		Lit,            // the shaded image
		Albedo,         // base colour (factor times map, linear), alpha ignored
		Normals,        // the shading normal (normal-mapped, world space) as N * 0.5 + 0.5
		Roughness,      // perceptual roughness (after the 0.045 clamp, §8.5) as grey
		Metallic,       // metallic as grey
		Emissive,       // emitted radiance (Emissive times map times EmissiveStrength), clamped to [0, 1]
		AO,             // final denoised/upscaled GTAO (before materialAO), scalar grey; disabled/background = white
		ShadowCascades, // receiver cascade index colors, blended/faded exactly like shadow visibility; no allocation = black
		Overdraw,       // accepted fragment count before depth rejection, palette specified in ADR0017; background black
		// M23 (R-CLC): the number of lights the pixel's light cluster holds (LightClusters.h; every light of the list when the
		// forward passes loop over all of them), in GetLightCountDebugColor's palette; background black
		LightCount
	};

	// The number of debug views: RenderDebugView's last enumerator plus one, so the view that M23 appends last is named here.
	// The view names (RenderDebugViewToString), the forward pipelines per view (SceneRendererPipelines::EnsureDebugView) and
	// viewport.screenshot's list of valid names all follow it.
	inline constexpr uint32_t RenderDebugViewCount = static_cast<uint32_t>(RenderDebugView::LightCount) + 1;

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
		Right,
		// M38 (Docs/Decisions/0055-m38-runtime-locale.md decision 11): MeasureText aligns each line by its paragraph's resolved
		// base direction (Start: Left for left to right, Right for right to left; End the opposite); TextLayout, which lays
		// out unshaped left-to-right text, reads them as Left and Right.
		Start,
		End
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
		// The viewport the projection was made for: the scene resolution (M18: RenderOutputSettings, the view's size by
		// default). Pixel rays (ComputeViewPixelRay) take scene pixels of this size.
		uint32_t ViewportWidth = 1;
		uint32_t ViewportHeight = 1;
		// CameraComponent::ClearColor, linear. SceneColor is cleared to it; with ClearToSkybox and an environment map the
		// skybox pass draws over every pixel the scene left at depth 0 (M8).
		glm::vec3 ClearColor = glm::vec3(0.05f, 0.05f, 0.06f);
		bool ClearToSkybox = true; // CameraComponent::Clear == ClearMode::Skybox
		UUID Entity{};             // the camera entity; the invalid UUID for an explicit camera (the editor's scene view)
		// M26 (C26.1, R-TAA-VEL; Docs/Decisions/0037-m26-contract.md decision 10, the temporal-state section): the view's
		// temporal inputs, which its RenderViewHistory (Renderer/TemporalHistory.h) fills before each Render and nothing else
		// writes. PreviousViewProjection: Projection * View of the view's previous rendered frame, unjittered; empty when the
		// history was reset (the first frame of a view, a capture's first warm-up frame, a camera cut, a scene change), and the
		// renderer then starts its temporal state afresh: zero motion, the current image as TAA's history and an exposure that
		// converges at once. Jitter: the sub-pixel offset, in scene pixels in [-0.5, 0.5], the renderer adds to the projection
		// for TAA (GetTaaJitter of the history's frame); (0, 0) without TAA, in pixel-art mode, and in every snapshot no history
		// filled. DeltaTime: seconds since the view's previous rendered frame (0 after a reset), what auto-exposure adapts over.
		std::optional<glm::mat4> PreviousViewProjection{};
		glm::vec2 Jitter = glm::vec2(0.0f);
		float DeltaTime = 0.0f;
	};

	// M31 (Docs/Decisions/0046-m31-material-parameters.md decisions 2 and 3): an item's per-entity material parameters, its
	// MaterialParameters component as render extraction resolves it (RenderExtraction.h), applied on top of every material of
	// the item's submeshes. The defaults are neutral: an item that holds them draws exactly as one without the component
	// (MakeMeshInstanceRecord leaves DrawFlagMaterialParameters clear, RenderPrepare.h). Every value is finite.
	//   - Tint: linear, multiplies the base colour's RGB (after the material's factor, its map and the vertex colour).
	//   - Emissive: linear radiance added to the material's emission (the component's Emissive times EmissiveStrength).
	//   - UVOffset: added to every map's texture coordinates after the material's UV transform (the component's UVOffset plus
	//     UVScroll times the request's simulation time), each component wrapped into [0, 1), since every map samples with a
	//     repeating sampler.
	//   - DissolveAmount in [0, 1]: the fragments whose dissolve noise (BuiltinAssetHandles::DissolveNoiseTexture, sampled at
	//     the first texture coordinates times DissolveNoiseScale) lies below it are clipped; DissolveEdge (linear radiance,
	//     the component's DissolveEdgeColor times DissolveEdgeStrength) is added where the noise lies below DissolveAmount +
	//     DissolveEdgeWidth. Without a dissolve (DissolveAmount 0) the three edge values are neutral.
	//   - Opacity in [0, 1]: the fade, a dithered clip of an Opaque or Mask material (shadows unchanged) and a factor of a
	//     Blend material's alpha.
	struct MeshMaterialParameters
	{
		glm::vec3 Tint = glm::vec3(1.0f);
		glm::vec3 Emissive = glm::vec3(0.0f);
		glm::vec2 UVOffset = glm::vec2(0.0f);
		float DissolveAmount = 0.0f;
		glm::vec3 DissolveEdge = glm::vec3(0.0f);
		float DissolveEdgeWidth = 0.0f;
		float DissolveNoiseScale = 1.0f;
		float Opacity = 1.0f;

		// Whether these are the neutral defaults, which draw as no parameters at all.
		[[nodiscard]] bool IsNeutral() const { return *this == MeshMaterialParameters{}; }
		bool operator==(const MeshMaterialParameters&) const = default;
	};

	// One MeshRenderer to draw (§8.2 "world matrix, mesh handle+version, material per submesh, flags"). The renderer resolves
	// the asset versions, the mesh's default materials and placeholders through the AssetManager and its GpuResourceCache,
	// so extraction never touches assets. M23 (C23.1, decision 2): each of its submesh draws is one instance record
	// (Shared/InstanceRecord.h), and draws whose mesh, submesh, material and pipeline agree share an instanced draw.
	struct MeshDrawItem
	{
		glm::mat4 World = glm::mat4(1.0f); // the rendered (interpolated, §5.2) world matrix
		// M23 (C23.1): the item's world matrix at the previous frame's render, for motion vectors (M26's TAA): the instance
		// record's PreviousWorld. Empty means no motion (the record takes World). M26 (C26.1, decision 11): extraction still
		// leaves it empty; the view's RenderViewHistory fills it from the item's World at the view's previous rendered frame,
		// by Entity (empty for an item it did not see then, and after a reset).
		std::optional<glm::mat4> PreviousWorld{};
		AssetHandle Mesh{}; // a null handle draws nothing
		// MeshRendererComponent::Materials: one slot per submesh; a missing or null slot uses the mesh's default material.
		std::vector<AssetHandle> Materials{};
		bool CastShadows = true;
		bool ReceiveShadows = true;
		UUID Entity{};
		uint32_t PickId = 0; // 0: not pickable; otherwise PickTable[PickId - 1] is Entity, stable within this snapshot
		// M23 (C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 27): a SkinnedMesh's deformation, which the
		// Deform stage's Skinning pass applies to the mesh's vertices (Asset/MeshDeform.h's rule) before every pass draws them.
		// JointPalette: one matrix per joint of its Animator's pose, from the mesh's bind space into this item's local space at
		// the rendered (interpolated) poses: inverse(World) * the Animator entity's rendered world matrix *
		// AnimationPoseComponent::Skin[j]; empty without a pose (the bind pose). MorphWeights: the SkinnedMesh's weights, each
		// replaced by the weight the pose's blended channel for the entity holds at its index (as long as the longer of the two);
		// empty without a SkinnedMesh. Extraction does not load the mesh, so the list may hold fewer weights than the mesh's
		// targets (or more): the renderer resolves it against the targets' defaults the GPU mesh keeps (ResolveMorphWeights with
		// GpuMesh::DeformBounds.DefaultWeights), so a missing weight, every weight of an item without a SkinnedMesh, is its
		// target's DefaultWeight. A weight is any finite value (a clip's sampled weight may leave [-1, 1]; the deformed bounds
		// cover it). An item is deformed when its mesh has a skin and JointPalette covers the skin's joints, or has morph targets
		// and a resolved weight is not 0; otherwise it draws the mesh's own vertices. A deformed item's GPU state is keyed by its
		// Entity and Mesh, so each deformed item of a snapshot has its own Entity (render extraction gives every item its entity;
		// a snapshot built by hand must too), and deformed items are never instanced together (each has its own pose). Render
		// extraction fills them (RenderExtraction.h).
		std::vector<glm::mat4> JointPalette{};
		std::vector<float> MorphWeights{};
		// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 3): the entity's MaterialParameters as extraction resolved
		// them; neutral without the component. They ride in the item's instance records, so items that differ only in them
		// still share instanced draws, but a dissolving or fading item of an Opaque material draws with the Mask variants
		// (SelectMeshAlphaMode, RenderPrepare.h).
		MeshMaterialParameters Parameters{};
	};

	// One light (§8.2: directional, point, spot). The renderer culls them on the CPU and lights with at most
	// RenderQualitySettings::MaxShadedLights of them (RenderPrepare.h, RENDER_LIGHT_LIMIT_EXCEEDED); the walking skeleton used the first
	// directional light only. M23 (C23.1): the forward passes shade them through the view's cluster grid
	// (LightClusters.h), and point lights cast shadows (PointShadowMaps.h).
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
		// DirectionalLightComponent's shadow settings. Spot and point maps use DepthBias/NormalBias defaults; a point light casts
		// shadows from M23 (PointLightComponent::CastShadows, PointLight version 2), at most MaxPointShadowLights of them.
		float ShadowDistance = 100.0f;
		uint32_t CascadeCount = 4;
		float CascadeSplitLambda = 0.75f;
		float DepthBias = 1.0f;
		float NormalBias = 1.0f;
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

	// M26 (C26.1, R-TAA; Docs/Decisions/0037-m26-contract.md decision 12): PostProcess version 2's AntiAliasing, which replaces
	// version 1's FxaaEnabled (true is Fxaa, false None). Taa is opt-in and acts as None in the output stage's pixel-art mode.
	enum class RenderAntiAliasing : uint8_t
	{
		None,
		Fxaa, // FXAA on the LDR image (the default, so every existing image stays as it was)
		Taa   // the jittered HDR image resolved against its history, first in HdrPost (Renderer/TaaPass.h)
	};

	// M26 (C26.1, R-AEX; decision 12): PostProcess version 2's ExposureMode.
	enum class RenderExposureMode : uint8_t
	{
		Manual, // the image times 2^ExposureEV, as version 1 did
		Auto    // the histogram's adapted exposure (Renderer/AutoExposure.h), compensated by ExposureEV
	};

	// The scene's PostProcessComponent (§5.3), or its defaults when the scene has none. M8 applies the exposure, the
	// tonemapper (with the blue-noise dither), bloom and FXAA; the Ssao members take effect with GTAO (M9). M26 (C26.1,
	// decision 12): PostProcess version 2's members, whose defaults render as version 1 did (the manual ExposureEV, no LUT,
	// SSR off, FXAA): render extraction copies every member from the component (a version 1 component loads through its
	// migration, FxaaEnabled as Fxaa or None), and each takes effect with its stream (X-A the exposure, X-B the LUT, X-C TAA,
	// X-D SSR).
	struct PostProcessSettings
	{
		float ExposureEV = 0.0f; // multiplier 2^ExposureEV; with Auto, the compensation added to the adapted exposure
		RenderExposureMode ExposureMode = RenderExposureMode::Manual;
		float AutoExposureMinEV = -6.0f; // the adapted exposure's range, in EV (the multiplier's log2)
		float AutoExposureMaxEV = 12.0f;
		float AutoExposureSpeedUp = 3.0f;   // EV per second towards a brighter target
		float AutoExposureSpeedDown = 1.0f; // EV per second towards a darker target
		RenderTonemapper Tonemap = RenderTonemapper::AgX;
		AssetHandle Lut{};         // the grading LUT (Asset/LutData.h); null grades nothing
		float LutIntensity = 1.0f; // [0, 1]: the graded value's share
		bool SsaoEnabled = true;
		float SsaoRadius = 0.5f;
		float SsaoIntensity = 1.0f;
		RenderSsaoQuality SsaoQuality = RenderSsaoQuality::Medium;
		bool SsrEnabled = false;      // glossy screen-space reflections (Renderer/SsrPass.h)
		float SsrIntensity = 1.0f;    // [0, 1]
		float SsrMaxDistance = 50.0f; // metres a reflected ray travels
		float SsrThickness = 0.2f;    // metres a surface is assumed thick behind its depth
		float SsrMaxRoughness = 0.6f; // perceptual roughness above which only the probes reflect
		bool BloomEnabled = true;
		float BloomIntensity = 0.04f;
		RenderAntiAliasing AntiAliasing = RenderAntiAliasing::Fxaa;
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

	// The output stage of one view (M18, C18.1, R-2D-PIXEL; Docs/Decisions/0029-m18-contract.md decisions 5 to 8). Its
	// defaults render the scene at the view's size with the snapshot's post-processing, exactly as before the output stage.
	// M18 sets it from tests only; M21's PixelPerfectCamera sets it from the scene.
	struct RenderOutputSettings
	{
		// The scene resolution: the size of every scene-resolution target (depth, normals, colour, EntityId, the LDR targets)
		// and of the camera's viewport (CameraData::ViewportWidth and ViewportHeight). A 0 component takes the view's. When it
		// differs from the view, the output stage upscales the scene image by the integer factor of ComputeRenderOutputMapping
		// with point sampling, centred in a letterbox of LetterboxColor.
		uint32_t SceneWidth = 0;
		uint32_t SceneHeight = 0;
		// The 2D colour path (decision 7): the post chain runs the Linear tonemapper at exposure 1 (2^0) with the sRGB OETF
		// and without the blue-noise dither, bloom or FXAA, whatever Post says, so a texel drawn into SceneColor (a sprite's,
		// M21) reaches the output unchanged. GTAO still follows Post (it shades meshes only). M26 (C26.1; Docs/Decisions/
		// 0037-m26-contract.md decision 10) keeps the rule for its post-processing: in pixel-art mode TAA acts as None (no
		// jitter, no resolve), ExposureMode Auto as Manual at exposure 1 (no Exposure pass) and no LUT grades the image, so
		// a pixel-art capture never needs the temporal warm-up (NeedsTemporalWarmup).
		bool PixelArt = false;
		// The letterbox around the upscaled scene image: linear RGB, encoded with the sRGB OETF like a clear colour.
		glm::vec3 LetterboxColor = glm::vec3(0.0f);

		bool operator==(const RenderOutputSettings&) const = default;
	};

	// Where the scene image lands in the view (ComputeRenderOutputMapping): view pixel (x, y) shows scene pixel
	// (floor((x - OffsetX) / Scale), floor((y - OffsetY) / Scale)) when that lies in the scene image, and the letterbox
	// otherwise. Top-left origin, +y down, in both spaces.
	struct RenderOutputMapping
	{
		uint32_t ViewWidth = 1;
		uint32_t ViewHeight = 1;
		uint32_t SceneWidth = 1;
		uint32_t SceneHeight = 1;
		// View pixels per scene pixel along each axis, at least 1.
		uint32_t Scale = 1;
		// The view pixel of the scene image's top-left corner: floor((View - Scale * Scene) / 2) per axis, rounded toward
		// negative infinity (not C++'s truncating division), so an odd leftover pixel goes to the right or bottom letterbox;
		// negative when a scene larger than the view is cropped, and then an odd surplus pixel is cropped from the left or
		// top (a 161-pixel scene in a 100-pixel view has offset -31: 31 pixels cropped on the left, 30 on the right).
		int32_t OffsetX = 0;
		int32_t OffsetY = 0;

		// True when the scene image is the view (equal sizes, so Scale 1 and no offset): the output stage records nothing.
		[[nodiscard]] bool IsIdentity() const { return ViewWidth == SceneWidth && ViewHeight == SceneHeight; }

		bool operator==(const RenderOutputMapping&) const = default;
	};

	// One SpriteRenderer to draw (M21, C21.1; Docs/Decisions/0032-m21-contract.md decisions 3 and 4), from render extraction.
	// The renderer resolves the sheet's frame (its texel rectangle and pivot) and the texture through the AssetManager and its
	// GpuResourceCache, so extraction never touches assets, and draws the sprite in the Sprites stage as one QuadBatcher quad
	// in world space (Asset/SpriteGeometry.h gives its corners), depth-tested against the opaque meshes without writing depth.
	// A texture that is missing or fails to load (Texture, or the sheet's) draws its placeholder, the Missing checker, whole
	// over the sprite's quad (the frame's, or the placeholder's 64 x 64 texels centred); a sheet that is missing or fails to
	// load, or an out-of-range Frame, draws nothing (Scene/Components/SpriteRendererComponent.h states the one rule, which
	// picking and bounds follow).
	struct SpriteItem
	{
		glm::mat4 World = glm::mat4(1.0f); // the rendered (interpolated, §5.2) world matrix
		AssetHandle Sheet{};               // the sprite sheet; null draws Texture whole
		uint32_t Frame = 0;                // the index of the sheet's frame
		// The texture drawn whole when Sheet is null; null draws the built-in white square (engine://Textures/SpriteSquare).
		AssetHandle Texture{};
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB, straight alpha; multiplies every texel
		bool FlipX = false;
		bool FlipY = false;
		float PixelsPerUnit = 16.0f; // texels per unit of the entity's local space
		// The sprite's sorting layer: its index in the project's SortingLayers.Layers (RenderExtractionRequest::SortingLayers),
		// 0 for an undeclared name.
		uint32_t SortingLayer = 0;
		int32_t OrderInLayer = 0;
		// C21.2 (decision 22): the sprite takes the view's 2D lights (Lights2D): each texel is multiplied by the light
		// accumulated at its pixel.
		bool Lit = false;
		// C21.2 (decision 21): a lit sprite's tangent-space normal map (a linear texture laid like the image's); null for the
		// flat normal. Unlit sprites ignore it.
		AssetHandle NormalMap{};
		UUID Entity{};
	};

	// M21 (C21.2; Docs/Decisions/0032-m21-contract.md decision 20): what a 2D light lights (Light2DComponent's Light2DType).
	enum class RenderLight2DType : uint8_t
	{
		Point,
		Global
	};

	// One Light2D to draw (C21.2, decision 22), from render extraction: the Lights2D pass adds its light into the view's light
	// accumulation (Renderer/Light2DShading.h states the formula), which lit sprites and tilemaps sample.
	struct Light2DItem
	{
		RenderLight2DType Type = RenderLight2DType::Point;
		glm::vec3 Position = glm::vec3(0.0f); // the rendered world position; a Point light lights in the plane z = Position.z
		glm::vec3 Color = glm::vec3(1.0f);    // linear
		float Intensity = 1.0f;
		float Radius = 5.0f;  // Point: world units
		float Falloff = 0.5f; // Point: [0, 1]
		float Height = 1.0f;  // Point: world units above the lit plane, for normal-mapped sprites
		UUID Entity{};
	};

	// One TilemapRenderer to draw (C21.2, decision 24), from render extraction. The renderer resolves the tilemap, its
	// palette's sheets and their textures through its AssetManager and GpuResourceCache, as for sprites, and draws every
	// non-empty cell's tile as the sprite TilemapRendererComponent.h describes (world matrix World times the translation to
	// the cell's centre), in the Sprites stage through the QuadBatcher, in the tilemap's cell order: the chunks whose bounds
	// (the union of their tiles' quads) meet the view only. A tilemap that is missing or fails to load draws nothing.
	struct TilemapItem
	{
		glm::mat4 World = glm::mat4(1.0f); // the rendered (interpolated, §5.2) world matrix
		AssetHandle Tilemap{};
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB, straight alpha; multiplies every tile's texels
		float PixelsPerUnit = 16.0f;
		uint32_t SortingLayer = 0; // as SpriteItem's
		int32_t OrderInLayer = 0;
		bool Lit = false;
		// Where the tilemap draws among the snapshot's sprites: before Sprites[DrawPosition] (after all of them at
		// Sprites.size()), the sprites and tilemaps forming one draw order sorted by (SortingLayer, OrderInLayer, canonical
		// entity order). Tilemaps of one DrawPosition draw in their order in Tilemaps.
		uint32_t DrawPosition = 0;
		UUID Entity{};
	};

	// M26 (C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 7): how a ParticleEmitter's particles are blended
	// (ParticleEmitterComponent::Blend).
	enum class RenderParticleBlend : uint8_t
	{
		Alpha,   // straight alpha, the emitter's particles back to front in the view
		Additive // added (colour times alpha), in any order
	};

	// M33 (Docs/Decisions/0048-m33-particle-look.md decision 3): how an emitter's particle quads are laid (ParticleLookComponent's
	// ParticleAlignment; Camera without a look).
	enum class RenderParticleAlignment : uint8_t
	{
		Camera,   // a square facing the camera, turned by the particle's Rotation
		Velocity, // stretched along the particle's Velocity across the view
		Flat      // a square in the emitter's plane (PlaneRight, PlaneUp), turned by the particle's Rotation
	};

	// M33 (Docs/Decisions/0048-m33-particle-look.md decision 6): what a particle's quad was drawn from in a view's render: its
	// position, size, angle and velocity (the stretch of a Velocity particle), from which the Particles pass lays its previous
	// corners for the motion vectors.
	struct ParticleDrawState
	{
		glm::vec3 Position = glm::vec3(0.0f);
		float Size = 0.1f;
		float Rotation = 0.0f;
		glm::vec3 Velocity = glm::vec3(0.0f);

		bool operator==(const ParticleDrawState&) const = default;
	};

	// One live particle to draw (decision 7): a quad in world space, by its emitter's alignment (M33).
	struct ParticleItem
	{
		glm::vec3 Position = glm::vec3(0.0f); // world space, at the extraction's alpha between the particle's last two steps
		float Size = 0.1f;                    // the quad's side, metres
		// Linear RGB, straight alpha; multiplies the texture. M33: its RGB times the look's Intensity, so it may exceed 1.
		glm::vec4 Color = glm::vec4(1.0f);
		// M33 (Docs/Decisions/0048-m33-particle-look.md decisions 3 and 6), appended: the angle in radians (counter-clockwise as
		// seen) at the extraction's alpha between the particle's last two steps; its velocity in world space (metres per second,
		// a Local emitter's through its rendered world matrix), which a Velocity emitter stretches it along; the flipbook's
		// phase (OverLife: the particle's age over its lifetime as of its last step, below 1, times FrameCycles; Random: its
		// pick in [0, 1)), which the renderer turns into a frame of its emitter's range; its serial among its emitter's
		// emissions; and how the view's previous render drew it, matched by its emitter's entity and incarnation and its serial
		// (RenderViewHistory::Apply; empty for a particle that render did not draw, after a reset and in every snapshot no
		// history filled), from which the Particles pass lays its previous corners for the motion vectors.
		float Rotation = 0.0f;
		glm::vec3 Velocity = glm::vec3(0.0f);
		float FramePhase = 0.0f;
		uint64_t Serial = 0;
		std::optional<ParticleDrawState> Previous{};
	};

	// One ParticleEmitter's live particles (decision 7), from render extraction: RenderSnapshot::Particles[First, First +
	// Count). The Particles pass (the Sprites stage, after Lights2D) draws each emitter's particles as QuadBatcher quads with its
	// texture (the built-in particle texture, BuiltinAssetHandles::ParticleTexture, when null), depth-tested against SceneDepth
	// without writing it, in the order of ParticleEmitters; an Alpha emitter's particles back to front in the view (by view
	// depth, ties by index), an Additive one's in its order.
	struct ParticleEmitterItem
	{
		AssetHandle Texture{};
		RenderParticleBlend Blend = RenderParticleBlend::Alpha;
		uint32_t First = 0;
		uint32_t Count = 0;
		UUID Entity{};
		// M33 (Docs/Decisions/0048-m33-particle-look.md decisions 3 to 5), appended, from the emitter's ParticleLook (these
		// defaults without one, which draw as M26 did): the flipbook's sheet (null: Texture drawn whole), the first frame of
		// its range and its length (0: to the sheet's last frame), whether a particle cross-fades into the next frame
		// (OverLife's BlendFrames), the alignment, the stretch of a Velocity particle (seconds), the soft distance (metres; 0:
		// a hard cut) and, for Flat, the emitter's rendered local +X and -Z axes, normalized: the plane's right and up; then how
		// many times OverLife plays the range over a life (FrameCycles, so the last frame of the last play is held rather than
		// blended into the first), and the incarnation of the emitter's particle state (ParticleStateComponent::Incarnation),
		// which the view history matches with the serials, so a state made again never takes its predecessor's positions.
		AssetHandle Sheet{};
		uint32_t FirstFrame = 0;
		uint32_t FrameCount = 0;
		bool BlendFrames = false;
		RenderParticleAlignment Alignment = RenderParticleAlignment::Camera;
		float VelocityStretch = 0.1f;
		float SoftDistance = 0.0f;
		glm::vec3 PlaneRight = glm::vec3(1.0f, 0.0f, 0.0f);
		glm::vec3 PlaneUp = glm::vec3(0.0f, 0.0f, -1.0f);
		float FrameCycles = 1.0f;
		uint64_t Incarnation = 0;
	};

	// One IrradianceVolume to light with (M26, C26.1, R-GI-PROBE; decision 16), from render extraction: every effectively
	// enabled IrradianceVolume whose Data is set, in canonical entity order. The forward passes shade the diffuse indirect
	// term inside its box from its probes (Common/Probes.slang), the renderer resolving Data through its GpuResourceCache.
	struct IrradianceVolumeItem
	{
		glm::mat4 World = glm::mat4(1.0f); // the rendered world matrix; the box is Size centred on its origin, in its axes
		glm::vec3 Size = glm::vec3(10.0f, 4.0f, 10.0f);
		AssetHandle Data{};
		float Intensity = 1.0f;
		float BlendDistance = 1.0f;
		UUID Entity{};
	};

	// One ReflectionProbe to reflect (decision 16), from render extraction: every effectively enabled ReflectionProbe whose
	// Data is set, in canonical entity order. Data is an Environment; the forward passes sample its prefiltered specular cube
	// inside the box (box-projected with BoxProjection), and SSR falls back to it.
	struct ReflectionProbeItem
	{
		glm::mat4 World = glm::mat4(1.0f); // the rendered world matrix; the box is Size centred on its origin, in its axes
		glm::vec3 Size = glm::vec3(10.0f, 5.0f, 10.0f);
		AssetHandle Data{};
		bool BoxProjection = true;
		float Intensity = 1.0f;
		float BlendDistance = 1.0f;
		UUID Entity{};
	};

	// One material layer of a TerrainItem (TerrainComponent's TerrainLayer).
	struct TerrainLayerItem
	{
		AssetHandle BaseColorMap{}; // null: white
		AssetHandle NormalMap{};    // null: flat
		glm::vec3 Tint = glm::vec3(1.0f);
		float Tiling = 4.0f;
		float Roughness = 0.9f;
		float Metallic = 0.0f;
	};

	// One Terrain to draw (M26, C26.1, C-TERR; decision 22), from render extraction: every effectively enabled Terrain whose
	// Heightfield is set, in canonical entity order. The renderer resolves the heightfield and the splat map through its
	// GpuResourceCache and draws the terrain's LOD chunks (Renderer/TerrainLod.h) in TerrainDepthNormal (the prepass's
	// targets, picking included) and TerrainForward (lit by M23's light loop and the probes), with Asset/TerrainGeometry.h's
	// placement; a heightfield that is missing or fails to load draws nothing. It has no PreviousWorld (the view's history
	// keeps the mesh items' only): TerrainDepthNormal writes the camera's motion alone to SceneVelocity, as for an item whose
	// world did not change, so a terrain that moves (under a moving parent, or moved by a script) ghosts under TAA, a kept
	// limit (decision 22, Backlog §6).
	struct TerrainItem
	{
		glm::mat4 World = glm::mat4(1.0f); // the rendered world matrix; the terrain spans [0, Size.x] x [0, Size.y] in local XZ
		AssetHandle Heightfield{};
		glm::vec2 Size = glm::vec2(64.0f);
		float HeightScale = 16.0f;
		std::vector<TerrainLayerItem> Layers{}; // at most MaxTerrainLayers; empty draws one white layer
		AssetHandle SplatMap{};                 // null: the first layer alone
		bool ReceiveShadows = true;
		float LodBias = 1.0f;
		UUID Entity{};
		uint32_t PickId = 0; // 0: not pickable; otherwise PickTable[PickId - 1] is Entity, as a mesh item's
	};

	// M32 (Docs/Decisions/0047-m32-fog-ssr.md decisions 3 and 4): the scene's FogComponent, from render extraction: the first
	// effectively enabled one in canonical order (it is unique per scene), with Enabled set; Enabled false (the defaults) when
	// the scene has none, which renders exactly as a renderer without fog did. The renderer fogs a Lit view with a camera
	// (Renderer/Fog.h: the model, the passes it reaches; Shared/FogConstants.h: the constants); lighting.bake's probe
	// captures turn it off (Renderer/ProbeCapture.h). The members are the component's (Scene/Components/FogComponent.h); colours are linear.
	struct RenderFog
	{
		bool Enabled = false;
		glm::vec3 Color = glm::vec3(0.5f, 0.55f, 0.6f);
		float AmbientTint = 0.0f;
		float Density = 0.02f;      // per metre of view depth
		float StartDistance = 0.0f; // metres of view depth
		float MaxOpacity = 1.0f;    // [0, 1]
		float HeightDensity = 0.0f; // per metre at BaseHeight
		float HeightFalloff = 0.5f; // per metre of height
		float BaseHeight = 0.0f;    // world metres
		float SunGlow = 0.0f;       // a multiple of the first directional light's Color * Intensity
		float SunGlowExponent = 8.0f;
		bool FogSkybox = true;
	};

	// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the bounds of PlanarReflectionItem::ResolutionScale, the
	// PlanarReflection component's field (Scene/Components/PlanarReflectionComponent.h), and its default.
	inline constexpr float MinPlanarReflectionResolutionScale = 0.25f;
	inline constexpr float MaxPlanarReflectionResolutionScale = 1.0f;
	inline constexpr float DefaultPlanarReflectionResolutionScale = 0.5f;

	// One PlanarReflection to render (M42, decision 3), from render extraction: every effectively enabled entity that holds a
	// PlanarReflection and a MeshRenderer that is Visible with a mesh, in canonical entity order, in every view (an edit
	// scene's included). World is the rendered world matrix its mesh item draws at; the plane passes through the local point
	// Normal * Offset with the local normal Normal (the component's, normalized by the renderer; a zero or non-finite one
	// renders no reflection), facing its reflecting side, and the renderer takes it into the world through World
	// (Renderer/PlanarReflections.h: ComputePlanarReflectionPlane), so a non-uniform scale is handled and a moving glass is
	// followed. ResolutionScale, in [MinPlanarReflectionResolutionScale,
	// MaxPlanarReflectionResolutionScale], sizes the reflection's target from the view's scene resolution. The renderer
	// reflects in the plane the draws of every mesh item but the reflection's own Entity, whose draws sample the reflection
	// (§8.3, "Planar reflections"); lighting.bake's captures clear the list (Renderer/ProbeCapture.h).
	struct PlanarReflectionItem
	{
		glm::mat4 World = glm::mat4(1.0f);
		glm::vec3 Normal = glm::vec3(0.0f, 0.0f, 1.0f);
		float Offset = 0.0f;
		float ResolutionScale = DefaultPlanarReflectionResolutionScale;
		UUID Entity{};
	};

	// One view's render snapshot (§8.2). Vectors are in canonical entity order (§5.1), so a snapshot of a deterministic scene
	// is deterministic; Sprites (M21) are in draw order instead.
	struct RenderSnapshot
	{
		// The containers allocate from the default memory resource (see the file comment).
		RenderSnapshot() = default;
		// M18 (R-ARENA, decision 9): the containers allocate from `resource` (not null, asserted), which outlives the snapshot;
		// a host measures or uses a per-frame arena through it. Copies allocate from the default resource.
		explicit RenderSnapshot(std::pmr::memory_resource* resource);

		// False when the view has no camera (a game view of a scene without a primary camera): the renderer then clears to
		// the default ClearColor and draws nothing but screen texts (M8, TextRenderInputs::HasCamera).
		bool HasCamera = false;
		CameraData Camera{};
		std::pmr::vector<MeshDrawItem> Meshes{};
		std::pmr::vector<LightData> Lights{};
		RenderEnvironment Environment{};
		PostProcessSettings Post{};
		// The interpolation alpha the poses were extracted at (§5.2): the frame's Alpha for the game view (1 for ManualClock
		// frames and lockstep ticks); PlaySession::GetViewAlpha for a view of the play scene between ticks (screenshots: 1
		// while paused, in lockstep or after a ManualClock frame, otherwise the last frame's Alpha); 1 for an edit scene. Read
		// by the FeatureTest Timing suite (Test.GetLastExtraction, M13).
		float Alpha = 1.0f;
		// M8: every effectively enabled TextComponent with non-empty Text, in canonical entity order (screen and world texts
		// together; the text pass draws the world ones, depth-tested, then the screen ones, §8.3 pass 14).
		std::pmr::vector<TextItem> Texts{};
		// M8: the ordinary debug primitives of this view (DebugDrawList.h): M13's scripts through the session's persistent list,
		// the editor's gizmos. Extraction leaves it empty; its producers append to it. The AO, ShadowCascades, Overdraw and
		// LightCount data views leave it out (ADR 0017).
		DebugDrawList DebugDraw{};
		// M9: the view's annotations (Scene/RenderAnnotations.h: entity labels, bounds, axes and collider outlines, for a capture
		// or the editor's viewport options). Drawn in every debug view after DebugDraw, data views included.
		DebugDrawList AnnotationDraw{};
		// M8: what the forward passes output; Lit except a viewport.screenshot with "debugView" or the editor's debug view.
		RenderDebugView DebugView = RenderDebugView::Lit;
		// M9: assigned once in canonical mesh-entity order before visibility culling; no duplicate or invalid UUIDs.
		std::pmr::vector<UUID> PickTable{};
		RenderViewFlags Flags = RenderViewFlags::None;
		std::vector<UUID> SelectedEntities{}; // copied, canonicalized; unknown ids ignored; never component references
		RenderAnnotations Annotations{};
		RenderQualitySettings Quality{};
		std::pmr::vector<RenderIcon> Icons{};
		uint64_t FrameIndex = 0;    // host frame for nonblocking timings; capture hosts use their own sequence
		uint64_t SceneRevision = 0; // opaque host revision stamped alongside FrameIndex before Render, echoed by picking
		// M18 (C18.1): the output stage (RenderOutputSettings); the default renders the scene at the view's size.
		RenderOutputSettings Output{};
		// M21 (C21.1, Docs/Decisions/0032-m21-contract.md decision 4): every effectively enabled SpriteRenderer, in draw order:
		// sorted by (SortingLayer, OrderInLayer), ties in canonical entity order (a stable sort of the canonical list), so the
		// Sprites stage draws them back to front exactly in this order.
		std::pmr::vector<SpriteItem> Sprites{};
		// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 24): every effectively enabled TilemapRenderer with a
		// Tilemap, in draw order (sorted as Sprites, each placed among them by its DrawPosition).
		std::pmr::vector<TilemapItem> Tilemaps{};
		// M21 (C21.2, decision 22): every effectively enabled Light2D, in canonical entity order, the order the Lights2D pass
		// adds them in.
		std::pmr::vector<Light2DItem> Lights2D{};
		// M22 (C22.1, Docs/Decisions/0033-m22-contract.md decision 7): the view's UI (UiDrawList.h): every effectively enabled
		// canvas laid out at the scene resolution of Output (the view's size by default), its visible elements' images and
		// texts in draw order, and a capture's UI annotations; empty without a canvas, and in the editor's scene view, which
		// draws no Screen canvas, but for C22.3's World canvases (ExtractRenderSnapshot). The Ui pass draws it after game
		// Text in the SceneOverlay stage; the data views (AO, ShadowCascades, Overdraw) show none, as they show no Text.
		UiDrawList Ui{};
		// M26 (C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 7), in ADR 0028's phase-3 ledger's order after Ui: the
		// play scene's live particles (an edit scene has none), per emitter in canonical entity order, drawn in a Lit view by the
		// Particles pass (the data views and the Overdraw view draw none, as they draw no sprites).
		std::pmr::vector<ParticleEmitterItem> ParticleEmitters{};
		std::pmr::vector<ParticleItem> Particles{};
		// M26 (C26.1, R-GI-PROBE; decision 16): the probes the forward passes light and reflect with.
		std::pmr::vector<IrradianceVolumeItem> IrradianceVolumes{};
		std::pmr::vector<ReflectionProbeItem> ReflectionProbes{};
		// M26 (C26.1, C-TERR; decision 22): the terrains, drawn in TerrainDepthNormal and TerrainForward. A pickable terrain's
		// PickId follows the meshes' in PickTable.
		std::pmr::vector<TerrainItem> Terrains{};
		// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 4; ADR 0042's phase-5 ledger: new members after Terrains): the scene's
		// fog, drawn by the forward passes, the terrains, the skybox, the SSR composite and the Sprites stage's quads of a Lit view.
		RenderFog Fog{};
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3; its ledger: after the fog): the planar reflections of
		// the view, each rendered by the Forward stage's PlanarReflections pass for a Lit view outside the wireframe, at most
		// MaxPlanarReflectionsPerView of them (Renderer/PlanarReflections.h).
		std::pmr::vector<PlanarReflectionItem> PlanarReflections{};
	};

	// World ray through pixel centre (x + 0.5, y + 0.5), top-left origin, in Camera.ViewportWidth/Height: scene pixels (M18:
	// a view pixel maps to them through MapViewPixelToScenePixel first).
	// Perspective starts at Position; orthographic starts on the near plane and uses constant forward direction.
	// Pure, thread-safe, deterministic (DetMath). InvalidArgument: non-finite/singular camera, invalid clip range,
	// zero extent or pixel outside [0, width) x [0, height). No viewport or GPU state is read.
	[[nodiscard]] Result<RenderRay> ComputeViewPixelRay(const CameraData& camera, uint32_t x, uint32_t y);

	// M18 (C18.1, decision 6): the output stage's mapping for a view of `viewWidth` x `viewHeight` (both >= 1, asserted) and
	// `settings`. The scene resolution is the settings' (a 0 component takes the view's); Scale is the largest integer s >= 1
	// with s * SceneWidth <= ViewWidth and s * SceneHeight <= ViewHeight, or 1 when the scene does not fit the view (then the
	// scene image is centred and cropped); the offsets centre the scaled image (RenderOutputMapping). Default settings give
	// the identity. Pure, thread-safe and deterministic (integer arithmetic only).
	[[nodiscard]] RenderOutputMapping ComputeRenderOutputMapping(uint32_t viewWidth, uint32_t viewHeight, const RenderOutputSettings& settings);

	// The scene pixel that view pixel (x, y) shows under `mapping`; nullopt for a pixel of the letterbox (outside the scene
	// image) or outside the view. Picking (SceneRenderer::RequestPick), CPU rays (viewport.pick) and captures map view pixels
	// to scene pixels with it. Pure, thread-safe.
	[[nodiscard]] std::optional<glm::uvec2> MapViewPixelToScenePixel(const RenderOutputMapping& mapping, uint32_t x, uint32_t y);

	// The UI coordinate space (decision 8): a view point (pixel units, top-left origin; (x + 0.5, y + 0.5) is the centre of
	// pixel (x, y)) in scene pixel units, (point - Offset) / Scale, without clamping: a point of the letterbox maps outside
	// [0, SceneWidth) x [0, SceneHeight). UI hit testing maps the pointer's view position with it. Pure, thread-safe,
	// deterministic (IEEE-exact operations only).
	[[nodiscard]] glm::vec2 MapViewPointToScenePoint(const RenderOutputMapping& mapping, glm::vec2 viewPoint);

	// Canonical debug-view spelling, including M9's AO, ShadowCascades and Overdraw and M23's LightCount.
	[[nodiscard]] std::string_view RenderDebugViewToString(RenderDebugView view);

	// The debug view named `name`, ignoring ASCII case; nullopt for any other name. All enumerated views are supported
	// by rendering and capture.
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

	struct RenderRayInterval
	{
		RenderRay Ray{};
		float MinDistance = 0.0f;
		float MaxDistance = 0.0f;
	};

	// ComputeViewPixelRay plus inclusive camera clipping. For perspective let c = -viewDirection.z of the normalized
	// world ray transformed by View (rotation only): [NearClip/c, FarClip/c]. Orthographic origin is already on the near
	// plane: [0, FarClip-NearClip]. Distances refer to Ray.Origin, not view-axis depth. Pure, same errors as pixel ray;
	// InvalidArgument for an unrepresentable interval. Narrow-phase queries reject out-of-interval hits while searching,
	// so a near-clipped foreground triangle cannot hide a valid farther triangle.
	[[nodiscard]] Result<RenderRayInterval> ComputeViewPixelRayInterval(const CameraData& camera, uint32_t x, uint32_t y);

	// M18 (C18.1, decision 6): the camera-clipped ray through the centre of view pixel (x, y) of a view under `mapping`:
	// ComputeViewPixelRayInterval of `camera` with its viewport set to the scene resolution (mapping's SceneWidth x
	// SceneHeight, as the renderer sets it, whatever viewport the camera came with: a snapshot's camera has the request's
	// view size) at the scene pixel MapViewPixelToScenePixel gives, the mapping applied exactly once; nullopt for a pixel of
	// the letterbox or outside the view, which shows no scene (no hit). viewport.pick's CPU ray goes through it. Pure,
	// thread-safe, deterministic. Errors: those of ComputeViewPixelRayInterval.
	[[nodiscard]] Result<std::optional<RenderRayInterval>> ComputeOutputPixelRayInterval(const CameraData& camera, const RenderOutputMapping& mapping,
		uint32_t x, uint32_t y);

	// Pure data-view palette helpers, thread-safe. Display-encoded colors, bypass OETF/dither. Counts: 0 black,
	// 1 blue, 2 cyan, 3 green, 4 yellow, >=5 red (unit RGB primaries). Input is accepted-fragment count per pixel.
	[[nodiscard]] glm::vec3 GetOverdrawDebugColor(uint32_t count);
	// Cascade 0 red, 1 green, 2 blue, 3 yellow; invalid index asserted. Blend/fade weights applied by renderer.
	[[nodiscard]] glm::vec3 GetShadowCascadeDebugColor(uint32_t cascadeIndex);

}
