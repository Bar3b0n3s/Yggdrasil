#include "EnginePCH.h"
#include "Engine/Asset/PakMount.h"

// M6 contract stub (Roadmap rule 3): stream D (caches, pak, PakMount) implements the mount.

namespace Engine {

	struct PakMount::State
	{
		Ref<const PakReader> Pak;
	};

	PakMount::PakMount(ConstructionKey /*key*/, Ref<const PakReader> pak)
		: m_State(CreateScope<State>())
	{
		m_State->Pak = std::move(pak);
	}

	PakMount::~PakMount() = default;

	Result<Scope<PakMount>> PakMount::Create(Ref<const PakReader> /*pak*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakMount::Create is an M6 contract stub");
	}

	Result<Buffer> PakMount::ReadFile(const VfsPath& /*path*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakMount::ReadFile is an M6 contract stub");
	}

	Result<Scope<IFileStream>> PakMount::Open(const VfsPath& /*path*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakMount::Open is an M6 contract stub");
	}

	Status PakMount::WriteFileAtomic(const VfsPath& /*path*/, std::span<const std::byte> /*data*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakMount::WriteFileAtomic is an M6 contract stub");
	}

	Result<FileInfo> PakMount::GetInfo(const VfsPath& /*path*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakMount::GetInfo is an M6 contract stub");
	}

	Result<std::vector<VfsEntry>> PakMount::List(const VfsPath& /*directory*/, bool /*recursive*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakMount::List is an M6 contract stub");
	}

	Status PakMount::CreateDirectories(const VfsPath& /*directory*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakMount::CreateDirectories is an M6 contract stub");
	}

	Status PakMount::Remove(const VfsPath& /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakMount::Remove is an M6 contract stub");
	}

	Status PakMount::Move(const VfsPath& /*from*/, const VfsPath& /*to*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakMount::Move is an M6 contract stub");
	}

}
