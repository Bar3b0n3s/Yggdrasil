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
		// non-finite bounds, invalid camera or aspect <= 0. The clip range only widens (near = min(old, fitted), far =
		// max(old, fitted)): the bounds always fit inside it and nothing the old range showed is clipped.
		[[nodiscard]] static Result<ExplicitRenderCamera> FrameBounds(const ExplicitRenderCamera& camera,
			const glm::vec3& minimum, const glm::vec3& maximum, float aspect);

		// M21 (Docs/Decisions/0032-m21-contract.md decision 14, stream D): the 2D mode's camera for `camera`: orthographic,
		// looking down -Z at (Target.x, Target.y, 0) from Position (Target.x, Target.y, Make2DDistance), +Y up, with the
		// orthographic size kept for an orthographic camera and, for a perspective one, the half height its field of view
		// shows at its target distance; the clip range [0.1, 2 * Make2DDistance]. InvalidArgument for an invalid camera.
		[[nodiscard]] static Result<ExplicitRenderCamera> Make2D(const ExplicitRenderCamera& camera);
		// M21: one frame of 2D navigation of a camera Make2D made: pan (MMB, or LMB-drag with Pan) moves Position and Target
		// together in X and Y by the pointer delta at the camera's pixels per unit (`viewHeight` logical pixels, >= 1); the
		// wheel zooms the orthographic size by a factor of 1.2 per notch, kept in [0.01, 100000]; Orbit and Fly are ignored.
		// The camera stays a 2D camera. InvalidArgument for an invalid camera or input.
		[[nodiscard]] static Result<ExplicitRenderCamera> Navigate2D(const ExplicitRenderCamera& camera, const EditorCameraInput& input, float viewHeight);
		// The distance along +Z of Make2D's camera from the plane z = 0, in metres.
		static constexpr float Make2DDistance = 500.0f;
	};

}
