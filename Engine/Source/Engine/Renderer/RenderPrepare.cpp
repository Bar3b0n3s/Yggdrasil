#include "EnginePCH.h"
#include "Engine/Renderer/RenderPrepare.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>
#include <vector>

// The CPU rules of §8.3 pass 1 (RenderPrepare.h). The view volume is tested in clip space for boxes (all eight corners
// outside one plane) and against the view-space planes of the projection for spheres (Gribb and Hartmann's extraction:
// each plane is a sum or difference of the projection's rows, normalized), so both shapes agree with the reverse-Z
// conventions of §8.3: -w <= x, y <= w and 0 <= z <= w, where z <= w is the near plane and z >= 0 an orthographic far plane
// (a perspective projection's z row has no view-space term, so that plane is degenerate and skipped: its far plane is at
// infinity, and FarClip is the culling distance of both projections).

namespace Engine {

	namespace Utils {

		// A sphere whose radius may be zero.
		struct BoundingSphere
		{
			glm::vec3 Centre = glm::vec3(0.0f);
			float Radius = 0.0f;
		};

		[[nodiscard]] static bool IsFinite(const glm::vec3& vector)
		{
			return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
		}

		[[nodiscard]] static bool IsFinite(const glm::mat4& matrix)
		{
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

		// Row `row` of `matrix` (glm is column-major: matrix[column][row]).
		[[nodiscard]] static glm::vec4 GetRow(const glm::mat4& matrix, glm::length_t row)
		{
			return glm::vec4(matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]);
		}

		// Whether `sphere` (world space) lies entirely outside the view volume of `camera`: beyond one of the projection's
		// planes by more than its radius, or farther than FarClip along the view direction.
		[[nodiscard]] static bool IsSphereOutsideView(const BoundingSphere& sphere, const CameraData& camera)
		{
			const glm::vec4 centre = camera.View * glm::vec4(sphere.Centre, 1.0f);
			if (-centre.z - sphere.Radius > camera.FarClip)
				return true;
			const glm::mat4& projection = camera.Projection;
			const glm::vec4 x = GetRow(projection, 0);
			const glm::vec4 y = GetRow(projection, 1);
			const glm::vec4 z = GetRow(projection, 2);
			const glm::vec4 w = GetRow(projection, 3);
			const std::array<glm::vec4, 6> planes = { w + x, w - x, w + y, w - y, w - z, z };
			for (const glm::vec4& plane : planes)
			{
				const float length = glm::length(glm::vec3(plane));
				if (!(length > 1e-12f))
					continue; // degenerate: the infinite far plane of a perspective projection
				if (glm::dot(plane, centre) / length < -sphere.Radius)
					return true;
			}
			return false;
		}

		// The bounding sphere of the region a point or spot light reaches: its Range sphere, or for a spot light the
		// tighter sphere of its cone (the cone's base circle when the outer half-angle exceeds 45 degrees, else the sphere
		// through the apex and the base circle).
		[[nodiscard]] static BoundingSphere GetLightSphere(const LightData& light)
		{
			if (light.Type == RenderLightType::Spot)
			{
				const float halfAngle = glm::radians(light.OuterConeAngle);
				if (halfAngle > 0.0f && halfAngle < glm::half_pi<float>())
				{
					const glm::vec3 direction = glm::normalize(light.Direction);
					const float cosine = std::cos(halfAngle);
					if (halfAngle > glm::quarter_pi<float>())
						return { .Centre = light.Position + direction * (light.Range * cosine), .Radius = light.Range * std::sin(halfAngle) };
					const float radius = light.Range / (2.0f * cosine);
					return { .Centre = light.Position + direction * radius, .Radius = radius };
				}
			}
			return { .Centre = light.Position, .Radius = light.Range };
		}

		// Whether `light` contributes anything at all: finite values, a non-zero radiance and, for point and spot lights, a
		// positive range (and, for directional and spot lights, a direction).
		[[nodiscard]] static bool IsShadable(const LightData& light)
		{
			const glm::vec3 radiance = light.Color * light.Intensity;
			if (!IsFinite(light.Color) || !std::isfinite(light.Intensity) || !IsFinite(radiance) || std::max({ radiance.r, radiance.g, radiance.b }) <= 0.0f)
				return false;
			const bool hasDirection = light.Type != RenderLightType::Point;
			if (hasDirection && (!IsFinite(light.Direction) || !(glm::length(light.Direction) > 0.0f)))
				return false;
			if (light.Type == RenderLightType::Directional)
				return true;
			if (!IsFinite(light.Position) || !std::isfinite(light.Range) || !(light.Range > 0.0f) || !std::isfinite(light.SourceRadius))
				return false;
			return light.Type != RenderLightType::Spot || (std::isfinite(light.InnerConeAngle) && std::isfinite(light.OuterConeAngle));
		}

		// A visible point or spot light with its rank.
		struct RankedLight
		{
			float Importance = 0.0f;
			UUID Entity{};
			uint32_t Index = 0;
		};

	}

	bool IsOutsideView(const Aabb& bounds, const glm::mat4& world, const CameraData& camera)
	{
		if (bounds.IsEmpty())
			return false;
		const glm::mat4 worldView = camera.View * world;
		const glm::mat4 worldViewProjection = camera.Projection * worldView;
		std::array<bool, 7> allOutside = { true, true, true, true, true, true, true };
		for (uint32_t corner = 0; corner < 8; ++corner)
		{
			const glm::vec4 local((corner & 1U) != 0 ? bounds.Max.x : bounds.Min.x, (corner & 2U) != 0 ? bounds.Max.y : bounds.Min.y,
				(corner & 4U) != 0 ? bounds.Max.z : bounds.Min.z, 1.0f);
			const glm::vec4 clip = worldViewProjection * local;
			const float viewDistance = -(worldView * local).z;
			allOutside[0] = allOutside[0] && clip.x < -clip.w;
			allOutside[1] = allOutside[1] && clip.x > clip.w;
			allOutside[2] = allOutside[2] && clip.y < -clip.w;
			allOutside[3] = allOutside[3] && clip.y > clip.w;
			allOutside[4] = allOutside[4] && clip.z > clip.w;
			allOutside[5] = allOutside[5] && clip.z < 0.0f;
			allOutside[6] = allOutside[6] && viewDistance > camera.FarClip;
		}
		return std::ranges::any_of(allOutside, [](bool outside)
		{
			return outside;
		});
	}

	LightCullResult CullLights(std::span<const LightData> lights, const CameraData& camera)
	{
		LightCullResult result;
		std::vector<Utils::RankedLight> ranked;
		const bool hasView = Utils::IsFinite(camera.View) && Utils::IsFinite(camera.Projection) && Utils::IsFinite(camera.Position);
		for (uint32_t index = 0; index < lights.size(); ++index)
		{
			const LightData& light = lights[index];
			if (!Utils::IsShadable(light))
			{
				++result.Culled;
				continue;
			}
			if (light.Type == RenderLightType::Directional)
			{
				result.Visible.push_back(index);
				continue;
			}
			const Utils::BoundingSphere sphere = Utils::GetLightSphere(light);
			if (!hasView || Utils::IsSphereOutsideView(sphere, camera))
			{
				++result.Culled;
				continue;
			}
			const float distance = std::max(glm::length(sphere.Centre - camera.Position) - sphere.Radius, 1.0f);
			const float importance = light.Intensity * std::max({ light.Color.r, light.Color.g, light.Color.b }) / (distance * distance);
			ranked.push_back({ .Importance = importance, .Entity = light.Entity, .Index = index });
		}

		// Directional lights come first in snapshot order; the others by descending importance, ties by entity UUID (then
		// by snapshot order, for lights of one entity or without one).
		std::ranges::sort(ranked, [](const Utils::RankedLight& left, const Utils::RankedLight& right)
		{
			if (left.Importance != right.Importance)
				return left.Importance > right.Importance;
			return std::tie(left.Entity, left.Index) < std::tie(right.Entity, right.Index);
		});
		for (const Utils::RankedLight& light : ranked)
		{
			if (result.Visible.size() < MaxVisibleLights)
				result.Visible.push_back(light.Index);
			else
				++result.Dropped;
		}
		if (result.Visible.size() > MaxVisibleLights)
		{
			// More directional lights than the limit: the later ones are dropped.
			result.Dropped += static_cast<uint32_t>(result.Visible.size() - MaxVisibleLights);
			result.Visible.resize(MaxVisibleLights);
		}
		return result;
	}

	void SortOpaqueDraws(std::span<OpaqueSortKey> keys)
	{
		std::ranges::sort(keys, [](const OpaqueSortKey& left, const OpaqueSortKey& right)
		{
			return std::tie(left.Pipeline, left.Material, left.Mesh, left.DrawIndex) < std::tie(right.Pipeline, right.Material, right.Mesh, right.DrawIndex);
		});
	}

	void SortTransparentDraws(std::span<TransparentSortKey> keys)
	{
		// A non-finite depth sorts as the farthest; the comparison stays a strict weak order (NaN never compares).
		const auto depthOf = [](const TransparentSortKey& key)
		{
			return std::isfinite(key.ViewDepth) ? key.ViewDepth : std::numeric_limits<float>::infinity();
		};
		std::ranges::sort(keys, [&depthOf](const TransparentSortKey& left, const TransparentSortKey& right)
		{
			const float leftDepth = depthOf(left);
			const float rightDepth = depthOf(right);
			if (leftDepth != rightDepth)
				return leftDepth > rightDepth;
			return std::tie(left.Entity, left.Submesh, left.DrawIndex) < std::tie(right.Entity, right.Submesh, right.DrawIndex);
		});
	}

}
