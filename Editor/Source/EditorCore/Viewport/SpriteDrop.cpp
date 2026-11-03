#include "EditorPCH.h"
#include "EditorCore/Viewport/SpriteDrop.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>

namespace Engine {

	std::optional<glm::vec3> IntersectSpritePlane(const RenderRay& ray)
	{
		const glm::dvec3 origin(ray.Origin);
		const glm::dvec3 direction(ray.Direction);
		if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) || !std::isfinite(direction.x)
			|| !std::isfinite(direction.y) || !std::isfinite(direction.z) || direction.z == 0.0)
			return std::nullopt;
		const double distance = -origin.z / direction.z;
		if (distance < 0.0)
			return std::nullopt;
		const glm::dvec3 point = origin + direction * distance;
		if (!std::isfinite(point.x) || !std::isfinite(point.y) || std::abs(point.x) > std::numeric_limits<float>::max()
			|| std::abs(point.y) > std::numeric_limits<float>::max())
			return std::nullopt;
		// Exactly on the plane, whatever the rounding of the step along the ray.
		return glm::vec3(static_cast<float>(point.x), static_cast<float>(point.y), 0.0f);
	}

	Result<Json> MakeSpriteDropParams(const SpriteDropRequest& request)
	{
		if (!request.Asset.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "a dropped sprite needs a valid asset handle");
		if (request.Type != AssetType::SpriteSheet && request.Type != AssetType::Texture)
			return MakeError(ErrorCode::InvalidArgument, "only a sprite sheet or a texture becomes a sprite, not a {}", AssetTypeToString(request.Type));
		if (!std::isfinite(request.Position.x) || !std::isfinite(request.Position.y) || !std::isfinite(request.Position.z))
			return MakeError(ErrorCode::InvalidArgument, "a dropped sprite needs a finite position");
		// A sheet draws its frame 0 (SpriteRenderer's default Frame); a texture draws whole.
		const Json sprite{ { request.Type == AssetType::SpriteSheet ? "Sheet" : "Texture", request.Asset.ToString() } };
		return Json{ { "name", "Sprite" },
			{ "target", request.PlayScene ? "play" : "edit" },
			{ "ifRevision", request.Revision },
			{ "components",
				{ { "Transform", { { "Translation", Json::array({ request.Position.x, request.Position.y, request.Position.z }) } } }, { "SpriteRenderer", sprite } } } };
	}

}
