#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

namespace Engine {

	// An in-memory write layer over another mount (Architecture §4.10), used by automation dry runs (§13.4): the
	// project:// mount is unmounted, wrapped in an OverlayMount for the duration of the call and mounted back afterwards,
	// so parsing, compiling and registry updates see the would-be files while nothing reaches the disk. The swap is
	// visible to every thread, so it happens only while no job uses project:// (VirtualFileSystem class comment).
	//   - Reads see the overlay's own writes first, then the lower mount; files and directories removed in the overlay
	//     are hidden from reads and listings even though they still exist below.
	//   - Every mutation (write, create directory, remove, move) changes only the overlay. The lower mount is never
	//     written, whatever its access mode, so a read-only lower mount (a pak) can be overlaid too.
	//   - List merges both layers, applies removals and returns the IMount order.
	//   - The case policy applies against the merged view: a write, directory creation or move destination whose name
	//     differs only in case from an entry of either layer is Validation, and a case-only rename of an entry moves it
	//     within the overlay.
	//   - Open streams a snapshot, from whichever layer holds the file.
	// The overlay is always read-write. Thread-safe; the lower mount is used only through its own thread-safe interface.
	class OverlayMount final : public IMount
	{
	public:
		// Takes ownership of `lower` (non-null, asserted).
		explicit OverlayMount(Scope<IMount> lower);

		[[nodiscard]] Result<Buffer> ReadFile(const VfsPath& path) const override;
		[[nodiscard]] Result<Scope<IFileStream>> Open(const VfsPath& path) const override;
		[[nodiscard]] Status WriteFileAtomic(const VfsPath& path, std::span<const std::byte> data) override;
		[[nodiscard]] Result<FileInfo> GetInfo(const VfsPath& path) const override;
		[[nodiscard]] Result<std::vector<VfsEntry>> List(const VfsPath& directory, bool recursive) const override;
		[[nodiscard]] Status CreateDirectories(const VfsPath& directory) override;
		[[nodiscard]] Status Remove(const VfsPath& path) override;
		[[nodiscard]] Status Move(const VfsPath& from, const VfsPath& to) override;
		[[nodiscard]] MountAccess GetAccess() const override { return MountAccess::ReadWrite; }

		// The relative paths (GetPath() form) written, created, removed or moved in the overlay, sorted and without
		// duplicates: what a dry run would have changed.
		[[nodiscard]] std::vector<std::string> GetChangedPaths() const;

		// Discards every overlay change; reads see the lower mount again.
		void Clear();

		// Hands the lower mount back (to remount it after a dry run). The overlay is unusable afterwards: any further call
		// is a programmer error (asserted).
		[[nodiscard]] Scope<IMount> ReleaseLower();

		// The lower mount (valid until ReleaseLower).
		[[nodiscard]] const IMount& GetLower() const;
	private:
		Scope<IMount> m_Lower;
		MemoryMount m_Upper;               // files and directories written in the overlay
		mutable std::shared_mutex m_Mutex; // guards m_Removed and m_Changed, and orders upper/lower lookups
		std::set<std::string> m_Removed;   // relative paths hidden from the lower mount
		std::set<std::string> m_Changed;
	};

}
