#include "EnginePCH.h"
#include "Engine/Asset/RuntimeAssetManager.h"

#include "Engine/Core/Jobs/JobSystem.h"

// M6 contract stub (Roadmap rule 3): stream D (caches, pak, PakMount) implements the pak-backed asset manager.

namespace Engine {

	struct RuntimeAssetManager::State
	{
		RuntimeAssetManagerSpecification Specification;
	};

	RuntimeAssetManager::RuntimeAssetManager(const RuntimeAssetManagerSpecification& specification)
		: m_State(CreateScope<State>())
	{
		m_State->Specification = specification;
	}

	RuntimeAssetManager::~RuntimeAssetManager() = default;

	Status RuntimeAssetManager::AddPak(Ref<const PakReader> /*pak*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "RuntimeAssetManager::AddPak is an M6 contract stub");
	}

	Result<AssetRef<Asset>> RuntimeAssetManager::Load(AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "RuntimeAssetManager::Load is an M6 contract stub");
	}

	JobHandle<AssetRef<Asset>> RuntimeAssetManager::LoadAsync(AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Specification.Jobs->Submit([]() -> Result<AssetRef<Asset>>
		{
			return MakeError(ErrorCode::Unsupported, "RuntimeAssetManager::LoadAsync is an M6 contract stub");
		});
	}

	AssetState RuntimeAssetManager::GetState(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return AssetState::Unloaded;
	}

	const AssetMetadata* RuntimeAssetManager::GetMetadata(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	AssetType RuntimeAssetManager::GetAssetType(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return AssetType::None;
	}

	std::optional<AssetHandle> RuntimeAssetManager::Resolve(std::string_view /*reference*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::string RuntimeAssetManager::GetReferencePath(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void RuntimeAssetManager::WaitIdle()
	{
		ENGINE_CONTRACT_STUB();
	}

}
