#include "EnginePCH.h"
#include "Engine/Core/Mounts/MemoryMount.h"

// M1 contract stub (Roadmap rule 3): stream C implements the in-memory mount.

namespace Engine {

	MemoryMount::MemoryMount(MountAccess access)
		: m_Access(access)
	{
	}

	Result<Buffer> MemoryMount::ReadFile(const VfsPath& /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "MemoryMount::ReadFile is an M1 contract stub");
	}

	Result<Scope<IFileStream>> MemoryMount::Open(const VfsPath& /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "MemoryMount::Open is an M1 contract stub");
	}

	Status MemoryMount::WriteFileAtomic(const VfsPath& /*path*/, std::span<const std::byte> /*data*/)
	{
		return MakeError(ErrorCode::Unsupported, "MemoryMount::WriteFileAtomic is an M1 contract stub");
	}

	Result<FileInfo> MemoryMount::GetInfo(const VfsPath& /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "MemoryMount::GetInfo is an M1 contract stub");
	}

	Result<std::vector<VfsEntry>> MemoryMount::List(const VfsPath& /*directory*/, bool /*recursive*/) const
	{
		return MakeError(ErrorCode::Unsupported, "MemoryMount::List is an M1 contract stub");
	}

	Status MemoryMount::CreateDirectories(const VfsPath& /*directory*/)
	{
		return MakeError(ErrorCode::Unsupported, "MemoryMount::CreateDirectories is an M1 contract stub");
	}

	Status MemoryMount::Remove(const VfsPath& /*path*/)
	{
		return MakeError(ErrorCode::Unsupported, "MemoryMount::Remove is an M1 contract stub");
	}

	Status MemoryMount::Move(const VfsPath& /*from*/, const VfsPath& /*to*/)
	{
		return MakeError(ErrorCode::Unsupported, "MemoryMount::Move is an M1 contract stub");
	}

	MountAccess MemoryMount::GetAccess() const
	{
		return m_Access;
	}

	void MemoryMount::SetAccess(MountAccess /*access*/)
	{
	}

	uint64_t MemoryMount::GetMutationCount() const
	{
		return m_MutationCount;
	}

}
