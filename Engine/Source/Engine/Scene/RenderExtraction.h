#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstdint>
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
// Pure function of the scene's components and the request: no asset access, no clock, no randomness; vectors in canonical
// order (§5.1), so a deterministic scene gives an identical snapshot in every configuration. Only IEEE-exact operations and
// Core/DetMath (Scene is on the simulation path's lint scope, §4.12). Main thread, like the scene (§4.11).

namespace Engine {

	class ConstEntity;
	class Scene;

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
	};

	// The scene's primary camera: the first entity in canonical order (§5.1) that is effectively enabled (no
	// HierarchyDisabledTag) and has a CameraComponent with Primary set (§5.3; the validator reports SCENE_NO_PRIMARY_CAMERA
	// and SCENE_MULTIPLE_PRIMARY_CAMERAS). An invalid handle when there is none.
	[[nodiscard]] ConstEntity FindPrimaryCamera(const Scene& scene);

	// The rendered world matrix of `entity` (valid, asserted) at `alpha` in [0, 1] (asserted): the interpolation rule of the
	// file comment, over the entity's WorldTransformComponent as it is (callers refresh it first). Pure.
	[[nodiscard]] glm::mat4 ComputeRenderedWorldMatrix(ConstEntity entity, float alpha);

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
	// M9 additions: copy Flags, Quality, Annotations and SelectedEntities (sort/unique/filter unknown ids), assign mesh
	// PickIds/PickTable in canonical entity order before GPU culling, and copy all five directional shadow fields.
	// The default request preserves M8 behavior during contract scaffolding; nondefault M9 options currently return
	// Unsupported. The integrator replaces that marked guard with the behavior above, preserving the existing extraction.
	// Asset-dependent overlays are appended afterwards with AppendRenderAnnotations at the snapshot's Alpha.
	// Errors: InvalidArgument for a zero Width or Height or an Alpha that is not finite or outside [0, 1]; NotFound for an
	// Entity request whose CameraEntity names no entity of the scene, and InvalidArgument when that entity has no
	// CameraComponent (both name the id); InvalidArgument for an Explicit camera whose Target equals its Position.
	// M9 implementation also rejects unknown flag bits, invalid annotation enums and invalid ShadowMapSize before copying
	// options. Explicit label IDs are copied, sorted/deduplicated and unknown IDs filtered; other label modes require
	// an empty LabelEntities vector. Returned values own selection and label storage.
	[[nodiscard]] Result<RenderSnapshot> ExtractRenderSnapshot(const Scene& scene, const RenderExtractionRequest& request);

}
