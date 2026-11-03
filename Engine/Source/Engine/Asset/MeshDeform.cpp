#include "EnginePCH.h"
#include "Engine/Asset/MeshDeform.h"

#include <algorithm>

// The CPU deformation of a skinned or morphed mesh (MeshDeform.h). The weight resolution is the contract's; the rest are
// contract stubs of M23's C23.3, implemented by stream C (Docs/Decisions/0034-m23-contract.md decision 27): until then
// nothing is deformed and the bounds are empty.

namespace Engine {

	std::vector<float> ResolveMorphWeights(std::span<const float> defaultWeights, std::span<const float> morphWeights)
	{
		std::vector<float> resolved(defaultWeights.begin(), defaultWeights.end());
		const size_t given = std::min(resolved.size(), morphWeights.size());
		std::copy_n(morphWeights.begin(), given, resolved.begin());
		return resolved;
	}

	MeshDeformBounds ComputeMeshDeformBounds(const MeshData& /*mesh*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Aabb ComputeDeformedBounds(const MeshDeformBounds& /*bounds*/, std::span<const glm::mat4> /*palette*/, std::span<const float> /*morphWeights*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<std::vector<MeshVertex>> DeformMeshVertices(const MeshData& /*mesh*/, std::span<const glm::mat4> /*palette*/, std::span<const float> /*morphWeights*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M23 skeletal animation contract (Docs/Decisions/0034-m23-contract.md decision 27, stream C)");
	}

}
