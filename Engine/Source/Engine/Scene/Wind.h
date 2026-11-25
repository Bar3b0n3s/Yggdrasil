#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

// The scene's wind (Architecture §8.3 "Wind"; M46, Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision
// 3F): sampling a WindComponent at a world position and time, and finding a scene's Wind. Wind:Sample (scripts, on the
// simulation path) and the renderer's wind constants read these, so they use only IEEE-exact operations, floor and sqrt,
// in double precision: every configuration gives the same bits. They are the interface later readers take (water and
// clouds once a later row couples them, particles in M61).

namespace Engine {

	class Scene;
	struct WindComponent;

	// The wind's horizontal unit direction, (x, 0, z): Direction's X and Z normalized. A Direction without a horizontal part
	// (which the component's Validate rule refuses on every write path) gives (1, 0, 0).
	[[nodiscard]] glm::dvec3 GetWindDirection(const WindComponent& wind);

	// The gusts' wavelength in metres, Strength / GustFrequency: 0 when either is 0 (gusts that stand still, or no wind to
	// carry them), so they do not vary across positions.
	[[nodiscard]] double GetWindGustWavelength(const WindComponent& wind);

	// The gust wave: a smoothed triangle wave of period 1 in [0, 1], 0 at every whole x and 1 halfway between, smoothed by
	// the cubic u * u * (3 - 2u) of the triangle u = |2 frac(x) - 1| mirrored (so it eases in and out of each peak).
	[[nodiscard]] double WindGustWave(double x);

	// The wind's velocity in m/s at the world point `position` at the world time `time` in seconds (the world's clock in
	// play; Wind:Sample's default):
	//   - along the direction d, the steady speed plus the gusts: Strength + GustStrength * G(phase + Turbulence * G(c / 2)),
	//     where G is WindGustWave, phase = GustFrequency * time - a / wavelength (gusts travel with the wind), a = dot(xz, d)
	//     and c = dot(xz, p) the point's coordinates along d and across it (p = (-d.z, 0, d.x)), both 0 when the wavelength
	//     is 0;
	//   - across it, the turbulence's sideways part: Turbulence * GustStrength * (2 G(phase / 2 + c / wavelength + 1/4) - 1).
	// No vertical part. Pure; the same bits in every configuration.
	[[nodiscard]] glm::dvec3 SampleWind(const WindComponent& wind, const glm::dvec3& position, double time);

	// The scene's effectively enabled Wind (no HierarchyDisabledTag), nullptr without one. Wind is UniquePerScene, so a scene
	// holds at most one; with several (a hand-edited file that SCENE_DUPLICATE_UNIQUE_COMPONENT reports), the one on the
	// entity of the lowest UUID, so the choice is the same on every run.
	[[nodiscard]] const WindComponent* FindSceneWind(const Scene& scene);

}
