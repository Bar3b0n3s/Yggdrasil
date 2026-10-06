#include "EnginePCH.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"

// M1 contract stub (Roadmap rule 3): stream C implements the host-directory mount on FileSystem.

namespace Engine {

	NativeDirectoryMount::NativeDirectoryMount(std::filesystem::path root, MountAccess access,
		const AtomicWriteOptions& writeOptions)
		: m_Root(std::move(root)), m_Access(access), m_WriteOptions(writeOptions)
	{
	}

	Result<Scope<NativeDirectoryMount>> NativeDirectoryMount::Create(std::filesystem::path /*root*/, MountAccess /*access*/,
		const AtomicWriteOptions& /*writeOptions*/)
	{
		return MakeError(ErrorCode::Unsupported, "NativeDirectoryMount::Create is an M1 contract stub");
	}

	Result<Buffer> NativeDirectoryMount::ReadFile(const VfsPath& /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "NativeDirectoryMount::ReadFile is an M1 contract stub");
	}

	Result<Scope<IFileStream>> NativeDirectoryMount::Open(const VfsPath& /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "NativeDirectoryMount::Open is an M1 contract stub");
	}

	Status NativeDirectoryMount::WriteFileAtomic(const VfsPath& /*path*/, std::span<const std::byte> /*data*/)
	{
		return MakeError(ErrorCode::Unsupported, "NativeDirectoryMount::WriteFileAtomic is an M1 contract stub");
	}

	Result<FileInfo> NativeDirectoryMount::GetInfo(const VfsPath& /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "NativeDirectoryMount::GetInfo is an M1 contract stub");
	}

	Result<std::vector<VfsEntry>> NativeDirectoryMount::List(const VfsPath& /*directory*/, bool /*recursive*/) const
	{
		return MakeError(ErrorCode::Unsupported, "NativeDirectoryMount::List is an M1 contract stub");
	}

	Status NativeDirectoryMount::CreateDirectories(const VfsPath& /*directory*/)
	{
		return MakeError(ErrorCode::Unsupported, "NativeDirectoryMount::CreateDirectories is an M1 contract stub");
	}

	Status NativeDirectoryMount::Remove(const VfsPath& /*path*/)
	{
		return MakeError(ErrorCode::Unsupported, "NativeDirectoryMount::Remove is an M1 contract stub");
	}

	Status NativeDirectoryMount::Move(const VfsPath& /*from*/, const VfsPath& /*to*/)
	{
		return MakeError(ErrorCode::Unsupported, "NativeDirectoryMount::Move is an M1 contract stub");
	}

}
