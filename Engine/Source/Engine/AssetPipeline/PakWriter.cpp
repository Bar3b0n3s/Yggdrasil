#include "EnginePCH.h"
#include "Engine/AssetPipeline/PakWriter.h"

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Core/VfsPath.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <numeric>
#include <utility>

namespace Engine {

	namespace Utils {

		// The checks of PakWriter::Add on one entry by itself (the uniqueness checks need the writer's entries).
		static Status ValidatePakWriterEntry(const PakWriterEntry& entry)
		{
			if (entry.Path.empty())
				return MakeError(ErrorCode::InvalidArgument, "a pak entry needs a path");
			if (!IsValidUtf8(entry.Path))
				return MakeError(ErrorCode::InvalidArgument, "the pak entry path is not valid UTF-8");

			if (entry.Type == PakFileEntryType)
			{
				if (entry.Handle.IsValid())
					return MakeError(ErrorCode::InvalidArgument, "the plain file '{}' has the handle {}: plain files have none", entry.Path, entry.Handle);
				const Status valid = VfsPath::ValidateRelativePath(entry.Path);
				if (!valid.has_value())
				{
					return MakeError(ErrorCode::InvalidArgument, "the plain file path '{}' is not a relative VFS path: {}", entry.Path,
						valid.error().GetMessageText());
				}
				return {};
			}

			const std::optional<AssetType> type = AssetTypeFromString(entry.Type);
			if (!type.has_value() || *type == AssetType::None)
			{
				return MakeError(ErrorCode::InvalidArgument, "the pak entry '{}' has the type '{}': expected an asset type other than None, or '{}'",
					entry.Path, entry.Type, PakFileEntryType);
			}
			if (!entry.Handle.IsValid())
				return MakeError(ErrorCode::InvalidArgument, "the cooked asset '{}' needs a handle", entry.Path);
			const Result<CookedArtifactView> cooked = ReadCookedArtifact(entry.Data);
			if (!cooked.has_value())
			{
				return MakeError(ErrorCode::InvalidArgument, "the cooked asset '{}' is not a valid cooked artifact: {}", entry.Path,
					cooked.error().ToString());
			}
			if (cooked->Header.Type != *type)
			{
				return MakeError(ErrorCode::InvalidArgument, "the cooked asset '{}' holds a {}, not a {}", entry.Path,
					AssetTypeToString(cooked->Header.Type), entry.Type);
			}
			return {};
		}

	}

	Status PakWriter::Add(PakWriterEntry entry)
	{
		ENGINE_TRY(Utils::ValidatePakWriterEntry(entry));
		for (const PakWriterEntry& existing : m_Entries)
		{
			if (existing.Path == entry.Path)
				return MakeError(ErrorCode::AlreadyExists, "the pak already has an entry at '{}'", entry.Path);
			if (entry.Handle.IsValid() && existing.Handle == entry.Handle)
				return MakeError(ErrorCode::AlreadyExists, "the pak already has asset {} (at '{}', now '{}')", entry.Handle, existing.Path, entry.Path);
		}
		m_Entries.push_back(std::move(entry));
		return {};
	}

	void PakWriter::SetMetadata(VariantValue metadata)
	{
		ENGINE_CORE_ASSERT(metadata.IsNull() || metadata.Get().is_object(), "The pak metadata is a JSON object or null");
		// The TOC writer must accept it (finite numbers within the float range, valid UTF-8), or Build could not write it.
		ENGINE_CORE_ASSERT(JsonWriter::Write(metadata.Get(), JsonStyle::Minified).has_value(), "The pak metadata cannot be written canonically");
		m_Metadata = std::move(metadata);
	}

	Buffer PakWriter::Build() const
	{
		// The canonical order (PakFormat.h): by handle, plain files (the null handle) first, then by path. Paths are unique,
		// so the order, and with it every byte, is independent of the order of Add.
		std::vector<size_t> order(m_Entries.size());
		std::iota(order.begin(), order.end(), size_t{ 0 });
		std::ranges::sort(order, [this](size_t lhs, size_t rhs)
		{
			const PakWriterEntry& left = m_Entries[lhs];
			const PakWriterEntry& right = m_Entries[rhs];
			if (left.Handle != right.Handle)
				return left.Handle < right.Handle;
			return left.Path < right.Path;
		});

		BinaryWriter writer;
		writer.WriteBytes(std::array<std::byte, PakHeader::Size>{}); // filled in below
		PakToc toc;
		toc.Entries.reserve(order.size());
		for (const size_t index : order)
		{
			const PakWriterEntry& entry = m_Entries[index];
			writer.AlignTo(static_cast<size_t>(PakEntryAlignment));
			toc.Entries.push_back(PakEntry{
				.Handle = entry.Handle,
				.Type = entry.Type,
				.Path = entry.Path,
				.Offset = writer.GetSize(),
				.Size = entry.Data.size(),
				.Hash = XXH64(entry.Data),
			});
			writer.WriteBytes(entry.Data);
		}
		toc.Metadata = m_Metadata;

		// The TOC follows the entry data at the next 16-byte boundary and ends the pak.
		writer.AlignTo(static_cast<size_t>(PakEntryAlignment));
		const std::string tocText = SerializePakToc(toc);
		const PakHeader header{
			.Version = PakHeader::CurrentVersion,
			.EntryCount = static_cast<uint32_t>(toc.Entries.size()),
			.TocOffset = writer.GetSize(),
			.TocSize = tocText.size(),
			.TocHash = XXH64(tocText),
		};
		writer.WriteBytes(AsBytes(tocText));
		writer.OverwriteBytes(0, WritePakHeader(header));
		return writer.TakeBuffer();
	}

	Status PakWriter::WriteToFile(const std::filesystem::path& path) const
	{
		const Buffer pak = Build();
		return FileSystem::WriteFileAtomic(path, pak, AtomicWriteOptions{ .KeepBackup = false, .InjectFailure = AtomicWriteStep::None });
	}

}
