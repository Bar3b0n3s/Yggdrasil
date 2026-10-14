#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>
#include <string_view>

// The build configuration an export targets, on its own so the command line (EditorCommandLine.h) names it without the
// whole exporter (Docs/Decisions/0024-m15-contract.md, review fix n2). Exporter.h includes it.

namespace Engine {

	// The build configuration an export targets (§2.2): registry enum "ExportConfiguration".
	enum class ExportConfiguration : uint8_t
	{
		Debug,
		Release,
		Dist
	};

	// "Debug", "Release" or "Dist" (defined in Exporter.cpp).
	[[nodiscard]] std::string_view ExportConfigurationToString(ExportConfiguration configuration);

}
