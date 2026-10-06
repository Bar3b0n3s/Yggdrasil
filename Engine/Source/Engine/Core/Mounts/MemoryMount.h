#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <cstdint>
#include <map>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

namespace Engine {

	// A mount held entirely in memory (Architecture §4.10): tests, and the backing of the PollingFileWatcher tests (§7.5).
	// Deterministic: ModificationTime is a per-mount counter incremented by every mutation and stored on each written
	// file, so a test controls exactly when a file "changes". The root always exists; other directories exist only once
	// created with CreateDirectories, and writes require an existing parent exactly like a native mount. Follows every
	// IMount rule (the case policy for reads, parent directories and the final component of writes, directory creation
	// and moves; ordering; errors). Thread-safe.
	class MemoryMount final : public IMount
	{
	public:
		explicit MemoryMount(MountAccess access = MountAccess::ReadWrite);

		[[nodiscard]] Result<Buffer> ReadFile(const VfsPath& path) const override;
		// The stream reads a snapshot of the file taken when it was opened.
		[[nodiscard]] Result<Scope<IFileStream>> Open(const VfsPath& path) const override;
		[[nodiscard]] Status WriteFileAtomic(const VfsPath& path, std::span<const std::byte> data) override;
		[[nodiscard]] Result<FileInfo> GetInfo(const VfsPath& path) const override;
		[[nodiscard]] Result<std::vector<VfsEntry>> List(const VfsPath& directory, bool recursive) const override;
		[[nodiscard]] Status CreateDirectories(const VfsPath& directory) override;
		[[nodiscard]] Status Remove(const VfsPath& path) override;
		[[nodiscard]] Status Move(const VfsPath& from, const VfsPath& to) override;
		[[nodiscard]] MountAccess GetAccess() const override;

		// Switches between read-write and read-only, so a test can populate a mount and then freeze it (a stand-in for a
		// read-only pak below an OverlayMount).
		void SetAccess(MountAccess access);

		// The number of successful mutations so far (writes, directory creations, removals, moves). Tests use it to prove
		// that nothing was written, for example through an OverlayMount.
		[[nodiscard]] uint64_t GetMutationCount() const;
	private:
		struct File
		{
			Buffer Data;
			uint64_t ModificationTime = 0;
		};
	private:
		mutable std::shared_mutex m_Mutex;   // guards every member below
		std::map<std::string, File> m_Files; // by relative path
		std::set<std::string> m_Directories; // relative paths of directories other than the root
		uint64_t m_MutationCount = 0;
		MountAccess m_Access = MountAccess::ReadWrite;
	};

}
