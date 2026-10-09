#include "EditorPCH.h"
#include "EditorCore/EditorUiState.h"

#include <algorithm>
#include <array>

namespace Engine {

	std::string_view EditorPanelToString(EditorPanel panel)
	{
		constexpr std::array Names{ "SceneHierarchy", "Inspector", "SceneViewport", "GameViewport", "ContentBrowser", "Console", "Diagnostics", "ProjectSettings", "Stats", "Automation", "UndoHistory", "ProjectLauncher" };
		const size_t index = static_cast<size_t>(panel);
		return index < Names.size() ? std::string_view(Names[index]) : std::string_view{};
	}

	Status EditorUiState::SetPanelOpen(EditorPanel panel, bool open)
	{
		if (EditorPanelToString(panel).empty())
			return MakeError(ErrorCode::InvalidArgument, "unknown editor panel");
		auto position = std::lower_bound(m_OpenPanels.begin(), m_OpenPanels.end(), panel);
		if (open && (position == m_OpenPanels.end() || *position != panel))
			m_OpenPanels.insert(position, panel);
		else if (!open && position != m_OpenPanels.end() && *position == panel)
			m_OpenPanels.erase(position);
		return {};
	}

	void EditorUiState::SetSelectedAsset(AssetHandle asset)
	{
		m_SelectedAsset = asset;
	}

	void EditorUiState::CompleteFrame()
	{
		++m_CompletedFrame;
	}

	void EditorUiState::ResetLayout(bool hasProject)
	{
		m_OpenPanels = { EditorPanel::SceneHierarchy, EditorPanel::Inspector, EditorPanel::SceneViewport, EditorPanel::GameViewport, EditorPanel::ContentBrowser, EditorPanel::Console };
		if (!hasProject)
			m_OpenPanels.push_back(EditorPanel::ProjectLauncher);
	}

}
