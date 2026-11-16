#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

// Render extraction (Architecture §5.2, §5.7 frame phase step 4, §8.2): builds the plain RenderSnapshot of one view from a
// scene, so the renderer never sees the ECS. The play session extracts its game view at the end of every frame phase
// (PlaySession, Session/PlaySession.h); screenshots extract their view afresh for the call (§8.13), through
// PlaySession::ExtractView for the play scene, which brings its transforms up to date first; the editor's viewport (M10)
// extracts its scene view every frame.
//
// Render interpolation (§5.2). In a runtime scene (SceneSpecification::Runtime) an entity renders between its
// PreviousWorldTransform and its WorldTransform at Alpha: the translation lerped; when both matrices decompose
// (TransformSystem::DecomposeMatrix) the rotation slerped (shortest arc) and the scale lerped, otherwise (a degenerate
// scale) the current matrix's rotation and scale. It renders at its WorldTransform instead (no interpolation) when:
//   - it or an ancestor has InterpolationResetTag (written in the frame phase, created or enabled since the last step,
//     teleported; the play session sets the tag, PlaySession.h);
//   - it has no PreviousWorldTransformComponent yet (created after the last interpolation snapshot);
//   - the scene is an edit scene.
// Entities without a WorldTransformComponent (none was computed yet) render at TransformSystem::ComputeWorldMatrix.
// This is the rule of TransformSystem::GetRenderPosition and GetRenderRotation (scripts' Transform.RenderPosition and
// RenderRotation, §11.5): the same start pose, the same tags, the same fallback for a previous pose that does not
// decompose. They interpolate towards the freshly computed world pose where extraction reads the cached
// WorldTransformComponent; the two are equal wherever a play session extracts, because FrameTransformUpdate and
// PlaySession::ExtractView run TransformSystem::Update first, so a follow camera placed from RenderPosition (M13) and the
// rendered image agree.
//
// Skinned items (M23, Docs/Decisions/0034-m23-contract.md decision 27): a SkinnedMesh's item carries its driving Animator's
// palette (the runtime AnimationPoseComponent's skin matrices taken into the item's space at the rendered poses) and its
// morph weights, the pose's channel for the entity's path overriding the component's (RenderSnapshot.h's MeshDrawItem).
//
// Particles (M26, Docs/Decisions/0037-m26-contract.md decision 7): every effectively enabled ParticleEmitter whose entity
// holds its runtime ParticleStateComponent (a play scene's, from its first Particles phase on; edit scenes hold none, so
// their views draw no particles) gives one ParticleEmitterItem, even without a live particle, and its live particles in
// emission order: each at Alpha between its PreviousPosition and its Position (Position at Alpha 1), through the emitter's
// rendered world matrix for a Local emitter, its size its StartSize times SizeOverLife and its colour ColorOverLife at its
// age over its lifetime (ParticleSimulation.h's EvaluateParticleSize and EvaluateParticleColor). M33
// (Docs/Decisions/0048-m33-particle-look.md decision 3): each particle also takes its angle at Alpha between its last two
// steps', its world velocity (a Local emitter's through its rendered world matrix), its flipbook phase and its serial, and,
// with the entity's ParticleLook, its colour's RGB times the look's Intensity; the item takes the look's flipbook range,
// alignment, stretch and soft distance, and a Flat look's plane from the emitter's rendered world matrix (an emitter without
// a look takes ParticleEmitterItem's defaults, which draw as M26 did).
//
// Pure function of the scene's components and the request: no asset access, no clock, no randomness; vectors in canonical
// order (§5.1), so a deterministic scene gives an identical snapshot in every configuration. Only IEEE-exact operations and
// Core/DetMath (Scene is on the simulation path's lint scope, §4.12). Main thread, like the scene (§4.11).

namespace Engine {

	class ConstEntity;
	class Scene;
	struct UiInteractionState;

	// The camera a view renders through.
	enum class RenderCameraSource : uint8_t
	{
		Primary, // the scene's primary camera (FindPrimaryCamera): the game view
		Entity,  // the camera entity RenderExtractionRequest::CameraEntity (viewport.screenshot "camera")
		Explicit // RenderExtractionRequest::ExplicitCamera: the editor's scene view
	};

	// A camera that is not an entity: the editor's scene view (§8.13). Until the editor camera of M10 (viewport.camera) the
	// editor renders its scene view through the default below (Docs/Decisions/0012-m7-decisions.md decision 9): a perspective
	// camera at (0, 3, 10) looking at the origin with +Y up.
	struct ExplicitRenderCamera
	{
		glm::vec3 Position = glm::vec3(0.0f, 3.0f, 10.0f);
		glm::vec3 Target = glm::vec3(0.0f); // must differ from Position; the camera looks from Position at Target, +Y up
		RenderProjection Projection = RenderProjection::Perspective;
		float VerticalFov = 60.0f;      // degrees
		float OrthographicSize = 10.0f; // half height, metres
		float NearClip = 0.1f;
		float FarClip = 1000.0f;
		glm::vec3 ClearColor = glm::vec3(0.05f, 0.05f, 0.06f); // linear
	};

	struct RenderExtractionRequest
	{
		RenderCameraSource Camera = RenderCameraSource::Primary;
		UUID CameraEntity{};                   // RenderCameraSource::Entity
		ExplicitRenderCamera ExplicitCamera{}; // RenderCameraSource::Explicit
		uint32_t Width = 1;                    // the view's size in pixels (the projection's aspect ratio), both >= 1
		uint32_t Height = 1;
		float Alpha = 1.0f; // the interpolation factor in [0, 1] (FrameTime::Alpha; 1 for ManualClock frames and lockstep)
		RenderViewFlags Flags = RenderViewFlags::None;
		std::vector<UUID> SelectedEntities{};
		RenderAnnotations Annotations{};
		RenderQualitySettings Quality{}; // host copies ProjectSettings.Rendering, including screenshot and Runtime paths
		// M21 (C21.1, Docs/Decisions/0032-m21-contract.md decision 4): the project's SortingLayers.Layers, which every host
		// copies as it copies Quality; a sprite's sorting layer is its name's index here (SpriteItem::SortingLayer), 0 for a
		// name it lacks (SPRITE_UNKNOWN_SORTING_LAYER). Empty: every sprite is in layer 0.
		std::vector<std::string> SortingLayers{};
		// M22 (C22.2, Docs/Decisions/0033-m22-contract.md decision 25): the play session's UI interaction state, whose widget
		// states pick the UI's tints (AppendUiDrawItems, Scene/UiExtraction.h); a borrowed pointer valid for the call. The
		// session sets it for its game view and its views between ticks (PlaySession's RenderExtraction phase and
		// ExtractView); null (every other host, the edit scene) draws every widget Normal, or Disabled when not interactable.
		const UiInteractionState* UiInteraction = nullptr;
	};

	// The scene's primary camera: the first entity in canonical order (§5.1) that is effectively enabled (no
	// HierarchyDisabledTag) and has a CameraComponent with Primary set (§5.3; the validator reports SCENE_NO_PRIMARY_CAMERA
	// and SCENE_MULTIPLE_PRIMARY_CAMERAS). An invalid handle when there is none.
	[[nodiscard]] ConstEntity FindPrimaryCamera(const Scene& scene);

	// The rendered world matrix of `entity` (valid, asserted) at `alpha` in [0, 1] (asserted): the interpolation rule of the
	// file comment, over the entity's WorldTransformComponent as it is (callers refresh it first). Pure.
	[[nodiscard]] glm::mat4 ComputeRenderedWorldMatrix(ConstEntity entity, float alpha);

	// M22's landing (Docs/Decisions/0033-m22-contract.md decisions 3 and 24, "Landing"; ADR 0028's phase-2 ledger): the
	// output-stage settings render extraction gives a view through `camera` (a valid entity, asserted): its
	// PixelPerfectCamera's (ComputePixelPerfectView's Output, which the camera's position does not change) when the entity
	// holds a PixelPerfectCameraComponent, else the defaults (the scene resolution is the view's size). It is the one place
	// a camera's scene resolution is decided: ExtractRenderSnapshot sets RenderSnapshot::Output from it for the Primary and
	// Entity sources, and the play session's UiLayout and FrameUiLayout phases, UiInput's pointer mapping, ui.tree and the
	// editor's UI preview take the primary camera's through ComputeGameViewOutputMapping, so the layout the session
	// hit-tests is the one the game view draws. Pure.
	[[nodiscard]] RenderOutputSettings GetCameraOutputSettings(ConstEntity camera);

	// The output mapping of the scene's game view (RenderCameraSource::Primary) in a `viewSize` view (each >= 1, asserted):
	// ComputeRenderOutputMapping with the primary camera's GetCameraOutputSettings, or the defaults (the identity) without
	// a primary camera. Its SceneWidth x SceneHeight is the scene resolution the game view lays its UI out at. Pure.
	[[nodiscard]] RenderOutputMapping ComputeGameViewOutputMapping(const Scene& scene, glm::uvec2 viewSize);

	// The snapshot of one view of `scene` (§8.2):
	//   - Camera: per request.Camera. Primary without a primary camera is not an error: HasCamera is false and the renderer
	//     clears to the default clear colour. View = the inverse of the camera's rendered world matrix with its scale
	//     removed (cameras look down local -Z, +Y up); Projection = ComputeReverseZProjection for request.Width x Height
	//     from the camera's Projection, VerticalFov, OrthographicSize, NearClip and FarClip; ClearColor and ClearToSkybox
	//     from its Clear mode. The camera entity's own HierarchyDisabledTag does not stop an Entity request (a screenshot
	//     through a disabled camera is allowed), but does exclude it from Primary.
	//   - Meshes: every effectively enabled entity with a MeshRendererComponent whose Visible is set and whose Mesh is not
	//     null, at its rendered world matrix, with its Materials, CastShadows and ReceiveShadows.
	//   - Lights: every effectively enabled DirectionalLight, PointLight and SpotLight, at its rendered world pose (position
	//     from the matrix's translation, direction its normalized -Z axis), with the component's fields.
	//   - Environment and Post: the first effectively enabled EnvironmentComponent and PostProcessComponent in canonical
	//     order (both are unique per scene, §5.3), or their defaults when there is none.
	//   - Alpha: request.Alpha.
	//   - Texts (M8): every effectively enabled entity with a TextComponent whose Text is not empty, with its fields mapped to
	//     TextItem (Space, Alignment and the font handle as they are; World = the rendered world matrix, used by World
	//     texts). DebugDraw stays empty (its producers append: M11's colliders, M13's scripts) and DebugView is Lit (a
	//     screenshot sets it).
	//   - Sprites (M21, C21.1; Docs/Decisions/0032-m21-contract.md decision 4): every effectively enabled entity with a
	//     SpriteRendererComponent, at its rendered world matrix, with Sheet, Frame, Texture, Color, FlipX, FlipY,
	//     PixelsPerUnit, OrderInLayer and Lit as they are and SortingLayer resolved against request.SortingLayers, sorted
	//     stably by (SortingLayer, OrderInLayer) from canonical order (RenderSnapshot::Sprites); NormalMap as it is (C21.2,
	//     decision 21; stream L).
	//   - Tilemaps (M21, C21.2; decision 24, stream T1): every effectively enabled entity with a TilemapRendererComponent whose
	//     Tilemap is not null, at its rendered world matrix, with Tilemap, Color, PixelsPerUnit, OrderInLayer and Lit as they
	//     are and SortingLayer resolved as a sprite's, sorted stably by (SortingLayer, OrderInLayer) from canonical order; each
	//     one's DrawPosition is the number of sprites before it in the one order of sprites and tilemaps by (SortingLayer,
	//     OrderInLayer, canonical entity order) (RenderSnapshot::Tilemaps).
	//   - Lights2D (M21, C21.2; decision 22, stream L): every effectively enabled entity with a Light2DComponent, in canonical
	//     order, Position its rendered world matrix's translation, with Type, Color, Intensity, Radius, Falloff and Height as
	//     they are (RenderSnapshot::Lights2D).
	//   - ParticleEmitters and Particles (M26, Docs/Decisions/0037-m26-contract.md decision 7): every effectively enabled
	//     entity with a ParticleEmitterComponent and its runtime ParticleStateComponent, in canonical order, by the rule of
	//     the file comment's Particles paragraph; an edit scene holds no particle state, so its views draw none.
	//   - IrradianceVolumes and ReflectionProbes (M26, decision 16): every effectively enabled entity's IrradianceVolume and
	//     ReflectionProbe whose Data is set, in canonical order, at its rendered world matrix, with Size, Data, Intensity and
	//     BlendDistance (and a reflection probe's BoxProjection) as they are, in every view and every scene, edit scenes
	//     included, so baked lighting shows while editing; whether the data loads is the renderer's concern.
	//   - Terrains (M26, decision 22): every effectively enabled entity's Terrain whose Heightfield is set, in canonical order,
	//     at its rendered world matrix, with Heightfield, Size, HeightScale, SplatMap, ReceiveShadows and LodBias as they are
	//     and its first MaxTerrainLayers Layers (their maps' handles, Tint, Tiling, Roughness, Metallic); each is pickable, its
	//     PickId after the meshes' (the entity's mesh PickId when its MeshRenderer has one, else a PickTable entry appended
	//     after every mesh's, in the terrains' order), in every view and every scene, edit scenes included.
	//   - PixelPerfectCamera (M21, decision 6): for the Primary and Entity sources, when the camera entity holds a
	//     PixelPerfectCameraComponent, Output, the camera's position and its orthographic projection come from
	//     ComputePixelPerfectView (Scene/PixelPerfectCamera.h), at the reference resolution's aspect, whatever the Camera's
	//     own Projection and OrthographicSize, and the camera's viewport is the reference resolution (the scene resolution);
	//     Explicit cameras ignore it. Without one, Output keeps its defaults.
	// M9 additions: copy Flags, Quality, Annotations and SelectedEntities (sort/unique/filter unknown ids), assign mesh
	// PickIds/PickTable in canonical entity order before GPU culling, and copy all five directional shadow fields.
	// M22 (C22.1, Docs/Decisions/0033-m22-contract.md decision 7): Ui, every effectively enabled canvas laid out at the scene
	// resolution of the snapshot's Output (ComputeUiLayout, Scene/UiLayout.h; the view's size by default) and its visible
	// elements' draw items appended in draw order (AppendUiDrawItems, Scene/UiExtraction.h), for the Primary and Entity camera
	// sources (the game view and a camera's view; a view without a primary camera draws its UI too, as it draws its screen
	// texts). The editor's scene view (Explicit) draws no Screen canvas. C22.3 (decision 51, stream W): World canvases
	// (UiCanvasComponent::RenderMode) draw in every view that draws the world, the editor's scene view included
	// (AppendUiDrawItems' worldOnly for the Explicit source), at their render pose at request.Alpha, as the meshes (the
	// milestone review); Screen canvases still draw in the Primary and Entity views only.
	// Asset-dependent overlays are appended afterwards with AppendRenderAnnotations at the snapshot's Alpha.
	// Errors: InvalidArgument for a zero Width or Height or an Alpha that is not finite or outside [0, 1]; NotFound for an
	// Entity request whose CameraEntity names no entity of the scene, and InvalidArgument when that entity has no
	// CameraComponent (both name the id); InvalidArgument for an Explicit camera whose Target equals its Position.
	// Also rejects unknown flag bits, invalid annotation enums and invalid ShadowMapSize before copying
	// options. Explicit label IDs are copied, sorted/deduplicated and unknown IDs filtered; other label modes require
	// an empty LabelEntities vector. Returned values own selection and label storage.
	// M18 (R-ARENA; Docs/Decisions/0029-m18-contract.md decision 9): the snapshot is built over `resource` (not null,
	// asserted; RenderSnapshot(resource)), which must outlive it, so a host can extract into a per-frame arena such as a
	// std::pmr::monotonic_buffer_resource it releases once the frame is rendered; the result keeps the resource while it is
	// moved (a copy allocates from the default resource). The default resource is the global heap, as before. The call is a
	// CPU zone of the process profiler ("ExtractRenderSnapshot"), which the arena's measurement reads.
	[[nodiscard]] Result<RenderSnapshot> ExtractRenderSnapshot(const Scene& scene, const RenderExtractionRequest& request,
		std::pmr::memory_resource* resource = std::pmr::get_default_resource());

}
