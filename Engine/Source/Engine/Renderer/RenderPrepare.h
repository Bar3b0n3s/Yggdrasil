#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Shared/InstanceRecord.h"

#include <glm/glm.hpp>

#include <compare>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The CPU part of §8.3 pass 1 (Prepare) as pure functions, so the rules are unit-tested without a GPU (§15.2 "Renderer
// (CPU): ... light culling; transparent sort ties"): frustum culling of submesh bounds, the light list with its limit, the
// two draw orders and (M23) their split into instanced draws. SceneRenderer calls them on every Render. Deterministic: no
// unordered containers, ties broken by stable keys, only the snapshot's data. Thread-safe (no state). Frozen by the M8
// contract (Docs/Decisions/0013-m8-decisions.md decision 7).

namespace Engine {

	// The code of §13.7's light limit, defined here because the renderer logs it (Warning, once per renderer, when a view
	// drops lights) and EditorCore's ProjectValidator reports it under the same spelling (a scene with more effectively
	// enabled lights than the project's Rendering.MaxShadedLights; EditorCore/Project/ProjectValidator.h).
	inline constexpr std::string_view RenderLightLimitExceededCode = "RENDER_LIGHT_LIMIT_EXCEEDED";

	// Whether `bounds` (local space) transformed by `world` lies entirely outside the view of `camera` (CameraData::View and
	// Projection): all eight corners outside one clip plane (-w <= x, y <= w; 0 <= z <= w, reverse-Z), or every corner
	// farther than FarClip along the view direction (the culling distance of both projections; a perspective far plane is
	// at infinity). Empty bounds are never outside.
	[[nodiscard]] bool IsOutsideView(const Aabb& bounds, const glm::mat4& world, const CameraData& camera);

	// M29 (Docs/Decisions/0044-m29-light-limits.md decision 3): a light's identity from one render of a view to the next, for
	// the hysteresis at a cap: its entity and its type. A light without an entity (a snapshot built by hand) has none.
	struct LightKey
	{
		UUID Entity{};
		RenderLightType Type = RenderLightType::Directional;

		auto operator<=>(const LightKey&) const = default;
	};

	// M29 (decision 3): where more candidates than places compete at a cap, a light that held such a place in the view's
	// previous render ranks with its importance times this factor, so a newcomer must be a quarter more important to take
	// its place. The chosen lights are then ordered by their plain importance.
	inline constexpr float LightHysteresis = 1.25f;

	// The light list of one view.
	struct LightCullResult
	{
		// Indices into the snapshot's Lights of the lights to shade, at most the cap: every directional light in snapshot
		// order, then the visible point and spot lights by descending importance, ties by entity UUID then snapshot index.
		std::vector<uint32_t> Visible{};
		// M29 (decision 3): one per Visible entry, the light's importance (below); infinity for a directional light. The shadow
		// planners' hysteresis (PointShadowMaps.h) ranks with it.
		std::vector<float> Importance{};
		// Lights not shaded besides the dropped ones: point and spot lights outside the view, and lights of any type
		// (directional included) whose radiance or range is zero or not finite.
		uint32_t Culled = 0;
		uint32_t Dropped = 0; // visible lights beyond the cap (the least important first, after the hysteresis)
		// M29 (decision 3): the keys of the shaded point and spot lights that have an entity, sorted: the next render's
		// `previousShaded`.
		std::vector<LightKey> Shaded{};
	};

	// Culls `lights` for `camera`: a directional light is always visible when its radiance (Color * Intensity) is non-zero
	// and finite; a point light when its Range sphere is not outside the view (as IsOutsideView, against the sphere); a spot
	// light when the bounding sphere of its cone is not. A light with a non-finite value is culled. Importance is Intensity *
	// max(Color) / max(distance from the camera to the light's sphere, 1)². M29 (decisions 1 and 3): at most
	// `maxShadedLights` (IsValidMaxShadedLights, asserted) lights are shaded; when more point and spot lights are visible than
	// the places the directional lights leave, those whose key is in `previousShaded` (sorted; the previous render's
	// LightCullResult::Shaded, empty after a reset) rank with their importance times LightHysteresis, and the chosen ones are
	// ordered by their plain importance. When every visible light fits, the hysteresis changes nothing. Pure.
	[[nodiscard]] LightCullResult CullLights(std::span<const LightData> lights, const CameraData& camera, uint32_t maxShadedLights = DefaultMaxShadedLights,
		std::span<const LightKey> previousShaded = {});

	// A draw's place in the opaque order (§8.3 pass 1: "opaque sorted by pipeline -> material -> mesh").
	struct OpaqueSortKey
	{
		uint32_t Pipeline = 0; // the scene pipeline variant's index
		AssetHandle Material{};
		AssetHandle Mesh{};
		// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): the submesh's index in its mesh, so the draws
		// of one submesh of one mesh and material are adjacent and batch into one instanced draw (GroupMeshInstances). The
		// Prepare stage fills it from the draw's submesh.
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

	// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decisions 2 and 8; M23's milestone review): the instance record
	// (Shared/InstanceRecord.h) of a submesh draw of `item`, the one rule by which every pass that draws meshes writes its
	// records: the Prepare stage's camera draws, ShadowPass::PrepareCasters' casters and SelectionPass::PrepareDraws' mask
	// draws. World and EntityId are the item's World and PickId; PreviousWorld is the item's PreviousWorld, or its World
	// when it has none (no motion: every item until M26 fills it for TAA's motion vectors, plan §4.1); Flags is
	// DrawFlagMirrored for a World whose upper 3 x 3 determinant is negative, combined with `flags` (the camera draws'
	// DrawFlagReceiveShadows). Pure.
	// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 4): the record also carries the item's Parameters
	// (MeshDrawItem::Parameters) and DrawFlagMaterialParameters when they are not neutral; neutral parameters leave the
	// record's parameter members at their neutral defaults and the flag clear, so such an item draws as before M31.
	[[nodiscard]] InstanceRecord MakeMeshInstanceRecord(const MeshDrawItem& item, uint32_t flags = 0);

	// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 5): the alpha mode whose pipeline variant draws a submesh
	// of an item with `parameters` and a material of `material`'s alpha mode, in the camera's passes (the prepass and the
	// forward passes) or, with `shadowCaster`, among the shadow casters. A Blend or Mask material keeps its mode; an Opaque
	// material takes the Mask variants (whose fragment shaders clip by the parameters) while the item dissolves
	// (DissolveAmount > 0) and, in the camera's passes only, while it fades (Opacity < 1), since a faded item keeps its
	// shadow. Pure.
	[[nodiscard]] AlphaMode SelectMeshAlphaMode(AlphaMode material, const MeshMaterialParameters& parameters, bool shadowCaster);

}
