#include "EnginePCH.h"
#include "Engine/Asset/PakFormat.h"

#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/UUID.h"
#include "Engine/Core/VfsPath.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <set>
#include <utility>

namespace Engine {

	namespace {

		// The bytes of PakHeader::Magic followed by the header fields, then the zero reserved bytes.
		constexpr size_t PakHeaderReservedOffset = 40;
		constexpr size_t PakHeaderReservedSize = PakHeader::Size - PakHeaderReservedOffset;

		// The members of the TOC and of one entry, in canonical order (PakFormat.h).
		constexpr std::string_view TocMembers[] = { "Format", "Version", "Entries", "Metadata" };
		constexpr std::string_view EntryMembers[] = { "Handle", "Type", "Path", "Offset", "Size", "XXH64" };

	}

	namespace Utils {

		// The canonical TOC order: by handle (the null handle of plain files first), then by path.
		static bool PrecedesInToc(const PakEntry& lhs, const PakEntry& rhs)
		{
			if (lhs.Handle != rhs.Handle)
				return lhs.Handle < rhs.Handle;
			return lhs.Path < rhs.Path;
		}

		// A located Validation error for an unknown member of `reader` (an engine-written document: an extra member is a
		// mistake to report, not data to keep).
		static Status RejectUnknownMembers(const JsonReader& reader, std::span<const std::string_view> known, std::string_view what)
		{
			ENGINE_TRY_ASSIGN(const std::vector<std::string> unknown, reader.FindUnknownMembers(known));
			if (!unknown.empty())
			{
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation,
					std::format("unknown member '{}' in {}", unknown.front(), what)));
			}
			return {};
		}

		// One TOC entry, checked on its own (the cross-entry rules are ParsePakToc's).
		static Result<PakEntry> ReadTocEntry(const JsonReader& reader)
		{
			ENGINE_TRY(reader.ExpectType(JsonType::Object));
			ENGINE_TRY(RejectUnknownMembers(reader, EntryMembers, "a pak entry"));

			PakEntry entry;
			ENGINE_TRY_ASSIGN(const JsonReader handle, reader.GetMember("Handle"));
			if (!handle.IsNull())
			{
				ENGINE_TRY_ASSIGN(entry.Handle, handle.ReadUUID());
				// The null handle is written as JSON null, never as sixteen zeros (AssetHandle.h).
				if (!entry.Handle.IsValid())
					return std::unexpected(handle.MakeLocatedError(ErrorCode::Validation, "the null handle is written as null"));
			}

			ENGINE_TRY_ASSIGN(const JsonReader type, reader.GetMember("Type"));
			ENGINE_TRY_ASSIGN(entry.Type, type.ReadString());
			if (entry.Type == PakFileEntryType)
			{
				if (entry.Handle.IsValid())
					return std::unexpected(handle.MakeLocatedError(ErrorCode::Validation, "a plain-file entry has a null handle"));
			}
			else
			{
				const std::optional<AssetType> assetType = AssetTypeFromString(entry.Type);
				if (!assetType.has_value() || *assetType == AssetType::None)
				{
					return std::unexpected(type.MakeLocatedError(ErrorCode::Validation,
						std::format("unknown entry type '{}' (an asset type other than None, or '{}')", entry.Type, PakFileEntryType)));
				}
				if (!entry.Handle.IsValid())
					return std::unexpected(handle.MakeLocatedError(ErrorCode::Validation, "a cooked-asset entry needs a handle"));
			}

			ENGINE_TRY_ASSIGN(const JsonReader path, reader.GetMember("Path"));
			ENGINE_TRY_ASSIGN(entry.Path, path.ReadString());
			if (entry.Path.empty())
				return std::unexpected(path.MakeLocatedError(ErrorCode::Validation, "an entry needs a path"));
			if (entry.Type == PakFileEntryType)
			{
				// PakMount serves plain files at their path, so it must be a valid relative VFS path.
				const Status valid = VfsPath::ValidateRelativePath(entry.Path);
				if (!valid.has_value())
					return std::unexpected(path.MakeLocatedError(ErrorCode::Validation, valid.error().GetMessageText()));
			}

			ENGINE_TRY_ASSIGN(entry.Offset, reader.ReadMember<uint64_t>("Offset"));
			ENGINE_TRY_ASSIGN(entry.Size, reader.ReadMember<uint64_t>("Size"));

			ENGINE_TRY_ASSIGN(const JsonReader hash, reader.GetMember("XXH64"));
			ENGINE_TRY_ASSIGN(const std::string hashText, hash.ReadString());
			const std::optional<UUID> hashValue = UUID::FromString(hashText);
			if (!hashValue.has_value())
				return std::unexpected(hash.MakeLocatedError(ErrorCode::Validation, "expected 16 hex digits"));
			entry.Hash = hashValue->GetValue();
			return entry;
		}

		// The byte-range rules of one entry against the data area [PakHeader::Size, dataEnd).
		static Status CheckEntryRange(const JsonReader& reader, const PakEntry& entry, uint64_t dataEnd)
		{
			if (entry.Offset % PakEntryAlignment != 0)
			{
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation,
					std::format("offset {} is not a multiple of {}", entry.Offset, PakEntryAlignment)));
			}
			if (entry.Offset < PakHeader::Size)
			{
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation,
					std::format("offset {} lies inside the {}-byte header", entry.Offset, PakHeader::Size)));
			}
			// Overflow-safe: Offset + Size <= dataEnd.
			if (entry.Offset > dataEnd || entry.Size > dataEnd - entry.Offset)
			{
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation,
					std::format("bytes [{}, {} + {}) leave the entry data, which ends at {}", entry.Offset, entry.Offset, entry.Size, dataEnd)));
			}
			return {};
		}

	}

	std::array<std::byte, PakHeader::Size> WritePakHeader(const PakHeader& header)
	{
		BinaryWriter writer;
		writer.WriteBytes(std::as_bytes(std::span(PakHeader::Magic)));
		writer.WriteU32(header.Version);
		writer.WriteU32(header.EntryCount);
		writer.WriteU64(header.TocOffset);
		writer.WriteU64(header.TocSize);
		writer.WriteU64(header.TocHash);
		ENGINE_CORE_ASSERT(writer.GetSize() == PakHeaderReservedOffset, "The pak header fields take {} bytes", writer.GetSize());

		std::array<std::byte, PakHeader::Size> bytes{};
		std::ranges::copy(writer.GetData(), bytes.begin());
		return bytes;
	}

	Result<PakHeader> ReadPakHeader(std::span<const std::byte> bytes)
	{
		if (bytes.size() < PakHeader::Size)
			return MakeError(ErrorCode::Parse, "truncated pak: {} bytes, the header alone takes {}", bytes.size(), PakHeader::Size);

		BinaryReader reader(bytes.first(PakHeader::Size));
		ENGINE_TRY_ASSIGN(const std::span<const std::byte> magic, reader.ReadBytes(PakHeader::Magic.size()));
		if (std::memcmp(magic.data(), PakHeader::Magic.data(), PakHeader::Magic.size()) != 0)
			return MakeError(ErrorCode::Parse, "not a pak: the file does not start with 'ENGPAK01'");

		PakHeader header;
		ENGINE_TRY_ASSIGN(header.Version, reader.ReadU32());
		ENGINE_TRY_ASSIGN(header.EntryCount, reader.ReadU32());
		ENGINE_TRY_ASSIGN(header.TocOffset, reader.ReadU64());
		ENGINE_TRY_ASSIGN(header.TocSize, reader.ReadU64());
		ENGINE_TRY_ASSIGN(header.TocHash, reader.ReadU64());
		ENGINE_TRY_ASSIGN(const std::span<const std::byte> reserved, reader.ReadBytes(PakHeaderReservedSize));
		if (std::ranges::any_of(reserved, [](std::byte value)
		{
			return value != std::byte{ 0 };
		}))
			return MakeError(ErrorCode::Parse, "the reserved bytes of the pak header are not zero");
		if (header.Version != PakHeader::CurrentVersion)
		{
			return MakeError(ErrorCode::UnsupportedVersion, "pak version {} is not supported: this build reads version {}", header.Version,
				PakHeader::CurrentVersion);
		}
		return header;
	}

	std::string SerializePakToc(const PakToc& toc)
	{
		JsonWriter writer(JsonStyle::Pretty);
		writer.BeginObject();
		writer.WriteKey("Format");
		writer.WriteString(PakToc::FormatName);
		writer.WriteKey("Version");
		writer.WriteUInt(PakToc::CurrentVersion);
		writer.WriteKey("Entries");
		writer.BeginArray();
		for (size_t index = 0; index < toc.Entries.size(); ++index)
		{
			const PakEntry& entry = toc.Entries[index];
			ENGINE_CORE_ASSERT(index == 0 || Utils::PrecedesInToc(toc.Entries[index - 1], entry), "Pak TOC entries must be sorted by (Handle, Path) "
																								  "(entry {}, '{}')",
				index, entry.Path);
			writer.BeginObject();
			writer.WriteKey("Handle");
			if (entry.Handle.IsValid())
				writer.WriteUUID(entry.Handle);
			else
				writer.WriteNull();
			writer.WriteKey("Type");
			writer.WriteString(entry.Type);
			writer.WriteKey("Path");
			writer.WriteString(entry.Path);
			writer.WriteKey("Offset");
			writer.WriteUInt(entry.Offset);
			writer.WriteKey("Size");
			writer.WriteUInt(entry.Size);
			writer.WriteKey("XXH64");
			writer.WriteString(std::format("{:016x}", entry.Hash));
			writer.EndObject();
		}
		writer.EndArray();
		writer.WriteKey("Metadata");
		if (toc.Metadata.IsNull())
		{
			writer.BeginObject();
			writer.EndObject();
		}
		else
		{
			ENGINE_CORE_ASSERT(toc.Metadata.Get().is_object(), "The pak TOC's Metadata is a JSON object or null");
			writer.WriteJson(toc.Metadata.Get());
		}
		writer.EndObject();

		Result<std::string> text = writer.Finish();
		// The writers of a TOC (PakWriter) validate paths and metadata before they get here, so a failure is a programmer
		// error that would otherwise produce a pak whose TOC cannot be read.
		ENGINE_CORE_VERIFY(text.has_value(), "Cannot write the pak TOC: {}", text.has_value() ? std::string() : text.error().ToString());
		return std::move(*text);
	}

	Result<PakToc> ParsePakToc(std::string_view text, uint64_t dataEnd)
	{
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		const JsonReader root(document);
		ENGINE_TRY(root.ExpectType(JsonType::Object));
		ENGINE_TRY(root.ReadFormatHeader(PakToc::FormatName, PakToc::CurrentVersion, PakToc::CurrentVersion));
		ENGINE_TRY(Utils::RejectUnknownMembers(root, TocMembers, "the pak TOC"));

		PakToc toc;
		ENGINE_TRY_ASSIGN(const JsonReader entries, root.GetMember("Entries"));
		ENGINE_TRY_ASSIGN(const size_t count, entries.GetArraySize());
		toc.Entries.reserve(count);
		std::set<std::string, std::less<>> paths;
		for (size_t index = 0; index < count; ++index)
		{
			ENGINE_TRY_ASSIGN(const JsonReader element, entries.GetElement(index));
			ENGINE_TRY_ASSIGN(PakEntry entry, Utils::ReadTocEntry(element));
			ENGINE_TRY(Utils::CheckEntryRange(element, entry, dataEnd));
			if (!toc.Entries.empty())
			{
				const PakEntry& previous = toc.Entries.back();
				if (!Utils::PrecedesInToc(previous, entry))
					return std::unexpected(element.MakeLocatedError(ErrorCode::Validation, "entries are not sorted by (Handle, Path)"));
				if (entry.Handle.IsValid() && entry.Handle == previous.Handle)
				{
					return std::unexpected(element.MakeLocatedError(ErrorCode::Validation,
						std::format("handle {} has two entries ('{}' and '{}')", entry.Handle, previous.Path, entry.Path)));
				}
			}
			if (!paths.insert(entry.Path).second)
				return std::unexpected(element.MakeLocatedError(ErrorCode::Validation, std::format("path '{}' has two entries", entry.Path)));
			toc.Entries.push_back(std::move(entry));
		}

		// No two entries share a byte: compare each with its neighbour in offset order (empty entries overlap nothing).
		std::vector<std::pair<uint64_t, size_t>> byOffset;
		byOffset.reserve(toc.Entries.size());
		for (size_t index = 0; index < toc.Entries.size(); ++index)
		{
			if (toc.Entries[index].Size != 0)
				byOffset.emplace_back(toc.Entries[index].Offset, index);
		}
		std::ranges::sort(byOffset);
		for (size_t position = 1; position < byOffset.size(); ++position)
		{
			const PakEntry& previous = toc.Entries[byOffset[position - 1].second];
			const PakEntry& entry = toc.Entries[byOffset[position].second];
			if (entry.Offset < previous.Offset + previous.Size)
			{
				ENGINE_TRY_ASSIGN(const JsonReader element, entries.GetElement(byOffset[position].second));
				return std::unexpected(element.MakeLocatedError(ErrorCode::Validation,
					std::format("the bytes of '{}' overlap those of '{}'", entry.Path, previous.Path)));
			}
		}

		ENGINE_TRY_ASSIGN(const JsonReader metadata, root.GetMember("Metadata"));
		ENGINE_TRY(metadata.ExpectType(JsonType::Object));
		toc.Metadata = VariantValue(metadata.GetValue());
		return toc;
	}

}
