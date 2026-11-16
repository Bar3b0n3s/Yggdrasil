#include "EnginePCH.h"
#include "Engine/Session/SaveDataStore.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Platform/PublishedFile.h"
#include "Engine/Scripting/ScriptLimits.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		// The most "Store.corrupt-<n>.json" names LoadSaveDataFile tries before it gives up moving a corrupt file aside.
		constexpr uint32_t MaxCorruptSaveDataCopies = 1000;

		// The leaves of CanonicalizeSaveValue, out of its recursion so that its frame stays small: a value nested
		// MaxSaveDataDepth deep must fit the stack of a Debug build. Each returns the problem, or empty.
		[[nodiscard]] static std::string CanonicalizeSaveNumber(const Json& value, Json& out)
		{
			const Result<double> number = JsonReader(value).ReadDouble();
			if (!number || !std::isfinite(*number))
				return "a number is not finite";
			out = Json(*number);
			return {};
		}

		[[nodiscard]] static std::string CheckSaveString(const Json& value)
		{
			const Result<std::string> text = JsonReader(value).ReadString();
			return text && IsValidUtf8(*text) ? std::string() : std::string("a string is not valid UTF-8");
		}

		// `value` canonical in `out`: object keys sorted at every depth, every number a double. `depth` counts the arrays and
		// objects around `value`, so a container at depth MaxSaveDataDepth is one level too deep. Returns the problem, or empty.
		[[nodiscard]] static std::string CanonicalizeSaveValue(const Json& value, Json& out, size_t depth)
		{
			if ((value.is_object() || value.is_array()) && depth >= MaxSaveDataDepth)
				return std::format("a save-data value nests at most {} arrays and tables", MaxSaveDataDepth);
			if (value.is_object())
			{
				std::vector<std::pair<std::string_view, const Json*>> members;
				members.reserve(value.size());
				for (const auto& [key, member] : value.items())
				{
					if (!IsValidUtf8(key))
						return "an object key is not valid UTF-8";
					members.emplace_back(key, &member);
				}
				std::ranges::sort(members, {}, &std::pair<std::string_view, const Json*>::first);
				out = Json::object();
				for (const auto& [key, member] : members)
				{
					Json& child = out[std::string(key)];
					if (std::string problem = CanonicalizeSaveValue(*member, child, depth + 1); !problem.empty())
						return problem;
				}
				return {};
			}
			if (value.is_array())
			{
				out = Json::array();
				for (const Json& element : value)
				{
					out.push_back(Json());
					if (std::string problem = CanonicalizeSaveValue(element, out.back(), depth + 1); !problem.empty())
						return problem;
				}
				return {};
			}
			if (value.is_number())
				return CanonicalizeSaveNumber(value, out);
			if (value.is_string())
			{
				if (std::string problem = CheckSaveString(value); !problem.empty())
					return problem;
			}
			out = value;
			return {};
		}

		// The compact JSON size of one entry: "key":value.
		[[nodiscard]] static size_t GetSaveEntryBytes(std::string_view key, const Json& value)
		{
			return Json(key).dump().size() + 1 + value.dump().size();
		}

	}

	Status SaveDataStore::ValidateKey(std::string_view key)
	{
		if (key.empty() || key.size() > MaxSaveDataKeyBytes)
			return MakeError(ErrorCode::InvalidArgument, "a save-data key is 1 to {} bytes (got {})", MaxSaveDataKeyBytes, key.size());
		if (!IsValidUtf8(key))
			return MakeError(ErrorCode::InvalidArgument, "a save-data key must be valid UTF-8");
		return {};
	}

	Result<Json> SaveDataStore::Canonicalize(const Json& value)
	{
		Json canonical;
		if (const std::string problem = Utils::CanonicalizeSaveValue(value, canonical, 0); !problem.empty())
			return MakeError(ErrorCode::InvalidArgument, "{}", problem);
		return canonical;
	}

	const Json* SaveDataStore::Find(std::string_view key) const
	{
		const auto found = m_Values.find(key);
		return found != m_Values.end() ? &found->second.Get() : nullptr;
	}

	Status SaveDataStore::Set(std::string_view key, const Json& value)
	{
		ENGINE_TRY(ValidateKey(key));
		if (value.is_null())
		{
			Delete(key);
			return {};
		}
		ENGINE_TRY_ASSIGN(Json canonical, Canonicalize(value));
		const size_t entryBytes = Utils::GetSaveEntryBytes(key, canonical);
		const auto previous = m_EntryBytes.find(key);
		const size_t current = GetSerializedBytes();
		// The store's size with this entry in place of the old one (a new entry adds its separating comma).
		size_t replaced = current + entryBytes;
		if (previous != m_EntryBytes.end())
			replaced -= previous->second;
		else if (!m_Values.empty())
			replaced += 1;
		if (replaced > MaxSaveDataBytes)
		{
			return MakeError(ErrorCode::InvalidArgument, "the save data would be {} bytes with '{}' set, over its limit of {} bytes", replaced, key,
				MaxSaveDataBytes);
		}
		m_Values.insert_or_assign(std::string(key), VariantValue(std::move(canonical)));
		m_EntryBytes.insert_or_assign(std::string(key), entryBytes);
		return {};
	}

	bool SaveDataStore::Delete(std::string_view key)
	{
		const auto found = m_Values.find(key);
		if (found == m_Values.end())
			return false;
		m_Values.erase(found);
		m_EntryBytes.erase(m_EntryBytes.find(key));
		return true;
	}

	Status SaveDataStore::Replace(const Json& values)
	{
		if (!values.is_object())
			return MakeError(ErrorCode::InvalidArgument, "save data is an object of values by key");
		SaveDataStore replaced;
		for (const auto& [key, value] : values.items())
			ENGINE_TRY(WithContext(replaced.Set(key, value), std::format("at key '{}'", key)));
		*this = std::move(replaced);
		return {};
	}

	void SaveDataStore::Clear()
	{
		m_Values.clear();
		m_EntryBytes.clear();
	}

	std::vector<std::string> SaveDataStore::GetKeys() const
	{
		std::vector<std::string> keys;
		keys.reserve(m_Values.size());
		for (const auto& [key, value] : m_Values)
			keys.push_back(key);
		return keys;
	}

	size_t SaveDataStore::GetSerializedBytes() const
	{
		size_t bytes = 2 + (m_EntryBytes.empty() ? 0 : m_EntryBytes.size() - 1);
		for (const auto& [key, entry] : m_EntryBytes)
			bytes += entry;
		return bytes;
	}

	Json SaveDataStore::ToJson() const
	{
		Json values = Json::object();
		for (const auto& [key, value] : m_Values)
			values[key] = value.Get();
		return values;
	}

	void SaveDataStore::AppendStateHash(XXH64Hasher& hasher) const
	{
		if (m_Values.empty())
			return;
		// "SaveDat1" as the little-endian value of its 8 ASCII bytes.
		constexpr uint64_t Marker = 0x3174614465766153ull;
		hasher.UpdateU64(Marker);
		hasher.UpdateU64(static_cast<uint64_t>(m_Values.size()));
		for (const auto& [key, value] : m_Values)
		{
			const std::string text = value.Get().dump();
			hasher.UpdateU64(static_cast<uint64_t>(key.size()));
			hasher.Update(key);
			hasher.UpdateU64(static_cast<uint64_t>(text.size()));
			hasher.Update(text);
		}
	}

	Result<std::string> SerializeSaveDataFile(const Json& values)
	{
		if (!values.is_object())
			return MakeError(ErrorCode::InvalidArgument, "save data is an object of values by key");
		Json document = Json::object();
		document["Format"] = SaveDataFormatName;
		document["Version"] = SaveDataFormatVersion;
		document["Values"] = values;
		std::string text = document.dump(1, '\t');
		text += '\n';
		return text;
	}

	Result<Json> ParseSaveDataFile(std::string_view text)
	{
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		const JsonReader root(document);
		ENGINE_TRY(root.ExpectType(JsonType::Object));
		ENGINE_TRY(root.ReadFormatHeader(SaveDataFormatName, 1, SaveDataFormatVersion));
		ENGINE_TRY_ASSIGN(const JsonReader values, root.GetMember("Values"));
		ENGINE_TRY(values.ExpectType(JsonType::Object));
		ENGINE_TRY_ASSIGN(const std::vector<std::string> keys, values.GetMemberNames());
		SaveDataStore store;
		for (const std::string& key : keys)
		{
			ENGINE_TRY_ASSIGN(const JsonReader member, values.GetMember(key));
			if (const Status set = store.Set(key, member.GetValue()); !set)
				return std::unexpected(member.MakeLocatedError(ErrorCode::Validation, set.error().GetMessageText()));
		}
		return store.ToJson();
	}

	SaveDataFileLoad LoadSaveDataFile(const std::filesystem::path& file, SaveDataCorruption corruption)
	{
		SaveDataFileLoad load{ .Values = VariantValue(Json::object()) };
		std::error_code error;
		const bool exists = std::filesystem::exists(file, error);
		if (!error && !exists)
			return load;
		Result<Json> values = MakeError(ErrorCode::Io, "cannot check whether it exists: {}", error.message());
		if (!error)
		{
			const Result<std::string> text = FileSystem::ReadText(file);
			values = text.has_value() ? ParseSaveDataFile(*text) : Result<Json>(std::unexpected(text.error()));
		}
		if (values.has_value())
		{
			load.Values = VariantValue(std::move(*values));
			return load;
		}
		const std::string name = FileSystem::PathToUtf8(file.filename());
		ENGINE_CORE_WARN("The save data in '{}' cannot be read, so the game starts with none: {}", FileSystem::PathToUtf8(file), values.error().ToString());
		if (corruption == SaveDataCorruption::LeaveInPlace)
		{
			load.Error = std::format("{} could not be read, so the game started with no save data ({}); it was left in place for the next run "
									 "that can write the store",
				name, values.error().GetMessageText());
			return load;
		}
		// Kept beside the store for whoever wants to recover it, under the first free name, so an earlier copy stays.
		std::string kept;
		for (uint32_t copy = 1; copy <= Utils::MaxCorruptSaveDataCopies && kept.empty(); ++copy)
		{
			std::filesystem::path candidate = file.parent_path() / std::format("{}.corrupt-{}{}", FileSystem::PathToUtf8(file.stem()), copy, FileSystem::PathToUtf8(file.extension()));
			std::error_code checked;
			if (std::filesystem::exists(candidate, checked) || checked)
				continue;
			std::error_code moved;
			std::filesystem::rename(file, candidate, moved);
			if (!moved)
				kept = FileSystem::PathToUtf8(candidate.filename());
			else
			{
				ENGINE_CORE_WARN("The corrupt save data '{}' cannot be moved aside: {}", FileSystem::PathToUtf8(file), moved.message());
				break;
			}
		}
		load.Error = kept.empty() ? std::format("{} could not be read, so the game started with no save data ({}); it could not be moved aside",
										name, values.error().GetMessageText())
								  : std::format("{} could not be read, so the game started with no save data ({}); it was kept as {}", name,
										values.error().GetMessageText(), kept);
		return load;
	}

	Status WriteSaveDataFile(const std::filesystem::path& file, const Json& values)
	{
		ENGINE_TRY_ASSIGN(const std::string text, SerializeSaveDataFile(values));
		std::error_code error;
		std::filesystem::create_directories(file.parent_path(), error);
		if (error)
			return MakeError(ErrorCode::Io, "cannot create the save-data directory '{}': {}", FileSystem::PathToUtf8(file.parent_path()), error.message());
		const std::span<const std::byte> bytes = std::as_bytes(std::span(text.data(), text.size()));
		const Status durable = PublishedFile::WriteDurably(file, bytes);
		if (durable.has_value() || durable.error().GetCode() != ErrorCode::Unsupported)
			return durable;
		ENGINE_CORE_INFO("The save data in '{}' is written atomically without a durable flush: {}", FileSystem::PathToUtf8(file), durable.error().GetMessageText());
		return FileSystem::WriteFileAtomic(file, bytes, { .KeepBackup = false });
	}

}
