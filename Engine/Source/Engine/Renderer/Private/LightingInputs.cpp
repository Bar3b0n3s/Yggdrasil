#include "EnginePCH.h"
#include "Engine/Renderer/Private/LightingInputs.h"

#include "Engine/Renderer/GpuResourceCache.h"
#include "Shared/EnvironmentBakeConstants.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace Engine {

	namespace Utils {

		static_assert(static_cast<uint32_t>(RenderLightType::Directional) == ShaderLightTypeDirectional
				&& static_cast<uint32_t>(RenderLightType::Point) == ShaderLightTypePoint && static_cast<uint32_t>(RenderLightType::Spot) == ShaderLightTypeSpot,
			"ShaderLight::Type takes RenderLightType's values");

		static_assert(MaxEnvironmentIntensity == EnvironmentBakeMaxHalf, "the intensity is bounded like the baked texels it scales");

		// The largest cone half-angle a spot light keeps (just below a hemisphere).
		constexpr float MaxSpotHalfAngleDegrees = 89.9f;

		ShaderLight MakeShaderLight(const LightData& light)
		{
			ShaderLight shaderLight{};
			shaderLight.Type = static_cast<uint32_t>(light.Type);
			shaderLight.Radiance = light.Color * light.Intensity;
			shaderLight.Position = light.Type == RenderLightType::Directional ? glm::vec3(0.0f) : light.Position;
			shaderLight.Direction = light.Type == RenderLightType::Point ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::normalize(light.Direction);
			if (light.Type != RenderLightType::Directional)
			{
				shaderLight.InverseRangeSquared = 1.0f / (light.Range * light.Range);
				shaderLight.SourceRadius = std::max(light.SourceRadius, 0.0f);
			}
			if (light.Type == RenderLightType::Spot)
			{
				const float outer = std::clamp(light.OuterConeAngle, 0.0f, MaxSpotHalfAngleDegrees);
				const float inner = std::clamp(light.InnerConeAngle, 0.0f, outer);
				shaderLight.SpotCosOuter = std::cos(glm::radians(outer));
				shaderLight.SpotCosInner = std::cos(glm::radians(inner));
			}
			return shaderLight;
		}

		EnvironmentConstants MakeEnvironmentConstants(const RenderEnvironment& environment, const GpuEnvironment* mirror)
		{
			EnvironmentConstants constants{};
			const float intensity = std::isfinite(environment.Intensity) ? std::clamp(environment.Intensity, 0.0f, MaxEnvironmentIntensity) : 0.0f;
			const float rotation = std::isfinite(environment.Rotation) ? glm::radians(environment.Rotation) : 0.0f;
			constants.Intensity = intensity;
			constants.RotationSin = std::sin(rotation);
			constants.RotationCos = std::cos(rotation);
			for (glm::vec4& coefficient : constants.IrradianceSH9)
				coefficient = glm::vec4(0.0f);
			if (mirror == nullptr)
			{
				const glm::vec3 ambient = environment.FallbackColor * intensity;
				const bool finite = std::isfinite(ambient.r) && std::isfinite(ambient.g) && std::isfinite(ambient.b);
				constants.AmbientColor = finite ? glm::max(ambient, glm::vec3(0.0f)) : glm::vec3(0.0f);
				constants.HasEnvironment = 0;
				return constants;
			}
			for (size_t index = 0; index < mirror->IrradianceSH9.size(); ++index)
			{
				const glm::vec3 coefficient = mirror->IrradianceSH9[index] * intensity;
				const bool finite = std::isfinite(coefficient.r) && std::isfinite(coefficient.g) && std::isfinite(coefficient.b);
				constants.IrradianceSH9[index] = glm::vec4(finite ? coefficient : glm::vec3(0.0f), 0.0f);
			}
			constants.AmbientColor = glm::vec3(0.0f);
			constants.SpecularMaxLod = static_cast<float>(std::max(mirror->SpecularMipCount, 1U) - 1);
			const float blur = std::isfinite(environment.SkyboxBlur) ? std::clamp(environment.SkyboxBlur, 0.0f, 1.0f) : 0.0f;
			constants.SkyboxLod = blur * static_cast<float>(std::max(mirror->SkyboxMipCount, 1U) - 1);
			constants.HasEnvironment = 1;
			return constants;
		}

	}

}
