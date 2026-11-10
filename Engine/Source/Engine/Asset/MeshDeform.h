#pragma once

#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

// The deformation of a skinned or morphed mesh on the CPU (M23, C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md
// decision 27): the reference the Skinning pass (Renderer/SkinningPass.h) equals, and the conservative bounds the renderer
// culls deformed draws with. In Asset, beside MeshData, because Renderer (culling, the pass) and Scene and the tests (the
// reference) all include Asset and Renderer includes neither Scene nor Animation (Architecture §3).
//
// The morph weights a mesh deforms with are always resolved by one rule, ResolveMorphWeights: one per morph target, the
// given weight where there is one, the target's DefaultWeight (MeshMorphTarget, glTF's mesh weights) where there is none, an
// extra one dropped. So the CPU reference, the deformed bounds and the Skinning pass agree, and an item without a
// SkinnedMesh, or with fewer weights than targets (a SkinnedMesh added by hand), draws at its targets' defaults
// (decision 27). A weight may be any finite value: a SkinnedMesh's own weights are in [-1, 1], but a clip's sampled weights
// are only finite (a cubic spline overshoots its keys).
//
// The deformation of one vertex, in this order, in the mesh's space:
//   1. Morph: with the resolved weights w, Position + sum(w_t * Delta_t.Position), Normal + sum(w_t * Delta_t.Normal) and
//      Tangent.xyz + sum(w_t * Delta_t.Tangent), summed in target order; a mesh without morph targets keeps the vertex.
//   2. Skin: with a palette (one matrix per skin joint, palette[j] = the joint's skin matrix taken into the drawn entity's
//      space, RenderSnapshot.h's MeshDrawItem::JointPalette) and a mesh with a skin, M = sum(Weights[i] * palette[Joints[i]])
//      over the vertex's four influences in order, skipping a weight of 0; Position = (M * (Position, 1)).xyz, Normal =
//      normalize((M * (Normal, 0)).xyz) and Tangent.xyz = normalize((M * (Tangent.xyz, 0)).xyz), the upper 3 x 3 of M.
//      Without a palette or a skin, Normal and Tangent.xyz are normalized after the morph.
//   3. TexCoord and Tangent.w are kept; the attribute stream (MeshData::Attributes) is not deformed.
// normalize(v) is v * (1 / sqrt(dot(v, v))), and a zero vector (a morph that cancels a normal) stays zero. Normals are
// transformed by the blended matrix itself, not its inverse transpose, so a joint with a non-uniform scale shades
// approximately (Backlog §6). Only IEEE-exact operations and sqrt, so the CPU result is identical in every configuration;
// the GPU's (Passes/Skinning.slang, the same operations in the same order) equals it within the tolerance the Skinning
// pass's test states (fused multiply-adds and the GPU's square roots).
//
// Frozen by the M23 contract C23.3; stream C implements it.

namespace Engine {

	// The effective morph weights of a mesh whose targets' defaults are `defaultWeights` (one per morph target, in order),
	// drawn with `morphWeights` (a SkinnedMesh's, overridden by its Animator's pose, RenderSnapshot.h's
	// MeshDrawItem::MorphWeights; possibly fewer or more than the targets): one per target, `morphWeights[t]` where it
	// exists, else `defaultWeights[t]`; the weights past the target count are dropped. Pure.
	[[nodiscard]] std::vector<float> ResolveMorphWeights(std::span<const float> defaultWeights, std::span<const float> morphWeights);

	// What ComputeDeformedBounds needs of a mesh, computed once per mesh (the GPU mirror keeps it, GpuMesh::DeformBounds,
	// so the morph defaults travel with the GPU mesh to the Prepare stage and the Skinning pass).
	struct MeshDeformBounds
	{
		// The mesh's own bounds (MeshData::Bounds).
		Aabb Base{};
		// Per morph target: the largest magnitude of its position deltas on each axis.
		std::vector<glm::vec3> MorphReach{};
		// Per morph target: its DefaultWeight (MeshMorphTarget), what ResolveMorphWeights fills a missing weight with.
		std::vector<float> DefaultWeights{};
		// Per skin joint (MeshData::SkinJointCount; empty without a skin): the bounds of the positions of the vertices with a
		// non-zero weight on it, each widened on every axis by the morph reach of every target at full weight (the sum of
		// MorphReach), so a morphed vertex stays inside its joints' boxes for weights in [-1, 1]; an empty box for a joint no
		// vertex weights.
		std::vector<Aabb> Joints{};

		bool operator==(const MeshDeformBounds&) const = default;
	};

	// The deform bounds of `mesh` (ValidateMeshData holds, asserted). Pure.
	[[nodiscard]] MeshDeformBounds ComputeMeshDeformBounds(const MeshData& mesh);

	// Bounds that contain every vertex of the deformation of the mesh `bounds` describe with `palette` and `morphWeights`
	// (resolved against bounds.DefaultWeights, the file comment's rule), for any finite weights: with a palette that covers
	// the skin, the union of each weighted joint's box, widened further on every axis by (|w_t| - 1) times MorphReach[t] for
	// each resolved weight beyond [-1, 1], transformed by its palette matrix (its eight corners), since a skinned vertex is
	// a convex combination of the points its joints carry it to; without a skin or a palette, Base widened by the sum of
	// |w_t| times MorphReach[t]. A palette shorter than the skin's joints gives Base widened that way (the renderer then
	// draws the mesh unskinned). Pure.
	[[nodiscard]] Aabb ComputeDeformedBounds(const MeshDeformBounds& bounds, std::span<const glm::mat4> palette, std::span<const float> morphWeights);

	// The CPU reference: the vertices of `mesh` deformed by the file comment's rule, `morphWeights` resolved against its
	// morph targets' DefaultWeight. Errors: InvalidArgument when `palette` is not empty and holds fewer matrices than the
	// mesh's SkinJointCount, or holds a non-finite matrix, or when a morph weight is not finite; also, for a mesh that breaks
	// ValidateMeshData's skin and morph rules (a skin or a target without one entry per vertex, a weighted joint beyond
	// SkinJointCount), never reading past its data. Pure.
	[[nodiscard]] Result<std::vector<MeshVertex>> DeformMeshVertices(const MeshData& mesh, std::span<const glm::mat4> palette,
		std::span<const float> morphWeights);

}
