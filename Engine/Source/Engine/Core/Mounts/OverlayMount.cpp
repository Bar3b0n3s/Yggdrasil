#include "EnginePCH.h"
#include "Engine/Core/Mounts/OverlayMount.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Mounts/Private/SnapshotFileStream.h"
#include "Engine/Core/Private/AsciiText.h"
#include "Engine/Core/Private/PathText.h"

#include <mutex>
#include <shared_mutex>

// The merged view: an entry is visible when the upper layer holds it, or when the lower layer holds it and neither it nor
// an ancestor is in m_Removed. Upper entries are always visible, so the case policy holds for the merged view as long as
// every mutation checks the merged view first, which each one does before it changes anything.

namespace Engine {

	namespace {

		// What the merged view holds at one path.
		struct MergedEntry
		{
			bool InUpper = false; // the upper layer holds it with exactly this spelling
			bool InLower = false; // the lower layer holds it with exactly this spelling and the overlay does not hide it
			FileInfo Info;        // the upper layer's when InUpper, otherwise the lower layer's
		};

	}

	namespace Utils {

		// The scheme of the paths the overlay builds for its own bookkeeping; mounts ignore schemes.
		static constexpr std::string_view InternalScheme = "overlay";

		static std::unexpected<Error> ReleasedError()
		{
			return MakeError(ErrorCode::InvalidState, "the OverlayMount was used after ReleaseLower");
		}

		// True when `relativePath` or one of its ancestors was removed in the overlay, which hides it in the lower layer.
		static bool IsHidden(const std::set<std::string>& removed, std::string_view relativePath)
		{
			if (removed.empty() || relativePath.empty())
				return false;
			size_t position = 0;
			while (true)
			{
				const size_t slash = relativePath.find('/', position);
				if (removed.contains(std::string(relativePath.substr(0, slash))))
					return true;
				if (slash == std::string_view::npos)
					return false;
				position = slash + 1;
			}
		}

		// True when a sibling of `relativePath` whose name differs from it only in ASCII case was removed in the overlay,
		// that is, when a lower-layer case variant of it is hidden.
		static bool HasRemovedCaseVariant(const std::set<std::string>& removed, std::string_view relativePath)
		{
			const std::string_view parent = ParentPathOf(relativePath);
			const std::string_view name = FileNameOf(relativePath);
			const std::string prefix = parent.empty() ? std::string() : std::string(parent) + '/';
			for (auto iterator = removed.lower_bound(prefix); iterator != removed.end() && iterator->starts_with(prefix); ++iterator)
			{
				const std::string_view rest = std::string_view(*iterator).substr(prefix.size());
				if (rest.find('/') == std::string_view::npos && rest != name && EqualsIgnoreAsciiCase(rest, name))
					return true;
			}
			return false;
		}

		// Resolves an existing path in the merged view, component by component, applying the case policy: a component
		// that matches a visible entry only ignoring case is Validation, a complete miss is NotFound. A lower-layer case
		// variant hidden by the overlay does not count.
		static Result<MergedEntry> ResolveMerged(const MemoryMount& upper, const IMount& lower, const std::set<std::string>& removed,
			std::string_view scheme, std::string_view relativePath)
		{
			MergedEntry entry;
			entry.InUpper = true;
			entry.InLower = true;
			entry.Info.IsDirectory = true;
			if (relativePath.empty())
				return entry;

			size_t start = 0;
			while (true)
			{
				const size_t end = relativePath.find('/', start);
				const bool isLast = end == std::string_view::npos;
				const std::string prefix(relativePath.substr(0, isLast ? relativePath.size() : end));
				ENGINE_TRY_ASSIGN(const VfsPath prefixPath, VfsPath::Create(scheme, prefix));

				// Each layer is asked only while it holds the parent with exactly this spelling, so a Validation answer
				// is about this component.
				MergedEntry current;
				if (entry.InUpper)
				{
					Result<FileInfo> upperInfo = upper.GetInfo(prefixPath);
					if (upperInfo)
					{
						current.InUpper = true;
						current.Info = *upperInfo;
					}
					else if (upperInfo.error().GetCode() != ErrorCode::NotFound)
					{
						return std::unexpected(std::move(upperInfo).error()); // a visible case variant, or a failure
					}
				}
				if (entry.InLower && !removed.contains(prefix))
				{
					Result<FileInfo> lowerInfo = lower.GetInfo(prefixPath);
					if (lowerInfo)
					{
						current.InLower = true;
						if (!current.InUpper)
							current.Info = *lowerInfo;
					}
					else if (lowerInfo.error().GetCode() == ErrorCode::Validation)
					{
						// The lower case variant is shadowed by an exact upper entry, or hidden by a removal.
						if (!current.InUpper && !HasRemovedCaseVariant(removed, prefix))
							return std::unexpected(std::move(lowerInfo).error());
					}
					else if (lowerInfo.error().GetCode() != ErrorCode::NotFound)
					{
						return std::unexpected(std::move(lowerInfo).error());
					}
				}

				if (!current.InUpper && !current.InLower)
					return MakeError(ErrorCode::NotFound, "'{}://{}' does not exist", scheme, relativePath);
				if (isLast)
					return current;
				if (!current.Info.IsDirectory)
					return MakeError(ErrorCode::NotFound, "'{}://{}' does not exist: '{}' is a file", scheme, relativePath, prefix);
				entry = current;
				start = end + 1;
			}
		}

		// Like ResolveMerged, for the final component of a new name (write, directory creation, move destination): an
		// existing entry is returned, NotFound means the name is free, Validation means another spelling of it is visible.
		static Result<std::optional<MergedEntry>> ResolveNewName(const MemoryMount& upper, const IMount& lower,
			const std::set<std::string>& removed, std::string_view scheme, std::string_view relativePath)
		{
			Result<MergedEntry> entry = ResolveMerged(upper, lower, removed, scheme, relativePath);
			if (entry)
				return std::optional<MergedEntry>(*entry);
			if (entry.error().GetCode() == ErrorCode::NotFound)
				return std::optional<MergedEntry>();
			return std::unexpected(std::move(entry).error());
		}

		static Status RequireDirectory(const MergedEntry& entry, std::string_view scheme, std::string_view relativePath)
		{
			if (!entry.Info.IsDirectory)
				return MakeError(ErrorCode::NotFound, "the directory '{}://{}' does not exist: it is a file", scheme, relativePath);
			return {};
		}

		// Gives the upper layer the merged view's directory `relativeDirectory` (and its ancestors), so a file can be
		// written below it. Directories that only the lower layer holds are copied up silently: they are not changes.
		static Status EnsureUpperDirectory(MemoryMount& upper, std::string_view scheme, std::string_view relativeDirectory)
		{
			if (relativeDirectory.empty())
				return {};
			ENGINE_TRY_ASSIGN(const VfsPath directory, VfsPath::Create(scheme, relativeDirectory));
			return upper.CreateDirectories(directory);
		}

		static Result<Buffer> ReadMerged(const MemoryMount& upper, const IMount& lower, const MergedEntry& entry, const VfsPath& path)
		{
			if (entry.Info.IsDirectory)
				return MakeError(ErrorCode::Io, "cannot read '{}': it is a directory", path.ToString());
			return entry.InUpper ? upper.ReadFile(path) : lower.ReadFile(path);
		}

		// The merged listing of the directory `entry` at `directory`: upper entries, then the lower ones that are neither
		// hidden nor shadowed, in byte-wise path order.
		static Result<std::vector<VfsEntry>> ListMerged(const MemoryMount& upper, const IMount& lower, const std::set<std::string>& removed,
			const VfsPath& directory, const MergedEntry& entry, bool recursive)
		{
			if (!entry.Info.IsDirectory)
				return MakeError(ErrorCode::Io, "cannot list '{}': it is not a directory", directory.ToString());

			std::map<std::string, VfsEntry> merged;
			if (entry.InUpper)
			{
				ENGINE_TRY_ASSIGN(std::vector<VfsEntry> upperEntries, upper.List(directory, recursive));
				for (VfsEntry& upperEntry : upperEntries)
				{
					std::string key(upperEntry.Path.GetPath());
					merged.emplace(std::move(key), std::move(upperEntry));
				}
			}
			if (entry.InLower)
			{
				ENGINE_TRY_ASSIGN(std::vector<VfsEntry> lowerEntries, lower.List(directory, recursive));
				for (VfsEntry& lowerEntry : lowerEntries)
				{
					std::string key(lowerEntry.Path.GetPath());
					if (!IsHidden(removed, key))
						merged.emplace(std::move(key), std::move(lowerEntry)); // keeps an upper entry of the same path
				}
			}

			std::vector<VfsEntry> entries;
			entries.reserve(merged.size());
			for (auto& [key, mergedEntry] : merged)
				entries.push_back(std::move(mergedEntry));
			return entries;
		}

	}

	OverlayMount::OverlayMount(Scope<IMount> lower)
		: m_Lower(std::move(lower))
	{
		ENGINE_CORE_ASSERT(m_Lower != nullptr, "OverlayMount needs a lower mount");
	}

	Result<Buffer> OverlayMount::ReadFile(const VfsPath& path) const
	{
		const std::shared_lock lock(m_Mutex);
		ENGINE_CORE_ASSERT(m_Lower != nullptr, "OverlayMount::ReadFile after ReleaseLower");
		if (!m_Lower)
			return Utils::ReleasedError();
		ENGINE_TRY_ASSIGN(const MergedEntry entry, Utils::ResolveMerged(m_Upper, *m_Lower, m_Removed, path.GetScheme(), path.GetPath()));
		return Utils::ReadMerged(m_Upper, *m_Lower, entry, path);
	}

	Result<Scope<IFileStream>> OverlayMount::Open(const VfsPath& path) const
	{
		ENGINE_TRY_ASSIGN(Buffer data, ReadFile(path));
		return CreateScope<SnapshotFileStream>(std::move(data));
	}

	Status OverlayMount::WriteFileAtomic(const VfsPath& path, std::span<const std::byte> data)
	{
		const std::unique_lock lock(m_Mutex);
		ENGINE_CORE_ASSERT(m_Lower != nullptr, "OverlayMount::WriteFileAtomic after ReleaseLower");
		if (!m_Lower)
			return Utils::ReleasedError();
		const std::string_view scheme = path.GetScheme();
		const std::string_view relativePath = path.GetPath();
		if (relativePath.empty())
			return MakeError(ErrorCode::Io, "cannot write '{}': it is the mount root, a directory", path.ToString());

		const std::string_view parent = Utils::ParentPathOf(relativePath);
		ENGINE_TRY_ASSIGN(const MergedEntry parentEntry, Utils::ResolveMerged(m_Upper, *m_Lower, m_Removed, scheme, parent));
		ENGINE_TRY(Utils::RequireDirectory(parentEntry, scheme, parent));
		ENGINE_TRY_ASSIGN(const std::optional<MergedEntry> existing, Utils::ResolveNewName(m_Upper, *m_Lower, m_Removed, scheme, relativePath));
		if (existing && existing->Info.IsDirectory)
			return MakeError(ErrorCode::Io, "cannot write '{}': it is a directory", path.ToString());

		ENGINE_TRY(Utils::EnsureUpperDirectory(m_Upper, scheme, parent));
		ENGINE_TRY(m_Upper.WriteFileAtomic(path, data));
		m_Changed.emplace(relativePath);
		return {};
	}

	Result<FileInfo> OverlayMount::GetInfo(const VfsPath& path) const
	{
		const std::shared_lock lock(m_Mutex);
		ENGINE_CORE_ASSERT(m_Lower != nullptr, "OverlayMount::GetInfo after ReleaseLower");
		if (!m_Lower)
			return Utils::ReleasedError();
		ENGINE_TRY_ASSIGN(const MergedEntry entry, Utils::ResolveMerged(m_Upper, *m_Lower, m_Removed, path.GetScheme(), path.GetPath()));
		return entry.Info;
	}

	Result<std::vector<VfsEntry>> OverlayMount::List(const VfsPath& directory, bool recursive) const
	{
		const std::shared_lock lock(m_Mutex);
		ENGINE_CORE_ASSERT(m_Lower != nullptr, "OverlayMount::List after ReleaseLower");
		if (!m_Lower)
			return Utils::ReleasedError();
		ENGINE_TRY_ASSIGN(const MergedEntry entry,
			Utils::ResolveMerged(m_Upper, *m_Lower, m_Removed, directory.GetScheme(), directory.GetPath()));
		return Utils::ListMerged(m_Upper, *m_Lower, m_Removed, directory, entry, recursive);
	}

	Status OverlayMount::CreateDirectories(const VfsPath& directory)
	{
		const std::unique_lock lock(m_Mutex);
		ENGINE_CORE_ASSERT(m_Lower != nullptr, "OverlayMount::CreateDirectories after ReleaseLower");
		if (!m_Lower)
			return Utils::ReleasedError();
		const std::string_view scheme = directory.GetScheme();
		const std::string_view relativePath = directory.GetPath();

		// Find the first missing component; the ones before it must be directories of the merged view.
		std::vector<std::string> created;
		size_t start = 0;
		while (!relativePath.empty())
		{
			const size_t end = relativePath.find('/', start);
			const bool isLast = end == std::string_view::npos;
			std::string prefix(relativePath.substr(0, isLast ? relativePath.size() : end));
			if (created.empty())
			{
				ENGINE_TRY_ASSIGN(const std::optional<MergedEntry> existing, Utils::ResolveNewName(m_Upper, *m_Lower, m_Removed, scheme, prefix));
				if (existing && !existing->Info.IsDirectory)
				{
					return MakeError(ErrorCode::AlreadyExists, "cannot create the directory '{}': the file '{}://{}' is in the way",
						directory.ToString(), scheme, prefix);
				}
				if (!existing)
					created.push_back(std::move(prefix));
			}
			else
			{
				created.push_back(std::move(prefix)); // below a new directory: new as well
			}
			if (isLast)
				break;
			start = end + 1;
		}
		if (created.empty())
			return {};

		ENGINE_TRY(m_Upper.CreateDirectories(directory));
		for (std::string& path : created)
			m_Changed.insert(std::move(path));
		return {};
	}

	Status OverlayMount::Remove(const VfsPath& path)
	{
		const std::unique_lock lock(m_Mutex);
		ENGINE_CORE_ASSERT(m_Lower != nullptr, "OverlayMount::Remove after ReleaseLower");
		if (!m_Lower)
			return Utils::ReleasedError();
		const std::string_view relativePath = path.GetPath();
		if (relativePath.empty())
			return MakeError(ErrorCode::InvalidArgument, "cannot remove '{}': it is the mount root", path.ToString());

		ENGINE_TRY_ASSIGN(const MergedEntry entry, Utils::ResolveMerged(m_Upper, *m_Lower, m_Removed, path.GetScheme(), relativePath));
		if (entry.InUpper)
			ENGINE_TRY(m_Upper.Remove(path));
		if (entry.InLower)
			m_Removed.emplace(relativePath);
		m_Changed.emplace(relativePath);
		return {};
	}

	Status OverlayMount::Move(const VfsPath& from, const VfsPath& to)
	{
		const std::unique_lock lock(m_Mutex);
		ENGINE_CORE_ASSERT(m_Lower != nullptr, "OverlayMount::Move after ReleaseLower");
		if (!m_Lower)
			return Utils::ReleasedError();
		const std::string_view scheme = to.GetScheme();
		const std::string_view fromPath = from.GetPath();
		const std::string_view toPath = to.GetPath();
		if (fromPath.empty() || toPath.empty())
			return MakeError(ErrorCode::InvalidArgument, "cannot move '{}' to '{}': the mount root cannot move", from.ToString(), to.ToString());
		if (Utils::IsStrictlyUnder(toPath, fromPath))
			return MakeError(ErrorCode::InvalidArgument, "cannot move '{}' into itself ('{}')", from.ToString(), to.ToString());

		ENGINE_TRY_ASSIGN(const MergedEntry source, Utils::ResolveMerged(m_Upper, *m_Lower, m_Removed, from.GetScheme(), fromPath));
		const std::string_view toParent = Utils::ParentPathOf(toPath);
		ENGINE_TRY_ASSIGN(const MergedEntry parentEntry, Utils::ResolveMerged(m_Upper, *m_Lower, m_Removed, scheme, toParent));
		ENGINE_TRY(Utils::RequireDirectory(parentEntry, scheme, toParent));
		if (!Utils::IsCaseOnlyRename(fromPath, toPath))
		{
			ENGINE_TRY_ASSIGN(const std::optional<MergedEntry> destination, Utils::ResolveNewName(m_Upper, *m_Lower, m_Removed, scheme, toPath));
			if (destination)
				return MakeError(ErrorCode::AlreadyExists, "cannot move '{}' to '{}': the destination exists", from.ToString(), to.ToString());
		}

		// Read everything the move carries before changing anything, so the copy into the upper layer cannot fail halfway
		// on a lower-layer read.
		std::vector<std::string> directories;
		std::vector<std::pair<std::string, Buffer>> files;
		const auto renamed = [fromPath, toPath](std::string_view path)
		{
			return std::string(toPath) + std::string(path.substr(fromPath.size()));
		};
		if (source.Info.IsDirectory)
		{
			ENGINE_TRY_ASSIGN(const std::vector<VfsEntry> entries, Utils::ListMerged(m_Upper, *m_Lower, m_Removed, from, source, true));
			for (const VfsEntry& entry : entries)
			{
				if (entry.Info.IsDirectory)
				{
					directories.push_back(renamed(entry.Path.GetPath()));
					continue;
				}
				ENGINE_TRY_ASSIGN(const MergedEntry fileEntry,
					Utils::ResolveMerged(m_Upper, *m_Lower, m_Removed, from.GetScheme(), entry.Path.GetPath()));
				ENGINE_TRY_ASSIGN(Buffer data, Utils::ReadMerged(m_Upper, *m_Lower, fileEntry, entry.Path));
				files.emplace_back(renamed(entry.Path.GetPath()), std::move(data));
			}
		}
		else
		{
			ENGINE_TRY_ASSIGN(Buffer data, Utils::ReadMerged(m_Upper, *m_Lower, source, from));
			files.emplace_back(std::string(toPath), std::move(data));
		}

		if (source.InUpper)
			ENGINE_TRY(m_Upper.Remove(from));
		if (source.InLower)
			m_Removed.emplace(fromPath);
		ENGINE_TRY(Utils::EnsureUpperDirectory(m_Upper, scheme, toParent));
		if (source.Info.IsDirectory)
		{
			ENGINE_TRY(m_Upper.CreateDirectories(to));
			for (const std::string& directory : directories)
				ENGINE_TRY(Utils::EnsureUpperDirectory(m_Upper, scheme, directory));
		}
		for (const auto& [path, data] : files)
		{
			ENGINE_TRY_ASSIGN(const VfsPath destination, VfsPath::Create(scheme, path));
			ENGINE_TRY(m_Upper.WriteFileAtomic(destination, data));
		}
		m_Changed.emplace(fromPath);
		m_Changed.emplace(toPath);
		return {};
	}

	std::vector<std::string> OverlayMount::GetChangedPaths() const
	{
		const std::shared_lock lock(m_Mutex);
		return std::vector<std::string>(m_Changed.begin(), m_Changed.end());
	}

	void OverlayMount::Clear()
	{
		const std::unique_lock lock(m_Mutex);
		const Result<VfsPath> root = VfsPath::Create(Utils::InternalScheme, {});
		ENGINE_CORE_VERIFY(root.has_value(), "The overlay's internal scheme is invalid");
		if (!root)
			return;
		Result<std::vector<VfsEntry>> entries = m_Upper.List(*root, false);
		ENGINE_CORE_VERIFY(entries.has_value(), "The overlay's upper layer cannot be listed: {}", entries.has_value() ? "" : entries.error().ToString());
		if (entries)
		{
			for (const VfsEntry& entry : *entries)
			{
				const Status removed = m_Upper.Remove(entry.Path);
				ENGINE_CORE_VERIFY(removed.has_value(), "The overlay's upper layer cannot remove '{}'", entry.Path.ToString());
			}
		}
		m_Removed.clear();
		m_Changed.clear();
	}

	Scope<IMount> OverlayMount::ReleaseLower()
	{
		const std::unique_lock lock(m_Mutex);
		ENGINE_CORE_ASSERT(m_Lower != nullptr, "OverlayMount::ReleaseLower called twice");
		return std::move(m_Lower);
	}

	const IMount& OverlayMount::GetLower() const
	{
		ENGINE_CORE_ASSERT(m_Lower != nullptr, "OverlayMount::GetLower after ReleaseLower");
		return *m_Lower;
	}

}
