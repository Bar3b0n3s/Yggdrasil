#pragma once

#include "EditorCore/EditorPreferences.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/Process.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Engine {

	// Resolves a Console source location to an existing regular file on the main thread. project:// is confined to
	// projectRoot, engine:// to repositoryRoot/Resources; native paths and relative C++ locations may name either root.
	// Text is a URI only when it starts with a scheme of at least two characters ([A-Za-z][A-Za-z0-9+.-]*) and ':', so a
	// native path keeps every ':' it contains (a POSIX directory "Games: 2026", a Windows \\?\ extended path).
	// Empty projectRoot supports engine log links in the launcher. Canonicalization prevents symlink/.. escapes.
	// InvalidArgument: malformed/unknown URI or directory; InvalidState: project URI without a project; NotFound/Io:
	// missing/unreadable file; PermissionDenied: outside its allowed root. Does not launch a process or retain references.
	[[nodiscard]] Result<std::filesystem::path> ResolveEditorSourcePath(const std::filesystem::path& source,
		const std::filesystem::path& projectRoot, const std::filesystem::path& repositoryRoot);

	// One "Open source" click: the location and the context it resolves in, copied by value.
	struct EditorSourceRequest
	{
		std::filesystem::path Source{};
		uint32_t Line = 0;
		std::filesystem::path ProjectRoot{};     // empty without a project
		std::filesystem::path RepositoryRoot{};  // engine:// sources and relative C++ locations
		EditorPreferences Preferences{};         // read when the request is made, so hand edits apply at once
		std::filesystem::path PreferencesFile{}; // the native user://Editor.json, named by the not-configured error
	};

	// Main-thread host service behind EditorPanelContext::OpenSource. Queue validates synchronously, so the initiating
	// panel reports every predictable failure: Unsupported without a configured external editor (naming the
	// preferences file and its members), the ResolveEditorSourcePath errors, InvalidArgument for arguments without
	// {path}, NotFound for a missing executable. Pump starts each queued launch independently at a safe point; a failed
	// launch never drops the others. Editors start without captured output (their output goes to the null device, so
	// nothing grows with it), are reaped once they exit, and are detached when this service is destroyed: closing the
	// engine editor never ends the user's external editor or loses its unsaved work.
	class EditorSourceLauncher
	{
	public:
		EditorSourceLauncher() = default;
		~EditorSourceLauncher();
		EditorSourceLauncher(const EditorSourceLauncher&) = delete;
		EditorSourceLauncher& operator=(const EditorSourceLauncher&) = delete;

		[[nodiscard]] Status Queue(const EditorSourceRequest& request);
		// Starts every queued launch and reaps editors that exited. Returns the first launch failure, with the number of
		// further failures in its context; every other launch still starts.
		[[nodiscard]] Status Pump();
		// Drops queued launches (project close); running editors are unaffected.
		void Cancel();
		[[nodiscard]] size_t GetQueuedCount() const { return m_Queued.size(); }
		[[nodiscard]] size_t GetRunningCount() const { return m_Running.size(); }
	private:
		std::vector<ProcessSpecification> m_Queued{};
		std::vector<Process> m_Running{};
	};

}
