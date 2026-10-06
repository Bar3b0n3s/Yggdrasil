#include "EnginePCH.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"

#include "Engine/Core/Mounts/Private/SnapshotFileStream.h"
#include "Engine/Core/Private/NativePath.h"
#include "Engine/Core/Private/PathText.h"

namespace Engine {

	namespace Utils {

		static std::filesystem::path ToNative(const std::filesystem::path& root, std::string_view relativePath)
		{
			if (relativePath.empty())
				return root;
			return root / PathFromUtf8(relativePath);
		}

		static Status CheckWritable(MountAccess access, const VfsPath& path)
		{
			if (access == MountAccess::ReadOnly)
				return MakeError(ErrorCode::PermissionDenied, "cannot change '{}': the mount is read-only", path.ToString());
			return {};
		}

		// The parent of `relativePath` exists with exactly this spelling and is a directory.
		static Status VerifyParentDirectory(const std::filesystem::path& root, std::string_view relativePath)
		{
			const std::string_view parent = ParentPathOf(relativePath);
			ENGINE_TRY(FileSystem::VerifyCase(root, parent));
			if (parent.empty())
				return {};
			ENGINE_TRY_ASSIGN(const FileInfo info, FileSystem::GetInfo(ToNative(root, parent)));
			if (!info.IsDirectory)
				return MakeError(ErrorCode::NotFound, "the directory '{}' does not exist: it is a file", parent);
			return {};
		}

		// The name of the entry in the directory `parent` that the host resolves `native` to, or "" when none is found.
		static std::string FindHostEntryName(const std::filesystem::path& parent, const std::filesystem::path& native)
		{
			std::error_code error;
			std::filesystem::directory_iterator iterator(parent, error);
			const std::filesystem::directory_iterator end;
			while (!error && iterator != end)
			{
				std::error_code equivalentError;
				if (std::filesystem::equivalent(iterator->path(), native, equivalentError) && !equivalentError)
					return FileNameToUtf8(iterator->path());
				iterator.increment(error);
			}
			return {};
		}

		// The final component of a new name (write, directory creation, move destination) is free or names the existing
		// entry with exactly this spelling; another spelling of it is the case policy's Validation error. The parent must
		// have been verified.
		//
		// That no directory entry spells the name, even ignoring ASCII case, does not make it free: the host may still
		// resolve it to an existing entry (NTFS and APFS fold the case of non-ASCII letters, APFS and HFS+ ignore Unicode
		// normalization, Windows resolves 8.3 short names), and writing it would replace that entry. Such a name is a
		// Validation error too, so no host overwrites an entry spelled differently. Hosts that do not alias the name
		// (Linux) create a new entry, as MemoryMount does.
		static Status VerifyNewName(const std::filesystem::path& root, std::string_view relativePath)
		{
			Status status = FileSystem::VerifyCase(root, relativePath);
			if (status || status.error().GetCode() != ErrorCode::NotFound)
				return status;

			const std::filesystem::path native = ToNative(root, relativePath);
			std::error_code error;
			const std::filesystem::file_status hostStatus = std::filesystem::symlink_status(native, error);
			// A component that is a file (ENOTDIR on POSIX) means the name cannot exist; the caller reports the file in the way.
			if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory)
				return MakeError(ErrorCode::Io, "cannot check whether '{}' exists: OS error {}", relativePath, error.value());
			if (!std::filesystem::exists(hostStatus))
				return {};

			const std::string onDisk = FindHostEntryName(ToNative(root, ParentPathOf(relativePath)), native);
			if (onDisk.empty())
				return MakeError(ErrorCode::Validation, "'{}' names an existing entry spelled differently on this host", relativePath);
			return MakeError(ErrorCode::Validation, "'{}' names the existing entry '{}' on this host", relativePath,
				JoinRelative(ParentPathOf(relativePath), onDisk));
		}

		// Every component of `relativePath` from the first missing one on is new: the first one is checked like the final
		// component of a write (VerifyNewName); the ones after it cannot exist on the host when it does not.
		static Status VerifyNewDirectories(const std::filesystem::path& root, std::string_view relativePath)
		{
			size_t end = 0;
			while (end != std::string_view::npos)
			{
				end = relativePath.find('/', end + 1);
				const std::string_view prefix = relativePath.substr(0, end);
				Status existing = FileSystem::VerifyCase(root, prefix);
				if (existing)
					continue;
				if (existing.error().GetCode() != ErrorCode::NotFound)
					return existing;
				return VerifyNewName(root, prefix);
			}
			return {};
		}

	}

	NativeDirectoryMount::NativeDirectoryMount(std::filesystem::path root, MountAccess access, const AtomicWriteOptions& writeOptions)
		: m_Root(std::move(root)), m_Access(access), m_WriteOptions(writeOptions)
	{
	}

	Result<Scope<NativeDirectoryMount>> NativeDirectoryMount::Create(std::filesystem::path root, MountAccess access,
		const AtomicWriteOptions& writeOptions)
	{
		ENGINE_TRY_ASSIGN(const FileInfo info, FileSystem::GetInfo(root));
		if (!info.IsDirectory)
			return MakeError(ErrorCode::InvalidArgument, "cannot mount '{}': it is a file, not a directory", Utils::PathToUtf8(root));
		return CreateScope<NativeDirectoryMount>(std::move(root), access, writeOptions);
	}

	Result<Buffer> NativeDirectoryMount::ReadFile(const VfsPath& path) const
	{
		std::shared_lock lock(m_Mutex);
		ENGINE_TRY(FileSystem::VerifyCase(m_Root, path.GetPath()));
		return FileSystem::ReadFile(Utils::ToNative(m_Root, path.GetPath()));
	}

	Result<Scope<IFileStream>> NativeDirectoryMount::Open(const VfsPath& path) const
	{
		// ReadFile takes the lock; the snapshot is complete before the stream exists.
		ENGINE_TRY_ASSIGN(Buffer data, ReadFile(path));
		return CreateScope<SnapshotFileStream>(std::move(data));
	}

	Status NativeDirectoryMount::WriteFileAtomic(const VfsPath& path, std::span<const std::byte> data)
	{
		std::scoped_lock lock(m_Mutex);
		ENGINE_TRY(Utils::CheckWritable(m_Access, path));
		const std::string_view relativePath = path.GetPath();
		if (relativePath.empty())
			return MakeError(ErrorCode::Io, "cannot write '{}': it is the mount root, a directory", path.ToString());

		ENGINE_TRY(Utils::VerifyParentDirectory(m_Root, relativePath));
		ENGINE_TRY(Utils::VerifyNewName(m_Root, relativePath));
		return FileSystem::WriteFileAtomic(Utils::ToNative(m_Root, relativePath), data, m_WriteOptions);
	}

	Result<FileInfo> NativeDirectoryMount::GetInfo(const VfsPath& path) const
	{
		std::shared_lock lock(m_Mutex);
		ENGINE_TRY(FileSystem::VerifyCase(m_Root, path.GetPath()));
		return FileSystem::GetInfo(Utils::ToNative(m_Root, path.GetPath()));
	}

	Result<std::vector<VfsEntry>> NativeDirectoryMount::List(const VfsPath& directory, bool recursive) const
	{
		std::shared_lock lock(m_Mutex);
		const std::string_view relativeDirectory = directory.GetPath();
		ENGINE_TRY(FileSystem::VerifyCase(m_Root, relativeDirectory));
		const std::filesystem::path nativeDirectory = Utils::ToNative(m_Root, relativeDirectory);
		ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> nativeEntries, FileSystem::ListDirectory(nativeDirectory, recursive));

		std::vector<VfsEntry> entries;
		entries.reserve(nativeEntries.size());
		for (const std::filesystem::path& nativeEntry : nativeEntries)
		{
			// Host names that no VfsPath can spell (reserved names, trailing dots, control characters, invalid UTF-8; only
			// possible on hosts that allow them) cannot be addressed through the VFS, so they are not listed.
			const std::string relative = Utils::PathToUtf8(nativeEntry.lexically_relative(nativeDirectory));
			Result<VfsPath> entryPath = VfsPath::Create(directory.GetScheme(), Utils::JoinRelative(relativeDirectory, relative));
			if (!entryPath)
				continue;

			Result<FileInfo> info = FileSystem::GetInfo(nativeEntry);
			if (!info)
			{
				if (info.error().GetCode() == ErrorCode::NotFound)
					continue; // removed since it was listed
				return std::unexpected(std::move(info).error());
			}
			entries.push_back(VfsEntry{ .Path = std::move(*entryPath), .Info = *info });
		}
		return entries;
	}

	Status NativeDirectoryMount::CreateDirectories(const VfsPath& directory)
	{
		std::scoped_lock lock(m_Mutex);
		ENGINE_TRY(Utils::CheckWritable(m_Access, directory));
		ENGINE_TRY(FileSystem::VerifyCase(m_Root, {}));
		const std::string_view relativePath = directory.GetPath();
		if (relativePath.empty())
			return {};

		const std::filesystem::path nativePath = Utils::ToNative(m_Root, relativePath);
		const Status existing = FileSystem::VerifyCase(m_Root, relativePath);
		if (existing)
		{
			ENGINE_TRY_ASSIGN(const FileInfo info, FileSystem::GetInfo(nativePath));
			if (!info.IsDirectory)
				return MakeError(ErrorCode::AlreadyExists, "cannot create the directory '{}': a file is in the way", directory.ToString());
			return {};
		}
		// Validation: a component is another spelling of an existing entry. NotFound: every component before the first
		// missing one exists with exactly this spelling, so everything created from there on is a new name, provided the
		// host does not resolve the first missing one to an existing entry.
		if (existing.error().GetCode() != ErrorCode::NotFound)
			return existing;
		ENGINE_TRY(Utils::VerifyNewDirectories(m_Root, relativePath));
		return FileSystem::CreateDirectories(nativePath);
	}

	Status NativeDirectoryMount::Remove(const VfsPath& path)
	{
		std::scoped_lock lock(m_Mutex);
		ENGINE_TRY(Utils::CheckWritable(m_Access, path));
		if (path.GetPath().empty())
			return MakeError(ErrorCode::InvalidArgument, "cannot remove '{}': it is the mount root", path.ToString());
		ENGINE_TRY(FileSystem::VerifyCase(m_Root, path.GetPath()));
		return FileSystem::Remove(Utils::ToNative(m_Root, path.GetPath()));
	}

	Status NativeDirectoryMount::Move(const VfsPath& from, const VfsPath& to)
	{
		std::scoped_lock lock(m_Mutex);
		ENGINE_TRY(Utils::CheckWritable(m_Access, from));
		const std::string_view fromPath = from.GetPath();
		const std::string_view toPath = to.GetPath();
		if (fromPath.empty() || toPath.empty())
			return MakeError(ErrorCode::InvalidArgument, "cannot move '{}' to '{}': the mount root cannot move", from.ToString(), to.ToString());
		if (Utils::IsStrictlyUnder(toPath, fromPath))
			return MakeError(ErrorCode::InvalidArgument, "cannot move '{}' into itself ('{}')", from.ToString(), to.ToString());

		ENGINE_TRY(FileSystem::VerifyCase(m_Root, fromPath));
		ENGINE_TRY(Utils::VerifyParentDirectory(m_Root, toPath));
		if (!Utils::IsCaseOnlyRename(fromPath, toPath))
		{
			const Status destination = FileSystem::VerifyCase(m_Root, toPath);
			if (destination)
				return MakeError(ErrorCode::AlreadyExists, "cannot move '{}' to '{}': the destination exists", from.ToString(), to.ToString());
			if (destination.error().GetCode() != ErrorCode::NotFound)
				return destination;
			ENGINE_TRY(Utils::VerifyNewName(m_Root, toPath));
		}
		return FileSystem::Move(Utils::ToNative(m_Root, fromPath), Utils::ToNative(m_Root, toPath));
	}

}
