#pragma once

#include "Engine/Core/Json/Json.h"
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
		// M19 (Docs/Decisions/0030-m19-contract.md decision 29): "AllowNativeCode", the user's permission to build, load and
		// export a game's native module (NativeModuleHost). Changed only by the Native panel's checkbox; no automation method
		// writes it. Every writer of the file reads it first and writes it back as read.
		bool AllowNativeCode = false;
		std::string ExternalEditorExecutable{};
		// Individual argv entries. {path} and {line} substituted without a shell.
		std::vector<std::string> ExternalEditorArguments{};
	};

	// user://Editor.json, preserving RecentProjects and unknown members. Main thread. Missing file gives defaults.
	// Errors: Parse/Validation for malformed content; Io for file errors. Writing needs user:// (InvalidState otherwise).
	[[nodiscard]] Result<EditorPreferences> ReadEditorPreferences(const VirtualFileSystem& vfs);
	[[nodiscard]] Status WriteEditorPreferences(VirtualFileSystem& vfs, const EditorPreferences& preferences);

	// The text every writer of user://Editor.json stores (WriteEditorPreferences, ProjectManager::AddRecentProject):
	// tab-indented JSON that keeps every number exactly as parsed, so members of a newer editor survive (JsonWriter would
	// round doubles to float32 and reject those outside its range). Errors: Validation for a string or member name that
	// is not valid UTF-8.
	[[nodiscard]] Result<std::string> SerializeEditorPreferences(const Json& document);

	// Builds an executable-plus-argv launch, never shell text. Empty argv defaults to {path}; otherwise argv must
	// contain {path}. Substitutions are single-pass, so braces inside a source path stay literal. Line 0 uses 1.
	// InvalidState without a configured executable; InvalidArgument for empty path, NUL, invalid UTF-8 or no path token.
	// The launch does not capture output (ProcessSpecification::CaptureOutput false): the host reaps the editor once it
	// exits and detaches it at shutdown (EditorSourceLauncher), so the user's editor outlives the engine editor.
	[[nodiscard]] Result<ProcessSpecification> BuildEditorSourceProcessSpecification(const EditorPreferences& preferences,
		const std::filesystem::path& path, uint32_t line);

}
