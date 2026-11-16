#include "EnginePCH.h"
#include "Engine/Renderer/Fog.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace Engine {

	namespace Utils {

		// The Fog component's registered ranges (Scene/Registration/FogRegistration.cpp).
		constexpr float MaxFogDensity = 10.0f;
		constexpr float MaxFogStartDistance = 100000.0f;
		constexpr float MinFogHeightFalloff = 0.001f;
		constexpr float MaxFogHeightFalloff = 100.0f;
		constexpr float MaxFogSunGlow = 100.0f;
		constexpr float MinFogSunGlowExponent = 1.0f;
		constexpr float MaxFogSunGlowExponent = 1000.0f;
		// The SH basis' constant term, Y00 = 1 / (2 sqrt(pi)): the mean over the sphere of a projected function is its first
		// coefficient times it.
		constexpr float ShConstantTerm = 0.282094791773878f;

		// `value` clamped to [low, high]; `fallback` when it is not finite.
		static float ClampFinite(float value, float low, float high, float fallback)
		{
			return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
		}

		// Each channel of `color` finite and at least 0 (a non-finite channel is 0).
		static glm::vec3 MakeColorUsable(const glm::vec3& color)
		{
			glm::vec3 result(0.0f);
			for (glm::length_t channel = 0; channel < 3; ++channel)
				result[channel] = std::isfinite(color[channel]) ? std::max(color[channel], 0.0f) : 0.0f;
			return result;
		}

		// `vector` normalized, or nullopt when it is zero or not finite.
		static std::optional<glm::vec3> NormalizeFinite(const glm::vec3& vector)
		{
			const float lengthSquared = glm::dot(vector, vector);
			if (!std::isfinite(lengthSquared) || !(lengthSquared > 1e-20f))
				return std::nullopt;
			return vector / std::sqrt(lengthSquared);
		}

	}

	FogConstants MakeFogConstants(const RenderFog& fog, const CameraData& camera, std::span<const LightData> lights, const EnvironmentConstants& environment,
		bool fogged)
	{
		const RenderFog defaults;
		FogConstants constants;
		const glm::vec3 ambient = environment.HasEnvironment != 0 ? glm::vec3(environment.IrradianceSH9[0]) * Utils::ShConstantTerm : environment.AmbientColor;
		const float tint = Utils::ClampFinite(fog.AmbientTint, 0.0f, 1.0f, defaults.AmbientTint);
		constants.Color = Utils::MakeColorUsable(fog.Color) * glm::mix(glm::vec3(1.0f), Utils::MakeColorUsable(ambient), tint);
		constants.Density = Utils::ClampFinite(fog.Density, 0.0f, Utils::MaxFogDensity, defaults.Density);
		const bool positionFinite = std::isfinite(camera.Position.x) && std::isfinite(camera.Position.y) && std::isfinite(camera.Position.z);
		constants.CameraPosition = positionFinite ? camera.Position : glm::vec3(0.0f);
		constants.StartDistance = Utils::ClampFinite(fog.StartDistance, 0.0f, Utils::MaxFogStartDistance, defaults.StartDistance);
		// The camera's -Z axis in world space: the view matrix's third row (its rotation's inverse is its transpose), negated.
		const glm::vec3 viewZ(camera.View[0][2], camera.View[1][2], camera.View[2][2]);
		constants.ViewForward = -Utils::NormalizeFinite(viewZ).value_or(glm::vec3(0.0f, 0.0f, 1.0f));
		constants.MaxOpacity = Utils::ClampFinite(fog.MaxOpacity, 0.0f, 1.0f, defaults.MaxOpacity);
		constants.SunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
		constants.SunColor = glm::vec3(0.0f);
		const float glow = Utils::ClampFinite(fog.SunGlow, 0.0f, Utils::MaxFogSunGlow, defaults.SunGlow);
		const auto sun = std::ranges::find_if(lights, [](const LightData& light)
		{
			return light.Type == RenderLightType::Directional;
		});
		if (sun != lights.end())
		{
			if (const std::optional<glm::vec3> direction = Utils::NormalizeFinite(sun->Direction))
			{
				constants.SunDirection = -*direction;
				const float intensity = std::isfinite(sun->Intensity) ? std::max(sun->Intensity, 0.0f) : 0.0f;
				constants.SunColor = Utils::MakeColorUsable(sun->Color) * (intensity * glow);
			}
		}
		constants.SunGlowExponent = Utils::ClampFinite(fog.SunGlowExponent, Utils::MinFogSunGlowExponent, Utils::MaxFogSunGlowExponent, defaults.SunGlowExponent);
		constants.HeightDensity = Utils::ClampFinite(fog.HeightDensity, 0.0f, Utils::MaxFogDensity, defaults.HeightDensity);
		constants.HeightFalloff = Utils::ClampFinite(fog.HeightFalloff, Utils::MinFogHeightFalloff, Utils::MaxFogHeightFalloff, defaults.HeightFalloff);
		constants.BaseHeight = std::isfinite(fog.BaseHeight) ? fog.BaseHeight : defaults.BaseHeight;
		constants.Enabled = fogged && fog.Enabled ? 1U : 0U;
		constants.FogSkybox = fog.FogSkybox ? 1U : 0U;
		constants.Orthographic = camera.ProjectionKind == RenderProjection::Orthographic ? 1U : 0U;
		constants.ClearColor = Utils::MakeColorUsable(camera.ClearColor);
		return constants;
	}

}
