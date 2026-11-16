#pragma once

#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace Engine {

	struct EditorPanelContext;

	// M28 (Docs/Decisions/0043-m28-game-services.md decision 8): editor play's save data, the store a live play session starts
	// with and SaveData.Commit writes (EditorPlayController::ReadPlaySaveData: the project's Library/SaveData/Store.json, or a
	// read-only editor's private store). It lists the values, sets one from a key and JSON text, deletes one and clears them
	// all, as savedata.get, savedata.set and savedata.clear do for agents (Roadmap rule 5). Closed by default; opened from the
	// Window menu. The store is read when the panel first shows a project, after each of its writes and on Refresh, never
	// every frame. Main thread inside ImGui.
	class SaveDataPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		void Refresh(EditorPanelContext& context);
		void Write(EditorPanelContext& context, const Json& values);
	private:
		std::optional<VariantValue> m_Values{}; // the store as last read, an object; nullopt before the first read
		std::string m_LoadError{};              // why the store's file could not be read, as last read
		std::string m_Problem{};                // the last read's or write's error
		std::filesystem::path m_Project{};      // the project file the values belong to
		std::array<char, 257> m_Key{};          // the key to set: at most MaxSaveDataKeyBytes bytes and a terminator
		std::array<char, 4096> m_ValueText{};   // its value as JSON text
	};

}
