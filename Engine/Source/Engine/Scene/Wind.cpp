#include "EnginePCH.h"
#include "Engine/Scene/Wind.h"

#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/WindComponent.h"
#include "Engine/Scene/Scene.h"

#include <entt/entt.hpp>

#include <cmath>

namespace Engine {

	namespace Utils {

		// frac(x) = x - floor(x), in [0, 1).
		static double Fraction(double x)
		{
			return x - std::floor(x);
		}

	}

	glm::dvec3 GetWindDirection(const WindComponent& wind)
	{
		const double x = wind.Direction.x;
		const double z = wind.Direction.z;
		const double lengthSquared = x * x + z * z;
		if (!(lengthSquared > 0.0) || !std::isfinite(lengthSquared))
			return glm::dvec3(1.0, 0.0, 0.0);
		const double length = std::sqrt(lengthSquared);
		return glm::dvec3(x / length, 0.0, z / length);
	}

	double GetWindGustWavelength(const WindComponent& wind)
	{
		if (!(wind.Strength > 0.0f) || !(wind.GustFrequency > 0.0f))
			return 0.0;
		return static_cast<double>(wind.Strength) / static_cast<double>(wind.GustFrequency);
	}

	double WindGustWave(double x)
	{
		const double triangle = 1.0 - std::abs(2.0 * Utils::Fraction(x) - 1.0);
		return triangle * triangle * (3.0 - 2.0 * triangle);
	}

	glm::dvec3 SampleWind(const WindComponent& wind, const glm::dvec3& position, double time)
	{
		const glm::dvec3 along = GetWindDirection(wind);
		const glm::dvec3 across(-along.z, 0.0, along.x);
		const double wavelength = GetWindGustWavelength(wind);
		const double a = wavelength > 0.0 ? (position.x * along.x + position.z * along.z) / wavelength : 0.0;
		const double c = wavelength > 0.0 ? (position.x * across.x + position.z * across.z) / wavelength : 0.0;
		const double turbulence = wind.Turbulence;
		const double gustStrength = wind.GustStrength;
		const double phase = static_cast<double>(wind.GustFrequency) * time - a;
		const double speed = static_cast<double>(wind.Strength) + gustStrength * WindGustWave(phase + turbulence * WindGustWave(c * 0.5));
		const double side = turbulence * gustStrength * (2.0 * WindGustWave(phase * 0.5 + c + 0.25) - 1.0);
		return along * speed + across * side;
	}

	const WindComponent* FindSceneWind(const Scene& scene)
	{
		const entt::registry& registry = scene.GetRegistry();
		const WindComponent* found = nullptr;
		UUID foundID{};
		for (const auto [entity, wind, id] : registry.view<const WindComponent, const IDComponent>().each())
		{
			if (registry.all_of<HierarchyDisabledTag>(entity))
				continue;
			if (found == nullptr || id.ID < foundID)
			{
				found = &wind;
				foundID = id.ID;
			}
		}
		return found;
	}

}
