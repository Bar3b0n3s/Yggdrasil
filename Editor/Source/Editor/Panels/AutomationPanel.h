#pragma once

#include "Engine/Core/Result.h"

#include <string>

namespace Engine {

	struct EditorPanelContext;

	// Client list/request timing log, pause-agent and deny-mutations controls, plus persistent Allow AI automation.
	// Preference changes preserve RecentProjects; enabling attaches to the existing editor, disabling retires listener and
	// session file safely without deleting project data. No effect on explicit CLI one-shot modes or human UI actions.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class AutomationPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		std::string m_Error{};
	};

}
