#include "EnginePCH.h"
#include "Engine/AssetPipeline/Private/MeshSimplifier.h"

#include "Engine/Core/Assert.h"

#include <meshoptimizer.h>

#include <algorithm>

namespace Engine {

	namespace Utils {

		std::vector<uint32_t> SimplifyTriangles(const MeshSimplifierInput& input, size_t targetIndexCount, float targetError)
		{
			ENGINE_CORE_ASSERT(input.Indices.size() % 3 == 0, "SimplifyTriangles needs whole triangles");
			ENGINE_CORE_ASSERT(targetIndexCount % 3 == 0 && targetIndexCount <= input.Indices.size(),
				"SimplifyTriangles needs a target of whole triangles, at most the input's");
			ENGINE_CORE_ASSERT(std::ranges::all_of(input.Indices, [&input](uint32_t index)
			{
				return index < input.Positions.size();
			}),
				"SimplifyTriangles needs every index below the vertex count");
			if (input.Indices.empty() || input.Positions.empty())
				return {};

			static_assert(sizeof(glm::vec3) == 3 * sizeof(float), "meshopt_simplify reads tightly packed float positions");
			std::vector<uint32_t> result(input.Indices.size());
			float resultError = 0.0f;
			const size_t count = meshopt_simplify(result.data(), input.Indices.data(), input.Indices.size(), &input.Positions[0].x, input.Positions.size(),
				sizeof(glm::vec3), targetIndexCount, targetError, meshopt_SimplifyLockBorder, &resultError);
			ENGINE_CORE_VERIFY(count <= result.size() && count % 3 == 0, "meshopt_simplify returned more indices than it was given");
			result.resize(count);
			return result;
		}

	}

}
