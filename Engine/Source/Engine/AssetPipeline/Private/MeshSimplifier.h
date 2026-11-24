#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// meshoptimizer's simplifier for the glTF importer's generated levels of detail (M46,
// Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3B; Vendor/meshoptimizer/VENDOR.md). This header is
// the only interface to meshoptimizer: MeshSimplifier.cpp is the one first-party file that includes <meshoptimizer.h>
// (Scripts/ModuleRules.json RestrictedThirdParty), so no meshoptimizer type leaves it.

namespace Engine {

	namespace Utils {

		// A triangle list over a vertex buffer's positions. Every span must outlive the call; Indices.size() is a multiple of
		// 3 and every index is below Positions.size() (asserted: the importer validates its input first).
		struct MeshSimplifierInput
		{
			std::span<const glm::vec3> Positions{};
			std::span<const uint32_t> Indices{};
		};

		// `input`'s triangles simplified by meshopt_simplify towards at most `targetIndexCount` indices (a multiple of 3,
		// at most input.Indices.size()), moving no vertex on a topological border (meshopt_SimplifyLockBorder, so the seams
		// between submeshes stay closed) and keeping the error within `targetError`, relative to the extent of every
		// position in `input` (the mesh's, not only the triangles'). The result indexes input.Positions only (an index-only
		// level), holds whole triangles and never more indices than the input; it stops above the target where the error
		// bound would be exceeded, and may be empty. Deterministic for one toolchain and C runtime, in every configuration
		// (VENDOR.md: the precise floating-point model). Pure.
		[[nodiscard]] std::vector<uint32_t> SimplifyTriangles(const MeshSimplifierInput& input, size_t targetIndexCount, float targetError);

	}

}
