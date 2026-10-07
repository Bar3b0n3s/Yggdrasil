#include "EnginePCH.h"
#include "Engine/Asset/PakMount.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <utility>

namespace Engine {

	struct PakMount::State
	{
		Ref<const PakReader> Pak;
		// The plain files by relative path (the entries live in the reader, which outlives the index) and every directory
		// their paths imply, the root ("") excluded.
		std::map<std::string, const PakEntry*, std::less<>> Files;
		std::set<std::string, std::less<>> Directories;
	};

	namespace {

		enum class PakNodeKind : uint8_t
		{
			None,
			File,
			Directory
		};

		// A stream over one entry's bytes, read (and verified) from the pak when the stream is opened. It owns its bytes, so
		// it stays valid after the mount is gone (IFileStream).
		class PakFileStream final : public IFileStream
		{
		public:
			explicit PakFileStream(Buffer bytes)
				: m_Bytes(std::move(bytes))
			{
			}

			[[nodiscard]] Result<size_t> Read(std::span<std::byte> destination) override
			{
				const size_t count = std::min(destination.size(), m_Bytes.size() - m_Position);
				if (count != 0)
					std::memcpy(destination.data(), m_Bytes.data() + m_Position, count);
				m_Position += count;
				return count;
			}

			[[nodiscard]] Status Seek(uint64_t position) override
			{
				if (position > m_Bytes.size())
					return MakeError(ErrorCode::InvalidArgument, "cannot seek to {}: the file has {} bytes", position, m_Bytes.size());
				m_Position = static_cast<size_t>(position);
				return {};
			}

			[[nodiscard]] uint64_t GetPosition() const override { return m_Position; }
			[[nodiscard]] uint64_t GetSize() const override { return m_Bytes.size(); }
		private:
			Buffer m_Bytes;
			size_t m_Position = 0;
		};

	}

	namespace Utils {

		static char LowerAsciiLetter(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		static std::string ToAsciiLowerCase(std::string_view text)
		{
			std::string lower(text);
			std::ranges::transform(lower, lower.begin(), [](char character)
			{
				return LowerAsciiLetter(character);
			});
			return lower;
		}

		// "<scheme>://<relative>" for messages.
		static std::string DisplayPath(std::string_view scheme, std::string_view relativePath)
		{
			return std::format("{}://{}", scheme, relativePath);
		}

		static Status RejectMutation(const VfsPath& path)
		{
			return MakeError(ErrorCode::PermissionDenied, "cannot change '{}': the pak mount is read-only", path.ToString());
		}

		// The kind of the node stored under exactly `relativePath`; the root ("") is a directory.
		template<typename PakState>
		static PakNodeKind KindOf(const PakState& state, std::string_view relativePath)
		{
			if (relativePath.empty() || state.Directories.contains(relativePath))
				return PakNodeKind::Directory;
			if (state.Files.contains(relativePath))
				return PakNodeKind::File;
			return PakNodeKind::None;
		}

		// The stored node in the directory of `relativePath` whose name equals its last component ignoring ASCII case but not
		// exactly (the case policy); empty when there is none. Create guarantees there is at most one.
		template<typename PakState>
		static std::string FindCaseVariant(const PakState& state, std::string_view relativePath)
		{
			const size_t slash = relativePath.rfind('/');
			const std::string prefix = slash == std::string_view::npos ? std::string() : std::string(relativePath.substr(0, slash + 1));
			const std::string lowerName = ToAsciiLowerCase(relativePath.substr(prefix.size()));
			const auto matches = [&prefix, &lowerName, relativePath](std::string_view key)
			{
				if (!key.starts_with(prefix) || key == relativePath)
					return false;
				const std::string_view rest = key.substr(prefix.size());
				return rest.find('/') == std::string_view::npos && ToAsciiLowerCase(rest) == lowerName;
			};
			for (const auto& [key, entry] : state.Files)
			{
				if (matches(key))
					return key;
			}
			for (const std::string& key : state.Directories)
			{
				if (matches(key))
					return key;
			}
			return {};
		}

		// The kind of the existing node at `path`, applying the case policy to each component: a component that matches a
		// node only ignoring case is Validation "case mismatch", a complete miss is NotFound.
		template<typename PakState>
		static Result<PakNodeKind> Resolve(const PakState& state, const VfsPath& path)
		{
			const std::string_view relativePath = path.GetPath();
			const PakNodeKind exact = KindOf(state, relativePath);
			if (exact != PakNodeKind::None)
				return exact;

			size_t start = 0;
			while (true)
			{
				const size_t end = relativePath.find('/', start);
				const bool isLast = end == std::string_view::npos;
				const std::string_view prefix = relativePath.substr(0, isLast ? relativePath.size() : end);
				const PakNodeKind kind = KindOf(state, prefix);
				if (kind == PakNodeKind::None)
				{
					const std::string variant = FindCaseVariant(state, prefix);
					if (!variant.empty())
					{
						return MakeError(ErrorCode::Validation, "case mismatch: '{}' exists as '{}'", DisplayPath(path.GetScheme(), prefix),
							DisplayPath(path.GetScheme(), variant));
					}
					return MakeError(ErrorCode::NotFound, "'{}' does not exist", path.ToString());
				}
				if (isLast)
					return kind;
				if (kind == PakNodeKind::File)
					return MakeError(ErrorCode::NotFound, "'{}' does not exist: '{}' is a file", path.ToString(), DisplayPath(path.GetScheme(), prefix));
				start = end + 1;
			}
		}

	}

	PakMount::PakMount(ConstructionKey /*key*/, Ref<const PakReader> pak)
		: m_State(CreateScope<State>())
	{
		m_State->Pak = std::move(pak);
	}

	PakMount::~PakMount() = default;

	Result<Scope<PakMount>> PakMount::Create(Ref<const PakReader> pak)
	{
		ENGINE_CORE_ASSERT(pak != nullptr, "PakMount::Create needs a pak");
		Scope<PakMount> mount = CreateScope<PakMount>(ConstructionKey(), std::move(pak));
		State& state = *mount->m_State;
		const std::string& name = state.Pak->GetName();

		for (const PakEntry& entry : state.Pak->GetEntries())
		{
			if (entry.Type != PakFileEntryType)
				continue;
			// ParsePakToc checked that plain-file paths are valid relative paths and unique.
			state.Files.emplace(entry.Path, &entry);
			for (size_t slash = entry.Path.find('/'); slash != std::string::npos; slash = entry.Path.find('/', slash + 1))
				state.Directories.emplace(entry.Path.substr(0, slash));
		}

		// The case policy (§4.10): no directory holds two nodes whose names differ only in ASCII case, and no path is both a
		// file and a directory.
		std::map<std::string, std::string, std::less<>> spellings; // lower-case path -> stored spelling
		const auto add = [&spellings, &name](const std::string& path) -> Status
		{
			const auto [existing, inserted] = spellings.emplace(Utils::ToAsciiLowerCase(path), path);
			if (!inserted && existing->second != path)
			{
				return MakeError(ErrorCode::Validation, "the pak '{}' holds '{}' and '{}', which differ only in letter case", name, existing->second,
					path);
			}
			return {};
		};
		for (const auto& [path, entry] : state.Files)
		{
			if (state.Directories.contains(path))
				return MakeError(ErrorCode::Validation, "the pak '{}' holds the file '{}' and files below it", name, path);
			ENGINE_TRY(add(path));
		}
		for (const std::string& directory : state.Directories)
			ENGINE_TRY(add(directory));
		return mount;
	}

	Result<Buffer> PakMount::ReadFile(const VfsPath& path) const
	{
		const State& state = *m_State;
		ENGINE_TRY_ASSIGN(const PakNodeKind kind, Utils::Resolve(state, path));
		if (kind != PakNodeKind::File)
			return MakeError(ErrorCode::Io, "cannot read '{}': it is a directory", path.ToString());
		const PakEntry& entry = *state.Files.find(path.GetPath())->second;
		return WithContext(state.Pak->ReadEntry(entry), std::format("while reading '{}'", path.ToString()));
	}

	Result<Scope<IFileStream>> PakMount::Open(const VfsPath& path) const
	{
		ENGINE_TRY_ASSIGN(Buffer bytes, ReadFile(path));
		return Scope<IFileStream>(CreateScope<PakFileStream>(std::move(bytes)));
	}

	Status PakMount::WriteFileAtomic(const VfsPath& path, std::span<const std::byte> /*data*/)
	{
		return Utils::RejectMutation(path);
	}

	Result<FileInfo> PakMount::GetInfo(const VfsPath& path) const
	{
		const State& state = *m_State;
		ENGINE_TRY_ASSIGN(const PakNodeKind kind, Utils::Resolve(state, path));
		if (kind == PakNodeKind::Directory)
			return FileInfo{ .Size = 0, .ModificationTime = 0, .IsDirectory = true };
		// A mounted pak never changes, so the modification time is constant.
		const PakEntry& entry = *state.Files.find(path.GetPath())->second;
		return FileInfo{ .Size = entry.Size, .ModificationTime = 0, .IsDirectory = false };
	}

	Result<std::vector<VfsEntry>> PakMount::List(const VfsPath& directory, bool recursive) const
	{
		const State& state = *m_State;
		ENGINE_TRY_ASSIGN(const PakNodeKind kind, Utils::Resolve(state, directory));
		if (kind != PakNodeKind::Directory)
			return MakeError(ErrorCode::Io, "cannot list '{}': it is not a directory", directory.ToString());

		const std::string_view relativeDirectory = directory.GetPath();
		const std::string prefix = relativeDirectory.empty() ? std::string() : std::string(relativeDirectory) + '/';
		const auto isListed = [&prefix, recursive](std::string_view key)
		{
			return key.starts_with(prefix) && (recursive || key.find('/', prefix.size()) == std::string_view::npos);
		};

		std::vector<VfsEntry> entries;
		for (auto iterator = state.Files.lower_bound(prefix); iterator != state.Files.end() && iterator->first.starts_with(prefix); ++iterator)
		{
			if (!isListed(iterator->first))
				continue;
			ENGINE_TRY_ASSIGN(VfsPath entryPath, VfsPath::Create(directory.GetScheme(), iterator->first));
			entries.push_back(VfsEntry{ .Path = std::move(entryPath), .Info = FileInfo{ .Size = iterator->second->Size, .ModificationTime = 0, .IsDirectory = false } });
		}
		for (auto iterator = state.Directories.lower_bound(prefix); iterator != state.Directories.end() && iterator->starts_with(prefix); ++iterator)
		{
			if (!isListed(*iterator))
				continue;
			ENGINE_TRY_ASSIGN(VfsPath entryPath, VfsPath::Create(directory.GetScheme(), *iterator));
			entries.push_back(VfsEntry{ .Path = std::move(entryPath), .Info = FileInfo{ .Size = 0, .ModificationTime = 0, .IsDirectory = true } });
		}
		std::ranges::sort(entries, {}, &VfsEntry::Path);
		return entries;
	}

	Status PakMount::CreateDirectories(const VfsPath& directory)
	{
		return Utils::RejectMutation(directory);
	}

	Status PakMount::Remove(const VfsPath& path)
	{
		return Utils::RejectMutation(path);
	}

	Status PakMount::Move(const VfsPath& from, const VfsPath& /*to*/)
	{
		return Utils::RejectMutation(from);
	}

}
