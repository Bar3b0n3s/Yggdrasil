#include "EnginePCH.h"
#include "Engine/Scene/SpritePicking.h"

#include "Engine/Scene/Private/SpriteDrawing.h"
#include "Engine/Scene/Scene.h"

#include <array>
#include <cmath>
#include <optional>
#include <ranges>
#include <span>
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

	}

	Result<std::optional<SceneRaycastHit>> PickSprite(const Scene& scene, AssetManager& assets, const SpritePickRequest& request)
	{
		ENGINE_TRY(Utils::CheckSpritePickRequest(request));
		if (!std::isfinite(request.Alpha) || request.Alpha < 0 || request.Alpha > 1)
			return MakeError(ErrorCode::InvalidArgument, "ray alpha must be in [0,1]");
		const std::vector<SpriteItem> sprites = Utils::CollectSprites(scene, request.SortingLayers, request.Alpha);
		return PickSprite(sprites, assets, request);
	}

	Result<std::optional<SceneRaycastHit>> PickSprite(std::span<const SpriteItem> sprites, AssetManager& assets, const SpritePickRequest& request)
	{
		ENGINE_TRY(Utils::CheckSpritePickRequest(request));
		const glm::dvec3 origin(request.Ray.Origin);
		const glm::dvec3 direction = glm::dvec3(request.Ray.Direction) / glm::length(glm::dvec3(request.Ray.Direction));

		// The pixel shows the last sprite drawn over it: the draw order backwards, the first hit wins whatever its distance.
		for (const SpriteItem& sprite : std::views::reverse(sprites))
		{
			const glm::dmat4 world(sprite.World);
			const double determinant = glm::determinant(world);
			if (!std::isfinite(determinant) || determinant == 0)
				continue;
			const std::optional<std::array<glm::vec2, 4>> local = Utils::ResolveSpriteCorners(assets, sprite);
			if (!local.has_value())
				continue;
			std::array<glm::dvec3, 4> corners{};
			bool finite = true;
			for (size_t index = 0; index < corners.size(); ++index)
			{
				corners[index] = glm::dvec3(world * glm::dvec4((*local)[index].x, (*local)[index].y, 0.0, 1.0));
				finite = finite && Utils::IsFiniteSpriteRayVector(glm::vec3(corners[index]));
			}
			if (!finite)
				continue;
			if (std::optional<SceneRaycastHit> hit = Utils::IntersectSpriteQuad(corners, origin, direction, request.MinDistance, request.MaxDistance, sprite.Entity))
				return hit;
		}
		return std::optional<SceneRaycastHit>();
	}

	Result<std::optional<SceneRaycastHit>> PickSprite(std::span<const SpriteItem> /*sprites*/, std::span<const TilemapItem> /*tilemaps*/,
		AssetManager& /*assets*/, const SpritePickRequest& /*request*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "tilemap picking (Docs/Decisions/0032-m21-contract.md decision 24, stream T1)");
	}

}
