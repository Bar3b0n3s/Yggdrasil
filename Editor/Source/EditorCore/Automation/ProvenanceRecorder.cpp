#include "EditorPCH.h"
#include "EditorCore/Automation/ProvenanceRecorder.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <nlohmann/json.hpp>

#include <charconv>

namespace Engine {

	namespace Utils {

		constexpr std::string_view ProvenanceDirectory = "Automation";
		constexpr std::array<std::string_view, 3> ProvenanceRootKeys = { "Format", "Version", "Entries" };
		constexpr std::array<std::string_view, 6> ProvenanceEntryKeys = { "Path", "XXH64", "Method", "RequestId", "Client", "TranscriptLine" };

		static std::string FormatHash(uint64_t hash)
		{
			return std::format("{:016x}", hash);
		}

		// Exactly 16 hex digits (either case), as written by FormatHash.
		static std::optional<uint64_t> ParseHash(std::string_view text)
		{
			if (text.size() != 16)
				return std::nullopt;
			uint64_t value = 0;
			const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
			if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
				return std::nullopt;
			return value;
		}

		static Status RejectUnknownMembers(const JsonReader& object, std::span<const std::string_view> keys)
		{
			ENGINE_TRY_ASSIGN(const std::vector<std::string> unknown, object.FindUnknownMembers(keys));
			if (unknown.empty())
				return {};
			ENGINE_TRY_ASSIGN(const JsonReader member, object.GetMember(unknown.front()));
			return std::unexpected(member.MakeLocatedError(ErrorCode::Validation, std::format("unknown member '{}' in a provenance file", unknown.front())));
		}

		static Result<ProvenanceEntry> ReadEntry(const JsonReader& entry)
		{
			ENGINE_TRY(entry.ExpectType(JsonType::Object));
			ENGINE_TRY(RejectUnknownMembers(entry, ProvenanceEntryKeys));

			ProvenanceEntry result;
			ENGINE_TRY_ASSIGN(result.Path, entry.ReadMember<std::string>("Path"));
			if (result.Path.empty())
				return std::unexpected(entry.MakeLocatedError(ErrorCode::Validation, "\"Path\" must not be empty"));

			ENGINE_TRY_ASSIGN(const JsonReader hash, entry.GetMember("XXH64"));
			ENGINE_TRY_ASSIGN(const std::string hashText, hash.ReadString());
			const std::optional<uint64_t> hashValue = ParseHash(hashText);
			if (!hashValue.has_value())
				return std::unexpected(hash.MakeLocatedError(ErrorCode::Validation, std::format("\"XXH64\" must be 16 hex digits, got '{}'", hashText)));
			result.Hash = *hashValue;

			ENGINE_TRY_ASSIGN(result.Method, entry.ReadMember<std::string>("Method"));

			ENGINE_TRY_ASSIGN(const JsonReader requestId, entry.GetMember("RequestId"));
			const JsonType requestIdType = requestId.GetType();
			if (requestIdType != JsonType::Null && requestIdType != JsonType::Integer && requestIdType != JsonType::String)
				return std::unexpected(requestId.MakeLocatedError(ErrorCode::Validation, "\"RequestId\" must be an integer, a string or null"));
			if (requestIdType != JsonType::Null)
				result.RequestId = VariantValue(requestId.GetValue());

			ENGINE_TRY_ASSIGN(result.Client, entry.ReadMember<std::string>("Client"));

			ENGINE_TRY_ASSIGN(const JsonReader line, entry.GetMember("TranscriptLine"));
			if (!line.IsNull())
			{
				ENGINE_TRY_ASSIGN(const uint64_t lineNumber, line.ReadUInt64());
				if (lineNumber == 0)
					return std::unexpected(line.MakeLocatedError(ErrorCode::Validation, "\"TranscriptLine\" is 1-based"));
				result.TranscriptLine = lineNumber;
			}
			return result;
		}

	}

	bool ProvenanceRecorder::IsRecordedPath(std::string_view path)
	{
		constexpr std::string_view AssetsPrefix = "Assets/";
		constexpr std::string_view ProjectExtension = ".eproj";
		if (path.starts_with(AssetsPrefix))
			return path.size() > AssetsPrefix.size();
		return path.find('/') == std::string_view::npos && path.size() > ProjectExtension.size() && path.ends_with(ProjectExtension);
	}

	Result<ProvenanceRecorder> ProvenanceRecorder::Load(const VirtualFileSystem& vfs)
	{
		ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Create("project", FilePath));
		const Result<std::string> text = vfs.ReadText(path);
		if (!text)
		{
			if (text.error().GetCode() == ErrorCode::NotFound)
				return ProvenanceRecorder();
			return std::unexpected(Error(text.error()).WithContext(std::format("while reading '{}'", path.ToString())));
		}

		Result<std::vector<ProvenanceEntry>> entries = FromText(*text);
		if (!entries)
		{
			ErrorLocation location;
			location.File = std::string(FilePath);
			return std::unexpected(std::move(entries).error().WithLocation(std::move(location)));
		}
		ProvenanceRecorder recorder;
		recorder.m_Entries = std::move(*entries);
		return recorder;
	}

	std::string ProvenanceRecorder::ToText(std::span<const ProvenanceEntry> entries)
	{
		Json document = Json::object();
		document["Format"] = std::string(FormatName);
		document["Version"] = CurrentVersion;
		Json list = Json::array();
		for (size_t index = 0; index < entries.size(); ++index)
		{
			const ProvenanceEntry& entry = entries[index];
			ENGINE_ASSERT(index == 0 || entries[index - 1].Path < entry.Path, "Provenance entries must be sorted by path and unique ('{}')",
				entry.Path);
			Json item = Json::object();
			item["Path"] = entry.Path;
			item["XXH64"] = Utils::FormatHash(entry.Hash);
			item["Method"] = entry.Method;
			item["RequestId"] = entry.RequestId.Get();
			item["Client"] = entry.Client;
			item["TranscriptLine"] = entry.TranscriptLine.has_value() ? Json(*entry.TranscriptLine) : Json();
			list.push_back(std::move(item));
		}
		document["Entries"] = std::move(list);

		// Every member is valid UTF-8 (paths are VfsPaths, the rest protocol text), so the canonical writer cannot fail.
		Result<std::string> text = JsonWriter::Write(document, JsonStyle::Pretty);
		ENGINE_ASSERT(text.has_value(), "The provenance document cannot be written: {}", text.has_value() ? std::string() : text.error().ToString());
		return text.has_value() ? std::move(*text) : std::string();
	}

	Result<std::vector<ProvenanceEntry>> ProvenanceRecorder::FromText(std::string_view text)
	{
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		const JsonReader root(document);
		ENGINE_TRY(root.ExpectType(JsonType::Object));
		ENGINE_TRY(root.ReadFormatHeader(FormatName, CurrentVersion, CurrentVersion));
		ENGINE_TRY(Utils::RejectUnknownMembers(root, Utils::ProvenanceRootKeys));

		ENGINE_TRY_ASSIGN(const JsonReader list, root.GetMember("Entries"));
		ENGINE_TRY_ASSIGN(const size_t count, list.GetArraySize());
		std::vector<ProvenanceEntry> entries;
		entries.reserve(count);
		for (size_t index = 0; index < count; ++index)
		{
			ENGINE_TRY_ASSIGN(const JsonReader element, list.GetElement(index));
			ENGINE_TRY_ASSIGN(ProvenanceEntry entry, Utils::ReadEntry(element));
			if (!entries.empty() && !(entries.back().Path < entry.Path))
			{
				ENGINE_TRY_ASSIGN(const JsonReader path, element.GetMember("Path"));
				return std::unexpected(path.MakeLocatedError(ErrorCode::Validation,
					std::format("entries must be sorted by path without duplicates: '{}' follows '{}'", entry.Path, entries.back().Path)));
			}
			entries.push_back(std::move(entry));
		}
		return entries;
	}

	void ProvenanceRecorder::Record(std::string_view path, uint64_t hash, const WriteAttribution& attribution)
	{
		ENGINE_ASSERT(IsRecordedPath(path), "'{}' is not a recorded project path", path);
		ProvenanceEntry entry;
		entry.Path = std::string(path);
		entry.Hash = hash;
		entry.Method = attribution.Method;
		entry.RequestId = attribution.RequestId;
		entry.Client = attribution.Client;
		entry.TranscriptLine = attribution.TranscriptLine;

		const auto position = std::lower_bound(m_Entries.begin(), m_Entries.end(), path, [](const ProvenanceEntry& existing, std::string_view key)
		{
			return existing.Path < key;
		});
		if (position != m_Entries.end() && position->Path == path)
			*position = std::move(entry);
		else
			m_Entries.insert(position, std::move(entry));
	}

	Status ProvenanceRecorder::Save(VirtualFileSystem& vfs) const
	{
		const std::string text = ToText(m_Entries);
		if (text.empty())
			return MakeError(ErrorCode::Validation, "the provenance document cannot be written");
		ENGINE_TRY_ASSIGN(const VfsPath directory, VfsPath::Create("project", Utils::ProvenanceDirectory));
		ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Create("project", FilePath));
		ENGINE_TRY(vfs.CreateDirectories(directory));
		return vfs.WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size())));
	}

	const ProvenanceEntry* ProvenanceRecorder::Find(std::string_view path) const
	{
		const auto position = std::lower_bound(m_Entries.begin(), m_Entries.end(), path, [](const ProvenanceEntry& existing, std::string_view key)
		{
			return existing.Path < key;
		});
		return position != m_Entries.end() && position->Path == path ? &*position : nullptr;
	}

	bool ProvenanceRecorder::Remove(std::string_view /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}
