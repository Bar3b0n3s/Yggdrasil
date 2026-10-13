#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Project/ProjectSettings.h"

#include <cstdint>
#include <string>

// The labelling an exported executable carries (Architecture §6.1 "Export", §14.2): the project's Export.Icon, Version and
// Company, validated by the exporter before it writes anything (M14: their first consumer, so gate 8 observes them;
// Docs/Decisions/0022-m14-contract.md decision 12). M15's resource patcher writes them into the executable.

namespace Engine {

	class EditorContext;

	struct ExecutableResources
	{
		std::string Icon{}; // project-relative PNG of at least 256 x 256 pixels; empty: the engine's default icon
		uint16_t VersionMajor = 1;
		uint16_t VersionMinor = 0;
		uint16_t VersionPatch = 0;
		std::string Company{}; // free text, possibly empty
	};

	// The resources of `settings` for the project open in `editor`. Version must be "major.minor.patch", three decimal
	// numbers without signs or leading zeros, each at most 65535 (the executable's version resource). A non-empty Icon must
	// name a project file that decodes as a PNG of at least 256 x 256 pixels. Errors: InvalidState without an open project;
	// Validation located at /Export/Version or /Export/Icon naming the value and the rule.
	[[nodiscard]] Result<ExecutableResources> ResolveExecutableResources(const EditorContext& editor, const ExportSettings& settings);

}
