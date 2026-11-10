#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>

// The 2D light model (M21, C21.2, R-2D-LIGHT; Docs/Decisions/0032-m21-contract.md decisions 20 to 22): the light the view's
// Light2Ds give one lit pixel, which the Lights2D pass accumulates on the GPU (Renderer/Lights2DPass.h,
// Passes/Lights2D.slang) and these functions compute on the CPU. They are the reference "Lights2D: a lit sprite matches its
// CPU reference" compares the GPU image with: a lit sprite's pixel is its texel's RGB times its Color's RGB times
// EvaluateLights2D at the point the pixel shows, its alpha unchanged, and the GPU image matches within the rounding of the
// half-float light accumulation.
//
// One light (EvaluateLight2D), for the point `position` of the light's plane that the pixel shows and the pixel's world
// normal (nullopt: the flat normal of a lit item without a normal map):
//   - Global: Color x Intensity, normal or not.
//   - Point: with d the distance in the plane from the light's Position (x, y) to `position`, R its Radius and F its Falloff:
//     0 when d >= R; otherwise the attenuation a = 1 when F = 0 or d <= R (1 - F), else the smoothstep t^2 (3 - 2 t) of
//     t = (R - d) / (R F); and the light is Color x Intensity x a x n, where n = 1 for the flat normal and, for a normal N,
//     max(0, N . L) with L the unit vector from (position, 0) to (Position.xy, Height).
// The view's light (EvaluateLights2D) is the sum, in their order, of the lights the view draws: the first
// MaxVisibleLights2D of the snapshot's Lights2D that reach the view. A Global light always reaches it; a Point light reaches
// it unless its square [Position.x - Radius, Position.x + Radius] x [Position.y - Radius, Position.y + Radius] in its plane,
// projected by the view (its depth held inside the clip range, as the pass draws it), lies wholly outside the view's clip
// rectangle. So lights off the view never count against the cap, and the CPU reference of a view sums exactly the lights
// its GPU image adds: the snapshot's lights that reach the view, in their order (Light2DReachesView states the test the
// renderer culls with).
// A light whose values are not valid (IsLight2DValid: non-finite, or outside Light2DComponent's registered ranges) gives no
// light and never reaches a view, so it neither draws nor counts against the cap.
// Pure; thread-safe; IEEE operations only (square roots, products and sums), so the result is identical in every
// configuration.

namespace Engine {

	// The most 2D lights one view draws: the first ones in the snapshot's order (canonical entity order) that reach the view
	// (the file comment); the renderer skips the rest and logs the first skip once per renderer.
	inline constexpr uint32_t MaxVisibleLights2D = 1024;

	// Light2DComponent's registered ranges (Scene/Registration/Rendering2DRegistration.cpp), which IsLight2DValid checks: the
	// intensity in [0, MaxLight2DIntensity], a Point light's Radius and Height in [MinLight2DDistance, MaxLight2DDistance], the
	// colour's channels and the falloff in [0, 1].
	inline constexpr float MaxLight2DIntensity = 1000.0f;
	inline constexpr float MinLight2DDistance = 0.001f;
	inline constexpr float MaxLight2DDistance = 100000.0f;

	// Whether `light` has the values the model takes: a finite Color with channels in [0, 1] and a finite Intensity in
	// [0, MaxLight2DIntensity]; a Point light also a finite Position, Radius and Height in [MinLight2DDistance,
	// MaxLight2DDistance] and Falloff in [0, 1] (a Global light ignores those).
	[[nodiscard]] bool IsLight2DValid(const Light2DItem& light);

	// Whether `light` reaches the view `viewProjection` (a view's projection times its view, reverse-Z clip space, §8.3)
	// shows (the file comment): a valid Global light always; a valid Point light unless the four corners of its square, each
	// projected by `viewProjection`, all lie outside one of the clip rectangle's planes (-w <= x, x <= w, -w <= y, y <= w) or
	// behind the eye (w < 0); depth is not tested, since the pass holds it inside the clip range. An invalid light never
	// reaches a view.
	[[nodiscard]] bool Light2DReachesView(const Light2DItem& light, const glm::mat4& viewProjection);

	// The light `light` gives the pixel showing `position` (world X and Y on the light's plane) with world normal `normal`
	// (unit; nullopt for the flat normal). See the file comment. Lights of a non-finite or out-of-range value give no light.
	[[nodiscard]] glm::vec3 EvaluateLight2D(const Light2DItem& light, glm::vec2 position, const std::optional<glm::vec3>& normal);

	// The sum of EvaluateLight2D over the first MaxVisibleLights2D of `lights`, in their order. `lights` are the lights the
	// view draws, culled to it by the caller as the renderer culls them (the file comment): the snapshot's lights that reach
	// the view, in the snapshot's order; the cap then selects the same lights as the renderer's.
	[[nodiscard]] glm::vec3 EvaluateLights2D(std::span<const Light2DItem> lights, glm::vec2 position, const std::optional<glm::vec3>& normal);

}
