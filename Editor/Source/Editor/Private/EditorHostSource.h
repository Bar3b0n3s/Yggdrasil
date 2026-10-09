#pragma once

#include "Engine/Core/Result.h"

#include <filesystem>

namespace Engine {

	// Resolves a Console source location to an existing regular file on the main thread. project:// is confined to
	// projectRoot, engine:// to repositoryRoot/Resources; native paths and relative C++ locations may name either root.
	// Empty projectRoot supports engine log links in the launcher. Canonicalization prevents symlink/.. escapes.
	// InvalidArgument: malformed/unknown URI or directory; InvalidState: project URI without a project; NotFound/Io:
	// missing/unreadable file; PermissionDenied: outside its allowed root. Does not launch a process or retain references.
	[[nodiscard]] Result<std::filesystem::path> ResolveEditorSourcePath(const std::filesystem::path& source,
		const std::filesystem::path& projectRoot, const std::filesystem::path& repositoryRoot);

}
