#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Scene/RenderExtraction.h"

#include <glm/glm.hpp>

namespace Engine {

	// One frame of navigation after the UI has arbitrated focus/capture (no ImGui or input-system dependency).
	// Orbit: Alt+LMB; pan: MMB; fly: RMB with WASD/QE. Pointer delta is logical pixels, wheel positive zooms in.
	struct EditorCameraInput
	{
		glm::vec2 PointerDelta = glm::vec2(0.0f);
		glm::vec3 FlyAxis = glm::vec3(0.0f); // right, up, forward; each [-1, 1]
		float Wheel = 0.0f;
		float DeltaSeconds = 0.0f;
		float MoveSpeed = 5.0f;
		bool Orbit = false;
		bool Pan = false;
		bool Fly = false;
	};

	// CPU camera mathematics shared by UI and viewport.camera/frame. Pure, owns nothing, no wall clock or scene access.
	// Every function returns a candidate and leaves its input unchanged on error; the caller applies via SetCamera.
	class EditorCamera
	{
	public:
		EditorCamera() = delete;
		// Errors: InvalidArgument for an invalid camera/input, non-finite/bad delta or fly axis outside [-1,1].
		// Orbit keeps target fixed and avoids the +Y poles; pan translates both; fly preserves viewing direction.
		[[nodiscard]] static Result<ExplicitRenderCamera> Navigate(const ExplicitRenderCamera& camera, const EditorCameraInput& input);
		// Fit all corners of the world-space bounds with 10% padding at the supplied aspect, retaining view direction.
		// A zero-size bound gets a 0.5 m radius; adjusts ortho size in orthographic mode. InvalidArgument for reversed/
		// non-finite bounds, invalid camera or aspect <= 0. Never clips the result against the old near/far interval.
		[[nodiscard]] static Result<ExplicitRenderCamera> FrameBounds(const ExplicitRenderCamera& camera,
			const glm::vec3& minimum, const glm::vec3& maximum, float aspect);
	};

}
