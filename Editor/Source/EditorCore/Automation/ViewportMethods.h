#pragma once

#include "EditorCore/Viewport/EditorViewportState.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Result.h"
#include "Engine/Scene/Components/CameraComponent.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	struct ViewportCameraParams
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Target = glm::vec3(0.0f);
		ProjectionType Projection = ProjectionType::Perspective; // the Overlays popup's Orthographic toggle
		float VerticalFov = 60.0f;                               // degrees, perspective
		float OrthographicSize = 10.0f;                          // half height in metres, orthographic
		float NearClip = 0.1f;                                   // metres
		float FarClip = 1000.0f;                                 // metres; the culling distance of both projections
	};
	struct ViewportCameraResult
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Target = glm::vec3(0.0f);
		ProjectionType Projection = ProjectionType::Perspective;
		float VerticalFov = 60.0f;
		float OrthographicSize = 10.0f;
		float NearClip = 0.1f;
		float FarClip = 1000.0f;
	};
	struct ViewportFrameParams
	{
		// required, non-empty EntityRefs from the scene the scene view shows: the play scene while the editor shows it
		// (EditorContext::ShowsPlayScene), otherwise the active scene's edit scene (M20, Docs/Decisions/0031-m20-contract.md
		// decisions 13 and 23)
		std::vector<std::string> Entities{};
	};
	struct ViewportFrameResult
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Target = glm::vec3(0.0f);
		std::vector<EntitySummary> Entities{};
	};
	struct ViewportSetOptionsParams
	{
		bool Grid = true;
		bool Gizmos = true;
		bool Colliders = false;
		bool Icons = true;
		bool Wireframe = false;
		std::string DebugView{};                // the scene view's debug-view dropdown: any RenderDebugView name, any ASCII case
		std::vector<uint32_t> GameResolution{}; // the Game panel's resolution: [width, height], [0, 0] for free
		// M21 (Docs/Decisions/0032-m21-contract.md decision 14): the scene view's 2D mode, the toolbar's 2D toggle. Turning
		// it on makes the scene camera EditorCamera::Make2D's; turning it off keeps the camera.
		bool Mode2D = false;
		bool UiPreview = false; // M22 (decision 33): EditorViewportOptions::UiPreview
		// M30 (Docs/Decisions/0045-m30-navigation.md decision 12): EditorViewportOptions::NavMesh
		bool NavMesh = false;
	};
	struct ViewportSetOptionsResult
	{
		bool Grid = true;
		bool Gizmos = true;
		bool Colliders = false;
		bool Icons = true;
		bool Wireframe = false;
		std::string DebugView{};                // canonical RenderDebugView name
		std::vector<uint32_t> GameResolution{}; // [width, height]; [0, 0] is free resolution
		bool Mode2D = false;                    // M21: the scene view's 2D mode
		bool UiPreview = false;                 // M22 (decision 33): EditorViewportOptions::UiPreview
		bool NavMesh = false;                   // M30 (decision 12): EditorViewportOptions::NavMesh
	};

	namespace Automation {

		// Presence via HasParam: absent members are unchanged; none present is a read. The result is the whole camera:
		// pose, projection, field of view, orthographic size and clip range. InvalidArgument (located at the member) for a
		// non-finite, coincident or (near-)vertical pose, a field of view outside (0, 180), a nonpositive orthographic size
		// or a clip range without 0 < near < far. Atomic full-camera validation before replacement.
		[[nodiscard]] Result<ViewportCameraResult> ViewportCamera(EditorMethodContext& context, const ViewportCameraParams& params);
		// Resolve every EntityRef before moving; NotFound invalid target, InvalidArgument empty list.
		// Bounds use entity.bounds' existing semantics (transform point for no renderable bounds), aspect uses last
		// nonzero scene-view size, default 16:9 headless; EditorCamera::FrameBounds adds 10% padding. In the 2D mode (M21)
		// the framed camera stays a 2D camera (EditorCamera::Make2D of the framed one: its target's X and Y, its size).
		[[nodiscard]] Result<ViewportFrameResult> ViewportFrame(EditorMethodContext& context, const ViewportFrameParams& params);
		// Presence-aware patch. UI state only, valid headless and without a renderer; returns every current option,
		// including the scene view's debug view and the Game panel's resolution (which takes effect in the image once
		// rendered). InvalidArgument (located) for an unknown debug view or a resolution that is not [0, 0] or two sides
		// in [1, 8192]; nothing changes then. M21: mode2D true, when the mode was off, also makes the scene camera
		// EditorCamera::Make2D's (orthographic, looking down -Z at its target's X and Y); false keeps the camera.
		[[nodiscard]] Result<ViewportSetOptionsResult> ViewportSetOptions(EditorMethodContext& context, const ViewportSetOptionsParams& params);

	}

	void RegisterViewportMethodTypes(TypeRegistry& registry);
	// None mutates project/scene, supports dry run, runs in a batch, launcher or Runtime. All work --renderer none.
	// All three are reached through engine_call (the §13.8 tool list is unchanged). ifRevision still applies.
	void RegisterViewportMethods(MethodRegistry& methods);

}
