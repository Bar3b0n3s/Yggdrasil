#pragma once

#include "EditorCore/Viewport/EditorViewportState.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Result.h"

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
	};
	struct ViewportCameraResult
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Target = glm::vec3(0.0f);
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
	};
	struct ViewportSetOptionsResult
	{
		bool Grid = true;
		bool Gizmos = true;
		bool Colliders = false;
		bool Icons = true;
		bool Wireframe = false;
	};

	namespace Automation {

		// Presence via HasParam: absent position/target is unchanged; neither present is a read.
		// InvalidArgument non-finite/coincident/vertical camera. Atomic full-camera validation before replacement.
		[[nodiscard]] Result<ViewportCameraResult> ViewportCamera(EditorMethodContext& context, const ViewportCameraParams& params);
		// Resolve every EntityRef before moving; NotFound invalid target, InvalidArgument empty list.
		// Bounds use entity.bounds' existing semantics (transform point for no renderable bounds), aspect uses last
		// nonzero scene-view size, default 16:9 headless; EditorCamera::FrameBounds adds 10% padding.
		[[nodiscard]] Result<ViewportFrameResult> ViewportFrame(EditorMethodContext& context, const ViewportFrameParams& params);
		// Presence-aware patch. UI state only, valid headless and without a renderer; returns every current option.
		[[nodiscard]] Result<ViewportSetOptionsResult> ViewportSetOptions(EditorMethodContext& context, const ViewportSetOptionsParams& params);

	}

	void RegisterViewportMethodTypes(TypeRegistry& registry);
	// None mutates project/scene, supports dry run, runs in a batch, launcher or Runtime. All work --renderer none.
	// All three are reached through engine_call (the §13.8 tool list is unchanged). ifRevision still applies.
	void RegisterViewportMethods(MethodRegistry& methods);

}
