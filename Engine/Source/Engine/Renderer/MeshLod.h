#pragma once

#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

// The per-view choice of a mesh's level of detail (Architecture §8.3 "Mesh levels"; M46,
// Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3D): pure rules, which Prepare applies to every mesh
// item of every view (the main and editor views, captures, mirror views and probe captures), each by its own camera, and
// to every shadow caster. Stateless: no hysteresis and no history, so a capture is the same whatever came before.

namespace Engine {

	// A MeshRenderer's RenderDistance (Scene/Components/RenderDistanceComponent.h), as the selection reads it; the defaults
	// are a MeshRenderer's without the component.
	struct MeshLodParameters
	{
		float CullDistance = 0.0f; // metres from the camera; 0: none
		float LodBias = 1.0f;      // multiplies the screen size
		uint32_t ShadowLodOffset = 1;
		float FadeWidth = 0.1f; // the band's width, as a share of its switch size or of CullDistance
	};

	// An item's bounding sphere in world space: level 0's bounds' centre through `world`, and their half diagonal times the
	// world matrix's largest axis scale (so the sphere holds the transformed box).
	struct MeshLodSphere
	{
		glm::vec3 Center{ 0.0f };
		float Radius = 0.0f;
	};

	// One draw of an item: its level (0 is the mesh's own; k > 0 is MeshData::Lods[k - 1]) and its signed fade
	// (InstanceRecord::LodFade): 0 draws whole; t > 0 keeps the pixels whose dither threshold is below t (the coarser level
	// fading in); t < 0 keeps those whose threshold is at least -t (the finer level, or a level fading out). Two draws of
	// one item with fades t and -t split its pixels exactly (Common/LodFade.slang).
	struct MeshLodDraw
	{
		uint32_t Level = 0;
		float Fade = 0.0f;

		bool operator==(const MeshLodDraw&) const = default;
	};

	// What one view draws of one item.
	struct MeshLodSelection
	{
		std::array<MeshLodDraw, 2> Draws{}; // the first DrawCount are drawn, the finer level first
		uint32_t DrawCount = 0;             // 0 when culled, 2 inside a cross-fade band, else 1
		uint32_t Level = 0;                 // the view's level, which the casters offset (SelectShadowLod)
		bool CrossFade = false;             // two levels cross-fade (RenderStats::LodCrossFades)
		bool DistanceCulled = false;        // beyond CullDistance (RenderStats::DistanceCulledMeshes)
	};

	// The bounding sphere of `bounds` (an item's level 0 bounds in its own space) placed by `world`.
	[[nodiscard]] MeshLodSphere ComputeMeshLodSphere(const Aabb& bounds, const glm::mat4& world);

	// The item's screen size: its bounding sphere's projected diameter over the view's height, `radius` x `projectionScale`
	// / `distance` in perspective and `radius` x `projectionScale` in orthographic, where projectionScale is the view's
	// projection matrix's P[1][1] (1 / tan(fovY / 2), or 2 / the orthographic height) and distance the camera's distance to
	// the sphere's centre. A camera inside the sphere (distance at most radius) gets +infinity, so it draws level 0.
	[[nodiscard]] float ComputeMeshScreenSize(float radius, float distance, float projectionScale, bool orthographic);

	// The levels one view draws of an item whose mesh has the further levels' `switchSizes` (MeshLod::ScreenSize, strictly
	// decreasing; empty for a mesh without levels), at `screenSize` (ComputeMeshScreenSize) and `distance` from the camera:
	//   - Level is the finest whose switch size the biased size (screenSize x LodBias) still reaches: the number of switch
	//     sizes above it.
	//   - Inside the band of FadeWidth x s around a switch size s (from s (1 - FadeWidth / 2) to s (1 + FadeWidth / 2)), the
	//     levels on both sides draw: the coarser with t = (s (1 + FadeWidth / 2) - biased) / (s FadeWidth), in (0, 1), which
	//     rises as the item shrinks, and the finer with -t. The band of the switch below Level is checked before the one
	//     above it.
	//   - With a CullDistance, an item at or beyond it draws nothing (DistanceCulled), and one beyond
	//     CullDistance (1 - FadeWidth) draws Level alone with -t, t = (distance - CullDistance (1 - FadeWidth)) /
	//     (CullDistance FadeWidth), in (0, 1), fading out as it nears the distance; a level cross-fade does not apply there.
	// A FadeWidth of 0 switches and culls sharply. Pure.
	[[nodiscard]] MeshLodSelection SelectMeshLod(std::span<const float> switchSizes, float screenSize, float distance, const MeshLodParameters& parameters);

	// The level a shadow caster draws for a view whose level of the item is `viewLevel` (MeshLodSelection::Level): that
	// level plus ShadowLodOffset, clamped to the mesh's last level (switchSizes.size()), never faded; nullopt (it casts
	// nothing) when the item is at or beyond its CullDistance from the view's camera. Pure.
	[[nodiscard]] std::optional<uint32_t> SelectShadowLod(std::span<const float> switchSizes, uint32_t viewLevel, float distance,
		const MeshLodParameters& parameters);

	// The selection parameters of a snapshot item's RenderDistance (MeshDrawItem::Distance).
	[[nodiscard]] MeshLodParameters ToMeshLodParameters(const MeshDrawDistance& distance);

	// What `camera`'s view draws of an item placed by `world`, whose mesh's level 0 bounds are `bounds` and whose
	// further levels switch at `switchSizes`: ComputeMeshLodSphere, then ComputeMeshScreenSize with the camera's distance to
	// the sphere's centre and |Projection[1][1]|, then SelectMeshLod with `distance`'s parameters. Pure.
	[[nodiscard]] MeshLodSelection SelectViewMeshLod(std::span<const float> switchSizes, const Aabb& bounds, const glm::mat4& world,
		const MeshDrawDistance& distance, const CameraData& camera);

	// The level a shadow caster draws for `camera`'s view (SelectViewMeshLod's Level plus ShadowLodOffset, clamped), or
	// nullopt when the item is at or beyond its CullDistance from the camera. Pure.
	[[nodiscard]] std::optional<uint32_t> SelectViewShadowLod(std::span<const float> switchSizes, const Aabb& bounds, const glm::mat4& world,
		const MeshDrawDistance& distance, const CameraData& camera);

}
