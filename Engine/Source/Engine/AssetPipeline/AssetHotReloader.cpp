#include "EnginePCH.h"
#include "Engine/AssetPipeline/AssetHotReloader.h"

#include "Engine/Core/Jobs/JobSystem.h"

// M6 contract stub (Roadmap rule 3): stream E (commands, methods, hot reload) implements the hot reloader. The contract
// builds the watcher GetWatcher returns, so the AssetWriter can be wired to it.

namespace Engine {

	struct AssetHotReloader::State
	{
		State(const VirtualFileSystem& vfs, JobSystem& jobs, const AssetHotReloaderSpecification& specification)
			: Jobs(&jobs), Specification(specification), Watcher(vfs, { .Root = specification.Root, .DebounceSeconds = specification.DebounceSeconds })
		{
		}

		JobSystem* Jobs = nullptr; // documented back-reference
		AssetHotReloaderSpecification Specification;
		PollingFileWatcher Watcher;
	};

	AssetHotReloader::AssetHotReloader(const VirtualFileSystem& vfs, JobSystem& jobs, AssetHotReloaderSpecification specification)
		: m_State(CreateScope<State>(vfs, jobs, specification))
	{
	}

	AssetHotReloader::~AssetHotReloader() = default;

	Status AssetHotReloader::Start()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetHotReloader::Start is an M6 contract stub");
	}

	void AssetHotReloader::Stop()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool AssetHotReloader::IsRunning() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	PollingFileWatcher& AssetHotReloader::GetWatcher()
	{
		return m_State->Watcher;
	}

	void AssetHotReloader::SetChangeListener(ChangeListener /*listener*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AssetHotReloader::Update(double /*nowSeconds*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	AssetReimportTicket AssetHotReloader::BeginReimport(AssetHandle /*handle*/, uint64_t /*contentHash*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	bool AssetHotReloader::IsCurrent(const AssetReimportTicket& /*ticket*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void AssetHotReloader::SetDeferred(bool /*deferred*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool AssetHotReloader::IsDeferred() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	std::vector<FileChange> AssetHotReloader::GetDeferredChanges() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
