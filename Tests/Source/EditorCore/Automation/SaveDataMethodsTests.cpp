#include "TestsPCH.h"
#include "EditorCore/Automation/SaveDataMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"
#include "Support/PlatformProbes.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <system_error>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 8): the savedata.* methods in process.

namespace Engine {

	// The `loadError` member of a savedata result; "<absent>" when it has none.
	[[nodiscard]] static std::string ReadLoadError(const Json& result)
	{
		const std::optional<JsonReader> member = JsonReader(result).FindMember("loadError");
		return member.has_value() ? member->ReadString().value_or("<not a string>") : std::string("<absent>");
	}

	// Writes an unreadable play store, a save-data file cut short.
	static void WriteTruncatedStore(const std::filesystem::path& directory)
	{
		REQUIRE(FileSystem::CreateDirectories(directory).has_value());
		REQUIRE(FileSystem::WriteFileAtomic(directory / "Store.json", AsBytes("{\"Format\": \"SaveData\""), { .KeepBackup = false }).has_value());
	}

	TEST_SUITE("EditorCore")
	{
		// A merge reads the play store first, and that read moves a store file it cannot read aside, so a savedata.get after
		// the write finds a readable file: the merge's result (or its error) says why, as savedata.get would have.
		TEST_CASE("SaveDataMethods: savedata.set with merge reports a store file its read moved aside")
		{
			Test::AutomationFixture setup("SaveDataSetMerge", false);
			const std::filesystem::path directory = setup.GetEditor().GetProject().GetRoot() / "Library" / "SaveData";
			Test::ExpectLog warning(LogLevel::Warn, "save data");
			std::error_code error;

			WriteTruncatedStore(directory);
			const Result<Json> merged = setup.Call("savedata.set", Json{ { "values", Json{ { "Realm", 2 } } }, { "merge", true } });
			REQUIRE_MESSAGE(merged.has_value(), merged.error().ToString());
			CHECK(ReadLoadError(*merged).contains("Store.corrupt-1.json"));
			CHECK(std::filesystem::exists(directory / "Store.corrupt-1.json", error));
			const Result<Json> read = setup.Call("savedata.get", Json::object());
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			const std::optional<JsonReader> values = JsonReader(*read).FindMember("values");
			REQUIRE(values.has_value());
			CHECK(values->GetValue() == Json{ { "Realm", 2.0 } });
			CHECK(ReadLoadError(*read).empty());

			// A merge of a readable store, and a write that replaces the store, have nothing to report.
			const Result<Json> again = setup.Call("savedata.set", Json{ { "values", Json{ { "Wave", 3 } } }, { "merge", true } });
			REQUIRE_MESSAGE(again.has_value(), again.error().ToString());
			CHECK(ReadLoadError(*again).empty());
			const Result<Json> replaced = setup.Call("savedata.set", Json{ { "values", Json::object() } });
			REQUIRE_MESSAGE(replaced.has_value(), replaced.error().ToString());
			CHECK(ReadLoadError(*replaced).empty());

			// A value the merge refuses after its read moved the file aside: the error's contexts say why too, and nothing is
			// written.
			WriteTruncatedStore(directory);
			Json refused = setup.Request("savedata.set", Json{ { "values", Json{ { "", 1 } } }, { "merge", true } });
			REQUIRE(refused.contains("error"));
			CHECK(refused["error"]["data"]["contexts"].dump().contains("Store.corrupt-2.json"));
			CHECK_FALSE(std::filesystem::exists(directory / "Store.json", error));
		}

		// Batch 2's merge review (finding 11): a write that fails after the merge's read moved the store file aside says why
		// the file was moved, as a refused value does. On Windows the durable write's temporary (Store.json.tmp-<pid>) cannot
		// be created while a directory stands in its place; elsewhere the store is written without the durable path, whose
		// temporary that does not block.
		TEST_CASE("SaveDataMethods: savedata.set with merge reports a store file its read moved aside when its write fails")
		{
#if defined(ENGINE_PLATFORM_WINDOWS)
			Test::AutomationFixture setup("SaveDataSetMergeWriteFails", false);
			const std::filesystem::path directory = setup.GetEditor().GetProject().GetRoot() / "Library" / "SaveData";
			Test::ExpectLog warning(LogLevel::Warn, "save data");
			std::error_code error;
			WriteTruncatedStore(directory);
			const std::filesystem::path temporary = directory / std::format("Store.json.tmp-{}", Test::GetCurrentProcessIdentifier());
			REQUIRE(std::filesystem::create_directories(temporary / "Blocker", error));
			Json failed = setup.Request("savedata.set", Json{ { "values", Json{ { "Realm", 2 } } }, { "merge", true } });
			REQUIRE(failed.contains("error"));
			CHECK(failed["error"]["data"]["contexts"].dump().contains("Store.corrupt-1.json"));
			CHECK(std::filesystem::exists(directory / "Store.corrupt-1.json", error));
			CHECK_FALSE(std::filesystem::exists(directory / "Store.json", error));
#else
			MESSAGE("the durable write has a Windows backend only (Docs/Decisions/0028-backlog-program.md decision 10)");
#endif
		}
	}

}
