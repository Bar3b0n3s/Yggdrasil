#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/ResourcePatcher.h"
#include "Engine/Project/ProjectSettings.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// The labelling an exported executable carries (Architecture §6.1 "Export", §14.2): the project's Export.Icon, Version and
// Company, validated by the exporter before it writes anything (M14: their first consumer, so gate 8 observes them;
// Docs/Decisions/0022-m14-contract.md decision 12). M15's resource patcher writes them into the executable: the exporter
// builds the resource set with BuildExecutableResourceSet and hands it to ResourcePatcher::Patch on Windows, or to
// AppBundle's Info.plist and .icns writers on macOS (Docs/Decisions/0024-m15-contract.md decision 4).

namespace Engine {

	class EditorContext;

	struct ExecutableResources
	{
		std::string Icon{}; // project-relative square PNG of at least 256 x 256 pixels; empty: no icon resource (ADR 0024 decision 4)
		uint16_t VersionMajor = 1;
		uint16_t VersionMinor = 0;
		uint16_t VersionPatch = 0;
		std::string Company{}; // free text, possibly empty
	};

	// The sizes of an exported executable's icon images (§14.2 step 5: 16 to 256 px), ascending: the Windows icon group
	// holds all of them, the macOS .icns the ones its entry types name (AppBundle.h's BuildIcns).
	inline constexpr std::array<uint16_t, 7> ExecutableIconSizes = { 16, 24, 32, 48, 64, 128, 256 };

	// The resources of `settings` for the project open in `editor`. Version must be "major.minor.patch", three decimal
	// numbers without signs or leading zeros, each at most 65535 (the executable's version resource). A non-empty Icon must
	// name a project file that decodes as a square PNG of at least 256 x 256 pixels (M15 adds the square rule: icon images
	// are square, and padding would change the author's picture; ADR 0024 decision 4). Errors: InvalidState without an open
	// project; Validation located at /Export/Version or /Export/Icon naming the value and the rule.
	[[nodiscard]] Result<ExecutableResources> ResolveExecutableResources(const EditorContext& editor, const ExportSettings& settings);

	// The resource set the exporter writes for `resources` (ResolveExecutableResources' result) of the project open in
	// `editor`, whose game is named `gameName`:
	//   - Icons: empty without an Icon; otherwise one image per ExecutableIconSizes entry, the decoded Icon reduced with
	//     DownscaleImage (an integer box filter, so the images are deterministic) and encoded with EncodePng; the 256 px
	//     image is the decoded icon downscaled to 256.
	//   - Version: Major, Minor and Patch, CompanyName = Company, ProductName = gameName, OriginalFilename = "<gameName>.exe"
	//     on Windows and gameName elsewhere.
	// Errors: InvalidState without an open project; Validation located at /Export/Icon when the icon no longer reads or
	// decodes; the errors of DownscaleImage and EncodePng.
	[[nodiscard]] Result<ExecutableResourceSet> BuildExecutableResourceSet(const EditorContext& editor, const ExecutableResources& resources,
		std::string_view gameName);

}
