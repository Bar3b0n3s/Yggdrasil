#pragma once

#include "Engine/Core/Result.h"

namespace Engine {

	struct EditorPanelContext;

	// Ring-buffer log and script diagnostic view with level/logger/text filters. Source links use OpenSource with
	// argument lists and file/line, entity links select by UUID. No log per frame, no interpretation of log text as commands.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class ConsolePanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	};

}
