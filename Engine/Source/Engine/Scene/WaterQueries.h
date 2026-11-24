#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/WaterWaves.h"
#include "Engine/Scene/Components/WaterComponent.h"

#include <glm/glm.hpp>

#include <optional>
#include <vector>

// Which water holds a point, and the surface there (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §9.12).
// The scripts (Water:SampleHeight and its siblings, Physics.SampleWater), the physics' buoyancy and water.sample sample the
// waters of a scene through these, so they agree point for point; render extraction draws the same surfaces
// (Physics/WaterWaves.h). Pure reads of the scene on the main thread; deterministic (double precision, Core/DetMath, waters in
// canonical order).
//
// The rule. A Rectangle's volume is its footprint (Size times its world X and Z scale, centred on its entity, turned by its
// yaw) from Depth below its base to Headroom above it, boundaries included. A point's water is the effectively enabled
// Rectangle whose volume holds it with the highest surface at the point's X and Z, ties by canonical order; else the scene's
// Ocean (the first effectively enabled one in canonical order); else none. So inside any Rectangle's volume the Ocean is
// neither sampled nor drawn: a cave lake below the sea's level whose Headroom reaches the cave's ceiling is dry above its own
// surface, and a pool on a cliff is its own water.

namespace Engine {

	class ConstEntity;
	class Scene;

	// One water, resolved at a world matrix: its entity, shape, surface (frame, waves and current at its WaveScale) and, for
	// a Rectangle, its volume: the world half extents of its footprint along its frame's X and Z axes (Size / 2 times the
	// lengths of the world matrix's X and Z columns) and its Depth and Headroom.
	struct ResolvedWater
	{
		UUID Entity{};
		WaterShape Shape = WaterShape::Ocean;
		WaterSurfaceDesc Surface{};
		glm::dvec2 HalfExtents = glm::dvec2(0.0);
		double Depth = 0.0;
		double Headroom = 0.0;
	};

	// The waters the rule chooses from: the effectively enabled Rectangles, in canonical order, and the scene's Ocean.
	struct SceneWaters
	{
		std::vector<ResolvedWater> Rectangles{};
		std::optional<ResolvedWater> Ocean{};
	};

	// The surface of a water under a point: the water's entity, the surface's world height, unit normal and velocity (the
	// surface particle's plus the water's current, m/s) at the point's X and Z, and Depth, the height minus the point's Y:
	// positive under the surface.
	struct SceneWaterSample
	{
		UUID Water{};
		double Height = 0.0;
		glm::dvec3 Normal = glm::dvec3(0.0, 1.0, 0.0);
		glm::dvec3 Velocity = glm::dvec3(0.0);
		double Depth = 0.0;
	};

	// `water` of entity `id` at world matrix `world`: its frame (MakeWaterFrame), its waves resolved at its WaveScale
	// (ResolveWaterSurface) and its volume.
	[[nodiscard]] ResolvedWater ResolveWater(const WaterComponent& water, const glm::mat4& world, UUID id);
	// The Water of `entity` (readable, so live or pending destruction, and holding one, asserted) at its current world matrix (TransformSystem::ComputeWorldMatrix).
	[[nodiscard]] ResolvedWater ResolveWater(ConstEntity entity);

	// The scene's waters at their current world matrices: every effectively enabled Rectangle in canonical order, and the
	// first effectively enabled Ocean. Entities pending destruction are left out.
	[[nodiscard]] SceneWaters CollectSceneWaters(const Scene& scene);

	// Whether the world point's X and Z lie in `water`'s footprint, boundaries included; always for an Ocean.
	[[nodiscard]] bool IsInWaterFootprint(const ResolvedWater& water, const glm::dvec3& point);
	// Whether `water`'s volume holds the world point: in its footprint and from Depth below its base to Headroom above it,
	// boundaries included; always for an Ocean.
	[[nodiscard]] bool IsInWaterVolume(const ResolvedWater& water, const glm::dvec3& point);

	// `water`'s own surface under the world point at `time` seconds of world time (EvaluateWaterSurface), masking aside;
	// nullopt for a Rectangle when the point lies outside its footprint.
	[[nodiscard]] std::optional<SceneWaterSample> SampleWater(const ResolvedWater& water, const glm::dvec3& point, double time);
	// Whether the world point lies under `water`'s own surface at `time`, masking aside: under the surface there, and for a
	// Rectangle within its volume.
	[[nodiscard]] bool IsUnderWater(const ResolvedWater& water, const glm::dvec3& point, double time);

	// The water that holds the world point at `time` by the rule above, or null; a pointer into `waters`.
	[[nodiscard]] const ResolvedWater* FindWaterAt(const SceneWaters& waters, const glm::dvec3& point, double time);
	// The surface of the water that holds the world point at `time`, by the rule above; nullopt where none does.
	[[nodiscard]] std::optional<SceneWaterSample> SampleSceneWater(const SceneWaters& waters, const glm::dvec3& point, double time);
	// The same for the scene's waters now (CollectSceneWaters).
	[[nodiscard]] std::optional<SceneWaterSample> SampleSceneWater(const Scene& scene, const glm::dvec3& point, double time);

}
