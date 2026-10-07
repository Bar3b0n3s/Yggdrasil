#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <array>
#include <limits>

namespace Engine {

	// An axis-aligned bounding box (Architecture §6.8 submesh bounds, §8.3 culling, §13.5 entity.bounds): the inclusive
	// box from Min to Max. The default-constructed box is empty (Min = +EmptyBound, Max = -EmptyBound), so extending it
	// by a point gives that point's degenerate box; IsEmpty is true exactly for boxes no point was added to. Only
	// IEEE-exact operations (min, max, multiply, add), so the results are identical in every configuration (§4.12). A
	// plain value type; thread-compatible.
	struct Aabb
	{
		// The largest finite float: an empty box runs from +EmptyBound to -EmptyBound.
		static constexpr float EmptyBound = std::numeric_limits<float>::max();

		glm::vec3 Min{ EmptyBound };
		glm::vec3 Max{ -EmptyBound };

		// True when no point was added (some Min component above its Max component).
		[[nodiscard]] constexpr bool IsEmpty() const { return Min.x > Max.x || Min.y > Max.y || Min.z > Max.z; }

		// The box that also holds `point`.
		constexpr void Extend(const glm::vec3& point)
		{
			Min = glm::vec3(Min.x < point.x ? Min.x : point.x, Min.y < point.y ? Min.y : point.y, Min.z < point.z ? Min.z : point.z);
			Max = glm::vec3(Max.x > point.x ? Max.x : point.x, Max.y > point.y ? Max.y : point.y, Max.z > point.z ? Max.z : point.z);
		}

		// The box that also holds `other`; an empty `other` changes nothing.
		constexpr void Extend(const Aabb& other)
		{
			if (other.IsEmpty())
				return;
			Extend(other.Min);
			Extend(other.Max);
		}

		// (Min + Max) / 2; meaningless for an empty box.
		[[nodiscard]] constexpr glm::vec3 GetCenter() const { return (Min + Max) * 0.5f; }
		// Max - Min; meaningless for an empty box.
		[[nodiscard]] constexpr glm::vec3 GetSize() const { return Max - Min; }

		// The eight corners, in the order (x, y, z) = (Min|Max) with x varying fastest.
		[[nodiscard]] constexpr std::array<glm::vec3, 8> GetCorners() const
		{
			return { {
				{ Min.x, Min.y, Min.z },
				{ Max.x, Min.y, Min.z },
				{ Min.x, Max.y, Min.z },
				{ Max.x, Max.y, Min.z },
				{ Min.x, Min.y, Max.z },
				{ Max.x, Min.y, Max.z },
				{ Min.x, Max.y, Max.z },
				{ Max.x, Max.y, Max.z },
			} };
		}

		// The box around this box's eight corners transformed by the affine `matrix` (column-major glm, M * v); an empty box
		// stays empty. The result holds the transformed box (it is the tightest axis-aligned box around the transformed
		// corners, not the tightest around the transformed contents).
		[[nodiscard]] Aabb Transformed(const glm::mat4& matrix) const
		{
			if (IsEmpty())
				return {};
			Aabb result;
			for (const glm::vec3& corner : GetCorners())
				result.Extend(glm::vec3(matrix * glm::vec4(corner, 1.0f)));
			return result;
		}

		constexpr bool operator==(const Aabb&) const = default;
	};

}
