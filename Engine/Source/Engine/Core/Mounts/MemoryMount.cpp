#include "EnginePCH.h"
#include "Engine/Core/Mounts/MemoryMount.h"

#include "Engine/Core/Mounts/Private/SnapshotFileStream.h"
#include "Engine/Core/Private/AsciiText.h"
#include "Engine/Core/Private/PathText.h"

#include <mutex>
#include <shared_mutex>

namespace Engine {

	namespace {

		enum class EntryKind : uint8_t
		{
			None,
			File,
			Directory
		};

	}

	namespace Utils {

		static std::string DisplayPath(std::string_view scheme, std::string_view relativePath)
		{
			return std::format("{}://{}", scheme, relativePath);
		}

		static Status CheckWritable(MountAccess access, const VfsPath& path)
		{
			if (access == MountAccess::ReadOnly)
				return MakeError(ErrorCode::PermissionDenied, "cannot change '{}': the mount is read-only", path.ToString());
			return {};
		}

		// The kind of the entry stored under exactly `relativePath`; the root ("") is always a directory. Every stored
		// entry's parent directories are stored too.
		template<typename FileMap>
		static EntryKind KindOf(const FileMap& files, const std::set<std::string>& directories, const std::string& relativePath)
		{
			if (relativePath.empty())
				return EntryKind::Directory;
			if (files.contains(relativePath))
				return EntryKind::File;
			if (directories.contains(relativePath))
				return EntryKind::Directory;
			return EntryKind::None;
		}

		// The stored entry in the directory of `relativePath` whose name equals its final component ignoring ASCII case,
		// but not exactly (the case policy); empty when there is none. There is at most one, by the policy itself.
		template<typename FileMap>
		static std::string FindCaseVariant(const FileMap& files, const std::set<std::string>& directories, std::string_view relativePath)
		{
			const std::string_view parent = ParentPathOf(relativePath);
			const std::string_view name = FileNameOf(relativePath);
			const std::string prefix = parent.empty() ? std::string() : std::string(parent) + '/';

			const auto matches = [&prefix, name](const std::string& key)
			{
				const std::string_view rest = std::string_view(key).substr(prefix.size());
				return rest.find('/') == std::string_view::npos && rest != name && EqualsIgnoreAsciiCase(rest, name);
			};
			for (auto iterator = files.lower_bound(prefix); iterator != files.end() && iterator->first.starts_with(prefix); ++iterator)
			{
				if (matches(iterator->first))
					return iterator->first;
			}
			for (auto iterator = directories.lower_bound(prefix); iterator != directories.end() && iterator->starts_with(prefix); ++iterator)
			{
				if (matches(*iterator))
					return *iterator;
			}
			return {};
		}

		// The kind of the existing entry at `relativePath`, applying the case policy to every component: a component that
		// matches a stored entry only ignoring case is Validation, a complete miss is NotFound.
		template<typename FileMap>
		static Result<EntryKind> ResolveExisting(const FileMap& files, const std::set<std::string>& directories, std::string_view scheme,
			std::string_view relativePath)
		{
			const std::string fullPath(relativePath);
			const EntryKind exact = KindOf(files, directories, fullPath);
			if (exact != EntryKind::None)
				return exact;

			size_t start = 0;
			while (true)
			{
				const size_t end = relativePath.find('/', start);
				const bool isLast = end == std::string_view::npos;
				const std::string prefix(relativePath.substr(0, isLast ? relativePath.size() : end));
				const EntryKind kind = KindOf(files, directories, prefix);
				if (kind == EntryKind::None)
				{
					const std::string variant = FindCaseVariant(files, directories, prefix);
					if (!variant.empty())
					{
						return MakeError(ErrorCode::Validation, "case mismatch: '{}' exists as '{}'", DisplayPath(scheme, prefix),
							DisplayPath(scheme, variant));
					}
					return MakeError(ErrorCode::NotFound, "'{}' does not exist", DisplayPath(scheme, relativePath));
				}
				if (isLast)
					return kind;
				if (kind == EntryKind::File)
				{
					return MakeError(ErrorCode::NotFound, "'{}' does not exist: '{}' is a file", DisplayPath(scheme, relativePath),
						DisplayPath(scheme, prefix));
				}
				start = end + 1;
			}
		}

		// The final component of a new name is free or names the entry stored with exactly this spelling; a stored name
		// that differs only in case is the case policy's Validation error.
		template<typename FileMap>
		static Status VerifyNewName(const FileMap& files, const std::set<std::string>& directories, std::string_view scheme,
			const std::string& relativePath)
		{
			if (KindOf(files, directories, relativePath) != EntryKind::None)
				return {};
			const std::string variant = FindCaseVariant(files, directories, relativePath);
			if (!variant.empty())
			{
				return MakeError(ErrorCode::Validation, "case mismatch: '{}' exists as '{}'", DisplayPath(scheme, relativePath),
					DisplayPath(scheme, variant));
			}
			return {};
		}

		// Every key of `container` strictly below `directory`.
		template<typename Container, typename KeyOf>
		static std::vector<std::string> KeysUnder(const Container& container, std::string_view directory, KeyOf keyOf)
		{
			const std::string prefix = directory.empty() ? std::string() : std::string(directory) + '/';
			std::vector<std::string> keys;
			for (auto iterator = container.lower_bound(prefix); iterator != container.end() && keyOf(*iterator).starts_with(prefix); ++iterator)
				keys.push_back(keyOf(*iterator));
			return keys;
		}

	}

	MemoryMount::MemoryMount(MountAccess access)
		: m_Access(access)
	{
	}

	Result<Buffer> MemoryMount::ReadFile(const VfsPath& path) const
	{
		const std::shared_lock lock(m_Mutex);
		ENGINE_TRY_ASSIGN(const EntryKind kind, Utils::ResolveExisting(m_Files, m_Directories, path.GetScheme(), path.GetPath()));
		if (kind != EntryKind::File)
			return MakeError(ErrorCode::Io, "cannot read '{}': it is a directory", path.ToString());
		return m_Files.find(std::string(path.GetPath()))->second.Data;
	}

	Result<Scope<IFileStream>> MemoryMount::Open(const VfsPath& path) const
	{
		ENGINE_TRY_ASSIGN(Buffer data, ReadFile(path));
		return CreateScope<SnapshotFileStream>(std::move(data));
	}

	Status MemoryMount::WriteFileAtomic(const VfsPath& path, std::span<const std::byte> data)
	{
		const std::unique_lock lock(m_Mutex);
		ENGINE_TRY(Utils::CheckWritable(m_Access, path));
		const std::string relativePath(path.GetPath());
		if (relativePath.empty())
			return MakeError(ErrorCode::Io, "cannot write '{}': it is the mount root, a directory", path.ToString());

		const std::string_view parent = Utils::ParentPathOf(relativePath);
		ENGINE_TRY_ASSIGN(const EntryKind parentKind, Utils::ResolveExisting(m_Files, m_Directories, path.GetScheme(), parent));
		if (parentKind != EntryKind::Directory)
			return MakeError(ErrorCode::NotFound, "cannot write '{}': '{}' is a file, not a directory", path.ToString(), parent);
		if (Utils::KindOf(m_Files, m_Directories, relativePath) == EntryKind::Directory)
			return MakeError(ErrorCode::Io, "cannot write '{}': it is a directory", path.ToString());
		ENGINE_TRY(Utils::VerifyNewName(m_Files, m_Directories, path.GetScheme(), relativePath));

		++m_MutationCount;
		File& file = m_Files[relativePath];
		file.Data.assign(data.begin(), data.end());
		file.ModificationTime = m_MutationCount;
		return {};
	}

	Result<FileInfo> MemoryMount::GetInfo(const VfsPath& path) const
	{
		const std::shared_lock lock(m_Mutex);
		ENGINE_TRY_ASSIGN(const EntryKind kind, Utils::ResolveExisting(m_Files, m_Directories, path.GetScheme(), path.GetPath()));
		FileInfo info;
		if (kind == EntryKind::Directory)
		{
			info.IsDirectory = true;
			return info;
		}
		const File& file = m_Files.find(std::string(path.GetPath()))->second;
		info.Size = file.Data.size();
		info.ModificationTime = file.ModificationTime;
		return info;
	}

	Result<std::vector<VfsEntry>> MemoryMount::List(const VfsPath& directory, bool recursive) const
	{
		const std::shared_lock lock(m_Mutex);
		const std::string_view relativeDirectory = directory.GetPath();
		ENGINE_TRY_ASSIGN(const EntryKind kind, Utils::ResolveExisting(m_Files, m_Directories, directory.GetScheme(), relativeDirectory));
		if (kind != EntryKind::Directory)
			return MakeError(ErrorCode::Io, "cannot list '{}': it is not a directory", directory.ToString());

		const size_t prefixLength = relativeDirectory.empty() ? 0 : relativeDirectory.size() + 1;
		const auto isListed = [prefixLength, recursive](const std::string& key)
		{
			return recursive || key.find('/', prefixLength) == std::string::npos;
		};

		std::vector<VfsEntry> entries;
		const auto fileKey = [](const std::pair<const std::string, File>& entry) -> const std::string&
		{
			return entry.first;
		};
		for (const std::string& key : Utils::KeysUnder(m_Files, relativeDirectory, fileKey))
		{
			if (!isListed(key))
				continue;
			ENGINE_TRY_ASSIGN(VfsPath entryPath, VfsPath::Create(directory.GetScheme(), key));
			const File& file = m_Files.find(key)->second;
			entries.push_back(VfsEntry{ .Path = std::move(entryPath), .Info = FileInfo{ .Size = file.Data.size(), .ModificationTime = file.ModificationTime } });
		}

		const auto directoryKey = [](const std::string& entry) -> const std::string&
		{
			return entry;
		};
		for (const std::string& key : Utils::KeysUnder(m_Directories, relativeDirectory, directoryKey))
		{
			if (!isListed(key))
				continue;
			ENGINE_TRY_ASSIGN(VfsPath entryPath, VfsPath::Create(directory.GetScheme(), key));
			entries.push_back(VfsEntry{ .Path = std::move(entryPath), .Info = FileInfo{ .IsDirectory = true } });
		}

		std::ranges::sort(entries, {}, &VfsEntry::Path);
		return entries;
	}

	Status MemoryMount::CreateDirectories(const VfsPath& directory)
	{
		const std::unique_lock lock(m_Mutex);
		ENGINE_TRY(Utils::CheckWritable(m_Access, directory));
		const std::string_view relativePath = directory.GetPath();
		if (relativePath.empty())
			return {};

		// Errors can only come from existing components, which precede the first created one, so a failure changes
		// nothing.
		bool created = false;
		size_t start = 0;
		while (true)
		{
			const size_t end = relativePath.find('/', start);
			const bool isLast = end == std::string_view::npos;
			std::string prefix(relativePath.substr(0, isLast ? relativePath.size() : end));
			const EntryKind kind = Utils::KindOf(m_Files, m_Directories, prefix);
			if (kind == EntryKind::File)
			{
				return MakeError(ErrorCode::AlreadyExists, "cannot create the directory '{}': the file '{}' is in the way", directory.ToString(),
					Utils::DisplayPath(directory.GetScheme(), prefix));
			}
			if (kind == EntryKind::None)
			{
				ENGINE_TRY(Utils::VerifyNewName(m_Files, m_Directories, directory.GetScheme(), prefix));
				m_Directories.insert(std::move(prefix));
				created = true;
			}
			if (isLast)
				break;
			start = end + 1;
		}
		if (created)
			++m_MutationCount;
		return {};
	}

	Status MemoryMount::Remove(const VfsPath& path)
	{
		const std::unique_lock lock(m_Mutex);
		ENGINE_TRY(Utils::CheckWritable(m_Access, path));
		const std::string relativePath(path.GetPath());
		if (relativePath.empty())
			return MakeError(ErrorCode::InvalidArgument, "cannot remove '{}': it is the mount root", path.ToString());
		ENGINE_TRY_ASSIGN(const EntryKind kind, Utils::ResolveExisting(m_Files, m_Directories, path.GetScheme(), relativePath));

		if (kind == EntryKind::File)
		{
			m_Files.erase(relativePath);
		}
		else
		{
			const std::string prefix = relativePath + '/';
			std::erase_if(m_Files, [&prefix](const std::pair<const std::string, File>& entry)
			{
				return entry.first.starts_with(prefix);
			});
			std::erase_if(m_Directories, [&prefix](const std::string& entry)
			{
				return entry.starts_with(prefix);
			});
			m_Directories.erase(relativePath);
		}
		++m_MutationCount;
		return {};
	}

	Status MemoryMount::Move(const VfsPath& from, const VfsPath& to)
	{
		const std::unique_lock lock(m_Mutex);
		ENGINE_TRY(Utils::CheckWritable(m_Access, from));
		const std::string fromPath(from.GetPath());
		const std::string toPath(to.GetPath());
		if (fromPath.empty() || toPath.empty())
			return MakeError(ErrorCode::InvalidArgument, "cannot move '{}' to '{}': the mount root cannot move", from.ToString(), to.ToString());
		if (Utils::IsStrictlyUnder(toPath, fromPath))
			return MakeError(ErrorCode::InvalidArgument, "cannot move '{}' into itself ('{}')", from.ToString(), to.ToString());

		ENGINE_TRY_ASSIGN(const EntryKind kind, Utils::ResolveExisting(m_Files, m_Directories, from.GetScheme(), fromPath));
		const std::string_view toParent = Utils::ParentPathOf(toPath);
		ENGINE_TRY_ASSIGN(const EntryKind parentKind, Utils::ResolveExisting(m_Files, m_Directories, to.GetScheme(), toParent));
		if (parentKind != EntryKind::Directory)
			return MakeError(ErrorCode::NotFound, "cannot move to '{}': '{}' is a file, not a directory", to.ToString(), toParent);
		if (!Utils::IsCaseOnlyRename(fromPath, toPath))
		{
			if (Utils::KindOf(m_Files, m_Directories, toPath) != EntryKind::None)
				return MakeError(ErrorCode::AlreadyExists, "cannot move '{}' to '{}': the destination exists", from.ToString(), to.ToString());
			ENGINE_TRY(Utils::VerifyNewName(m_Files, m_Directories, to.GetScheme(), toPath));
		}

		const auto renamed = [&fromPath, &toPath](const std::string& key)
		{
			return toPath + key.substr(fromPath.size());
		};
		if (kind == EntryKind::File)
		{
			auto node = m_Files.extract(fromPath);
			node.key() = toPath;
			m_Files.insert(std::move(node));
		}
		else
		{
			// Extract everything first: a case-only rename maps keys onto spellings that sort elsewhere.
			const auto fileKey = [](const std::pair<const std::string, File>& entry) -> const std::string&
			{
				return entry.first;
			};
			const auto directoryKey = [](const std::string& entry) -> const std::string&
			{
				return entry;
			};
			std::vector<decltype(m_Files)::node_type> files;
			for (const std::string& key : Utils::KeysUnder(m_Files, fromPath, fileKey))
				files.push_back(m_Files.extract(key));
			std::vector<std::string> directories = Utils::KeysUnder(m_Directories, fromPath, directoryKey);
			for (const std::string& key : directories)
				m_Directories.erase(key);
			m_Directories.erase(fromPath);

			for (auto& node : files)
			{
				node.key() = renamed(node.key());
				m_Files.insert(std::move(node));
			}
			for (const std::string& key : directories)
				m_Directories.insert(renamed(key));
			m_Directories.insert(toPath);
		}
		++m_MutationCount;
		return {};
	}

	MountAccess MemoryMount::GetAccess() const
	{
		const std::shared_lock lock(m_Mutex);
		return m_Access;
	}

	void MemoryMount::SetAccess(MountAccess access)
	{
		const std::unique_lock lock(m_Mutex);
		m_Access = access;
	}

	uint64_t MemoryMount::GetMutationCount() const
	{
		const std::shared_lock lock(m_Mutex);
		return m_MutationCount;
	}

}
