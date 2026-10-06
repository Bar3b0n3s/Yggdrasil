#include "EnginePCH.h"
#include "Engine/Core/Mounts/OverlayMount.h"

// M1 contract stub (Roadmap rule 3): stream C implements the write layer.

namespace Engine {

	OverlayMount::OverlayMount(Scope<IMount> lower)
		: m_Lower(std::move(lower))
	{
	}

	Result<Buffer> OverlayMount::ReadFile(const VfsPath& /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "OverlayMount::ReadFile is an M1 contract stub");
	}

	Result<Scope<IFileStream>> OverlayMount::Open(const VfsPath& /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "OverlayMount::Open is an M1 contract stub");
	}

	Status OverlayMount::WriteFileAtomic(const VfsPath& /*path*/, std::span<const std::byte> /*data*/)
	{
		return MakeError(ErrorCode::Unsupported, "OverlayMount::WriteFileAtomic is an M1 contract stub");
	}

	Result<FileInfo> OverlayMount::GetInfo(const VfsPath& /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "OverlayMount::GetInfo is an M1 contract stub");
	}

	Result<std::vector<VfsEntry>> OverlayMount::List(const VfsPath& /*directory*/, bool /*recursive*/) const
	{
		return MakeError(ErrorCode::Unsupported, "OverlayMount::List is an M1 contract stub");
	}

	Status OverlayMount::CreateDirectories(const VfsPath& /*directory*/)
	{
		return MakeError(ErrorCode::Unsupported, "OverlayMount::CreateDirectories is an M1 contract stub");
	}

	Status OverlayMount::Remove(const VfsPath& /*path*/)
	{
		return MakeError(ErrorCode::Unsupported, "OverlayMount::Remove is an M1 contract stub");
	}

	Status OverlayMount::Move(const VfsPath& /*from*/, const VfsPath& /*to*/)
	{
		return MakeError(ErrorCode::Unsupported, "OverlayMount::Move is an M1 contract stub");
	}

	std::vector<std::string> OverlayMount::GetChangedPaths() const
	{
		return {};
	}

	void OverlayMount::Clear()
	{
	}

	Scope<IMount> OverlayMount::ReleaseLower()
	{
		return std::move(m_Lower);
	}

	const IMount& OverlayMount::GetLower() const
	{
		return *m_Lower;
	}

}
