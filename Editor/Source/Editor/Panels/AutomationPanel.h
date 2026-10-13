#pragma once

#include "Engine/Core/Result.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	struct EditorPanelContext;

	// Client list/request timing log, pause-agent and deny-mutations controls, plus persistent Allow AI automation.
	// Preference changes preserve RecentProjects; enabling attaches to the existing editor, disabling retires listener and
	// session file safely without deleting project data. No effect on explicit CLI one-shot modes or human UI actions.
	// The External source editor section edits the user preference Open source actions launch (executable plus one argv
	// entry per line, {path} and {line} substituted without a shell); Save queues it for the host's safe point
	// (EditorPanelContext::ExternalEditor), and the fields follow the saved preference until the user edits them.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class AutomationPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
		// The argv entries of the arguments field: one per line, carriage returns and empty lines dropped.
		[[nodiscard]] static std::vector<std::string> SplitEditorArguments(std::string_view text);
	private:
		[[nodiscard]] Status DrawExternalEditor(EditorPanelContext& context);
		// Shows `error` for one preference and logs it once: only when it differs from that preference's last error.
		static void Report(std::string& channel, const Error& error);

		std::array<char, 256> m_Search{};
		std::array<char, 1024> m_EditorExecutable{};
		std::array<char, 4096> m_EditorArguments{};
		int m_RequestState = 0;
		bool m_EditorEdited = false;         // the fields hold the user's draft, not the saved preference
		std::string m_AutomationError{};     // the Allow AI automation preference's last failure, shown until a change is queued
		std::string m_ExternalEditorError{}; // the external editor preference's last failure, shown until a change is queued
	};

}
