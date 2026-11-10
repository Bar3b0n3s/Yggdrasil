#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

	// Registry enum "Light2DType": what a Light2D lights.
	enum class Light2DType : uint8_t
	{
		Point, // a circle of light around the entity, fading to nothing at Radius
		Global // the same light at every lit pixel of the view
	};

	// Registry name "Light2D" (Architecture §5.3; M21, C21.2, R-2D-LIGHT; Docs/Decisions/0032-m21-contract.md decisions 20 to
	// 22): a 2D light, drawn by the Lights2D pass (Renderer/Light2DShading.h states the formula) into the view's light
	// accumulation at the scene resolution (the PixelPerfectCamera's reference resolution), which only lit sprites and lit
	// tilemaps sample (SpriteRendererComponent::Lit, TilemapRendererComponent::Lit); unlit sprites, meshes and texts ignore
	// every 2D light. Requires Transform.
	//   - A Point light sits at its entity's rendered world position and lights the lit pixels within Radius of it, measured
	//     in the light's plane (z = the entity's world Z): full Color times Intensity inside Radius x (1 - Falloff), fading
	//     smoothly (a smoothstep) to nothing at Radius; Falloff 0 is a hard edge. Height is how far above the lit plane the
	//     light sits for normal-mapped sprites (SpriteRendererComponent::NormalMap), which take the cosine between their
	//     normal and the direction to the light; a lit pixel without a normal map takes the light whole.
	//   - A Global light gives Color times Intensity to every lit pixel of the view, normal-mapped or not, wherever its entity
	//     is (ambient light).
	//   - Lights add up; a lit pixel without any light is black. Radius and Height are world units, unaffected by the entity's
	//     scale and rotation. A view draws at most MaxVisibleLights2D lights (Renderer/Light2DShading.h).
	//   - No 2D shadows: a light passes through every sprite, tile and collider (a kept limit, Docs/Backlog.md §6).
	// Rendering only: a Light2D changes no simulation state, so it adds nothing to the play session's state hash beyond its
	// own component data in the scene's canonical text.
	struct Light2DComponent
	{
		Light2DType Type = Light2DType::Point;
		glm::vec3 Color = glm::vec3(1.0f); // linear
		float Intensity = 1.0f;
		float Radius = 5.0f;  // Point: world units
		float Falloff = 0.5f; // Point: the fraction of Radius, from its edge inward, over which the light fades out
		float Height = 1.0f;  // Point: world units above the lit plane, for normal-mapped sprites
	};

}
