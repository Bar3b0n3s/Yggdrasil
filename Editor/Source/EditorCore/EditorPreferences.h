#pragma once

#include "Engine/Core/Result.h"

#include <string>
#include <vector>

namespace Engine {

	class VirtualFileSystem;

	struct EditorPreferences
	{
		bool AllowAiAutomation = false;
		std::string ExternalEditorExecutable{};
		// Individual argv entries. {path} and {line} substituted without a shell.
		std::vector<std::string> ExternalEditorArguments{};
	};

	// user://Editor.json, preserving RecentProjects and unknown members. Main thread. Missing file gives defaults.
	// Errors: Parse/Validation for malformed content; Io for file errors. Writing needs user:// (InvalidState otherwise).
	[[nodiscard]] Result<EditorPreferences> ReadEditorPreferences(const VirtualFileSystem& vfs);
	[[nodiscard]] Status WriteEditorPreferences(VirtualFileSystem& vfs, const EditorPreferences& preferences);

}
