#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Platform/Process.h"

#include <cstdint>
#include <filesystem>
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

	// Builds an executable-plus-argv launch, never shell text. Empty argv defaults to {path}; otherwise argv must
	// contain {path}. Substitutions are single-pass, so braces inside a source path stay literal. Line 0 uses 1.
	// InvalidState without a configured executable; InvalidArgument for empty path, NUL, invalid UTF-8 or no path token.
	// The host calls Process::Spawn and retains the Process until it exits (destroying a live Process kills it).
	[[nodiscard]] Result<ProcessSpecification> BuildEditorSourceProcessSpecification(const EditorPreferences& preferences,
		const std::filesystem::path& path, uint32_t line);

}
