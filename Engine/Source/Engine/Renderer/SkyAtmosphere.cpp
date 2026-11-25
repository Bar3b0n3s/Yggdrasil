#include "EnginePCH.h"
#include "Engine/Renderer/SkyAtmosphere.h"

#include "Engine/Core/DetMath.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace Engine {

	namespace Utils {

		// `value` clamped to [low, high]; `fallback` when it is not finite.
		[[nodiscard]] static double ClampSkyValue(double value, double low, double high, double fallback)
		{
			return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
		}

		// `vector` normalized, or nullopt when it is zero or not finite.
		[[nodiscard]] static std::optional<glm::dvec3> NormalizeSkyVector(const glm::dvec3& vector)
		{
			const double lengthSquared = glm::dot(vector, vector);
			if (!std::isfinite(lengthSquared) || !(lengthSquared > 1e-30))
				return std::nullopt;
			return vector / std::sqrt(lengthSquared);
		}

		[[nodiscard]] static double DegreesToRadians(double degrees)
		{
			return degrees * (std::numbers::pi / 180.0);
		}

		// The directional light of `lights` whose entity is `entity`.
		[[nodiscard]] static std::optional<uint32_t> FindDirectionalLight(std::span<const LightData> lights, UUID entity)
		{
			for (uint32_t index = 0; index < lights.size(); ++index)
				if (lights[index].Type == RenderLightType::Directional && lights[index].Entity == entity)
					return index;
			return std::nullopt;
		}

		[[nodiscard]] static std::optional<uint32_t> FindFirstDirectionalLight(std::span<const LightData> lights)
		{
			for (uint32_t index = 0; index < lights.size(); ++index)
				if (lights[index].Type == RenderLightType::Directional)
					return index;
			return std::nullopt;
		}

		// The unit direction toward `light`'s source (the opposite of its Direction), or nullopt.
		[[nodiscard]] static std::optional<glm::dvec3> GetDirectionToLight(const LightData& light)
		{
			return NormalizeSkyVector(-glm::dvec3(light.Direction));
		}

	}

	SkyAtmosphere MakeSkyAtmosphere(float haze, float overcast, const glm::vec3& groundAlbedo)
	{
		const double hazeScale = Utils::ClampSkyValue(haze, 0.0, 10.0, 1.0);
		const double overcastAmount = Utils::ClampSkyValue(overcast, 0.0, 1.0, 0.0);
		const double mieScale = hazeScale + overcastAmount * SkyOvercastMieScale;
		SkyAtmosphere atmosphere;
		atmosphere.MieScattering = SkyMieScattering * mieScale;
		atmosphere.MieExtinction = (SkyMieScattering + SkyMieAbsorption) * mieScale;
		atmosphere.MieAsymmetry = SkyMieAsymmetry + (SkyOvercastMieAsymmetry - SkyMieAsymmetry) * overcastAmount;
		for (glm::length_t channel = 0; channel < 3; ++channel)
			atmosphere.GroundAlbedo[channel] = Utils::ClampSkyValue(groundAlbedo[channel], 0.0, 1.0, 0.3);
		return atmosphere;
	}

	glm::dvec3 ComputeSkyExtinction(const SkyAtmosphere& atmosphere, double height)
	{
		const double rayleigh = DetMath::Exp(-height / SkyRayleighScaleHeight);
		const double mie = DetMath::Exp(-height / SkyMieScaleHeight);
		const double ozone = std::max(0.0, 1.0 - std::abs(height - SkyOzoneCenter) / SkyOzoneHalfWidth);
		return atmosphere.RayleighScattering * rayleigh + glm::dvec3(atmosphere.MieExtinction * mie) + atmosphere.OzoneAbsorption * ozone;
	}

	glm::dvec3 ComputeSkyTransmittance(const SkyAtmosphere& atmosphere, double radius, double mu)
	{
		const double r = std::clamp(radius, SkyBottomRadius, SkyTopRadius);
		const double cosine = std::clamp(mu, -1.0, 1.0);
		// The ray meets the ground when it points down and its closest approach lies inside the ground's sphere.
		const double discriminantGround = r * r * (cosine * cosine - 1.0) + SkyBottomRadius * SkyBottomRadius;
		if (cosine < 0.0 && discriminantGround >= 0.0)
			return glm::dvec3(0.0);
		const double discriminantTop = std::max(r * r * (cosine * cosine - 1.0) + SkyTopRadius * SkyTopRadius, 0.0);
		const double distance = std::max(-r * cosine + std::sqrt(discriminantTop), 0.0);
		const double step = distance / static_cast<double>(SkyTransmittanceSteps);
		glm::dvec3 depth(0.0);
		for (uint32_t index = 0; index < SkyTransmittanceSteps; ++index)
		{
			const double t = (static_cast<double>(index) + 0.5) * step;
			const double sampleRadius = std::sqrt(std::max(r * r + t * t + 2.0 * r * cosine * t, 0.0));
			depth += ComputeSkyExtinction(atmosphere, sampleRadius - SkyBottomRadius) * step;
		}
		return glm::dvec3(DetMath::Exp(-depth.x), DetMath::Exp(-depth.y), DetMath::Exp(-depth.z));
	}

	glm::dvec3 ComputeSkyTransmittance(double altitude, const glm::dvec3& direction, float haze, float overcast)
	{
		const std::optional<glm::dvec3> unit = Utils::NormalizeSkyVector(direction);
		if (!unit.has_value() || !std::isfinite(altitude))
			return glm::dvec3(0.0);
		return ComputeSkyTransmittance(MakeSkyAtmosphere(haze, overcast, glm::vec3(0.3f)), SkyBottomRadius + std::max(altitude, 0.0), unit->y);
	}

	double ComputeSkyCameraAltitude(const glm::vec3& position)
	{
		const double height = std::isfinite(position.y) ? std::max(static_cast<double>(position.y), 0.0) : 0.0;
		return height / 1000.0 + SkyCameraHeightOffset;
	}

	double ComputeMoonIlluminatedFraction(const glm::dvec3& toSun, const glm::dvec3& toMoon)
	{
		const std::optional<glm::dvec3> sun = Utils::NormalizeSkyVector(toSun);
		const std::optional<glm::dvec3> moon = Utils::NormalizeSkyVector(toMoon);
		if (!sun.has_value() || !moon.has_value())
			return 0.0;
		return std::clamp((1.0 - glm::dot(*sun, *moon)) * 0.5, 0.0, 1.0);
	}

	glm::dvec3 ComputeSkyPole(double latitude, double northAngle)
	{
		const SinCosResult<double> turn = DetMath::SinCos(Utils::DegreesToRadians(std::isfinite(northAngle) ? northAngle : 0.0));
		// -Z turned counter-clockwise about +Y (seen from above) by northAngle.
		const glm::dvec3 north(-turn.Sin, 0.0, -turn.Cos);
		const SinCosResult<double> elevation = DetMath::SinCos(Utils::DegreesToRadians(std::isfinite(latitude) ? std::clamp(latitude, -90.0, 90.0) : 45.0));
		return north * elevation.Cos + glm::dvec3(0.0, 1.0, 0.0) * elevation.Sin;
	}

	glm::dmat3 ComputeSkyStarFrame(const glm::dvec3& toSun, double latitude, double northAngle)
	{
		const SinCosResult<double> turn = DetMath::SinCos(Utils::DegreesToRadians(std::isfinite(northAngle) ? northAngle : 0.0));
		const glm::dvec3 north(-turn.Sin, 0.0, -turn.Cos);
		const glm::dvec3 pole = ComputeSkyPole(latitude, northAngle);
		const glm::dvec3 east = glm::cross(north, glm::dvec3(0.0, 1.0, 0.0));
		const glm::dvec3 meridian = glm::normalize(glm::cross(east, pole));
		const glm::dvec3 side = glm::cross(pole, meridian);
		double hourCos = 1.0;
		double hourSin = 0.0;
		if (const std::optional<glm::dvec3> sun = Utils::NormalizeSkyVector(toSun))
		{
			const double x = glm::dot(*sun, meridian);
			const double y = glm::dot(*sun, side);
			const double length = std::sqrt(x * x + y * y);
			if (length > 1e-12)
			{
				hourCos = x / length;
				hourSin = y / length;
			}
		}
		// The rows of the world-to-celestial rotation: the basis turned by -h about the pole (Rz(-h) times the basis' rows).
		const glm::dvec3 row0 = meridian * hourCos + side * hourSin;
		const glm::dvec3 row1 = side * hourCos - meridian * hourSin;
		const glm::dvec3 row2 = pole;
		// glm is column-major: column c holds each row's c-th component.
		return glm::dmat3(glm::dvec3(row0.x, row1.x, row2.x), glm::dvec3(row0.y, row1.y, row2.y), glm::dvec3(row0.z, row1.z, row2.z));
	}

	std::vector<uint32_t> OrderShadowCandidates(const RenderSnapshot& snapshot)
	{
		std::vector<uint32_t> order;
		std::optional<uint32_t> preferred;
		const RenderSky& sky = snapshot.Sky;
		if (sky.Enabled)
		{
			if (sky.SunLight.has_value() && *sky.SunLight < snapshot.Lights.size())
			{
				const std::optional<glm::dvec3> toSun = Utils::GetDirectionToLight(snapshot.Lights[*sky.SunLight]);
				if (toSun.has_value() && toSun->y > 0.0)
					preferred = *sky.SunLight;
			}
			if (!preferred.has_value() && sky.MoonLight.has_value() && *sky.MoonLight < snapshot.Lights.size())
				preferred = *sky.MoonLight;
		}
		if (preferred.has_value())
			order.push_back(*preferred);
		for (uint32_t index = 0; index < snapshot.Lights.size(); ++index)
			if (snapshot.Lights[index].Type == RenderLightType::Directional && index != preferred)
				order.push_back(index);
		return order;
	}

	std::optional<size_t> GetSkyGlowLight(const RenderSnapshot& snapshot)
	{
		const RenderSky& sky = snapshot.Sky;
		if (sky.Enabled && sky.SunLight.has_value() && *sky.SunLight < snapshot.Lights.size())
			return *sky.SunLight;
		return Utils::FindFirstDirectionalLight(snapshot.Lights);
	}

	void ResolveSkyLights(RenderSnapshot& snapshot)
	{
		RenderSky& sky = snapshot.Sky;
		if (!sky.Enabled)
			return;
		sky.SunLight = sky.Sun.IsValid() ? Utils::FindDirectionalLight(snapshot.Lights, sky.Sun) : Utils::FindFirstDirectionalLight(snapshot.Lights);
		if (sky.SunLight.has_value())
			sky.SunLightColor = snapshot.Lights[*sky.SunLight].Color;
		const bool moonIsSun = sky.Moon.IsValid() && sky.SunLight.has_value() && snapshot.Lights[*sky.SunLight].Entity == sky.Moon;
		sky.MoonLight = sky.Moon.IsValid() && !moonIsSun ? Utils::FindDirectionalLight(snapshot.Lights, sky.Moon) : std::nullopt;
		if (moonIsSun || !sky.Moon.IsValid())
			sky.HasMoon = false;
		if (sky.MoonLight.has_value())
		{
			const LightData& moon = snapshot.Lights[*sky.MoonLight];
			sky.MoonLightColor = moon.Color;
			if (const std::optional<glm::dvec3> toMoon = Utils::GetDirectionToLight(moon))
			{
				sky.HasMoon = true;
				sky.MoonDirection = glm::vec3(*toMoon);
			}
		}
	}

	void ApplySkyLightDriving(RenderSnapshot& snapshot)
	{
		RenderSky& sky = snapshot.Sky;
		if (!sky.Enabled || !sky.DriveLights)
			return;
		const double altitude = ComputeSkyCameraAltitude(snapshot.HasCamera ? snapshot.Camera.Position : glm::vec3(0.0f));
		const SkyAtmosphere atmosphere = MakeSkyAtmosphere(sky.Haze, sky.Overcast, sky.GroundAlbedo);
		const double radius = SkyBottomRadius + altitude;
		std::optional<glm::dvec3> toSun;
		if (sky.SunLight.has_value() && *sky.SunLight < snapshot.Lights.size())
		{
			LightData& sun = snapshot.Lights[*sky.SunLight];
			toSun = Utils::GetDirectionToLight(sun);
			const glm::dvec3 transmittance = toSun.has_value() ? ComputeSkyTransmittance(atmosphere, radius, toSun->y) : glm::dvec3(0.0);
			sun.Color = glm::vec3(glm::dvec3(sky.SunLightColor) * transmittance);
		}
		if (sky.MoonLight.has_value() && *sky.MoonLight < snapshot.Lights.size())
		{
			LightData& moon = snapshot.Lights[*sky.MoonLight];
			const std::optional<glm::dvec3> toMoon = Utils::GetDirectionToLight(moon);
			if (toMoon.has_value())
				sky.MoonDirection = glm::vec3(*toMoon);
			const glm::dvec3 transmittance = toMoon.has_value() ? ComputeSkyTransmittance(atmosphere, radius, toMoon->y) : glm::dvec3(0.0);
			// Without a sun, the moon is lit as at its full phase.
			const double fraction = toMoon.has_value() && toSun.has_value() ? ComputeMoonIlluminatedFraction(*toSun, *toMoon) : 1.0;
			moon.Color = glm::vec3(glm::dvec3(sky.MoonLightColor) * transmittance * fraction);
		}
	}

}
