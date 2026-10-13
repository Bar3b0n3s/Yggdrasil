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
		std::vector<std::string> Entities{}; // required, non-empty EntityRefs from the shown scene (Play when running)
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
	};

	namespace Automation {

		// Presence via HasParam: absent members are unchanged; none present is a read. The result is the whole camera:
		// pose, projection, field of view, orthographic size and clip range. InvalidArgument (located at the member) for a
		// non-finite, coincident or (near-)vertical pose, a field of view outside (0, 180), a nonpositive orthographic size
		// or a clip range without 0 < near < far. Atomic full-camera validation before replacement.
		[[nodiscard]] Result<ViewportCameraResult> ViewportCamera(EditorMethodContext& context, const ViewportCameraParams& params);
		// Resolve every EntityRef before moving; NotFound invalid target, InvalidArgument empty list.
		// Bounds use entity.bounds' existing semantics (transform point for no renderable bounds), aspect uses last
		// nonzero scene-view size, default 16:9 headless; EditorCamera::FrameBounds adds 10% padding.
		[[nodiscard]] Result<ViewportFrameResult> ViewportFrame(EditorMethodContext& context, const ViewportFrameParams& params);
		// Presence-aware patch. UI state only, valid headless and without a renderer; returns every current option,
		// including the scene view's debug view and the Game panel's resolution (which takes effect in the image once
		// rendered). InvalidArgument (located) for an unknown debug view or a resolution that is not [0, 0] or two sides
		// in [1, 8192]; nothing changes then.
		[[nodiscard]] Result<ViewportSetOptionsResult> ViewportSetOptions(EditorMethodContext& context, const ViewportSetOptionsParams& params);

	}

	void RegisterViewportMethodTypes(TypeRegistry& registry);
	// None mutates project/scene, supports dry run, runs in a batch, launcher or Runtime. All work --renderer none.
	// All three are reached through engine_call (the §13.8 tool list is unchanged). ifRevision still applies.
	void RegisterViewportMethods(MethodRegistry& methods);

}
