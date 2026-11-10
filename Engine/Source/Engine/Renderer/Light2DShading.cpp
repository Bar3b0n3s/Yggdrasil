#include "EnginePCH.h"
#include "Engine/Renderer/Light2DShading.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace Engine {

	namespace Utils {

		static bool IsInRange(float value, float minimum, float maximum)
		{
			return std::isfinite(value) && value >= minimum && value <= maximum;
		}

		// The clip rectangle's planes as (a, b, c, d), a point (x, y, z, w) inside when ax + by + cz + dw >= 0: -w <= x <= w,
		// -w <= y <= w, and w >= 0 (in front of the eye). Depth is held inside the clip range by the pass, so it has no plane.
		constexpr std::array<glm::vec4, 5> Light2DClipPlanes = { {
			{ 1.0f, 0.0f, 0.0f, 1.0f },
			{ -1.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 1.0f, 0.0f, 1.0f },
			{ 0.0f, -1.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
		} };

	}

	bool IsLight2DValid(const Light2DItem& light)
	{
		const bool color = Utils::IsInRange(light.Color.r, 0.0f, 1.0f) && Utils::IsInRange(light.Color.g, 0.0f, 1.0f)
			&& Utils::IsInRange(light.Color.b, 0.0f, 1.0f);
		if (!color || !Utils::IsInRange(light.Intensity, 0.0f, MaxLight2DIntensity))
			return false;
		switch (light.Type)
		{
			case RenderLight2DType::Global: return true;
			case RenderLight2DType::Point:
				return std::isfinite(light.Position.x) && std::isfinite(light.Position.y) && std::isfinite(light.Position.z)
					&& Utils::IsInRange(light.Radius, MinLight2DDistance, MaxLight2DDistance) && Utils::IsInRange(light.Height, MinLight2DDistance, MaxLight2DDistance)
					&& Utils::IsInRange(light.Falloff, 0.0f, 1.0f);
		}
		return false;
	}

	bool Light2DReachesView(const Light2DItem& light, const glm::mat4& viewProjection)
	{
		if (!IsLight2DValid(light))
			return false;
		if (light.Type == RenderLight2DType::Global)
			return true;
		const glm::vec3 center = light.Position;
		const float radius = light.Radius;
		const std::array<glm::vec4, 4> corners = {
			viewProjection * glm::vec4(center.x - radius, center.y - radius, center.z, 1.0f),
			viewProjection * glm::vec4(center.x + radius, center.y - radius, center.z, 1.0f),
			viewProjection * glm::vec4(center.x + radius, center.y + radius, center.z, 1.0f),
			viewProjection * glm::vec4(center.x - radius, center.y + radius, center.z, 1.0f),
		};
		return std::ranges::none_of(Utils::Light2DClipPlanes, [&corners](const glm::vec4& plane)
		{
			return std::ranges::all_of(corners, [&plane](const glm::vec4& corner)
			{
				return glm::dot(plane, corner) < 0.0f;
			});
		});
	}

	glm::vec3 EvaluateLight2D(const Light2DItem& light, glm::vec2 position, const std::optional<glm::vec3>& normal)
	{
		if (!IsLight2DValid(light))
			return glm::vec3(0.0f);
		const glm::vec3 radiance = light.Color * light.Intensity;
		if (light.Type == RenderLight2DType::Global)
			return radiance;
		const glm::vec2 offset = glm::vec2(light.Position) - position;
		const float distance = std::sqrt(offset.x * offset.x + offset.y * offset.y);
		// Not `distance >= Radius` alone: a non-finite position gives a NaN distance, which no comparison accepts.
		if (!(distance < light.Radius))
			return glm::vec3(0.0f);
		float attenuation = 1.0f;
		if (light.Falloff > 0.0f && distance > light.Radius * (1.0f - light.Falloff))
		{
			const float t = (light.Radius - distance) / (light.Radius * light.Falloff);
			attenuation = t * t * (3.0f - 2.0f * t);
		}
		float facing = 1.0f;
		if (normal.has_value())
		{
			// Height is at least MinLight2DDistance, so the direction to the light never has zero length.
			const glm::vec3 toLight(offset, light.Height);
			const glm::vec3 direction = toLight / std::sqrt(glm::dot(toLight, toLight));
			facing = std::max(0.0f, glm::dot(*normal, direction));
		}
		return radiance * attenuation * facing;
	}

	glm::vec3 EvaluateLights2D(std::span<const Light2DItem> lights, glm::vec2 position, const std::optional<glm::vec3>& normal)
	{
		glm::vec3 sum(0.0f);
		const size_t count = std::min(lights.size(), static_cast<size_t>(MaxVisibleLights2D));
		for (const Light2DItem& light : lights.first(count))
			sum += EvaluateLight2D(light, position, normal);
		return sum;
	}

}
