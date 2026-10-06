#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <filesystem>
#include <span>
#include <vector>

namespace Engine {

	// A mount backed by a host directory (Architecture §4.10): engine:// at Resources/ in development builds, project://
	// at the project root, user:// at <UserData>/<AppName>/, cache:// at <Project>/Library/Cache, enginecache:// at
	// bin/EngineCache. Paths map to root / path with forward slashes.
	//   - Writes use FileSystem::WriteFileAtomic with the mount's AtomicWriteOptions (temporary file, flush, optional
	//     ".bak" backup, atomic rename). Mounts of regenerable or per-user data turn the backup off: project:// keeps the
	//     default KeepBackup = true; cache://, enginecache:// and user:// pass KeepBackup = false.
	//   - Every access applies the IMount case policy through FileSystem::VerifyCase, including the final component of
	//     a write, a CreateDirectories or a Move destination, so a reference that would break on Linux breaks
	//     identically on Windows and macOS, and no host silently overwrites a file spelled differently.
	//   - Open reads the whole file and streams that snapshot (IFileStream), so no host file stays open behind a stream.
	// Thread-safe.
	class NativeDirectoryMount final : public IMount
	{
	public:
		// A mount of `root`, which is not checked here: when it is missing, every call fails with NotFound. Prefer Create.
		// Tests set writeOptions.InjectFailure to fail every write of the mount at one step.
		explicit NativeDirectoryMount(std::filesystem::path root, MountAccess access = MountAccess::ReadWrite,
			const AtomicWriteOptions& writeOptions = {});

		// A mount of `root` after checking that it is an existing directory. Errors: NotFound, InvalidArgument (a file),
		// Io.
		[[nodiscard]] static Result<Scope<NativeDirectoryMount>> Create(std::filesystem::path root,
			MountAccess access = MountAccess::ReadWrite, const AtomicWriteOptions& writeOptions = {});

		[[nodiscard]] Result<Buffer> ReadFile(const VfsPath& path) const override;
		[[nodiscard]] Result<Scope<IFileStream>> Open(const VfsPath& path) const override;
		[[nodiscard]] Status WriteFileAtomic(const VfsPath& path, std::span<const std::byte> data) override;
		[[nodiscard]] Result<FileInfo> GetInfo(const VfsPath& path) const override;
		[[nodiscard]] Result<std::vector<VfsEntry>> List(const VfsPath& directory, bool recursive) const override;
		[[nodiscard]] Status CreateDirectories(const VfsPath& directory) override;
		[[nodiscard]] Status Remove(const VfsPath& path) override;
		[[nodiscard]] Status Move(const VfsPath& from, const VfsPath& to) override;
		[[nodiscard]] MountAccess GetAccess() const override { return m_Access; }

		[[nodiscard]] const std::filesystem::path& GetRoot() const { return m_Root; }
		[[nodiscard]] const AtomicWriteOptions& GetWriteOptions() const { return m_WriteOptions; }
	private:
		std::filesystem::path m_Root;
		MountAccess m_Access = MountAccess::ReadWrite;
		AtomicWriteOptions m_WriteOptions;
	};

}
