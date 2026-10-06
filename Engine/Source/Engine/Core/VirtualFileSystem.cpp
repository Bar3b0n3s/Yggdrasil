#include "EnginePCH.h"
#include "Engine/Core/VirtualFileSystem.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Utf8.h"

#include <mutex>
#include <shared_mutex>

namespace Engine {

	namespace Utils {

		static std::unexpected<Error> NotMountedError(const VfsPath& path)
		{
			if (path.IsEmpty())
				return MakeError(ErrorCode::NotFound, "the empty VFS path names no file");
			return MakeError(ErrorCode::NotFound, "cannot access '{}': the scheme '{}' is not mounted", path.ToString(), path.GetScheme());
		}

	}

	VirtualFileSystem::VirtualFileSystem() = default;

	VirtualFileSystem::~VirtualFileSystem() = default;

	Status VirtualFileSystem::Mount(std::string_view scheme, Scope<IMount> mount)
	{
		ENGINE_CORE_ASSERT(mount != nullptr, "VirtualFileSystem::Mount needs a mount for '{}'", scheme);
		if (!mount)
			return MakeError(ErrorCode::InvalidArgument, "cannot mount nothing at '{}'", scheme);
		ENGINE_TRY(VfsPath::ValidateScheme(scheme));

		const std::unique_lock lock(m_MountsMutex);
		if (m_Mounts.contains(scheme))
			return MakeError(ErrorCode::AlreadyExists, "cannot mount '{}://': the scheme is already mounted", scheme);
		m_Mounts.emplace(std::string(scheme), std::move(mount));
		return {};
	}

	Result<Scope<IMount>> VirtualFileSystem::Unmount(std::string_view scheme)
	{
		const std::unique_lock lock(m_MountsMutex);
		const auto iterator = m_Mounts.find(scheme);
		if (iterator == m_Mounts.end())
			return MakeError(ErrorCode::NotFound, "cannot unmount '{}://': the scheme is not mounted", scheme);
		Scope<IMount> mount = std::move(iterator->second);
		m_Mounts.erase(iterator);
		return mount;
	}

	bool VirtualFileSystem::IsMounted(std::string_view scheme) const
	{
		const std::shared_lock lock(m_MountsMutex);
		return m_Mounts.contains(scheme);
	}

	std::vector<std::string> VirtualFileSystem::GetSchemes() const
	{
		const std::shared_lock lock(m_MountsMutex);
		std::vector<std::string> schemes;
		schemes.reserve(m_Mounts.size());
		for (const auto& [scheme, mount] : m_Mounts)
			schemes.push_back(scheme);
		return schemes;
	}

	Result<Buffer> VirtualFileSystem::ReadFile(const VfsPath& path) const
	{
		const std::shared_lock lock(m_MountsMutex);
		const auto iterator = m_Mounts.find(path.GetScheme());
		if (path.IsEmpty() || iterator == m_Mounts.end())
			return Utils::NotMountedError(path);
		return iterator->second->ReadFile(path);
	}

	Result<std::string> VirtualFileSystem::ReadText(const VfsPath& path) const
	{
		ENGINE_TRY_ASSIGN(const Buffer bytes, ReadFile(path));
		const std::string_view text = AsStringView(bytes);
		const size_t invalidOffset = FindInvalidUtf8(text);
		if (invalidOffset != text.size())
			return MakeError(ErrorCode::Validation, "'{}' is not valid UTF-8: invalid byte sequence at offset {}", path.ToString(), invalidOffset);
		return std::string(text);
	}

	Result<Scope<IFileStream>> VirtualFileSystem::Open(const VfsPath& path) const
	{
		const std::shared_lock lock(m_MountsMutex);
		const auto iterator = m_Mounts.find(path.GetScheme());
		if (path.IsEmpty() || iterator == m_Mounts.end())
			return Utils::NotMountedError(path);
		return iterator->second->Open(path);
	}

	Status VirtualFileSystem::WriteFileAtomic(const VfsPath& path, std::span<const std::byte> data)
	{
		const std::shared_lock lock(m_MountsMutex);
		const auto iterator = m_Mounts.find(path.GetScheme());
		if (path.IsEmpty() || iterator == m_Mounts.end())
			return Utils::NotMountedError(path);
		return iterator->second->WriteFileAtomic(path, data);
	}

	bool VirtualFileSystem::Exists(const VfsPath& path) const
	{
		return GetInfo(path).has_value();
	}

	Result<FileInfo> VirtualFileSystem::GetInfo(const VfsPath& path) const
	{
		const std::shared_lock lock(m_MountsMutex);
		const auto iterator = m_Mounts.find(path.GetScheme());
		if (path.IsEmpty() || iterator == m_Mounts.end())
			return Utils::NotMountedError(path);
		return iterator->second->GetInfo(path);
	}

	Result<std::vector<VfsEntry>> VirtualFileSystem::List(const VfsPath& directory, bool recursive) const
	{
		const std::shared_lock lock(m_MountsMutex);
		const auto iterator = m_Mounts.find(directory.GetScheme());
		if (directory.IsEmpty() || iterator == m_Mounts.end())
			return Utils::NotMountedError(directory);
		return iterator->second->List(directory, recursive);
	}

	Status VirtualFileSystem::CreateDirectories(const VfsPath& directory)
	{
		const std::shared_lock lock(m_MountsMutex);
		const auto iterator = m_Mounts.find(directory.GetScheme());
		if (directory.IsEmpty() || iterator == m_Mounts.end())
			return Utils::NotMountedError(directory);
		return iterator->second->CreateDirectories(directory);
	}

	Status VirtualFileSystem::Remove(const VfsPath& path)
	{
		const std::shared_lock lock(m_MountsMutex);
		const auto iterator = m_Mounts.find(path.GetScheme());
		if (path.IsEmpty() || iterator == m_Mounts.end())
			return Utils::NotMountedError(path);
		return iterator->second->Remove(path);
	}

	Status VirtualFileSystem::Move(const VfsPath& from, const VfsPath& to)
	{
		if (!from.IsEmpty() && !to.IsEmpty() && from.GetScheme() != to.GetScheme())
		{
			return MakeError(ErrorCode::InvalidArgument, "cannot move '{}' to '{}': both paths must have the same scheme", from.ToString(),
				to.ToString());
		}

		const std::shared_lock lock(m_MountsMutex);
		const auto iterator = m_Mounts.find(from.GetScheme());
		if (from.IsEmpty() || to.IsEmpty() || iterator == m_Mounts.end())
			return Utils::NotMountedError(from.IsEmpty() ? from : to);
		return iterator->second->Move(from, to);
	}

}
