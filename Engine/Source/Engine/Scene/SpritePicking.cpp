#include "EnginePCH.h"
#include "Engine/Scene/SpritePicking.h"

#include "Engine/Asset/TilemapDrawOrder.h"
#include "Engine/Scene/Private/SpriteDrawing.h"
#include "Engine/Scene/Private/TilemapDrawing.h"
#include "Engine/Scene/Scene.h"

#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		static bool IsFiniteSpriteRayVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// The request's ray and distances (both PickSprite overloads). Errors: InvalidArgument as PickSprite documents.
		static Status CheckSpritePickRequest(const SpritePickRequest& request)
		{
			if (!IsFiniteSpriteRayVector(request.Ray.Origin) || !IsFiniteSpriteRayVector(request.Ray.Direction))
				return MakeError(ErrorCode::InvalidArgument, "ray origin and direction must be finite");
			const double length = glm::length(glm::dvec3(request.Ray.Direction));
			if (length == 0 || !std::isfinite(length))
				return MakeError(ErrorCode::InvalidArgument, "ray direction must be nonzero");
			if (!std::isfinite(request.MaxDistance) || !std::isfinite(request.MinDistance) || request.MinDistance < 0
				|| request.MaxDistance <= request.MinDistance)
				return MakeError(ErrorCode::InvalidArgument, "ray distances must define a finite nonnegative increasing interval");
			return {};
		}

		// The quad's two triangles in QuadBatcher order (corners 0, 1, 2 and 0, 2, 3), so triangle 0 holds the top-right corner
		// and triangle 1 the bottom-left one.
		constexpr std::array<std::array<size_t, 3>, 2> SpriteQuadTriangles = { { { 0, 1, 2 }, { 0, 2, 3 } } };

		// The hit of the ray (`origin`, unit `direction`) on the world quad `corners` within [minimum, maximum], two-sided, or
		// nullopt: SceneRaycast's triangle test in double precision.
		static std::optional<SceneRaycastHit> IntersectSpriteQuad(const std::array<glm::dvec3, 4>& corners, const glm::dvec3& origin, const glm::dvec3& direction,
			double minimum, double maximum, UUID entity)
		{
			for (uint32_t triangle = 0; triangle < SpriteQuadTriangles.size(); ++triangle)
			{
				const glm::dvec3& a = corners[SpriteQuadTriangles[triangle][0]];
				const glm::dvec3 edge1 = corners[SpriteQuadTriangles[triangle][1]] - a;
				const glm::dvec3 edge2 = corners[SpriteQuadTriangles[triangle][2]] - a;
				const glm::dvec3 p = glm::cross(direction, edge2);
				const double divisor = glm::dot(edge1, p);
				if (divisor == 0)
					continue;
				const glm::dvec3 displacement = origin - a;
				const double u = glm::dot(displacement, p) / divisor;
				const glm::dvec3 q = glm::cross(displacement, edge1);
				const double v = glm::dot(direction, q) / divisor;
				const double distance = glm::dot(edge2, q) / divisor;
				if (!(u >= 0 && v >= 0 && u + v <= 1 && distance >= minimum && distance <= maximum))
					continue;
				glm::dvec3 normal = glm::normalize(glm::cross(edge1, edge2));
				if (glm::dot(normal, direction) > 0)
					normal = -normal;
				return SceneRaycastHit{ entity, static_cast<float>(distance), glm::vec3(origin + distance * direction), glm::vec3(normal),
					glm::vec3(1 - u - v, u, v), 0, triangle };
			}
			return std::nullopt;
		}

		// The hit of the ray (`origin`, unit `direction`) within the request's distances on the quad of local corners `local`
		// drawn with the world matrix `world` by `entity`, or nullopt (also for a singular or non-finite matrix or corner).
		static std::optional<SceneRaycastHit> IntersectSpriteAt(const glm::mat4& world, const std::array<glm::vec2, 4>& local, const glm::dvec3& origin,
			const glm::dvec3& direction, const SpritePickRequest& request, UUID entity)
		{
			const glm::dmat4 matrix(world);
			const double determinant = glm::determinant(matrix);
			if (!std::isfinite(determinant) || determinant == 0)
				return std::nullopt;
			std::array<glm::dvec3, 4> corners{};
			for (size_t index = 0; index < corners.size(); ++index)
			{
				corners[index] = glm::dvec3(matrix * glm::dvec4(local[index].x, local[index].y, 0.0, 1.0));
				if (!IsFiniteSpriteRayVector(glm::vec3(corners[index])))
					return std::nullopt;
			}
			return IntersectSpriteQuad(corners, origin, direction, request.MinDistance, request.MaxDistance, entity);
		}

		// Where the ray (`origin`, unit `direction`) meets the plane z = 0 of the space `world` maps to world space, in that
		// space's X and Y; nullopt for a singular or non-finite matrix, or a ray parallel to the plane (which crosses no quad
		// of it: IntersectSpriteQuad's test refuses a ray in a quad's plane).
		static std::optional<glm::dvec2> IntersectLocalPlane(const glm::mat4& world, const glm::dvec3& origin, const glm::dvec3& direction)
		{
			const glm::dmat4 matrix(world);
			const double determinant = glm::determinant(matrix);
			if (!std::isfinite(determinant) || determinant == 0)
				return std::nullopt;
			const glm::dmat4 inverse = glm::inverse(matrix);
			const glm::dvec3 localOrigin(inverse * glm::dvec4(origin, 1.0));
			const glm::dvec3 localDirection(inverse * glm::dvec4(direction, 0.0));
			if (!std::isfinite(localDirection.z) || localDirection.z == 0)
				return std::nullopt;
			const glm::dvec2 point = glm::dvec2(localOrigin) - (localOrigin.z / localDirection.z) * glm::dvec2(localDirection);
			if (!std::isfinite(point.x) || !std::isfinite(point.y))
				return std::nullopt;
			return point;
		}

		// The last-drawn tile of `tilemap` the ray crosses (C21.2, decision 24): its tiles backwards in their draw order, each
		// the sprite TilemapRendererComponent.h describes, a hit naming the tilemap's entity. M21's milestone review: every tile
		// lies in the tilemap's local plane z = 0, so the ray can cross only the tiles over the one point where it meets that
		// plane, and only the chunks whose bounds hold that point are walked: a chunk's bounds are the rectangle of its cells'
		// centres widened by the extents of the palette's quads about their centres (every tile it can hold lies inside),
		// and by one cell for the rounding between the point and the tiles' world quads. The tiles of those chunks are tested
		// exactly as before, in the one draw order, so the hit is the one a walk over every tile finds.
		static std::optional<SceneRaycastHit> PickTilemapTile(const TilemapItem& tilemap, AssetManager& assets, const glm::dvec3& origin,
			const glm::dvec3& direction, const SpritePickRequest& request)
		{
			const std::optional<TilemapTiles> tiles = ResolveTilemapTiles(assets, tilemap);
			if (!tiles.has_value())
				return std::nullopt;
			const std::optional<glm::dvec2> point = IntersectLocalPlane(tilemap.World, origin, direction);
			if (!point.has_value())
				return std::nullopt;
			// The extents of every palette entry's quad about its cell's centre.
			std::optional<std::pair<glm::dvec2, glm::dvec2>> extents;
			for (const std::optional<std::array<glm::vec2, 4>>& quad : tiles->Palette)
			{
				if (!quad.has_value())
					continue;
				for (const glm::vec2& corner : *quad)
				{
					const glm::dvec2 offset(corner);
					extents = extents.has_value() ? std::pair(glm::min(extents->first, offset), glm::max(extents->second, offset)) : std::pair(offset, offset);
				}
			}
			if (!extents.has_value())
				return std::nullopt;
			const TilemapData& data = *tiles->Tilemap;
			const glm::dvec2 cell(data.CellSize);
			const double chunkSize = static_cast<double>(data.ChunkSize);
			const glm::dvec2 low = extents->first - glm::abs(cell);
			const glm::dvec2 high = extents->second + glm::abs(cell);
			const auto includeChunk = [&data, &cell, chunkSize, &low, &high, &point](size_t chunk)
			{
				const TilemapChunk& bounds = data.Chunks[chunk];
				const glm::dvec2 first((bounds.X * chunkSize + 0.5) * cell.x, (bounds.Y * chunkSize + 0.5) * cell.y);
				const glm::dvec2 last(((bounds.X + 1) * chunkSize - 0.5) * cell.x, ((bounds.Y + 1) * chunkSize - 0.5) * cell.y);
				const glm::dvec2 minimum = glm::min(first, last) + low;
				const glm::dvec2 maximum = glm::max(first, last) + high;
				return point->x >= minimum.x && point->x <= maximum.x && point->y >= minimum.y && point->y <= maximum.y;
			};
			std::optional<SceneRaycastHit> hit;
			ForEachTilemapTileInDrawOrder(*tiles->Tilemap, true, includeChunk,
				[&tilemap, &tiles, &origin, &direction, &request, &hit](size_t /*chunk*/, glm::ivec2 cell, uint32_t value)
			{
				if (value > tiles->Palette.size() || !tiles->Palette[value - 1].has_value())
					return true;
				const glm::mat4 world = GetTilemapTileWorld(tilemap.World, *tiles->Tilemap, cell);
				hit = IntersectSpriteAt(world, *tiles->Palette[value - 1], origin, direction, request, tilemap.Entity);
				return !hit.has_value();
			});
			return hit;
		}

	}

	Result<std::optional<SceneRaycastHit>> PickSprite(const Scene& scene, AssetManager& assets, const SpritePickRequest& request)
	{
		ENGINE_TRY(Utils::CheckSpritePickRequest(request));
		if (!std::isfinite(request.Alpha) || request.Alpha < 0 || request.Alpha > 1)
			return MakeError(ErrorCode::InvalidArgument, "ray alpha must be in [0,1]");
		const std::vector<SpriteItem> sprites = Utils::CollectSprites(scene, request.SortingLayers, request.Alpha);
		const std::vector<TilemapItem> tilemaps = Utils::CollectTilemaps(scene, request.SortingLayers, request.Alpha);
		return PickSprite(sprites, tilemaps, assets, request);
	}

	Result<std::optional<SceneRaycastHit>> PickSprite(std::span<const SpriteItem> sprites, AssetManager& assets, const SpritePickRequest& request)
	{
		return PickSprite(sprites, std::span<const TilemapItem>(), assets, request);
	}

	Result<std::optional<SceneRaycastHit>> PickSprite(std::span<const SpriteItem> sprites, std::span<const TilemapItem> tilemaps, AssetManager& assets,
		const SpritePickRequest& request)
	{
		ENGINE_TRY(Utils::CheckSpritePickRequest(request));
		const glm::dvec3 origin(request.Ray.Origin);
		const glm::dvec3 direction = glm::dvec3(request.Ray.Direction) / glm::length(glm::dvec3(request.Ray.Direction));

		// The pixel shows the last sprite or tile drawn over it: the one draw order backwards (a tilemap draws before
		// Sprites[DrawPosition]), the first hit wins whatever its distance.
		size_t nextTilemap = tilemaps.size();
		const auto pickTilemapsFrom = [&tilemaps, &nextTilemap, &assets, &origin, &direction, &request](size_t position)
		{
			while (nextTilemap > 0 && tilemaps[nextTilemap - 1].DrawPosition >= position)
			{
				--nextTilemap;
				if (std::optional<SceneRaycastHit> hit = Utils::PickTilemapTile(tilemaps[nextTilemap], assets, origin, direction, request))
					return hit;
			}
			return std::optional<SceneRaycastHit>();
		};
		if (std::optional<SceneRaycastHit> hit = pickTilemapsFrom(sprites.size()))
			return hit;
		for (size_t index = sprites.size(); index-- > 0;)
		{
			const SpriteItem& sprite = sprites[index];
			if (const std::optional<std::array<glm::vec2, 4>> local = Utils::ResolveSpriteCorners(assets, sprite))
			{
				if (std::optional<SceneRaycastHit> hit = Utils::IntersectSpriteAt(sprite.World, *local, origin, direction, request, sprite.Entity))
					return hit;
			}
			if (std::optional<SceneRaycastHit> hit = pickTilemapsFrom(index))
				return hit;
		}
		return std::optional<SceneRaycastHit>();
	}

}
