#pragma once

#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

// A terrain's surface in its entity's local space (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decisions 20
// and 21): the one rule that the terrain's rendering (its LOD chunks, Renderer/TerrainLod.h), its physics (the
// HeightField shape, Physics/PhysicsShape.h), its queries (Terrain:SampleHeight and SampleNormal), its bounds and the
// editor's brushes all follow, so what is drawn is what collides and what is queried. In Asset, beside HeightfieldData,
// because Renderer, Scene and EditorCore all include Asset and none includes the others' terrain code (§3).
//   - Placement: sample (x, z) of a heightfield of resolution N sits at the local point
//     (x * Size.x / (N - 1), height(x, z), z * Size.y / (N - 1)), with height(x, z) = sample / MaxHeightfieldSample *
//     HeightScale (HeightfieldData.h), so the terrain spans [0, Size.x] x [0, Size.y] from its entity's origin, rows along
//     +Z; the entity's transform then places it (its scale applies, its rotation too).
//   - Triangulation: each cell (x, z) to (x + 1, z + 1) is two triangles split along its diagonal from (x, z) to
//     (x + 1, z + 1), as Jolt's height field splits it (HeightFieldShape::GetSurfacePosition): with the fractions fx and fz of
//     a point in its cell, the triangle (x, z), (x, z + 1), (x + 1, z + 1) where fz >= fx, else (x, z), (x + 1, z + 1),
//     (x + 1, z). The surface between samples is that triangle's plane, never a bilinear patch, so a body resting on the
//     terrain rests at SampleTerrainHeight (the rest-height acceptance, decision 21).
//
// Frozen by the M26 contract C26.1; stream T-A implements it.

namespace Engine {

	// How a terrain lays its heightfield out (TerrainComponent's Size and HeightScale).
	struct TerrainExtent
	{
		glm::vec2 Size{ 64.0f };   // metres along local X and Z, each > 0
		float HeightScale = 16.0f; // metres at MaxHeightfieldSample, > 0
	};

	// The local position of sample (x, z) (each below the resolution, asserted). Pure; IEEE-exact operations only, so it is
	// identical in every configuration.
	[[nodiscard]] glm::vec3 GetTerrainSamplePosition(const HeightfieldData& heightfield, const TerrainExtent& extent, uint32_t x, uint32_t z);

	// The surface height at the local point (localX, localZ) by the triangulation of the file comment; nullopt outside
	// [0, Size.x] x [0, Size.y] (edges inclusive) or for a heightfield that fails ValidateHeightfieldData's resolution rule.
	// Pure; IEEE-exact operations only.
	[[nodiscard]] std::optional<float> SampleTerrainHeight(const HeightfieldData& heightfield, const TerrainExtent& extent, glm::vec2 localXZ);

	// The unit normal (local space, +Y up) of the triangle under the local point, as SampleTerrainHeight finds it; nullopt
	// where SampleTerrainHeight is. On the diagonal both triangles are the point's: the (x, z + 1) one is used, as for the
	// height. Pure; IEEE-exact operations and sqrt only.
	[[nodiscard]] std::optional<glm::vec3> SampleTerrainNormal(const HeightfieldData& heightfield, const TerrainExtent& extent, glm::vec2 localXZ);

}
