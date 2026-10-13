#pragma once

#include "Editor/EditorPanelContext.h"
#include "EditorCore/EditorPreferences.h"
#include "Engine/Core/Result.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

	class VirtualFileSystem;

	// The editor host's user://Editor.json state behind the "Allow AI automation" and external source editor preferences
	// and the launcher's recent projects (Architecture §12.2, ADR 0018 §6). Main thread. Startup never fails on
	// preferences: a missing file gives the defaults, and so does a malformed or newer one, whose error is published as the
	// preference's Failure (the Automation panel shows it) and returned for one Warn line. Draw reads only the memory
	// snapshots; queued changes are applied at the next safe point by Pump and PumpExternalEditor.
	class EditorHostPreferences
	{
	public:
		// Applies the preference to the automation listener, or persists and applies it (EditorAutomationControls).
		using Apply = std::function<Status(bool)>;

		// Reads user://Editor.json. On failure the defaults stay installed and the error is published and returned.
		[[nodiscard]] Status Load(const VirtualFileSystem& vfs);
		// Startup: starts the preference-owned listener for the loaded preference through `listen`
		// (AutomationServer::SetPreferenceListening). On failure the editor keeps running without it: Allowed becomes
		// false, and the failure is published and returned.
		[[nodiscard]] Status StartListening(const Apply& listen);
		[[nodiscard]] const EditorPreferences& GetPreferences() const { return m_Preferences; }
		// Memory-only copy for Draw (EditorAutomationPreferenceServices::GetState).
		[[nodiscard]] EditorAutomationPreferenceState GetAutomationState() const { return m_Automation; }
		// Queue only, no I/O (EditorAutomationPreferenceServices::QueueChange). InvalidState while a change is pending.
		// Clears the published Failure.
		[[nodiscard]] Status QueueAutomationChange(bool allowed);
		// Safe point: applies the queued change through `apply`, clears Pending, and publishes Allowed (success) or Failure
		// (Allowed unchanged); returns the failure so the host logs it. No effect without a queued change.
		[[nodiscard]] Status Pump(const Apply& apply);

		// Memory-only copy for Draw (EditorExternalEditorServices::GetState): the loaded or last written preference.
		[[nodiscard]] EditorExternalEditorState GetExternalEditorState() const;
		// Queue only, no I/O (EditorExternalEditorServices::QueueChange). An empty executable with no arguments clears the
		// preference. Errors: InvalidState while a change is pending; InvalidArgument for text that is not valid UTF-8 or
		// holds NUL, for arguments without an executable, and for arguments none of which contains {path}. Clears Failure.
		[[nodiscard]] Status QueueExternalEditorChange(std::string executable, std::vector<std::string> arguments);
		// Safe point: writes the queued change into user://Editor.json (ReadEditorPreferences, then WriteEditorPreferences,
		// which keeps RecentProjects, unknown members and the file's own AllowAiAutomation), clears Pending, and publishes
		// the written preference (success) or Failure (snapshot unchanged); returns the failure so the host logs it. No
		// effect, and no file access, without a queued change.
		[[nodiscard]] Status PumpExternalEditor(VirtualFileSystem& vfs);

		// Reads the recent projects (ProjectManager::ReadRecentProjects) into the snapshot the launcher draws from
		// (EditorPanelContext::GetRecentProjects). The host calls it at startup and at the safe point after each project
		// open or close, never while a panel draws; a failed read is kept as the snapshot's error.
		void RefreshRecentProjects(const VirtualFileSystem& vfs);
		// Memory-only copy of the last RefreshRecentProjects; empty before the first.
		[[nodiscard]] const Result<std::vector<std::filesystem::path>>& GetRecentProjects() const { return m_RecentProjects; }
	private:
		struct ExternalEditorChange
		{
			std::string Executable{};
			std::vector<std::string> Arguments{};
		};

		EditorPreferences m_Preferences{};
		EditorAutomationPreferenceState m_Automation{};
		std::optional<bool> m_Pending{};
		std::optional<ExternalEditorChange> m_PendingExternalEditor{};
		std::optional<Error> m_ExternalEditorFailure{};
		Result<std::vector<std::filesystem::path>> m_RecentProjects{ std::vector<std::filesystem::path>{} };
	};

}
