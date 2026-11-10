#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/Components/PixelPerfectCameraComponent.h"

#include <glm/glm.hpp>

// The pixel-perfect camera's view (M21, C21.1, R-2D-CAM; Docs/Decisions/0032-m21-contract.md decision 6): what render
// extraction makes of a camera entity's PixelPerfectCameraComponent for the Primary and Entity camera sources (never the
// editor's Explicit scene-view camera). It drives M18's output stage through RenderSnapshot::Output and changes no
// output-stage code (ADR 0028's phase-2 ledger). Scripts' Camera:ScreenToWorldRay and WorldToScreen and the game view's
// pointer mapping use the same view, through the output mapping, so a pointer's view pixel maps to the scene pixel it shows.
//
// Frozen by the M21 contract (C21.1); stream C implements it.

namespace Engine {

	struct PixelPerfectView
	{
		// SceneWidth x SceneHeight = ReferenceWidth x ReferenceHeight, PixelArt set, LetterboxColor the component's.
		RenderOutputSettings Output{};
		// The orthographic half height in world units: ReferenceHeight / (2 * PixelsPerUnit).
		float OrthographicSize = 0.0f;
		// The camera's world position snapped so the view's left and bottom edges lie on the pixel grid: with
		// halfWidth = ReferenceWidth / (2 * PixelsPerUnit), x = round((x - halfWidth) * PixelsPerUnit) / PixelsPerUnit +
		// halfWidth, and y likewise with OrthographicSize (round half away from zero); z unchanged.
		glm::vec3 Position = glm::vec3(0.0f);

		bool operator==(const PixelPerfectView&) const = default;
	};

	// The view of `camera` (its fields in their registry ranges, asserted) whose entity is at the rendered world position
	// `position` (finite). Pure, deterministic (IEEE-exact operations only).
	[[nodiscard]] PixelPerfectView ComputePixelPerfectView(const PixelPerfectCameraComponent& camera, const glm::vec3& position);

}
