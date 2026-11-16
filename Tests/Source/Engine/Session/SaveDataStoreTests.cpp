#include "TestsPCH.h"
#include "Engine/Session/SaveDataStore.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Support/ExpectLog.h"
#include "Support/Fuzz.h"
#include "Support/TempDirectory.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <span>
#include <string>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 8): the save-data store, its state-hash block and its file.

namespace Engine {

	namespace {

		[[nodiscard]] uint64_t HashOf(const SaveDataStore& store)
		{
			XXH64Hasher hasher;
			store.AppendStateHash(hasher);
			return hasher.Digest();
		}

		void WriteText(const std::filesystem::path& path, std::string_view text)
		{
			REQUIRE(FileSystem::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size())), { .KeepBackup = false }).has_value());
		}

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("SaveDataStore: values are set, read, deleted and listed in key order")
		{
			SaveDataStore store;
			CHECK(store.IsEmpty());
			CHECK(store.GetSerializedBytes() == 2);
			REQUIRE(store.Set("Volume", Json(0.5)));
			REQUIRE(store.Set("Name", Json("Ash")));
			REQUIRE(store.Set("Unlocked", Json::array({ "Fire", "Ice" })));
			CHECK(store.GetKeys() == std::vector<std::string>{ "Name", "Unlocked", "Volume" });
			REQUIRE(store.Find("Volume") != nullptr);
			CHECK(*store.Find("Volume") == Json(0.5));
			CHECK(store.Find("Missing") == nullptr);
			// The compact JSON size: {"Name":"Ash","Unlocked":["Fire","Ice"],"Volume":0.5}
			CHECK(store.GetSerializedBytes() == std::string(R"({"Name":"Ash","Unlocked":["Fire","Ice"],"Volume":0.5})").size());
			REQUIRE(store.Set("Volume", Json(nullptr)));
			CHECK(store.Find("Volume") == nullptr);
			CHECK(store.Delete("Name"));
			CHECK_FALSE(store.Delete("Name"));
			CHECK(store.GetKeys() == std::vector<std::string>{ "Unlocked" });
			store.Clear();
			CHECK(store.IsEmpty());
		}

		TEST_CASE("SaveDataStore: values are canonical, object keys sorted and every number a double")
		{
			SaveDataStore store;
			Json progress = Json::object();
			progress["Zone"] = 3;
			progress["Coins"] = 12.5;
			progress["Flags"] = Json::array({ 1, true, nullptr });
			REQUIRE(store.Set("Progress", progress));
			CHECK(store.Find("Progress")->dump() == R"({"Coins":12.5,"Flags":[1.0,true,null],"Zone":3.0})");
			// The same values set from another order and number type hash alike.
			SaveDataStore other;
			REQUIRE(other.Replace(Json::parse(R"({"Progress": {"Zone": 3.0, "Flags": [1, true, null], "Coins": 12.5}})", nullptr, false)));
			CHECK(HashOf(store) == HashOf(other));
		}

		TEST_CASE("SaveDataStore: keys, values and sizes outside the limits are refused and leave the store unchanged")
		{
			SaveDataStore store;
			REQUIRE(store.Set("Kept", Json(1.0)));
			const auto refused = [&store](std::string_view key, const Json& value)
			{
				CAPTURE(std::string(key));
				const Status set = store.Set(key, value);
				REQUIRE_FALSE(set.has_value());
				CHECK(set.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(store.GetKeys() == std::vector<std::string>{ "Kept" });
			};
			refused("", Json(1.0));
			refused(std::string(MaxSaveDataKeyBytes + 1, 'k'), Json(1.0));
			refused("\xff", Json(1.0));
			CHECK(store.Set(std::string(MaxSaveDataKeyBytes, 'k'), Json(1.0)).has_value());
			REQUIRE(store.Delete(std::string(MaxSaveDataKeyBytes, 'k')));
			// A value nests at most MaxSaveDataDepth arrays and tables, far below the readers' MaxJsonDepth, which the store file
			// and a replay header add levels to (the W28.2 review's finding 4): at the limit it round-trips through the file.
			Json deep = Json::array();
			for (size_t level = 1; level < MaxSaveDataDepth; ++level)
				deep = Json::object({ { "Next", std::move(deep) } });
			REQUIRE(store.Set("Deep", deep).has_value());
			const Result<std::string> deepFile = SerializeSaveDataFile(store.ToJson());
			REQUIRE(deepFile.has_value());
			const Result<Json> deepRead = ParseSaveDataFile(*deepFile);
			REQUIRE_MESSAGE(deepRead.has_value(), deepRead.error().ToString());
			CHECK(*deepRead == store.ToJson());
			REQUIRE(store.Delete("Deep"));
			refused("Deep", Json::array({ deep }));
			refused("Text", Json("\xc3"));
			// The cap counts the whole store: a value that fits alone does not fit beside another.
			const std::string half(MaxSaveDataBytes / 2, 'x');
			REQUIRE(store.Set("A", Json(half)));
			const Status over = store.Set("B", Json(half));
			REQUIRE_FALSE(over.has_value());
			CHECK(over.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(store.GetKeys() == std::vector<std::string>{ "A", "Kept" });
			CHECK(store.GetSerializedBytes() <= MaxSaveDataBytes);
			REQUIRE(store.Set("A", Json(nullptr)));
			CHECK(store.Set("B", Json(half)).has_value());
			// Replace is all or nothing.
			const SaveDataStore before = store;
			CHECK_FALSE(store.Replace(Json::parse(R"({"Fine": 1, "": 2})", nullptr, false)).has_value());
			CHECK(store.GetKeys() == before.GetKeys());
			CHECK_FALSE(store.Replace(Json::array()).has_value());
		}

		TEST_CASE("SaveDataStore: the state-hash block is empty for an empty store and follows every value")
		{
			SaveDataStore store;
			CHECK(HashOf(store) == XXH64Hasher().Digest());
			REQUIRE(store.Set("Realm", Json(2.0)));
			const uint64_t two = HashOf(store);
			CHECK(two != XXH64Hasher().Digest());
			REQUIRE(store.Set("Realm", Json(3.0)));
			CHECK(HashOf(store) != two);
			REQUIRE(store.Set("Realm", Json(2.0)));
			CHECK(HashOf(store) == two);
		}

		TEST_CASE("SaveDataStore: the store file keeps doubles exactly and round-trips")
		{
			SaveDataStore store;
			REQUIRE(store.Set("Tenth", Json(0.1)));
			REQUIRE(store.Set("Large", Json(9007199254740993.0)));
			REQUIRE(store.Set("Tiny", Json(5e-324)));
			REQUIRE(store.Set("Nested", Json::parse(R"({"b": [1, "two"], "a": {"c": false}})", nullptr, false)));
			const Result<std::string> text = SerializeSaveDataFile(store.ToJson());
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			CHECK(text->starts_with("{\n\t\"Format\": \"SaveData\",\n\t\"Version\": 1,\n\t\"Values\": {"));
			CHECK(text->ends_with("}\n"));
			const Result<Json> read = ParseSaveDataFile(*text);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(*read == store.ToJson());
			SaveDataStore again;
			REQUIRE(again.Replace(*read));
			CHECK(HashOf(again) == HashOf(store));
		}

		TEST_CASE("SaveDataStore: another format, a newer version or a value the store refuses fails the file at its member")
		{
			const auto refused = [](std::string_view text, ErrorCode code, std::string_view pointer)
			{
				CAPTURE(std::string(text));
				const Result<Json> read = ParseSaveDataFile(text);
				REQUIRE_FALSE(read.has_value());
				CHECK(read.error().GetCode() == code);
				if (!pointer.empty())
					CHECK(read.error().GetLocation().JsonPointer.value_or("") == pointer);
			};
			refused("{\"Format\": \"SaveData\", \"Version\": 1, \"Values\": {", ErrorCode::Parse, "");
			refused(R"({"Format": "DisplaySettings", "Version": 1, "Values": {}})", ErrorCode::Validation, "/Format");
			refused(R"({"Format": "SaveData", "Version": 2, "Values": {}})", ErrorCode::UnsupportedVersion, "");
			refused(R"({"Format": "SaveData", "Version": 1})", ErrorCode::Validation, "");
			refused(R"({"Format": "SaveData", "Version": 1, "Values": []})", ErrorCode::Validation, "/Values");
			refused(R"({"Format": "SaveData", "Version": 1, "Values": {"": 1}})", ErrorCode::Validation, "/Values/");
			CHECK(ParseSaveDataFile(R"({"Format": "SaveData", "Version": 1, "Values": {}})").has_value());
		}

		TEST_CASE("SaveDataStore: a store file that cannot be read is moved aside as Store.corrupt-<n>.json and reported")
		{
			const Test::TempDirectory temp;
			const std::filesystem::path file = temp.GetPath() / "SaveData" / "Store.json";
			// No file: an empty store and no error.
			const SaveDataFileLoad missing = LoadSaveDataFile(file);
			CHECK(missing.Values.Get() == Json::object());
			CHECK_FALSE(missing.Error.has_value());
			// A written store reads back.
			REQUIRE(WriteSaveDataFile(file, Json::parse(R"({"Realm": 2.0})", nullptr, false)));
			const SaveDataFileLoad written = LoadSaveDataFile(file);
			CHECK(written.Values.Get() == Json::parse(R"({"Realm": 2.0})", nullptr, false));
			CHECK_FALSE(written.Error.has_value());
			// Truncated by hand: logged, kept as Store.corrupt-1.json, reported, and the store starts empty; a second one is
			// kept as Store.corrupt-2.json beside the first.
			for (const int copy : { 1, 2 })
			{
				CAPTURE(copy);
				WriteText(file, "{\"Format\": \"SaveData\", \"Version\": 1, \"Values\": {\"Realm\"");
				Test::ExpectLog warning(LogLevel::Warn, "save data");
				const SaveDataFileLoad corrupt = LoadSaveDataFile(file);
				CHECK(warning.GetMatchCount() == 1);
				CHECK(corrupt.Values.Get() == Json::object());
				REQUIRE(corrupt.Error.has_value());
				CHECK(corrupt.Error->find(std::format("Store.corrupt-{}.json", copy)) != std::string::npos);
				std::error_code error;
				CHECK(std::filesystem::exists(temp.GetPath() / "SaveData" / std::format("Store.corrupt-{}.json", copy), error));
				CHECK_FALSE(std::filesystem::exists(file, error));
			}
		}

		// The W28.2 review's finding 6: a headless run (the Dist smoke test, on the player's real user data) never writes the
		// store, so it leaves one it cannot read where it is, unchanged, for the next run that may write it to move aside and
		// report to the player.
		TEST_CASE("SaveDataStore: a store file that cannot be read is left in place, unchanged, when the run never writes it")
		{
			const Test::TempDirectory temp;
			const std::filesystem::path file = temp.GetPath() / "SaveData" / "Store.json";
			const std::string truncated = "{\"Format\": \"SaveData\", \"Version\": 1, \"Values\": {\"Realm\"";
			WriteText(file, truncated);
			Test::ExpectLog warning(LogLevel::Warn, "save data");
			const SaveDataFileLoad headless = LoadSaveDataFile(file, SaveDataCorruption::LeaveInPlace);
			CHECK(warning.GetMatchCount() == 1);
			CHECK(headless.Values.Get() == Json::object());
			CHECK(headless.Error.has_value());
			CHECK(FileSystem::ReadText(file).value_or("") == truncated);
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(temp.GetPath() / "SaveData" / "Store.corrupt-1.json", error));
			// The next run that may write the store moves it aside and reports it.
			const SaveDataFileLoad live = LoadSaveDataFile(file);
			REQUIRE(live.Error.has_value());
			CHECK(live.Error->find("Store.corrupt-1.json") != std::string::npos);
			CHECK_FALSE(std::filesystem::exists(file, error));
		}

		// The W28.2 review's finding 8a: when every Store.corrupt-<n>.json name is taken, the file stays where it is and the
		// error the session reports says it could not be moved aside.
		TEST_CASE("SaveDataStore: a store file that cannot be moved aside stays where it is and its error says so")
		{
			const Test::TempDirectory temp;
			const std::filesystem::path directory = temp.GetPath() / "SaveData";
			const std::filesystem::path file = directory / "Store.json";
			for (int copy = 1; copy <= 1000; ++copy)
				WriteText(directory / std::format("Store.corrupt-{}.json", copy), "{}");
			const std::string truncated = "{\"Format\": \"SaveData\"";
			WriteText(file, truncated);
			Test::ExpectLog warning(LogLevel::Warn, "save data");
			const SaveDataFileLoad load = LoadSaveDataFile(file);
			CHECK(warning.GetMatchCount() == 1);
			CHECK(load.Values.Get() == Json::object());
			REQUIRE(load.Error.has_value());
			CHECK(load.Error->find("could not be moved aside") != std::string::npos);
			CHECK(FileSystem::ReadText(file).value_or("") == truncated);
		}

		TEST_CASE("SaveDataStore: 1,000 seeded mutations of Store.json never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			SaveDataStore store;
			REQUIRE(store.Set("Volume", Json(0.75)));
			REQUIRE(store.Set("Progress", Json::parse(R"({"Realm": 2, "Seen": ["Fire", "Ice"], "Best": 12.5})", nullptr, false)));
			const Result<std::string> written = SerializeSaveDataFile(store.ToJson());
			REQUIRE(written.has_value());
			const std::array<std::string, 2> seeds = { *written,
				R"({"Format":"SaveData","Version":1,"Values":{"a":null,"b":[[[1e300]]],"c":{"d":"é"},"e":-0.0,"f":true}})" };
			const Test::FuzzRun run = Test::RunFuzzCase(0x5A7E, 1000, [&seeds](Random& random, uint64_t /*index*/)
			{
				std::string mutated = seeds[static_cast<size_t>(random.RangeInt(0, 1))];
				Test::MutateText(mutated, random);
				const Result<Json> read = ParseSaveDataFile(mutated);
				if (read.has_value())
				{
					// Whatever the reader accepts, the store takes, the writer writes and the reader reads the same.
					SaveDataStore accepted;
					REQUIRE(accepted.Replace(*read));
					const Result<std::string> again = SerializeSaveDataFile(accepted.ToJson());
					REQUIRE(again.has_value());
					CHECK(ParseSaveDataFile(*again).value_or(Json()) == *read);
					return true;
				}
				const ErrorCode code = read.error().GetCode();
				CHECK((code == ErrorCode::Parse || code == ErrorCode::Validation || code == ErrorCode::UnsupportedVersion));
				return false;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}
	}

}
