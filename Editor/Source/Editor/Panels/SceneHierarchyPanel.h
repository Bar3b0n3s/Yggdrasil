#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	struct EditorPanelContext;

	// Case-insensitive tree/search, stable UUID ImGui IDs, multi-select, active toggles and prefab badges. Rename,
	// duplicate and create-child actions live in each row's context menu. Drag reparent/reorder goes
	// through entity.reparent with keepWorld=true as one action; reject descendant cycles before writing. Selection uses
	// edit.select and clears the selected asset; every mutation is an EditorActions user command.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	// Pending rename identity includes the edit revision, play serial and runtime scene generation, not only entity UUID.
	class SceneHierarchyPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		void ReportFailure(const Error& error);
		std::string m_Search{};
		std::string m_Error{};
		std::string m_Rename{};
		UUID m_RenameEntity{};
		uint64_t m_RenameRevision = 0;
		uint64_t m_RenameSessionSerial = 0;
		uint64_t m_RenameSceneGeneration = 0;
		bool m_RenamePlay = false;
		bool m_ShowRename = false;
		std::vector<uint64_t> m_Tickets{};
	};

}
