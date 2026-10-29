#include "EnginePCH.h"
#include "Engine/Renderer/TextMeasurement.h"

// The text-measurement layer (TextMeasurement.h): stubs of the M22 contract (C22.1, Docs/Decisions/0033-m22-contract.md
// decision 6) that stream B implements.

namespace Engine {

	TextMeasurement MeasureText(std::span<const FontData* const> /*fonts*/, std::string_view /*text*/, const TextMeasureOptions& /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	glm::vec2 MeasureTextSize(std::span<const FontData* const> /*fonts*/, std::string_view /*text*/, const TextMeasureOptions& /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec2(0.0f);
	}

}
