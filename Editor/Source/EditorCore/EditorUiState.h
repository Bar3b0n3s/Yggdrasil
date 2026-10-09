#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Engine {

	enum class EditorPanel : uint8_t
	{
		SceneHierarchy,
		Inspector,
		SceneViewport,
		GameViewport,
		ContentBrowser,
		Console,
		Diagnostics,
		ProjectSettings,
		Stats,
		Automation,
		UndoHistory,
		ProjectLauncher
	};

	// Stable names used by editor.state, docking IDs and saved layout; unknown values give an empty view.
	[[nodiscard]] std::string_view EditorPanelToString(EditorPanel panel);

	// Main-thread UI state, owned by EditorContext even headless. Panels derive content from the context, never from ImGui.
	// Frame serial means CPU UI construction completed, not GPU completion. It increases only after EndFrame.
	class EditorUiState
	{
	public:
		[[nodiscard]] std::span<const EditorPanel> GetOpenPanels() const { return m_OpenPanels; }
		[[nodiscard]] AssetHandle GetSelectedAsset() const { return m_SelectedAsset; }
		[[nodiscard]] uint64_t GetCompletedFrame() const { return m_CompletedFrame; }
		// InvalidArgument for unknown panel; stable enum order, no duplicate entries. No scene/history mutation.
		[[nodiscard]] Status SetPanelOpen(EditorPanel panel, bool open);
		// An asset selection clears entity selection through the calling panel; an entity selection clears this value.
		void SetSelectedAsset(AssetHandle asset);
		// Called exactly once after constructing a complete UI frame, including a forced headless screenshot frame.
		void CompleteFrame();
		// Reset docking/panel defaults on explicit user request only, never every frame; ProjectLauncher open iff no project.
		void ResetLayout(bool hasProject);
	private:
		std::vector<EditorPanel> m_OpenPanels{};
		AssetHandle m_SelectedAsset{};
		uint64_t m_CompletedFrame = 0;
	};

}
