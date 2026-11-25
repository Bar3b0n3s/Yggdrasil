#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <compare>
#include <cstdint>

// The CPU rules of the renderer's static path (M43, Docs/Decisions/0061-m43-static-scenery.md decision 3; Architecture §8.3)
// as pure functions, so they are unit-tested without a GPU: the static grouping key and the cell tests a view runs before it
// tests a cell's items. A view culls a static cell (Renderer/RenderStaticSet.h) by its box, the world-space box of its items'
// draws, made robust against rounding by InflateStaticCellBox: a cell whose inflated box is Outside a view holds no draw the
// view's own test (IsOutsideView, a shadow face's, the mirror's clip plane) would keep, and one Inside holds no draw it would
// cull, so the static path keeps exactly the draws the dynamic path keeps; only a Partial cell's draws are tested one by one,
// with the dynamic path's own functions. Deterministic and thread-safe (no state).

namespace Engine {

	// M43's static grouping key (ADR 0060's ledger: (pipeline, material, mesh, submesh), as a struct; M46 appends its level, Lod):
	// the draws of one key share their vertices, indices, pipeline variant and material set, so a view draws every visible
	// static instance of a key with one indexed draw. Pipeline is the forward variant of the camera's passes, or a shadow
	// caster's variant (Material null for an Opaque caster, which binds none). Ordered as the opaque sort orders draws.
	struct RenderStaticDrawKey
	{
		uint32_t Pipeline = 0;
		AssetHandle Material{};
		AssetHandle Mesh{};
		uint32_t Submesh = 0;
		// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3E, W46.4): the level of detail whose index
		// range the key's draws draw, so two levels of one submesh never share an indexed draw.
		uint32_t Lod = 0;

		auto operator<=>(const RenderStaticDrawKey&) const = default;
	};

	// Where a static cell lies against one view's test.
	enum class StaticCellVisibility : uint8_t
	{
		Outside, // every draw of the cell is culled
		Partial, // the cell's draws are tested one by one
		Inside   // no draw of the cell is culled
	};

	// `box` grown on every side by its rounding margin, 1e-4 of the largest coordinate magnitude of the box and of
	// `reference` (the view's origin: the camera's position, or a shadow face's frustum centre) plus 1 mm, three orders of
	// magnitude above the rounding of the per-draw tests' single-precision clip coordinates. Empty stays empty.
	[[nodiscard]] Aabb InflateStaticCellBox(const Aabb& box, const glm::vec3& reference);

	// Against `camera`'s view (IsOutsideView's test, RenderPrepare.h: the six clip planes and FarClip): Outside when all eight
	// corners of `box` (world space) lie outside one of them, Inside when all eight lie inside every one, else Partial. An
	// empty box is Partial (IsOutsideView never culls empty bounds).
	[[nodiscard]] StaticCellVisibility ClassifyStaticCell(const Aabb& box, const CameraData& camera);

	// Against a mirror view's clip plane (IsBehindPlanarReflectionClipPlane's test, PlanarReflections.h: dot(plane, (p, 1)) <
	// 0 behind): Outside when every corner of `box` lies behind it, Inside when none does, else Partial; an empty box is
	// Partial.
	[[nodiscard]] StaticCellVisibility ClassifyStaticCellClipPlane(const Aabb& box, const glm::vec4& clipPlane);

	// Against a shadow face of `viewProjection` (ShadowPass.cpp's caster cull: -w <= x, y <= w and 0 <= z, and z <= w unless
	// `pancake`, a directional map's depth clamp): Outside, Inside or Partial as ClassifyStaticCell; an empty box is Partial.
	[[nodiscard]] StaticCellVisibility ClassifyStaticCellShadow(const Aabb& box, const glm::mat4& viewProjection, bool pancake);

	// Against a light's box (ShadowPass.h's ShadowBoxesOverlap, which preselects a spot tile's and a point light's casters):
	// Outside when `box` (already inflated by InflateStaticCellBox) cannot overlap the light's box of `lightCentre` and
	// `lightHalfExtent` with ShadowBoxesOverlap's margin, so none of its casters' boxes can; Inside when `box` lies within
	// the light's box, so every caster box overlaps it; else Partial. An empty box is Partial.
	[[nodiscard]] StaticCellVisibility ClassifyStaticCellLight(const Aabb& box, const glm::vec3& lightCentre, const glm::vec3& lightHalfExtent);

}
