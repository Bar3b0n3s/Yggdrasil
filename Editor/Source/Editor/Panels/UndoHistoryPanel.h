#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>

namespace Engine {

	struct EditorPanelContext;

	// Command labels/origin/position from the shared CommandHistory. Undo/redo routes through edit.undo/redo;
	// agent commands retain their [agent] labels. A failed undo shows its error and stops at that entry.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class UndoHistoryPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		uint64_t m_Ticket = 0;
		std::string m_Error{};
	};

}
