#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	struct EditorPanelContext;

	// Tree/search, stable UUID ImGui IDs, multi-select, active toggles and prefab badges. Drag reparent/reorder goes
	// through entity.reparent with keepWorld=true as one action; reject descendant cycles before writing. Selection uses
	// edit.select and clears the selected asset; every mutation is an EditorActions user command.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class SceneHierarchyPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		void ReportFailure(const Error& error);
		std::string m_Search{};
		std::string m_Error{};
		std::vector<uint64_t> m_Tickets{};
	};

}
