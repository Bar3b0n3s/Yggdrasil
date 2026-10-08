#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The CPU part of §8.3 pass 1 (Prepare) as pure functions, so the rules are unit-tested without a GPU (§15.2 "Renderer
// (CPU): ... light culling; transparent sort ties"): frustum culling of submesh bounds, the light list with its limit, and
// the two draw orders. SceneRenderer calls them on every Render. Deterministic: no unordered containers, ties broken by
// stable keys, only the snapshot's data. Thread-safe (no state). Frozen by the M8 contract
// (Docs/Decisions/0013-m8-decisions.md decision 7); stream A implements it.

namespace Engine {

	// The most lights one view shades (§8.2, §8.3 pass 1: "light list <= 256"); more visible lights is
	// RENDER_LIGHT_LIMIT_EXCEEDED (ProjectValidator's warning, and the renderer's warning logged once per renderer).
	inline constexpr uint32_t MaxVisibleLights = 256;

	// The code of §13.7's "more than 256 visible lights", defined here because the renderer logs it (Warning, once per
	// renderer, when a view drops lights) and EditorCore's ProjectValidator reports it under the same spelling (a scene
	// with more than MaxVisibleLights effectively enabled lights; EditorCore/Project/ProjectValidator.h).
	inline constexpr std::string_view RenderLightLimitExceededCode = "RENDER_LIGHT_LIMIT_EXCEEDED";

	// Whether `bounds` (local space) transformed by `world` lies entirely outside the view of `camera` (CameraData::View and
	// Projection): all eight corners outside one clip plane (-w <= x, y <= w; 0 <= z <= w, reverse-Z), or every corner
	// farther than FarClip along the view direction (the culling distance of both projections; a perspective far plane is
	// at infinity). Empty bounds are never outside.
	[[nodiscard]] bool IsOutsideView(const Aabb& bounds, const glm::mat4& world, const CameraData& camera);

	// The light list of one view.
	struct LightCullResult
	{
		// Indices into the snapshot's Lights of the lights to shade, at most MaxVisibleLights: every directional light in
		// snapshot order, then the visible point and spot lights by descending importance, ties by entity UUID.
		std::vector<uint32_t> Visible{};
		// Lights not shaded besides the dropped ones: point and spot lights outside the view, and lights of any type
		// (directional included) whose radiance or range is zero or not finite.
		uint32_t Culled = 0;
		uint32_t Dropped = 0; // visible lights beyond MaxVisibleLights (lowest importance first)
	};

	// Culls `lights` for `camera`: a directional light is always visible when its radiance (Color * Intensity) is non-zero
	// and finite; a point light when its Range sphere is not outside the view (as IsOutsideView, against the sphere); a spot
	// light when the bounding sphere of its cone is not. A light with a non-finite value is culled. Importance is Intensity *
	// max(Color) / max(distance from the camera to the light's sphere, 1)². Pure.
	[[nodiscard]] LightCullResult CullLights(std::span<const LightData> lights, const CameraData& camera);

	// A draw's place in the opaque order (§8.3 pass 1: "opaque sorted by pipeline -> material -> mesh").
	struct OpaqueSortKey
	{
		uint32_t Pipeline = 0; // the scene pipeline variant's index
		AssetHandle Material{};
		AssetHandle Mesh{};
		uint32_t DrawIndex = 0; // the draw's position before sorting (snapshot order)

		bool operator==(const OpaqueSortKey&) const = default;
	};

	// Sorts by (Pipeline, Material, Mesh, DrawIndex), so equal keys keep snapshot order. Pure.
	void SortOpaqueDraws(std::span<OpaqueSortKey> keys);

	// A draw's place in the transparent order (§8.3 pass 1: "transparent back-to-front, ties by UUID").
	struct TransparentSortKey
	{
		float ViewDepth = 0.0f; // the distance of the submesh bounds' centre along the view direction (larger is farther)
		UUID Entity{};
		uint32_t Submesh = 0;
		uint32_t DrawIndex = 0;

		bool operator==(const TransparentSortKey&) const = default;
	};

	// Sorts back to front: descending ViewDepth, then ascending Entity, then ascending Submesh, then ascending DrawIndex.
	// A non-finite ViewDepth sorts as the farthest. Pure.
	void SortTransparentDraws(std::span<TransparentSortKey> keys);

}
