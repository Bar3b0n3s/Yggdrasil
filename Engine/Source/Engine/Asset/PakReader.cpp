#include "EnginePCH.h"
#include "Engine/Asset/PakReader.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"

#include <algorithm>
#include <cerrno>
#include <format>
#include <fstream>
#include <ios>
#include <limits>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

namespace Engine {

	struct PakReader::State
	{
		std::string Name;
		PakHeader Header;
		PakToc Toc;
		// Indices into Toc.Entries sorted by Path, for FindByPath.
		std::vector<size_t> ByPath;

		// The source: the bytes of a memory pak, or the open file of a file pak (read under FileMutex, which guards the
		// stream's position and state).
		Buffer Bytes;
		mutable std::ifstream File;
		mutable std::mutex FileMutex;
		bool IsFile = false;

		// The entries whose XXH64 passed (by TOC index); guarded by VerifiedMutex.
		mutable std::vector<bool> Verified;
		mutable std::mutex VerifiedMutex;
	};

	namespace Utils {

		// The header and TOC checks shared by Open and OpenMemory: the TOC range lies inside the pak and ends it, its hash
		// matches, it parses, and its entry count is the header's. `readToc` reads the TOC bytes once the range is valid.
		template<typename ReadToc>
		static Status ReadPakIndex(std::span<const std::byte> headerBytes, uint64_t pakSize, ReadToc&& readToc, PakHeader& header, PakToc& toc)
		{
			ENGINE_TRY_ASSIGN(header, ReadPakHeader(headerBytes));
			// Overflow-safe: PakHeader::Size <= TocOffset and TocOffset + TocSize == pakSize (nothing follows the TOC).
			if (header.TocOffset < PakHeader::Size || header.TocOffset > pakSize || header.TocSize != pakSize - header.TocOffset)
			{
				return MakeError(ErrorCode::Parse, "the TOC range [{}, {} + {}) does not end the {}-byte pak", header.TocOffset, header.TocOffset,
					header.TocSize, pakSize);
			}
			ENGINE_TRY_ASSIGN(const Buffer tocBytes, readToc(header.TocOffset, header.TocSize));
			if (XXH64(tocBytes) != header.TocHash)
				return MakeError(ErrorCode::Validation, "TOC hash mismatch: the pak is corrupted");
			ENGINE_TRY_ASSIGN(toc, ParsePakToc(AsStringView(tocBytes), header.TocOffset));
			if (toc.Entries.size() != header.EntryCount)
			{
				return MakeError(ErrorCode::Validation, "the header counts {} entries, the TOC lists {}: the pak is corrupted", header.EntryCount,
					toc.Entries.size());
			}
			return {};
		}

		// Reads `size` bytes at `offset` of `file` (the caller holds the file's lock). Errors: Io naming the pak.
		static Result<Buffer> ReadFileRange(std::ifstream& file, uint64_t offset, uint64_t size, std::string_view name)
		{
			if (size > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()) || offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
				return MakeError(ErrorCode::Io, "cannot read {} bytes at offset {} of '{}': the range is too large", size, offset, name);
			file.clear();
			errno = 0;
			file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
			Buffer bytes(static_cast<size_t>(size));
			if (size != 0)
				file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
			if (!file.good() || static_cast<uint64_t>(file.gcount()) != size)
			{
				const std::string reason = errno != 0 ? std::generic_category().message(errno) : std::string("unexpected end of file");
				file.clear();
				return MakeError(ErrorCode::Io, "cannot read {} bytes at offset {} of '{}': {}", size, offset, name, reason);
			}
			return bytes;
		}

		// The path index and the verified-entry flags of a reader whose TOC was just read (PakReader::State is private, so
		// the state's type is deduced).
		template<typename PakState>
		static void BuildPakIndex(PakState& state)
		{
			const std::vector<PakEntry>& entries = state.Toc.Entries;
			state.ByPath.resize(entries.size());
			for (size_t index = 0; index < entries.size(); ++index)
				state.ByPath[index] = index;
			std::ranges::sort(state.ByPath, {}, [&entries](size_t index) -> const std::string&
			{
				return entries[index].Path;
			});
			state.Verified.assign(entries.size(), false);
		}

	}

	PakReader::PakReader(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	PakReader::~PakReader() = default;

	Result<Ref<const PakReader>> PakReader::Open(const std::filesystem::path& path)
	{
		const std::string name = FileSystem::PathToUtf8(path);
		const std::string context = std::format("while opening the pak '{}'", name);
		ENGINE_TRY_ASSIGN(const FileInfo info, WithContext(FileSystem::GetInfo(path), context));
		if (info.IsDirectory)
			return std::unexpected(Error(ErrorCode::Io, std::format("cannot open the pak '{}': it is a directory", name)));

		Ref<PakReader> reader = CreateRef<PakReader>(ConstructionKey());
		State& state = *reader->m_State;
		state.Name = name;
		state.IsFile = true;
		errno = 0;
		state.File.open(path, std::ios::binary);
		if (!state.File.is_open())
		{
			const std::string reason = errno != 0 ? std::generic_category().message(errno) : std::string("the file cannot be opened");
			return MakeError(ErrorCode::Io, "cannot open the pak '{}': {}", name, reason);
		}

		// Nobody else reads the stream before the reader is returned; the lock keeps the rule uniform.
		const std::scoped_lock lock(state.FileMutex);
		const uint64_t headerSize = std::min<uint64_t>(info.Size, PakHeader::Size);
		ENGINE_TRY_ASSIGN(const Buffer header, WithContext(Utils::ReadFileRange(state.File, 0, headerSize, name), context));
		const auto readToc = [&state, &name](uint64_t offset, uint64_t size)
		{
			return Utils::ReadFileRange(state.File, offset, size, name);
		};
		ENGINE_TRY(WithContext(Utils::ReadPakIndex(header, info.Size, readToc, state.Header, state.Toc), context));
		Utils::BuildPakIndex(state);
		return Ref<const PakReader>(std::move(reader));
	}

	Result<Ref<const PakReader>> PakReader::OpenMemory(Buffer bytes, std::string name)
	{
		const std::string context = std::format("while opening the pak '{}'", name);
		Ref<PakReader> reader = CreateRef<PakReader>(ConstructionKey());
		State& state = *reader->m_State;
		state.Name = std::move(name);
		state.Bytes = std::move(bytes);
		const std::span<const std::byte> all = state.Bytes;
		const auto readToc = [all](uint64_t offset, uint64_t size) -> Result<Buffer>
		{
			// ReadPakIndex checked the range against the size.
			const std::span<const std::byte> toc = all.subspan(static_cast<size_t>(offset), static_cast<size_t>(size));
			return Buffer(toc.begin(), toc.end());
		};
		ENGINE_TRY(WithContext(Utils::ReadPakIndex(all, all.size(), readToc, state.Header, state.Toc), context));
		Utils::BuildPakIndex(state);
		return Ref<const PakReader>(std::move(reader));
	}

	const std::string& PakReader::GetName() const
	{
		return m_State->Name;
	}

	const PakHeader& PakReader::GetHeader() const
	{
		return m_State->Header;
	}

	std::span<const PakEntry> PakReader::GetEntries() const
	{
		return m_State->Toc.Entries;
	}

	const VariantValue& PakReader::GetMetadata() const
	{
		return m_State->Toc.Metadata;
	}

	const PakEntry* PakReader::FindByHandle(AssetHandle handle) const
	{
		if (!handle.IsValid())
			return nullptr;
		const std::vector<PakEntry>& entries = m_State->Toc.Entries;
		// Sorted by (Handle, Path), and cooked-asset handles are unique (ParsePakToc).
		const auto found = std::ranges::lower_bound(entries, handle, {}, &PakEntry::Handle);
		if (found == entries.end() || found->Handle != handle)
			return nullptr;
		return &*found;
	}

	const PakEntry* PakReader::FindByPath(std::string_view path) const
	{
		const std::vector<PakEntry>& entries = m_State->Toc.Entries;
		const std::vector<size_t>& byPath = m_State->ByPath;
		const auto found = std::ranges::lower_bound(byPath, path, {}, [&entries](size_t index) -> std::string_view
		{
			return entries[index].Path;
		});
		if (found == byPath.end() || entries[*found].Path != path)
			return nullptr;
		return &entries[*found];
	}

	Result<Buffer> PakReader::ReadEntry(const PakEntry& entry) const
	{
		const State& state = *m_State;
		const std::vector<PakEntry>& entries = state.Toc.Entries;

		// `entry` is one of GetEntries(); an equal copy is accepted too (found by its unique path).
		size_t index = entries.size();
		if (!entries.empty() && &entry >= entries.data() && &entry < entries.data() + entries.size())
		{
			index = static_cast<size_t>(&entry - entries.data());
		}
		else if (const PakEntry* found = FindByPath(entry.Path); found != nullptr && *found == entry)
		{
			index = static_cast<size_t>(found - entries.data());
		}
		if (index == entries.size())
			return MakeError(ErrorCode::InvalidArgument, "the pak '{}' has no entry '{}'", state.Name, entry.Path);
		const PakEntry& stored = entries[index];

		// The TOC's ranges were checked at open (ParsePakToc: inside the entry data, so inside the pak).
		Buffer bytes;
		if (state.IsFile)
		{
			const std::scoped_lock lock(state.FileMutex);
			ENGINE_TRY_ASSIGN(bytes, Utils::ReadFileRange(state.File, stored.Offset, stored.Size, state.Name));
		}
		else
		{
			const std::span<const std::byte> range = std::span<const std::byte>(state.Bytes).subspan(static_cast<size_t>(stored.Offset), static_cast<size_t>(stored.Size));
			bytes.assign(range.begin(), range.end());
		}

		{
			const std::scoped_lock lock(state.VerifiedMutex);
			if (state.Verified[index])
				return bytes;
		}
		// Verified on the first read in every configuration (§14.1), outside the lock: a concurrent first read of the same
		// entry hashes it twice, which is harmless.
		if (XXH64(bytes) != stored.Hash)
			return MakeError(ErrorCode::Validation, "entry '{}' is corrupted (hash mismatch) in the pak '{}'", stored.Path, state.Name);
		const std::scoped_lock lock(state.VerifiedMutex);
		state.Verified[index] = true;
		return bytes;
	}

}
