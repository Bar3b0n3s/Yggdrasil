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
// (Docs/Decisions/0013-m8-decisions.md decision 7).

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
		// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): the submesh's index in its mesh, so the draws
		// of one submesh of one mesh and material are adjacent and batch into one instanced draw (GroupMeshInstances). The
		// Prepare stage fills it with stream A; until then every key's is 0, which keeps the order of M18.
		uint32_t Submesh = 0;
		uint32_t DrawIndex = 0; // the draw's position before sorting (snapshot order)

		bool operator==(const OpaqueSortKey&) const = default;
	};

	// Sorts by (Pipeline, Material, Mesh, Submesh, DrawIndex), so equal keys keep snapshot order. Pure.
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

	// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): what the draws of one instanced draw share. Two
	// submesh draws with equal keys draw the same vertices and indices with the same pipeline variant and material set, and
	// differ only in their instance records (Shared/InstanceRecord.h).
	struct MeshInstanceKey
	{
		uint32_t Pipeline = 0; // the scene pipeline variant's index (alpha mode, cull mode and the mirroring it implies)
		AssetHandle Material{};
		AssetHandle Mesh{};
		uint32_t Submesh = 0;
		// The vertices the draw reads: 0 for the mesh asset's own vertex buffers (every draw until C23.3); C23.3's Deform
		// stage gives each deformed draw (a skinned or morphed entity's, whose deformed vertices are its own) a value of its
		// own, so two deformed draws of one mesh and material never share an instanced draw.
		uint32_t VertexStream = 0;

		bool operator==(const MeshInstanceKey&) const = default;
	};

	// One instanced draw: Count draws from position First of a pass's draw order.
	struct MeshInstanceBatch
	{
		uint32_t First = 0;
		uint32_t Count = 0;

		bool operator==(const MeshInstanceBatch&) const = default;
	};

	// Splits a pass's draws, given by their keys in the order the pass records them (the opaque order of SortOpaqueDraws,
	// whose key gains the submesh before DrawIndex, or the transparent order of SortTransparentDraws), into instanced draws:
	// maximal runs of adjacent draws with equal keys, in order, together covering every draw once. Recording the runs as
	// instanced draws, each instance in its run's order, draws exactly what one draw per element would, in the same order,
	// which is why an instanced render equals the per-draw render byte for byte. Empty keys give no batch. Pure.
	[[nodiscard]] std::vector<MeshInstanceBatch> GroupMeshInstances(std::span<const MeshInstanceKey> keys);

}
