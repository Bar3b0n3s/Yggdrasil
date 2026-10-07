#include "EnginePCH.h"
#include "Engine/AssetPipeline/AssetWriter.h"

// M6 contract stub (Roadmap rule 3): stream E (commands, methods, hot reload) implements the no-echo writer.

namespace Engine {

	struct AssetWriter::State
	{
		VirtualFileSystem* Vfs = nullptr;      // documented back-reference
		PollingFileWatcher* Watcher = nullptr; // documented back-reference, cleared by its owner
		Listener WriteListener;
		bool IsDryRun = false;
	};

	AssetWriter::AssetWriter(VirtualFileSystem& vfs)
		: m_State(CreateScope<State>())
	{
		m_State->Vfs = &vfs;
	}

	AssetWriter::~AssetWriter() = default;

	void AssetWriter::SetWatcher(PollingFileWatcher* /*watcher*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AssetWriter::SetListener(Listener /*listener*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AssetWriter::SetDryRun(bool /*dryRun*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool AssetWriter::IsDryRun() const
	{
		return m_State->IsDryRun;
	}

	Status AssetWriter::Write(const VfsPath& /*path*/, std::span<const std::byte> /*data*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetWriter::Write is an M6 contract stub");
	}

	Status AssetWriter::Remove(const VfsPath& /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetWriter::Remove is an M6 contract stub");
	}

	Status AssetWriter::Move(const VfsPath& /*from*/, const VfsPath& /*to*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetWriter::Move is an M6 contract stub");
	}

	Status AssetWriter::CreateDirectories(const VfsPath& /*directory*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetWriter::CreateDirectories is an M6 contract stub");
	}

}
