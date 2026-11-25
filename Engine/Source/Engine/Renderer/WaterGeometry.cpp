#include "EnginePCH.h"
#include "Engine/Renderer/WaterGeometry.h"

#include "Engine/Core/Assert.h"

#include <glm/gtc/type_precision.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

// The Water pass's clipmap and records (WaterGeometry.h). Everything is pure and in double where it places or phases, so a
// view's geometry and records are a function of its snapshot and camera.

namespace Engine {

	namespace Utils {

		// The vertices of a grid level: WaterGridCells + 1 a side.
		constexpr uint32_t WaterGridSide = WaterGridCells + 1;

		[[nodiscard]] static uint32_t GridIndex(uint32_t x, uint32_t z)
		{
			return z * WaterGridSide + x;
		}

		// The cell (x, z) of grid vertex `index`.
		[[nodiscard]] static glm::i16vec2 GridCell(uint32_t index)
		{
			return glm::i16vec2(static_cast<int16_t>(index % WaterGridSide), static_cast<int16_t>(index / WaterGridSide));
		}

		// Twice the signed area of the triangle (a, b, c) in (x, z): negative when it winds counter-clockwise seen from +Y.
		[[nodiscard]] static int64_t SignedArea(const glm::i16vec2& a, const glm::i16vec2& b, const glm::i16vec2& c)
		{
			return (static_cast<int64_t>(b.x) - a.x) * (static_cast<int64_t>(c.y) - a.y) - (static_cast<int64_t>(b.y) - a.y) * (static_cast<int64_t>(c.x) - a.x);
		}

		// A grid level's mesh: every cell outside `hole` (a WaterHoleCells square from its first cell; none when `ring` is
		// false) split along its (x, z) to (x + 1, z + 1) diagonal, after the outer edge's odd vertices collapse onto their
		// predecessors along the edge, without the degenerate triangles, then compacted to the vertices it uses in grid order.
		[[nodiscard]] static WaterGridMesh BuildGridMesh(bool ring, const glm::uvec2& hole)
		{
			std::vector<uint32_t> collapsed(static_cast<size_t>(WaterGridSide) * WaterGridSide);
			for (uint32_t z = 0; z < WaterGridSide; ++z)
			{
				for (uint32_t x = 0; x < WaterGridSide; ++x)
				{
					uint32_t target = GridIndex(x, z);
					if ((x == 0 || x == WaterGridCells) && z % 2 == 1)
						target = GridIndex(x, z - 1);
					else if ((z == 0 || z == WaterGridCells) && x % 2 == 1)
						target = GridIndex(x - 1, z);
					collapsed[GridIndex(x, z)] = target;
				}
			}
			std::vector<uint32_t> triangles;
			triangles.reserve(static_cast<size_t>(WaterGridCells) * WaterGridCells * 6);
			const auto addTriangle = [&triangles, &collapsed](uint32_t a, uint32_t b, uint32_t c)
			{
				a = collapsed[a];
				b = collapsed[b];
				c = collapsed[c];
				if (a == b || b == c || a == c)
					return;
				triangles.insert(triangles.end(), { a, b, c });
			};
			for (uint32_t z = 0; z < WaterGridCells; ++z)
			{
				for (uint32_t x = 0; x < WaterGridCells; ++x)
				{
					if (ring && x >= hole.x && x < hole.x + WaterHoleCells && z >= hole.y && z < hole.y + WaterHoleCells)
						continue;
					const uint32_t corner = GridIndex(x, z);
					const uint32_t side = GridIndex(x, z + 1);
					const uint32_t diagonal = GridIndex(x + 1, z + 1);
					const uint32_t other = GridIndex(x + 1, z);
					addTriangle(corner, side, diagonal);
					addTriangle(corner, diagonal, other);
				}
			}
			WaterGridMesh mesh;
			constexpr uint32_t Unused = std::numeric_limits<uint32_t>::max();
			std::vector<uint32_t> compacted(collapsed.size(), Unused);
			for (const uint32_t index : triangles)
				compacted[index] = 0;
			for (uint32_t index = 0; index < compacted.size(); ++index)
			{
				if (compacted[index] == Unused)
					continue;
				compacted[index] = static_cast<uint32_t>(mesh.Cells.size());
				mesh.Cells.push_back(GridCell(index));
			}
			mesh.Indices.reserve(triangles.size());
			for (const uint32_t index : triangles)
				mesh.Indices.push_back(compacted[index]);
			return mesh;
		}

		// The skirt: the last level's stitched outer edge (its even cells, counter-clockwise seen from +Y from (0, 0)) and the
		// same points pushed out from the level's centre by GetWaterSkirtScale, each segment and its pushed copy two triangles.
		[[nodiscard]] static WaterGridMesh BuildSkirtMesh()
		{
			const int32_t scale = GetWaterSkirtScale();
			const auto cells = static_cast<int32_t>(WaterGridCells);
			const int32_t centre = cells / 2;
			std::vector<glm::i16vec2> inner;
			for (int32_t x = 0; x < cells; x += 2)
				inner.emplace_back(static_cast<int16_t>(x), static_cast<int16_t>(0));
			for (int32_t z = 0; z < cells; z += 2)
				inner.emplace_back(static_cast<int16_t>(cells), static_cast<int16_t>(z));
			for (int32_t x = cells; x > 0; x -= 2)
				inner.emplace_back(static_cast<int16_t>(x), static_cast<int16_t>(cells));
			for (int32_t z = cells; z > 0; z -= 2)
				inner.emplace_back(static_cast<int16_t>(0), static_cast<int16_t>(z));
			WaterGridMesh mesh;
			const auto count = static_cast<uint32_t>(inner.size());
			mesh.Cells = inner;
			for (const glm::i16vec2& point : inner)
			{
				mesh.Cells.emplace_back(static_cast<int16_t>(centre + (point.x - centre) * scale), static_cast<int16_t>(centre + (point.y - centre) * scale));
			}
			const auto addTriangle = [&mesh](uint32_t a, uint32_t b, uint32_t c)
			{
				if (SignedArea(mesh.Cells[a], mesh.Cells[b], mesh.Cells[c]) > 0)
					std::swap(b, c);
				mesh.Indices.insert(mesh.Indices.end(), { a, b, c });
			};
			for (uint32_t point = 0; point < count; ++point)
			{
				const uint32_t next = (point + 1) % count;
				addTriangle(point, count + point, count + next);
				addTriangle(point, count + next, next);
			}
			return mesh;
		}

		// The unit (x, z) direction of `axis`'s horizontal part, or none when it is within a millionth of vertical
		// (Physics/WaterWaves.cpp's NormalizeHorizontal).
		[[nodiscard]] static bool NormalizeHorizontal(const glm::dvec3& axis, glm::dvec2& direction)
		{
			const glm::dvec2 horizontal(axis.x, axis.z);
			const double length = std::sqrt(horizontal.x * horizontal.x + horizontal.y * horizontal.y);
			const double full = std::sqrt(glm::dot(axis, axis));
			if (!(length > 1.0e-6 * full) || !std::isfinite(length))
				return false;
			direction = horizontal / length;
			return true;
		}

		// The unit world (x, z) of a water's local +X (Physics/WaterWaves.h's MakeWaterFrame).
		[[nodiscard]] static glm::dvec2 GetWaterAxisX(const glm::dmat4& world)
		{
			glm::dvec2 axis(1.0, 0.0);
			if (NormalizeHorizontal(glm::dvec3(world[0]), axis))
				return axis;
			glm::dvec2 axisZ(0.0, 1.0);
			if (NormalizeHorizontal(glm::dvec3(world[2]), axisZ))
				return glm::dvec2(axisZ.y, -axisZ.x);
			return glm::dvec2(1.0, 0.0);
		}

		// An angle wrapped into [0, 2 pi).
		[[nodiscard]] static double WrapPhase(double phase)
		{
			constexpr double TwoPi = 2.0 * std::numbers::pi;
			const double wrapped = phase - TwoPi * std::floor(phase / TwoPi);
			return wrapped < TwoPi && wrapped >= 0.0 ? wrapped : 0.0;
		}

		// `value` within [minimum, maximum], or `fallback` when it is not finite (a snapshot built by hand).
		[[nodiscard]] static float ClampFinite(float value, float minimum, float maximum, float fallback)
		{
			return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
		}

	}

	std::array<WaterLevelPlacement, WaterLevelCount> PlaceWaterLevels(const glm::dvec2& camera)
	{
		std::array<WaterLevelPlacement, WaterLevelCount> levels{};
		const auto half = static_cast<double>(WaterHoleCells);
		for (uint32_t index = 0; index < WaterLevelCount; ++index)
		{
			const double cell = static_cast<double>(WaterFinestCellSize) * static_cast<double>(1U << index);
			const glm::dvec2 target = index == 0 ? camera - half * cell : levels[index - 1].Corner - half * levels[index - 1].CellSize;
			const glm::dvec2 corner = glm::floor(target / (2.0 * cell)) * (2.0 * cell);
			WaterLevelPlacement& level = levels[index];
			level.Corner = corner;
			level.CellSize = cell;
			if (index > 0)
				level.Hole = glm::uvec2((levels[index - 1].Corner - corner) / cell);
		}
		return levels;
	}

	uint32_t GetWaterLevelMesh(const WaterLevelPlacement& level, uint32_t index)
	{
		if (index == 0)
			return 0;
		ENGINE_CORE_ASSERT(level.Hole.x >= 16 && level.Hole.x <= 17 && level.Hole.y >= 16 && level.Hole.y <= 17, "a ring's hole starts at cell 16 or 17");
		const glm::uvec2 hole = glm::clamp(level.Hole, glm::uvec2(16U), glm::uvec2(17U)) - glm::uvec2(16U);
		return 1 + hole.x + 2 * hole.y;
	}

	int32_t GetWaterSkirtScale()
	{
		const double lastCell = static_cast<double>(WaterFinestCellSize) * static_cast<double>(1U << (WaterLevelCount - 1));
		const double half = static_cast<double>(WaterHoleCells) * lastCell;
		return static_cast<int32_t>(std::ceil((WaterHorizonDistance + 2.0 * lastCell) / half));
	}

	WaterGridMesh BuildWaterMesh(uint32_t mesh)
	{
		ENGINE_CORE_ASSERT(mesh < WaterMeshCount, "water mesh {} of {}", mesh, WaterMeshCount);
		if (mesh == 0)
			return Utils::BuildGridMesh(false, glm::uvec2(0));
		if (mesh < WaterSkirtMesh)
			return Utils::BuildGridMesh(true, glm::uvec2(16 + (mesh - 1) % 2, 16 + (mesh - 1) / 2));
		if (mesh == WaterSkirtMesh)
			return Utils::BuildSkirtMesh();
		return {};
	}

	WaterSquare GetWaterLevelSquare(const std::array<WaterLevelPlacement, WaterLevelCount>& levels, uint32_t index)
	{
		if (index < WaterLevelCount)
			return { .Min = levels[index].Corner, .Max = levels[index].Corner + static_cast<double>(WaterGridCells) * levels[index].CellSize };
		const WaterLevelPlacement& last = levels[WaterLevelCount - 1];
		const auto half = static_cast<double>(WaterHoleCells);
		const double reach = half * static_cast<double>(GetWaterSkirtScale()) * last.CellSize;
		const glm::dvec2 centre = last.Corner + half * last.CellSize;
		return { .Min = centre - reach, .Max = centre + reach };
	}

	WaterSquare GetWaterFootprintBounds(const WaterItem& water)
	{
		if (water.Shape != RenderWaterShape::Rectangle)
		{
			constexpr double Infinite = std::numeric_limits<double>::infinity();
			return { .Min = glm::dvec2(-Infinite), .Max = glm::dvec2(Infinite) };
		}
		const glm::dmat4 world(water.World);
		const glm::dvec2 axisX = Utils::GetWaterAxisX(world);
		const glm::dvec2 axisZ(-axisX.y, axisX.x);
		const glm::dvec2 half(static_cast<double>(water.Size.x) * 0.5 * glm::length(glm::dvec3(world[0])),
			static_cast<double>(water.Size.y) * 0.5 * glm::length(glm::dvec3(world[2])));
		const glm::dvec2 centre(world[3].x, world[3].z);
		const glm::dvec2 extent = glm::abs(axisX) * half.x + glm::abs(axisZ) * half.y;
		return { .Min = centre - extent, .Max = centre + extent };
	}

	bool IsNearPlaneUnderWater(std::span<const WaterItem> waters, const CameraData& camera)
	{
		const glm::dmat4 viewProjection = glm::dmat4(camera.Projection) * glm::dmat4(camera.View);
		const double determinant = glm::determinant(viewProjection);
		if (!std::isfinite(determinant) || determinant == 0.0)
			return false;
		const glm::dmat4 inverse = glm::inverse(viewProjection);
		std::array<glm::dvec3, 4> corners{};
		for (uint32_t corner = 0; corner < 4; ++corner)
		{
			// Reverse-Z: the near plane at depth 1.
			const glm::dvec4 point = inverse * glm::dvec4((corner & 1U) != 0 ? 1.0 : -1.0, (corner & 2U) != 0 ? 1.0 : -1.0, 1.0, 1.0);
			if (!(std::abs(point.w) > 0.0))
				return false;
			corners[corner] = glm::dvec3(point) / point.w;
		}
		for (const WaterItem& water : waters)
		{
			const glm::dmat4 world(water.World);
			const double base = world[3].y;
			double amplitudes = 0.0;
			double sideways = 0.0;
			for (uint32_t wave = 0; wave < std::min(water.WaveCount, MaxWaterWaves); ++wave)
			{
				amplitudes += std::abs(static_cast<double>(water.Waves[wave].Amplitude));
				sideways += std::abs(static_cast<double>(water.Waves[wave].Steepness) * static_cast<double>(water.Waves[wave].Amplitude));
			}
			double lowest = std::numeric_limits<double>::infinity();
			double highest = -std::numeric_limits<double>::infinity();
			for (const glm::dvec3& corner : corners)
			{
				lowest = std::min(lowest, corner.y);
				highest = std::max(highest, corner.y);
			}
			if (water.Shape != RenderWaterShape::Rectangle)
			{
				if (lowest <= base + amplitudes)
					return true;
				continue;
			}
			if (lowest > base + std::min(amplitudes, static_cast<double>(water.Headroom)) || highest < base - static_cast<double>(water.Depth))
				continue;
			// The corners' bounds in the Rectangle's local X and Z against its footprint, widened.
			const glm::dvec2 axisX = Utils::GetWaterAxisX(world);
			const glm::dvec2 axisZ(-axisX.y, axisX.x);
			const glm::dvec2 half(static_cast<double>(water.Size.x) * 0.5 * glm::length(glm::dvec3(world[0])) + sideways,
				static_cast<double>(water.Size.y) * 0.5 * glm::length(glm::dvec3(world[2])) + sideways);
			glm::dvec2 minimum(std::numeric_limits<double>::infinity());
			glm::dvec2 maximum(-std::numeric_limits<double>::infinity());
			for (const glm::dvec3& corner : corners)
			{
				const glm::dvec2 relative = glm::dvec2(corner.x, corner.z) - glm::dvec2(world[3].x, world[3].z);
				const glm::dvec2 local(glm::dot(relative, axisX), glm::dot(relative, axisZ));
				minimum = glm::min(minimum, local);
				maximum = glm::max(maximum, local);
			}
			if (maximum.x >= -half.x && minimum.x <= half.x && maximum.y >= -half.y && minimum.y <= half.y)
				return true;
		}
		return false;
	}

	WaterRecord MakeWaterRecord(const WaterItem& water, const glm::dvec2& origin, const glm::dvec2& camera, uint32_t waterCount, uint32_t planarSlot)
	{
		const WaterItem defaults{};
		const glm::dmat4 world(water.World);
		const glm::dvec2 waterOrigin(world[3].x, world[3].z);
		const glm::dvec2 axisX = Utils::GetWaterAxisX(world);
		const bool rectangle = water.Shape == RenderWaterShape::Rectangle;

		WaterRecord record{};
		record.PreviousFromCurrent = glm::mat4(1.0f);
		if (water.PreviousWorld.has_value() && *water.PreviousWorld != water.World)
		{
			record.PreviousFromCurrent = glm::mat4(glm::dmat4(*water.PreviousWorld) * glm::inverse(world));
			record.Flags |= WaterRecordFlagMoved;
		}
		const uint32_t waveCount = std::min(water.WaveCount, MaxWaterRecordWaves);
		for (uint32_t index = 0; index < waveCount; ++index)
		{
			const WaterWaveItem& wave = water.Waves[index];
			const double offset = static_cast<double>(wave.WaveNumber)
				* (static_cast<double>(wave.Direction.x) * (origin.x - waterOrigin.x) + static_cast<double>(wave.Direction.y) * (origin.y - waterOrigin.y));
			const float previous = water.PreviousPhases.has_value() ? (*water.PreviousPhases)[index] : wave.Phase;
			if (previous != wave.Phase)
				record.Flags |= WaterRecordFlagAdvanced;
			record.Waves[index] = glm::vec4(wave.Direction, wave.WaveNumber, wave.Amplitude);
			record.WavePhases[index] = glm::vec4(wave.Steepness, static_cast<float>(Utils::WrapPhase(offset + static_cast<double>(wave.Phase))),
				static_cast<float>(Utils::WrapPhase(offset + static_cast<double>(previous))), 0.0f);
		}
		record.Origin = glm::vec2(origin);
		record.CameraOffset = glm::vec2(camera - origin);
		record.FrameOrigin = glm::vec2(waterOrigin - origin);
		record.AxisX = glm::vec2(axisX);
		if (rectangle)
		{
			record.HalfExtents = glm::vec2(static_cast<double>(water.Size.x) * 0.5 * glm::length(glm::dvec3(world[0])),
				static_cast<double>(water.Size.y) * 0.5 * glm::length(glm::dvec3(world[2])));
			record.Depth = water.Depth;
			record.Headroom = water.Headroom;
			record.Flags |= WaterRecordFlagRectangle;
		}
		record.BaseHeight = water.World[3].y;
		record.WaveCount = waveCount;
		record.WaterCount = waterCount;
		record.ShallowColor = glm::vec4(water.ShallowColor, Utils::ClampFinite(water.Clarity, 0.01f, 1000.0f, defaults.Clarity));
		record.DeepColor = glm::vec4(water.DeepColor, Utils::ClampFinite(water.Roughness, 0.02f, 1.0f, defaults.Roughness));
		record.Look = glm::vec4(Utils::ClampFinite(water.RefractionStrength, 0.0f, 1.0f, defaults.RefractionStrength),
			Utils::ClampFinite(water.FoamAmount, 0.0f, 1.0f, defaults.FoamAmount), Utils::ClampFinite(water.ShoreFoamDepth, 0.0f, 10.0f, defaults.ShoreFoamDepth),
			Utils::ClampFinite(water.DetailNormalStrength, 0.0f, 1.0f, defaults.DetailNormalStrength));
		record.Detail = glm::vec4(Utils::ClampFinite(water.DetailScale, 0.1f, 1000.0f, defaults.DetailScale), water.DetailOffset, 0.0f);
		record.Underwater = glm::vec4(water.UnderwaterColor, Utils::ClampFinite(water.UnderwaterVisibility, 0.1f, 10000.0f, defaults.UnderwaterVisibility));
		record.PickId = water.PickId;
		record.PlanarSlot = planarSlot;
		return record;
	}

}
