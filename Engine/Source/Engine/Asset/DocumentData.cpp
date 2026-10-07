#include "EnginePCH.h"
#include "Engine/Asset/DocumentData.h"

// M6 contract stub (Roadmap rule 3): stream A (asset core and registry) implements the scene and prefab payloads.

namespace Engine {

	Result<Buffer> CookDocument(AssetType /*type*/, const Json& /*document*/, uint32_t /*importerVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CookDocument is an M6 contract stub");
	}

	Result<AssetRef<SceneData>> LoadCookedScene(std::span<const std::byte> /*cooked*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "LoadCookedScene is an M6 contract stub");
	}

	Result<AssetRef<PrefabData>> LoadCookedPrefab(std::span<const std::byte> /*cooked*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "LoadCookedPrefab is an M6 contract stub");
	}

}
