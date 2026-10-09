#pragma once

#include "Engine/Core/Result.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	struct EditorPanelContext;

	// Project folder tree/grid, type icons, cached thumbnails and import-error badges. Create folder/scene/material/
	// prefab/sound effect through existing asset/prefab methods; script creation is explicitly unavailable until M13.
	// Rename/move preserve handles; delete uses undoable trash. OS drops use asset.import, preview audio uses AudioPreview.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class ContentBrowserPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		std::string m_Project{};
		std::string m_Directory = "Assets";
		std::array<char, 256> m_Search{};
		std::array<char, 256> m_CreateName{};
		std::array<char, 4096> m_ImportSource{};
		std::array<char, 4096> m_MovePath{};
		std::vector<uint64_t> m_Tickets{};
		std::string m_Error{};
		int m_CreateType = 0;
	};

}
