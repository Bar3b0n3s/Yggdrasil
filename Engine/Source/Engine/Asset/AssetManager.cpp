#include "EnginePCH.h"
#include "Engine/Asset/AssetManager.h"

// M6 contract stub (Roadmap rule 3): stream A (asset core and registry) implements the shared failure policy: versions,
// diagnostics logged once, placeholders and the procedural built-in cache.

namespace Engine {

	struct AssetManager::State
	{
	};

	std::string_view AssetStateToString(AssetState state)
	{
		switch (state)
		{
			case AssetState::Unloaded: return "Unloaded";
			case AssetState::Loading:  return "Loading";
			case AssetState::Loaded:   return "Loaded";
			case AssetState::Failed:   return "Failed";
		}
		ENGINE_CORE_ASSERT(false, "Unknown AssetState {}", std::to_underlying(state));
		return "Unloaded";
	}

	AssetManager::AssetManager()
		: m_State(CreateScope<State>())
	{
	}

	AssetManager::~AssetManager() = default;

	AssetRef<Asset> AssetManager::GetPlaceholder(AssetType /*type*/)
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	uint64_t AssetManager::GetVersion(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::span<const AssetDiagnostic> AssetManager::GetDiagnostics() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	bool AssetManager::HasErrorDiagnostics() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void AssetManager::ReportDiagnostic(AssetDiagnostic /*diagnostic*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AssetManager::BumpVersion(AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AssetManager::ClearDiagnostics(AssetHandle /*handle*/, std::span<const std::string_view> /*codes*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AssetManager::SetScanDiagnostics(std::vector<AssetDiagnostic> /*diagnostics*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<AssetRef<Asset>> AssetManager::GetProceduralBuiltin(AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetManager::GetProceduralBuiltin is an M6 contract stub");
	}

	void AssetManager::SaveSharedState()
	{
		ENGINE_CONTRACT_STUB();
	}

	void AssetManager::RestoreSharedState()
	{
		ENGINE_CONTRACT_STUB();
	}

	void AssetManager::ReportUnavailable(AssetHandle /*handle*/, AssetType /*expectedType*/, const Error* /*error*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
