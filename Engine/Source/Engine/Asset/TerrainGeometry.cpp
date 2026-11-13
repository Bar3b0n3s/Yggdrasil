#include "EnginePCH.h"
#include "Engine/Asset/TerrainGeometry.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cmath>

// The terrain's surface (TerrainGeometry.h; Docs/Decisions/0037-m26-contract.md decision 21), stream T-A. Every value is
// computed with IEEE-exact operations (and sqrt for the normal) in one fixed order, so the drawing, the physics, the queries
// and the editor's brushes see the same surface in every configuration.

namespace Engine {

	namespace Utils {

		// The cell under a local point and the point's fractions in it, by the triangulation of TerrainGeometry.h.
		struct TerrainCellPoint
		{
			uint32_t X = 0;
			uint32_t Z = 0;
			float FractionX = 0.0f; // [0, 1]
			float FractionZ = 0.0f; // [0, 1]
		};

		// The height in metres of sample (x, z): sample / MaxHeightfieldSample * HeightScale.
		[[nodiscard]] static float GetSampleHeight(const HeightfieldData& heightfield, const TerrainExtent& extent, uint32_t x, uint32_t z)
		{
			const uint16_t sample = heightfield.Heights[x + static_cast<size_t>(heightfield.Resolution) * z];
			return static_cast<float>(sample) / static_cast<float>(MaxHeightfieldSample) * extent.HeightScale;
		}

		// The cell coordinate along one axis: the local coordinate scaled to cells, the cell its floor (the last cell for the
		// far edge) and the fraction what remains. The caller has checked 0 <= local <= size.
		static void FindCell(float local, float size, uint32_t cells, uint32_t& cell, float& fraction)
		{
			const float scaled = local * static_cast<float>(cells) / size;
			const float floored = std::min(std::floor(scaled), static_cast<float>(cells - 1));
			cell = static_cast<uint32_t>(floored);
			fraction = std::clamp(scaled - floored, 0.0f, 1.0f);
		}

		// The cell under (localX, localZ), or nullopt outside the terrain, for an extent that is not positive and finite or for
		// a heightfield that fails ValidateHeightfieldData.
		[[nodiscard]] static std::optional<TerrainCellPoint> FindTerrainCell(const HeightfieldData& heightfield, const TerrainExtent& extent, glm::vec2 localXZ)
		{
			if (!ValidateHeightfieldData(heightfield).has_value())
				return std::nullopt;
			if (!(extent.Size.x > 0.0f) || !(extent.Size.y > 0.0f) || !std::isfinite(extent.Size.x) || !std::isfinite(extent.Size.y))
				return std::nullopt;
			// Written so a NaN coordinate fails the test: the edges are inside.
			if (!(localXZ.x >= 0.0f && localXZ.x <= extent.Size.x && localXZ.y >= 0.0f && localXZ.y <= extent.Size.y))
				return std::nullopt;
			const uint32_t cells = heightfield.Resolution - 1;
			TerrainCellPoint point;
			FindCell(localXZ.x, extent.Size.x, cells, point.X, point.FractionX);
			FindCell(localXZ.y, extent.Size.y, cells, point.Z, point.FractionZ);
			return point;
		}

	}

	glm::vec3 GetTerrainSamplePosition(const HeightfieldData& heightfield, const TerrainExtent& extent, uint32_t x, uint32_t z)
	{
		ENGINE_CORE_ASSERT(x < heightfield.Resolution && z < heightfield.Resolution, "Sample ({}, {}) of a heightfield of {} samples a side", x, z,
			heightfield.Resolution);
		ENGINE_CORE_ASSERT(heightfield.Heights.size() == static_cast<size_t>(heightfield.Resolution) * heightfield.Resolution,
			"A heightfield of {} samples a side holds {} samples", heightfield.Resolution, heightfield.Heights.size());
		const float cells = static_cast<float>(heightfield.Resolution - 1);
		return glm::vec3(static_cast<float>(x) * extent.Size.x / cells, Utils::GetSampleHeight(heightfield, extent, x, z),
			static_cast<float>(z) * extent.Size.y / cells);
	}

	std::optional<float> SampleTerrainHeight(const HeightfieldData& heightfield, const TerrainExtent& extent, glm::vec2 localXZ)
	{
		const std::optional<Utils::TerrainCellPoint> point = Utils::FindTerrainCell(heightfield, extent, localXZ);
		if (!point.has_value())
			return std::nullopt;
		const uint32_t x = point->X;
		const uint32_t z = point->Z;
		const float fx = point->FractionX;
		const float fz = point->FractionZ;
		const float corner = Utils::GetSampleHeight(heightfield, extent, x, z);
		const float diagonal = Utils::GetSampleHeight(heightfield, extent, x + 1, z + 1);
		if (fz >= fx)
		{
			// The triangle (x, z), (x, z + 1), (x + 1, z + 1): along +Z from the corner, then along +X to the diagonal's end.
			const float side = Utils::GetSampleHeight(heightfield, extent, x, z + 1);
			return corner + fz * (side - corner) + fx * (diagonal - side);
		}
		// The triangle (x, z), (x + 1, z + 1), (x + 1, z): along +X from the corner, then along +Z to the diagonal's end.
		const float side = Utils::GetSampleHeight(heightfield, extent, x + 1, z);
		return corner + fx * (side - corner) + fz * (diagonal - side);
	}

	std::optional<glm::vec3> SampleTerrainNormal(const HeightfieldData& heightfield, const TerrainExtent& extent, glm::vec2 localXZ)
	{
		const std::optional<Utils::TerrainCellPoint> point = Utils::FindTerrainCell(heightfield, extent, localXZ);
		if (!point.has_value())
			return std::nullopt;
		const uint32_t x = point->X;
		const uint32_t z = point->Z;
		const float cells = static_cast<float>(heightfield.Resolution - 1);
		const float cellX = extent.Size.x / cells;
		const float cellZ = extent.Size.y / cells;
		const float corner = Utils::GetSampleHeight(heightfield, extent, x, z);
		const float diagonal = Utils::GetSampleHeight(heightfield, extent, x + 1, z + 1);
		// The triangle's rise across the cell along X and along Z (the same triangle SampleTerrainHeight picks).
		float riseX = 0.0f;
		float riseZ = 0.0f;
		if (point->FractionZ >= point->FractionX)
		{
			const float side = Utils::GetSampleHeight(heightfield, extent, x, z + 1);
			riseX = diagonal - side;
			riseZ = side - corner;
		}
		else
		{
			const float side = Utils::GetSampleHeight(heightfield, extent, x + 1, z);
			riseX = side - corner;
			riseZ = diagonal - side;
		}
		// The plane's normal (-dh/dx, 1, -dh/dz) scaled by the cell's area, so no division by a rise: (-riseX cellZ, cellX
		// cellZ, -riseZ cellX), then normalized.
		const glm::vec3 normal(0.0f - riseX * cellZ, cellX * cellZ, 0.0f - riseZ * cellX);
		const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
		return glm::vec3(normal.x / length, normal.y / length, normal.z / length);
	}

}
