#include "EnginePCH.h"
#include "Engine/Core/VirtualFileSystem.h"

// M1 contract stub (Roadmap rule 3): stream C implements mounting and routing.

namespace Engine {

	VirtualFileSystem::VirtualFileSystem() = default;

	VirtualFileSystem::~VirtualFileSystem() = default;

	Status VirtualFileSystem::Mount(std::string_view /*scheme*/, Scope<IMount> /*mount*/)
	{
		return MakeError(ErrorCode::Unsupported, "VirtualFileSystem::Mount is an M1 contract stub");
	}

	Result<Scope<IMount>> VirtualFileSystem::Unmount(std::string_view /*scheme*/)
	{
		return MakeError(ErrorCode::Unsupported, "VirtualFileSystem::Unmount is an M1 contract stub");
	}

	bool VirtualFileSystem::IsMounted(std::string_view /*scheme*/) const
	{
		return false;
	}

	std::vector<std::string> VirtualFileSystem::GetSchemes() const
	{
		return {};
	}

	Result<Buffer> VirtualFileSystem::ReadFile(const VfsPath& /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "VirtualFileSystem::ReadFile is an M1 contract stub");
	}

	Result<std::string> VirtualFileSystem::ReadText(const VfsPath& /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "VirtualFileSystem::ReadText is an M1 contract stub");
	}

	Result<Scope<IFileStream>> VirtualFileSystem::Open(const VfsPath& /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "VirtualFileSystem::Open is an M1 contract stub");
	}

	Status VirtualFileSystem::WriteFileAtomic(const VfsPath& /*path*/, std::span<const std::byte> /*data*/)
	{
		return MakeError(ErrorCode::Unsupported, "VirtualFileSystem::WriteFileAtomic is an M1 contract stub");
	}

	bool VirtualFileSystem::Exists(const VfsPath& /*path*/) const
	{
		return false;
	}

	Result<FileInfo> VirtualFileSystem::GetInfo(const VfsPath& /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "VirtualFileSystem::GetInfo is an M1 contract stub");
	}

	Result<std::vector<VfsEntry>> VirtualFileSystem::List(const VfsPath& /*directory*/, bool /*recursive*/) const
	{
		return MakeError(ErrorCode::Unsupported, "VirtualFileSystem::List is an M1 contract stub");
	}

	Status VirtualFileSystem::CreateDirectories(const VfsPath& /*directory*/)
	{
		return MakeError(ErrorCode::Unsupported, "VirtualFileSystem::CreateDirectories is an M1 contract stub");
	}

	Status VirtualFileSystem::Remove(const VfsPath& /*path*/)
	{
		return MakeError(ErrorCode::Unsupported, "VirtualFileSystem::Remove is an M1 contract stub");
	}

	Status VirtualFileSystem::Move(const VfsPath& /*from*/, const VfsPath& /*to*/)
	{
		return MakeError(ErrorCode::Unsupported, "VirtualFileSystem::Move is an M1 contract stub");
	}

}
