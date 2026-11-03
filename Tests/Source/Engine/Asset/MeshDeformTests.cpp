#include "TestsPCH.h"

#include "Engine/Asset/MeshDeform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <vector>

// The CPU deformation of skinned and morphed meshes (M23, C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md
// decision 27, stream C): the reference the Skinning pass equals and the bounds deformed draws are culled with. The weight
// resolution is the contract's (live); the rest is skipped until stream C implements it.

namespace Engine {

	namespace {

		// A strip of two quads along +Y (y = 0, 1, 2), facing +Z: the bottom row weighted to joint 0, the top row to joint 1 and
		// the middle row half to each; one morph target that pushes the top row 0.5 along +Z.
		MeshData MakeBendingStrip()
		{
			MeshData mesh;
			for (int row = 0; row < 3; ++row)
			{
				for (int column = 0; column < 2; ++column)
				{
					const glm::vec3 position(static_cast<float>(column) - 0.5f, static_cast<float>(row), 0.0f);
					mesh.Vertices.push_back({ .Position = position, .Normal = { 0.0f, 0.0f, 1.0f }, .Tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .TexCoord = { static_cast<float>(column), static_cast<float>(row) / 2.0f } });
					mesh.Bounds.Extend(position);
					const float top = static_cast<float>(row) / 2.0f;
					mesh.Skin.push_back({ .Joints = { 0, 1, 0, 0 }, .Weights = { 1.0f - top, top, 0.0f, 0.0f } });
				}
			}
			mesh.SkinJointCount = 2;
			mesh.Indices = { 0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4 };
			mesh.Submeshes.push_back({ .IndexOffset = 0, .IndexCount = 12, .MaterialSlot = 0, .Bounds = mesh.Bounds });
			mesh.Slots.push_back({ .Name = "Default" });
			MeshMorphTarget push{ .Name = "Push" };
			for (size_t index = 0; index < mesh.Vertices.size(); ++index)
				push.Deltas.push_back({ .Position = { 0.0f, 0.0f, index >= 4 ? 0.5f : 0.0f } });
			mesh.MorphTargets.push_back(push);
			return mesh;
		}

	}

	TEST_SUITE("Asset")
	{
		// The one rule every deformation resolves its weights by: one per morph target, the given weight where there is one,
		// the target's default where there is none (a SkinnedMesh added by hand, an item without one), an extra one dropped.
		TEST_CASE("MeshDeform: a missing morph weight takes its target's default, and an extra one is dropped")
		{
			const std::vector<float> defaults = { 0.6f, 0.0f, -0.25f };
			CHECK(ResolveMorphWeights(defaults, {}) == defaults);
			const std::vector<float> one = { 1.0f };
			CHECK(ResolveMorphWeights(defaults, one) == std::vector<float>{ 1.0f, 0.0f, -0.25f });
			const std::vector<float> beyond = { 0.5f, 1.5f, 0.0f, 0.75f };
			CHECK(ResolveMorphWeights(defaults, beyond) == std::vector<float>{ 0.5f, 1.5f, 0.0f });
			CHECK(ResolveMorphWeights({}, beyond).empty());
		}

		// Morph first, then skin: the top row pushed 0.5 along +Z by the full-weight target, then carried by joint 1, which the
		// palette translates by (0, 0, 1) and rotates 90 degrees about +X around the origin; the bottom row stays (joint 0 is
		// the identity); the middle row moves half way.
		TEST_CASE("MeshDeform: the CPU reference morphs, then skins positions, normals and tangents" * doctest::skip(true))
		{
			const MeshData strip = MakeBendingStrip();
			const glm::mat4 bend = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 1.0f))
				* glm::mat4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
			const std::vector<glm::mat4> palette = { glm::mat4(1.0f), bend };
			const std::vector<float> weights = { 1.0f };
			const Result<std::vector<MeshVertex>> deformed = DeformMeshVertices(strip, palette, weights);
			REQUIRE_MESSAGE(deformed.has_value(), deformed.error().ToString());
			REQUIRE(deformed->size() == 6);
			// The bottom row: joint 0 alone, no morph.
			CHECK((*deformed)[0].Position == strip.Vertices[0].Position);
			CHECK((*deformed)[0].Normal == glm::vec3(0.0f, 0.0f, 1.0f));
			// The top row: (x, 2, 0.5) under the bend: x, -0.5, 2 + 1.
			CHECK((*deformed)[4].Position.x == -0.5f);
			CHECK(std::abs((*deformed)[4].Position.y - -0.5f) < 1e-6f);
			CHECK(std::abs((*deformed)[4].Position.z - 3.0f) < 1e-6f);
			// Its normal follows the rotation: +Z becomes -Y; the tangent +X stays, its sign kept.
			CHECK(std::abs((*deformed)[4].Normal.y - -1.0f) < 1e-6f);
			CHECK((*deformed)[4].Tangent == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
			CHECK((*deformed)[4].TexCoord == strip.Vertices[4].TexCoord);
			// Without weights the target takes its default, 0 here, so the morph is skipped; without a palette the skin is.
			const Result<std::vector<MeshVertex>> unmorphed = DeformMeshVertices(strip, palette, {});
			REQUIRE(unmorphed.has_value());
			CHECK(std::abs((*unmorphed)[4].Position.y) < 1e-6f);
			CHECK(std::abs((*unmorphed)[4].Position.z - 3.0f) < 1e-6f);
			// A target whose default is 1 morphs fully without weights, as with the weight 1 given.
			MeshData defaulted = strip;
			defaulted.MorphTargets[0].DefaultWeight = 1.0f;
			const Result<std::vector<MeshVertex>> byDefault = DeformMeshVertices(defaulted, palette, {});
			REQUIRE(byDefault.has_value());
			CHECK((*byDefault)[4].Position == (*deformed)[4].Position);
			const Result<std::vector<MeshVertex>> unskinned = DeformMeshVertices(strip, {}, weights);
			REQUIRE(unskinned.has_value());
			CHECK((*unskinned)[4].Position == glm::vec3(-0.5f, 2.0f, 0.5f));
			// A palette shorter than the skin's joints is refused.
			const std::vector<glm::mat4> shortPalette = { glm::mat4(1.0f) };
			CHECK_FALSE(DeformMeshVertices(strip, shortPalette, weights).has_value());
		}

		// For any finite weights, a clip's sampled weight beyond [-1, 1] (a cubic spline's overshoot) included, with or without
		// a palette, and for a weight resolved from the target's default.
		TEST_CASE("MeshDeform: deformed bounds contain every deformed vertex" * doctest::skip(true))
		{
			MeshData strip = MakeBendingStrip();
			strip.MorphTargets[0].DefaultWeight = 0.5f;
			const MeshDeformBounds bounds = ComputeMeshDeformBounds(strip);
			REQUIRE(bounds.Joints.size() == 2);
			REQUIRE(bounds.MorphReach.size() == 1);
			CHECK(bounds.MorphReach[0] == glm::vec3(0.0f, 0.0f, 0.5f));
			CHECK(bounds.DefaultWeights == std::vector<float>{ 0.5f });
			const auto contains = [](const Aabb& box, const std::vector<MeshVertex>& vertices)
			{
				for (const MeshVertex& vertex : vertices)
				{
					CHECK(vertex.Position.x >= box.Min.x);
					CHECK(vertex.Position.y >= box.Min.y);
					CHECK(vertex.Position.z >= box.Min.z);
					CHECK(vertex.Position.x <= box.Max.x);
					CHECK(vertex.Position.y <= box.Max.y);
					CHECK(vertex.Position.z <= box.Max.z);
				}
			};
			for (const float angle : { 0.0f, 0.5f, 1.0f, 1.5f })
			{
				CAPTURE(angle);
				const glm::mat4 joint = glm::translate(glm::mat4(1.0f), glm::vec3(angle, 0.0f, 1.0f));
				const std::vector<glm::mat4> palette = { glm::mat4(1.0f), joint };
				for (const float weight : { -1.0f, 0.0f, 0.5f, 1.0f, 1.5f, -2.5f })
				{
					CAPTURE(weight);
					const std::vector<float> weights = { weight };
					const Result<std::vector<MeshVertex>> deformed = DeformMeshVertices(strip, palette, weights);
					REQUIRE(deformed.has_value());
					contains(ComputeDeformedBounds(bounds, palette, weights), *deformed);
					const Result<std::vector<MeshVertex>> morphed = DeformMeshVertices(strip, {}, weights);
					REQUIRE(morphed.has_value());
					contains(ComputeDeformedBounds(bounds, {}, weights), *morphed);
				}
				const Result<std::vector<MeshVertex>> byDefault = DeformMeshVertices(strip, palette, {});
				REQUIRE(byDefault.has_value());
				contains(ComputeDeformedBounds(bounds, palette, {}), *byDefault);
			}
		}
	}

}
