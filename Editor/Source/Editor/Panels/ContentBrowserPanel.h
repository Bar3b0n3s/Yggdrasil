#pragma once

#include "Editor/FileDialog.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace Engine {

	class EditorContext;
	struct EditorPanelContext;

	// Project folder tree/grid, type icons, cached thumbnails and import-error badges. Create folder/scene/material/
	// prefab/sound effect through existing asset/prefab methods, and scripts through the registered templates.
	// Rename/move preserve handles; delete uses undoable trash. OS drops use asset.import, preview audio uses AudioPreview.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	//
	// Draw never walks the project per frame: the folder listing (one VFS listing) and the current folder's tiles are
	// cached and rebuilt only after a change signal: a new project, a finished panel operation (Refresh included), a
	// command executed, undone or redone, a changed asset registry revision (AssetRegistry::GetRevision, which also follows
	// files renamed or moved outside the editor) or diagnostics count, or an AssetReloaded or AssetImportFailed event.
	// Opening a scene over unsaved changes asks to save or discard first. OS drops that arrive while the panel was not
	// drawn ask for their destination folder before anything is imported; a read-only project reports them as not
	// imported. Thumbnail failures stay on their tile (icon and tooltip), never in the panel's operation error line.
	class ContentBrowserPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		struct Tile
		{
			AssetHandle Handle{};
			AssetType Type = AssetType::None;
			std::string Label{};
			std::string TypeLabel{};
			std::string Path{}; // the reference path, for search, tooltips and scene.open
			std::string SearchKey{};
			std::string Issue{}; // the first import diagnostic of the source, empty without one
			bool SubAsset = false;
		};
		void DetectChanges(EditorContext& editor);
		void RefreshListing(EditorContext& editor);
		void RefreshTiles(EditorContext& editor);
	private:
		std::string m_Project{};
		std::string m_Directory = "Assets";
		std::array<char, 256> m_Search{};
		std::array<char, 256> m_CreateName{};
		std::array<char, 4096> m_ImportSource{};
		std::array<char, 4096> m_MovePath{};
		FileDialog m_ImportDialog{}; // the import source: the native dialog or the ImGui picker (Editor/FileDialog.h)
		std::string m_ImportDirectory{};
		std::string m_ActionDirectory{};
		AssetHandle m_ActionAsset{};
		std::vector<uint64_t> m_Tickets{};
		std::string m_Error{};
		int m_CreateType = 0;
		int m_AssetDialog = 0;
		bool m_ReopenImport = false;
		// Cached listing and its change signals.
		std::vector<std::string> m_Folders{};                           // every folder below Assets, sorted
		std::map<std::string, std::vector<std::string>> m_Subfolders{}; // parent folder -> its folders, sorted
		std::string m_ListingError{};
		std::vector<Tile> m_Tiles{}; // the assets directly in m_TilesDirectory, by reference path
		std::string m_TilesDirectory{};
		bool m_ListingStale = true;
		bool m_TilesStale = true;
		bool m_HasSignals = false;
		uint64_t m_HistorySequence = 0;
		size_t m_HistoryUndoCount = 0;
		size_t m_HistoryRedoCount = 0;
		uint64_t m_RegistryRevision = 0;
		size_t m_DiagnosticCount = 0;
		uint64_t m_EventCursor = 0;
		// OS drops awaiting a destination, and a scene open awaiting the save/discard choice.
		std::vector<std::filesystem::path> m_Drops{};
		std::string m_DropDirectory{};
		int m_LastDrawFrame = -1;
		bool m_OpenDropPrompt = false;
		std::string m_PendingScene{};
		bool m_OpenScenePrompt = false;
	};

}
