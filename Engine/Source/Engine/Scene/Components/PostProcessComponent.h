#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>

namespace Engine {

	// Registry enum "Tonemapper" (Architecture §8.9).
	enum class Tonemapper : uint8_t
	{
		AgX,
		ACES,
		PbrNeutral,
		Linear
	};

	// Registry enum "SsaoQuality" (Architecture §8.8).
	enum class SsaoQuality : uint8_t
	{
		Low,
		Medium,
		High
	};

	// Registry name "PostProcess" (Architecture §5.3, §8.8, §8.9): exposure, tonemapping, GTAO, bloom and FXAA. Unique per
	// scene. ExposureEV multiplies by 2^EV; SsaoRadius in metres. The SsaoQuality member shares its enum's name, so the
	// type is qualified with the namespace where the member is in scope.
	struct PostProcessComponent
	{
		float ExposureEV = 0.0f;
		Tonemapper Tonemap = Tonemapper::AgX;
		bool SsaoEnabled = true;
		float SsaoRadius = 0.5f;
		float SsaoIntensity = 1.0f;
		Engine::SsaoQuality SsaoQuality = Engine::SsaoQuality::Medium;
		bool BloomEnabled = true;
		float BloomIntensity = 0.04f;
		bool FxaaEnabled = true;
	};

}
