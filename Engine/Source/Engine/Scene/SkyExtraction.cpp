#include "EnginePCH.h"
#include "Engine/Scene/SkyExtraction.h"

#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SkyComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"

#include <glm/glm.hpp>

#include <cmath>
#include <optional>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static bool IsSkyEntityEnabled(ConstEntity entity)
		{
			return entity.IsValid() && !entity.HasComponent<HierarchyDisabledTag>();
		}

		// The unit local +Z of `world`, or nullopt for a degenerate or non-finite matrix.
		[[nodiscard]] static std::optional<glm::vec3> GetLocalForwardZ(const glm::mat4& world)
		{
			const glm::vec3 axis(world[2]);
			const float lengthSquared = glm::dot(axis, axis);
			if (!std::isfinite(lengthSquared) || !(lengthSquared > 1e-20f))
				return std::nullopt;
			return axis / std::sqrt(lengthSquared);
		}

	}

	void ExtractRenderSky(const Scene& scene, const RenderExtractionRequest& request, RenderSnapshot& snapshot)
	{
		const entt::registry& registry = scene.GetRegistry();
		std::optional<ConstEntity> found;
		for (const entt::entity handle : registry.view<SkyComponent>())
		{
			const ConstEntity entity(handle, &scene);
			if (!Utils::IsSkyEntityEnabled(entity))
				continue;
			if (!found.has_value() || entity.GetUUID() < found->GetUUID())
				found = entity;
		}
		if (!found.has_value())
		{
			snapshot.Sky = RenderSky{};
			return;
		}
		const SkyComponent& component = found->GetComponent<SkyComponent>();
		RenderSky sky{
			.Enabled = true,
			.Sun = component.Sun,
			.Moon = component.Moon,
			.Intensity = component.Intensity,
			.DriveLights = component.DriveLights,
			.Haze = component.Haze,
			.Overcast = component.Overcast,
			.GroundAlbedo = component.GroundAlbedo,
			.SunDisc = component.SunDisc,
			.MoonSize = component.MoonSize,
			.Stars = component.Stars,
			.Latitude = component.Latitude,
			.NorthAngle = component.NorthAngle,
			.Clouds = component.Clouds,
			.CloudSpeed = component.CloudSpeed,
			.SimulationTime = request.SimulationTime,
			.Entity = found->GetUUID(),
		};
		if (component.Moon.IsValid())
		{
			const ConstEntity moon = scene.FindEntityByID(component.Moon);
			const ConstEntity sun = component.Sun.IsValid() ? scene.FindEntityByID(component.Sun) : ConstEntity();
			const bool isSun = sun.IsValid() && sun.GetUUID() == component.Moon;
			if (Utils::IsSkyEntityEnabled(moon) && !isSun)
			{
				if (const std::optional<glm::vec3> forward = Utils::GetLocalForwardZ(ComputeRenderedWorldMatrix(moon, request.Alpha)))
				{
					sky.HasMoon = true;
					sky.MoonDirection = *forward;
				}
			}
		}
		snapshot.Sky = sky;
		ResolveSkyLights(snapshot);
		ApplySkyLightDriving(snapshot);
	}

}
