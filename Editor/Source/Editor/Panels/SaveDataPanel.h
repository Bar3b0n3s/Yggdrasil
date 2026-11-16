#pragma once

#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"
#include "Engine/Scripting/ScriptLimits.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Engine {

	struct EditorPanelContext;

	// M28 (Docs/Decisions/0043-m28-game-services.md decision 8): editor play's save data, the store a live play session starts
	// with and SaveData.Commit writes (EditorPlayController::ReadPlaySaveData: the project's Library/SaveData/Store.json, or a
	// read-only editor's private store). It lists the values, sets one from a key and JSON text, deletes one and clears them
	// all, as savedata.get, savedata.set and savedata.clear do for agents (Roadmap rule 5). Closed by default; opened from the
	// Window menu. The store is read when the panel first shows a project, before and after each of its writes and on Refresh, never
	// every frame. Main thread inside ImGui.
	class SaveDataPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
		// The Set button's and a row's Delete: sets `key` to the JSON `valueText`, or deletes it (nullopt), in the store as it
		// is now, read again first, so a commit or a savedata.set made since the panel last read it is kept (the W28.2
		// review's finding 5). A problem (text that is not JSON, a key or value the store refuses, a failed read or write) is
		// shown and changes nothing; a store file that read finds unreadable is moved aside, and the panel shows why.
		void ApplyChange(EditorPanelContext& context, std::string_view key, std::optional<std::string_view> valueText);
	private:
		void Refresh(EditorPanelContext& context);
		void Write(EditorPanelContext& context, const Json& values);
	private:
		std::optional<VariantValue> m_Values{};            // the store as last read, an object; nullopt before the first read
		std::string m_LoadError{};                         // why the store's file could not be read, as last read
		std::string m_Problem{};                           // the last read's or write's error
		std::filesystem::path m_Project{};                 // the project file the values belong to
		std::array<char, MaxSaveDataKeyBytes + 1> m_Key{}; // the key to set: at most MaxSaveDataKeyBytes bytes and a terminator
		std::array<char, 4096> m_ValueText{};              // its value as JSON text
	};

}
