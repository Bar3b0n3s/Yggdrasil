#include "EditorPCH.h"
#include "EditorCore/Viewport/TilemapPainting.h"

#include "EditorCore/Automation/TilemapMethods.h"
#include "Engine/Asset/TilemapData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <utility>

namespace Engine {

	namespace {

		[[nodiscard]] bool IsFinite(const glm::dvec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// The cell of the local coordinate `value` for cells of `size` units: floored, or nullopt outside
		// [-MaxTilemapCellCoordinate, MaxTilemapCellCoordinate).
		[[nodiscard]] std::optional<int32_t> ToCellCoordinate(double value, double size)
		{
			const double cell = std::floor(value / size);
			if (!std::isfinite(cell) || cell < -static_cast<double>(MaxTilemapCellCoordinate) || cell >= static_cast<double>(MaxTilemapCellCoordinate))
				return std::nullopt;
			return static_cast<int32_t>(cell);
		}

	}

	std::optional<glm::ivec2> IntersectTilemapCell(const RenderRay& ray, const glm::mat4& tilemapWorld, const glm::vec2& cellSize)
	{
		if (!std::isfinite(cellSize.x) || !std::isfinite(cellSize.y) || cellSize.x <= 0.0f || cellSize.y <= 0.0f)
			return std::nullopt;
		// In double precision, so a cursor a hair beside a cell's edge lands in the cell it shows: the inverse of an entity's
		// world matrix (an affine transform) takes the ray into the tilemap's local space, where the plane is z = 0.
		const glm::dmat4 world(tilemapWorld);
		const double determinant = glm::determinant(world);
		if (!std::isfinite(determinant) || determinant == 0.0)
			return std::nullopt;
		const glm::dmat4 inverse = glm::inverse(world);
		const glm::dvec3 origin(inverse * glm::dvec4(glm::dvec3(ray.Origin), 1.0));
		const glm::dvec3 direction(inverse * glm::dvec4(glm::dvec3(ray.Direction), 0.0));
		if (!IsFinite(origin) || !IsFinite(direction) || direction.z == 0.0)
			return std::nullopt;
		// An affine map keeps the ray's parameter, so a distance below 0 lies behind the ray's origin in both spaces.
		const double distance = -origin.z / direction.z;
		if (!(distance >= 0.0))
			return std::nullopt;
		const std::optional<int32_t> x = ToCellCoordinate(origin.x + direction.x * distance, cellSize.x);
		const std::optional<int32_t> y = ToCellCoordinate(origin.y + direction.y * distance, cellSize.y);
		if (!x || !y)
			return std::nullopt;
		return glm::ivec2(*x, *y);
	}

	std::vector<glm::ivec2> GetTilemapStrokeCells(glm::ivec2 from, glm::ivec2 to)
	{
		// Bresenham's line in 64-bit steps, so no difference of two coordinates overflows.
		const int64_t deltaX = std::llabs(static_cast<int64_t>(to.x) - from.x);
		const int64_t deltaY = -std::llabs(static_cast<int64_t>(to.y) - from.y);
		const int64_t stepX = from.x < to.x ? 1 : -1;
		const int64_t stepY = from.y < to.y ? 1 : -1;
		int64_t error = deltaX + deltaY;
		int64_t x = from.x;
		int64_t y = from.y;
		std::vector<glm::ivec2> cells;
		cells.reserve(static_cast<size_t>(std::max(deltaX, -deltaY)) + 1);
		while (true)
		{
			cells.emplace_back(static_cast<int32_t>(x), static_cast<int32_t>(y));
			if (x == to.x && y == to.y)
				break;
			const int64_t twice = 2 * error;
			if (twice >= deltaY)
			{
				error += deltaY;
				x += stepX;
			}
			if (twice <= deltaX)
			{
				error += deltaX;
				y += stepY;
			}
		}
		return cells;
	}

	Result<Json> MakeTilemapPaintParams(const TilemapPaintRequest& request)
	{
		if (!request.Tilemap.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "a tile brush stroke needs a valid tilemap handle");
		if (request.Cells.empty())
			return MakeError(ErrorCode::InvalidArgument, "a tile brush stroke needs at least one cell");
		// Each cell once, at its first place in the stroke: a stroke that crosses a cell again paints it once.
		std::set<std::pair<int32_t, int32_t>> seen;
		Json cells = Json::array();
		for (const glm::ivec2 cell : request.Cells)
		{
			if (!seen.emplace(cell.x, cell.y).second)
				continue;
			if (seen.size() > MaxTilemapPaintCells)
				return MakeError(ErrorCode::InvalidArgument, "a tile brush stroke crosses more than {} cells; paint it in shorter strokes", MaxTilemapPaintCells);
			cells.push_back(Json{ { "x", cell.x }, { "y", cell.y }, { "tile", request.Tile } });
		}
		return Json{ { "tilemap", request.Tilemap.ToString() }, { "cells", std::move(cells) } };
	}

}
