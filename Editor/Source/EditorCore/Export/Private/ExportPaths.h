#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <string_view>

// The rules the exporter (Exporter::Start) and project.export (ExportMethods) share about where an export may go (§13.2:
// project.export writes only under <Project>/Build/), so the method can locate a violation at /outDir before it checks
// anything else.

namespace Engine {

	namespace Utils {

		// The project directory every export goes below (§14.1).
		inline constexpr std::string_view ExportBuildDirectory = "Build";

		// Checks a project-relative output directory (ExportSpecification::OutputDirectory, project.export's outDir): empty
		// (the default directory), or a relative path by VfsPath's rules ("Build/Windows-Release/Tetris": forward slashes, no
		// "..", no reserved names) strictly below Build/, never Build/ itself, whose contents an export replaces. Errors:
		// InvalidArgument naming the rule that `directory` breaks.
		[[nodiscard]] Status CheckExportOutputDirectory(std::string_view directory);

		// Whether the project-relative path `path` ("Assets/Tests/Level.scene") matches the Export.Exclude glob `pattern`:
		// '*' matches any characters except '/', '?' one character except '/', "**" any characters including '/' (and "**/"
		// also matches no directory at all, so "Assets/**/Old.png" matches "Assets/Old.png"); everything else matches itself,
		// case-sensitively (§4.10). Pure.
		[[nodiscard]] bool MatchesExportGlob(std::string_view pattern, std::string_view path);

	}

}
