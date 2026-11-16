#include "TestsPCH.h"
#include "Editor/Panels/SaveDataPanel.h"

#include "Editor/Panels/UtilityPanelFixture.h"
#include "Editor/SupportingPanelTestUi.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Session/SaveDataStore.h"
#include "Support/ExpectLog.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 8): the Save Data panel over editor play's store.

namespace Engine {

	// The play store as it is on disk now.
	[[nodiscard]] static Json ReadPlayStore(Test::UtilityPanelFixture& fixture)
	{
		const Result<SaveDataFileLoad> load = fixture.GetEditor().GetPlay().ReadPlaySaveData();
		REQUIRE_MESSAGE(load.has_value(), load.error().ToString());
		return load->Values.Get();
	}

	TEST_SUITE("Editor")
	{
		// The W28.2 review's finding 5: the panel reads the store when it first shows and after its own writes, so a game's
		// commit (or a savedata.set) made since then must not be lost when the panel sets or deletes a value.
		TEST_CASE("SaveDataPanel: Set and Delete apply to the store as it is now, keeping a commit made after the panel read it")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			SaveDataPanel panel;
			fixture.Draw(panel);
			EditorPlayController& play = fixture.GetEditor().GetPlay();

			// The game commits its progress while the panel shows the empty store, then the developer sets a value.
			REQUIRE(play.WritePlaySaveData(Json{ { "Realm", 2.0 } }).has_value());
			panel.ApplyChange(fixture.GetContext(), "Volume", "0.5");
			CHECK(ReadPlayStore(fixture) == Json{ { "Realm", 2.0 }, { "Volume", 0.5 } });
			CHECK(fixture.Draw(panel).contains("Volume"));

			// Another commit, then a row's Delete (the second row, Volume): the new key stays.
			REQUIRE(play.WritePlaySaveData(Json{ { "Realm", 2.0 }, { "Volume", 0.5 }, { "Wave", 3.0 } }).has_value());
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Delete", 1);
			CHECK(ReadPlayStore(fixture) == Json{ { "Realm", 2.0 }, { "Wave", 3.0 } });

			// A problem changes nothing and is shown.
			panel.ApplyChange(fixture.GetContext(), "Name", "not json");
			panel.ApplyChange(fixture.GetContext(), "", "1");
			CHECK(ReadPlayStore(fixture) == Json{ { "Realm", 2.0 }, { "Wave", 3.0 } });
			const Status refused = SaveDataStore::ValidateKey("");
			REQUIRE_FALSE(refused.has_value());
			CHECK(fixture.Draw(panel).contains(refused.error().GetMessageText()));
		}

		// The store file became unreadable after the panel read it: the Set's own read moves it aside and the value is set in
		// an empty store, and the panel still shows why, although its refresh after the write finds a readable file.
		TEST_CASE("SaveDataPanel: a Set that finds the store unreadable shows why after its write")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			SaveDataPanel panel;
			fixture.Draw(panel);
			const std::filesystem::path file = fixture.GetEditor().GetProject().GetRoot() / "Library" / "SaveData" / "Store.json";
			REQUIRE(FileSystem::CreateDirectories(file.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(file, AsBytes("{\"Format\": \"SaveData\""), { .KeepBackup = false }).has_value());

			Test::ExpectLog warning(LogLevel::Warn, "save data");
			panel.ApplyChange(fixture.GetContext(), "Volume", "0.5");
			CHECK(ReadPlayStore(fixture) == Json{ { "Volume", 0.5 } });
			CHECK(fixture.Draw(panel).contains("Store.corrupt-1.json"));
		}
	}

}
