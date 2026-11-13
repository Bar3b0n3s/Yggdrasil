#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
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

	// Registry enum "ExposureMode" (M26, R-AEX; Docs/Decisions/0037-m26-contract.md decision 12): Manual multiplies the image
	// by 2^ExposureEV; Auto adapts the exposure from the image's luminance histogram (Renderer/AutoExposure.h), ExposureEV
	// added as compensation.
	enum class ExposureMode : uint8_t
	{
		Manual,
		Auto
	};

	// Registry enum "AntiAliasing" (M26, decision 12): version 1's FxaaEnabled became Fxaa (true) or None (false); Taa is
	// opt-in and acts as None in the output stage's pixel-art mode.
	enum class AntiAliasing : uint8_t
	{
		None,
		Fxaa,
		Taa
	};

	// Registry name "PostProcess" (Architecture §5.3, §8.8, §8.9), version 2 (M26, decision 12): exposure (manual or automatic),
	// tonemapping and grading, GTAO, screen-space reflections, bloom and anti-aliasing. Unique per scene. ExposureEV multiplies
	// by 2^EV (with Auto, the compensation added to the adapted exposure); the AutoExposure range in EV and its speeds in EV per
	// second; SsaoRadius, SsrMaxDistance and SsrThickness in metres. Version 1 had FxaaEnabled in AntiAliasing's place and none
	// of the exposure, LUT and SSR members after ExposureEV, Tonemap and SsaoQuality; its migration maps FxaaEnabled and leaves
	// the new members at these defaults, which render as version 1 did. The SsaoQuality, ExposureMode and AntiAliasing members
	// share their enums' names, so the types are qualified with the namespace where the members are in scope.
	struct PostProcessComponent
	{
		float ExposureEV = 0.0f;
		Engine::ExposureMode ExposureMode = Engine::ExposureMode::Manual;
		float AutoExposureMinEV = -6.0f;
		float AutoExposureMaxEV = 12.0f;
		float AutoExposureSpeedUp = 3.0f;
		float AutoExposureSpeedDown = 1.0f;
		Tonemapper Tonemap = Tonemapper::AgX;
		TypedAssetHandle<AssetType::Lut> Lut{};
		float LutIntensity = 1.0f;
		bool SsaoEnabled = true;
		float SsaoRadius = 0.5f;
		float SsaoIntensity = 1.0f;
		Engine::SsaoQuality SsaoQuality = Engine::SsaoQuality::Medium;
		bool SsrEnabled = false;
		float SsrIntensity = 1.0f;
		float SsrMaxDistance = 50.0f;
		float SsrThickness = 0.2f;
		float SsrMaxRoughness = 0.6f;
		bool BloomEnabled = true;
		float BloomIntensity = 0.04f;
		Engine::AntiAliasing AntiAliasing = Engine::AntiAliasing::Fxaa;
	};

}
