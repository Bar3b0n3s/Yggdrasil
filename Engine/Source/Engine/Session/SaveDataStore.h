#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 8): a game's save data, the SaveData script module's store. A play
// session owns one, kept across Scene.Load and Scene.Reload and hashed as the state hash's save-data block; its host seeds it
// (a live session's file, a replay's header, a recording's start, a suite's Test.SetSaveData) and writes it at SaveData.Commit
// (a live Runtime's user://SaveData/Store.json, editor play's <project>/Library/SaveData/Store.json).
//
// The values are canonical, so the hash, the file and a replay's header see one form: object keys sorted at every depth and
// every number a double (a script's numbers are doubles; a file's integers read as doubles). The store file:
//   {"Format": "SaveData", "Version": 1, "Values": {<key>: <value>, ...}}
// written tab-indented with nlohmann's shortest round-trip doubles (never JsonWriter, which writes 32-bit floats), so a
// double the game saved reads back exactly.

namespace Engine {

	class XXH64Hasher;

	inline constexpr std::string_view SaveDataFormatName = "SaveData";
	inline constexpr uint32_t SaveDataFormatVersion = 1;

	class SaveDataStore
	{
	public:
		// A key: 1 to MaxSaveDataKeyBytes bytes of valid UTF-8 (ScriptLimits.h). Errors: InvalidArgument naming the problem.
		[[nodiscard]] static Status ValidateKey(std::string_view key);
		// `value` in the store's canonical form (keys sorted at every depth, numbers as doubles). Errors: InvalidArgument for a
		// non-finite number or a value nested deeper than MaxJsonDepth.
		[[nodiscard]] static Result<Json> Canonicalize(const Json& value);

		// The value of `key`, or null when the store does not hold it.
		[[nodiscard]] const Json* Find(std::string_view key) const;
		// Sets `key` to `value` (null deletes it). Errors, the store unchanged: ValidateKey's and Canonicalize's, and
		// InvalidArgument when the store's compact JSON would exceed MaxSaveDataBytes, naming both sizes.
		[[nodiscard]] Status Set(std::string_view key, const Json& value);
		// Removes `key`; whether the store held it.
		bool Delete(std::string_view key);
		// Replaces every value with `values`, an object (nothing is replaced on error). Errors: InvalidArgument for a
		// non-object, and Set's for any member.
		[[nodiscard]] Status Replace(const Json& values);
		void Clear();

		// The keys in byte order.
		[[nodiscard]] std::vector<std::string> GetKeys() const;
		[[nodiscard]] bool IsEmpty() const { return m_Values.empty(); }
		[[nodiscard]] size_t GetSize() const { return m_Values.size(); }
		// The size of the store's compact JSON ({"key":value,...}), the measure MaxSaveDataBytes caps.
		[[nodiscard]] size_t GetSerializedBytes() const;
		// The values as one object, keys sorted.
		[[nodiscard]] Json ToJson() const;

		// The state hash's save-data block (after the ui block): nothing while the store is empty, so every hash of a session
		// that never saves stays; else the marker "SaveDat1", the count and each key and its value's compact JSON, in key
		// order.
		void AppendStateHash(XXH64Hasher& hasher) const;
	private:
		std::map<std::string, VariantValue, std::less<>> m_Values{};
		// The compact JSON size of each entry ("key":value), which GetSerializedBytes sums.
		std::map<std::string, size_t, std::less<>> m_EntryBytes{};
	};

	// The store file's text for `values` (an object of canonical values). Errors: InvalidArgument for a non-object.
	[[nodiscard]] Result<std::string> SerializeSaveDataFile(const Json& values);
	// The values a store file holds, canonical and within the store's limits. Errors: the JSON reader's (located), Validation
	// for another format, a missing or non-object Values, a key or value the store refuses (located at its member) and
	// UnsupportedVersion for a newer version.
	[[nodiscard]] Result<Json> ParseSaveDataFile(std::string_view text);

	// What a session starts with from a store file: its values, and the problem that made it start empty.
	struct SaveDataFileLoad
	{
		VariantValue Values{};              // an object (LoadSaveDataFile always sets one)
		std::optional<std::string> Error{}; // SaveData.GetLoadError's text
	};

	// Reads the store file `file`. A missing file is an empty store. A file that cannot be read or parsed is logged as a
	// warning, moved aside as "Store.corrupt-<n>.json" beside it (the first free n from 1, so an earlier one is kept) and
	// reported in Error, and the store starts empty; when it cannot be moved aside, the Error says so too.
	[[nodiscard]] SaveDataFileLoad LoadSaveDataFile(const std::filesystem::path& file);
	// Writes `values` to `file`, creating its directory, replaced atomically and flushed durably to the disk on Windows
	// (PublishedFile::WriteDurably); where there is no durable flush (Linux and macOS) it falls back to the atomic write without
	// a flush (FileSystem::WriteFileAtomic), logged at Info. Errors: SerializeSaveDataFile's and the file system's.
	[[nodiscard]] Status WriteSaveDataFile(const std::filesystem::path& file, const Json& values);

}
