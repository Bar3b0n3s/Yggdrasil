#include "EnginePCH.h"
#include "Engine/Asset/MeshDeform.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

// The CPU deformation of a skinned or morphed mesh (MeshDeform.h): the reference Passes/Skinning.slang follows operation for
// operation (morph in target order skipping a weight of 0, the four influences blended in order skipping a weight of 0, the
// directions scaled by 1 / sqrt of their squared length), and the conservative bounds of a deformation.

namespace Engine {

	namespace Utils {

		// `direction` at unit length, scaled by 1 / sqrt of its squared length as the Skinning pass scales it; a zero vector (a
		// morph that cancels a normal) stays zero, never NaN.
		static glm::vec3 NormalizeDeformedDirection(const glm::vec3& direction)
		{
			const float lengthSquared = glm::dot(direction, direction);
			if (!(lengthSquared > 0.0f))
				return direction;
			return direction * (1.0f / std::sqrt(lengthSquared));
		}

		static bool IsFiniteMatrix(const glm::mat4& matrix)
		{
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

		// `box` widened by `widening` on every axis; an empty box stays empty.
		static Aabb WidenBox(const Aabb& box, const glm::vec3& widening)
		{
			if (box.IsEmpty())
				return box;
			return { box.Min - widening, box.Max + widening };
		}

	}

	std::vector<float> ResolveMorphWeights(std::span<const float> defaultWeights, std::span<const float> morphWeights)
	{
		std::vector<float> resolved(defaultWeights.begin(), defaultWeights.end());
		const size_t given = std::min(resolved.size(), morphWeights.size());
		std::copy_n(morphWeights.begin(), given, resolved.begin());
		return resolved;
	}

	MeshDeformBounds ComputeMeshDeformBounds(const MeshData& mesh)
	{
		ENGINE_CORE_ASSERT(ValidateMeshData(mesh).has_value(), "ComputeMeshDeformBounds needs a valid mesh");
		MeshDeformBounds bounds;
		bounds.Base = mesh.Bounds;
		glm::vec3 fullReach(0.0f);
		bounds.MorphReach.reserve(mesh.MorphTargets.size());
		bounds.DefaultWeights.reserve(mesh.MorphTargets.size());
		for (const MeshMorphTarget& target : mesh.MorphTargets)
		{
			glm::vec3 reach(0.0f);
			for (const MeshMorphDelta& delta : target.Deltas)
				reach = glm::max(reach, glm::abs(delta.Position));
			bounds.MorphReach.push_back(reach);
			bounds.DefaultWeights.push_back(target.DefaultWeight);
			fullReach += reach;
		}
		if (mesh.Skin.empty() || mesh.SkinJointCount == 0)
			return bounds;
		bounds.Joints.resize(mesh.SkinJointCount);
		const size_t vertexCount = std::min(mesh.Skin.size(), mesh.Vertices.size());
		for (size_t index = 0; index < vertexCount; ++index)
		{
			const MeshSkinVertex& skin = mesh.Skin[index];
			for (glm::length_t influence = 0; influence < 4; ++influence)
			{
				const uint16_t joint = skin.Joints[static_cast<size_t>(influence)];
				if (skin.Weights[influence] != 0.0f && joint < mesh.SkinJointCount)
					bounds.Joints[joint].Extend(mesh.Vertices[index].Position);
			}
		}
		for (Aabb& joint : bounds.Joints)
			joint = Utils::WidenBox(joint, fullReach);
		return bounds;
	}

	Aabb ComputeDeformedBounds(const MeshDeformBounds& bounds, std::span<const glm::mat4> palette, std::span<const float> morphWeights)
	{
		const std::vector<float> weights = ResolveMorphWeights(bounds.DefaultWeights, morphWeights);
		const bool skinned = !bounds.Joints.empty() && palette.size() >= bounds.Joints.size();
		// A joint's box already holds every target at full weight; the base box holds none.
		glm::vec3 widening(0.0f);
		for (size_t target = 0; target < weights.size() && target < bounds.MorphReach.size(); ++target)
		{
			const float magnitude = std::abs(weights[target]);
			const float factor = skinned ? std::max(magnitude - 1.0f, 0.0f) : magnitude;
			widening += factor * bounds.MorphReach[target];
		}
		if (!skinned)
			return Utils::WidenBox(bounds.Base, widening);
		Aabb result;
		for (size_t joint = 0; joint < bounds.Joints.size(); ++joint)
			result.Extend(Utils::WidenBox(bounds.Joints[joint], widening).Transformed(palette[joint]));
		return result;
	}

	Result<std::vector<MeshVertex>> DeformMeshVertices(const MeshData& mesh, std::span<const glm::mat4> palette, std::span<const float> morphWeights)
	{
		const size_t vertexCount = mesh.Vertices.size();
		const bool hasSkin = mesh.SkinJointCount > 0 && !mesh.Skin.empty();
		if (!mesh.Skin.empty() && mesh.Skin.size() != vertexCount)
			return MakeError(ErrorCode::InvalidArgument, "the mesh's skin holds {} vertices, not one per each of its {} vertices", mesh.Skin.size(), vertexCount);
		for (size_t target = 0; target < mesh.MorphTargets.size(); ++target)
		{
			if (mesh.MorphTargets[target].Deltas.size() != vertexCount)
			{
				return MakeError(ErrorCode::InvalidArgument, "morph target {} of the mesh holds {} deltas, not one per each of its {} vertices", target,
					mesh.MorphTargets[target].Deltas.size(), vertexCount);
			}
		}
		if (hasSkin && !palette.empty() && palette.size() < mesh.SkinJointCount)
			return MakeError(ErrorCode::InvalidArgument, "the palette holds {} matrices, fewer than the {} joints of the mesh's skin", palette.size(), mesh.SkinJointCount);
		for (size_t joint = 0; joint < palette.size(); ++joint)
		{
			if (!Utils::IsFiniteMatrix(palette[joint]))
				return MakeError(ErrorCode::InvalidArgument, "palette matrix {} is not finite", joint);
		}
		for (size_t target = 0; target < morphWeights.size(); ++target)
		{
			if (!std::isfinite(morphWeights[target]))
				return MakeError(ErrorCode::InvalidArgument, "morph weight {} is not finite", target);
		}

		std::vector<float> defaults;
		defaults.reserve(mesh.MorphTargets.size());
		for (const MeshMorphTarget& target : mesh.MorphTargets)
			defaults.push_back(target.DefaultWeight);
		const std::vector<float> weights = ResolveMorphWeights(defaults, morphWeights);
		const bool skinned = hasSkin && !palette.empty();
		std::vector<MeshVertex> deformed = mesh.Vertices;
		for (size_t index = 0; index < vertexCount; ++index)
		{
			MeshVertex& vertex = deformed[index];
			glm::vec3 position = vertex.Position;
			glm::vec3 normal = vertex.Normal;
			glm::vec3 tangent(vertex.Tangent);
			// 1. Morph, in target order; a target of weight 0 adds nothing.
			for (size_t target = 0; target < weights.size(); ++target)
			{
				const float weight = weights[target];
				if (weight == 0.0f)
					continue;
				const MeshMorphDelta& delta = mesh.MorphTargets[target].Deltas[index];
				position += weight * delta.Position;
				normal += weight * delta.Normal;
				tangent += weight * delta.Tangent;
			}
			// 2. Skin: the influences' palette matrices blended in order, an influence of weight 0 skipped.
			if (skinned)
			{
				const MeshSkinVertex& skin = mesh.Skin[index];
				glm::mat4 blended(0.0f);
				for (glm::length_t influence = 0; influence < 4; ++influence)
				{
					const float weight = skin.Weights[influence];
					const uint16_t joint = skin.Joints[static_cast<size_t>(influence)];
					if (weight == 0.0f)
						continue;
					if (joint >= mesh.SkinJointCount)
						return MakeError(ErrorCode::InvalidArgument, "vertex {} of the mesh's skin addresses joint {}, beyond its {} joints", index, joint, mesh.SkinJointCount);
					blended += weight * palette[joint];
				}
				position = glm::vec3(blended * glm::vec4(position, 1.0f));
				normal = glm::vec3(blended * glm::vec4(normal, 0.0f));
				tangent = glm::vec3(blended * glm::vec4(tangent, 0.0f));
			}
			// 3. The directions at unit length; the texture coordinates and the bitangent sign are kept.
			vertex.Position = position;
			vertex.Normal = Utils::NormalizeDeformedDirection(normal);
			vertex.Tangent = glm::vec4(Utils::NormalizeDeformedDirection(tangent), vertex.Tangent.w);
		}
		return deformed;
	}

}
